/*
 * loopback-c — 8ch ALSA loopback (2 threads + ring buffer architecture)
 *
 * capture hw:2,0 -> playback hw:2,0 (PCM_DUPLEX device, same hw:2,0)
 * Format S32_LE 48000 Hz 8 channels (SOF native).
 *
 * Architecture:
 *   - cap_thread  : snd_pcm_readi(cap) → push into ring buffer
 *   - play_thread : pop from ring buffer → snd_pcm_writei(play)
 *   - Main thread : init, SIGINT handler, cleanup
 *   - Ring buffer : 4 periods deep, mutex + condvar synchronisation
 *
 * No snd_pcm_link: the ring buffer absorbs inter-stream jitter. Both
 * cap and play are clocked by the same physical SAI BCLK so samples
 * produced ≡ samples consumed in steady state — the ring buffer never
 * grows or shrinks unboundedly.
 *
 * Defaults: period=256 (5.33 ms), n_periods=2 (buffer=10.66 ms),
 *           swap=0 (direct mapping ch_i → ch_i), tone=0
 *
 * Build (on board, requires alsa-lib + pthread headers):
 *   gcc -O2 -Wall loopback-c.c -lasound -lm -lpthread -o loopback-c
 *
 * Stop with Ctrl+C.
 */

#include <alsa/asoundlib.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdatomic.h>

#define CHANNELS 8
#define RATE     48000
#define FORMAT   SND_PCM_FORMAT_S32_LE
#define CDEV     "hw:2,0"
#define PDEV     "hw:2,0"
#define BYTES_PER_FRAME (CHANNELS * 4)
#define RING_FRAMES 384    /* 1.5× default period @ 256; 8 ms max ring latency */

/* ========== Globals ========== */
static volatile sig_atomic_t running = 1;
static void on_sigint(int sig) { (void)sig; running = 0; }

/* Ring buffer (CHANNELS interleaved samples per frame) */
static int32_t *ring_buf;        /* RING_FRAMES * CHANNELS samples */
static size_t ring_w = 0;         /* total frames written by cap thread */
static size_t ring_r = 0;         /* total frames read by play thread */
static pthread_mutex_t ring_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ring_cond_data = PTHREAD_COND_INITIALIZER;
static pthread_cond_t ring_cond_space = PTHREAD_COND_INITIALIZER;

/* Stats */
static atomic_long stat_in = 0;
static atomic_long stat_out = 0;
static atomic_long stat_xrun_cap = 0;
static atomic_long stat_xrun_play = 0;
static atomic_long stat_overrun_ring = 0;   /* ring full, cap dropped */

/* Common params */
static snd_pcm_uframes_t period;
static snd_pcm_uframes_t buffer;
static int swap = 0;
static int tone = 0;

/* ========== ALSA helpers ========== */
static int set_hw_params(snd_pcm_t *pcm, const char *name)
{
    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    int err;
    if ((err = snd_pcm_hw_params_any(pcm, hw)) < 0) {
        fprintf(stderr, "%s: hw_params_any: %s\n", name, snd_strerror(err));
        return err;
    }
    snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    if ((err = snd_pcm_hw_params_set_format(pcm, hw, FORMAT)) < 0) {
        fprintf(stderr, "%s: format: %s\n", name, snd_strerror(err));
        return err;
    }
    if ((err = snd_pcm_hw_params_set_channels(pcm, hw, CHANNELS)) < 0) {
        fprintf(stderr, "%s: channels: %s\n", name, snd_strerror(err));
        return err;
    }
    unsigned int rate = RATE;
    if ((err = snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, 0)) < 0) {
        fprintf(stderr, "%s: rate: %s\n", name, snd_strerror(err));
        return err;
    }
    snd_pcm_uframes_t p = period, b = buffer;
    if ((err = snd_pcm_hw_params_set_period_size_near(pcm, hw, &p, 0)) < 0) {
        fprintf(stderr, "%s: period: %s\n", name, snd_strerror(err));
        return err;
    }
    if ((err = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &b)) < 0) {
        fprintf(stderr, "%s: buffer: %s\n", name, snd_strerror(err));
        return err;
    }
    if ((err = snd_pcm_hw_params(pcm, hw)) < 0) {
        fprintf(stderr, "%s: hw_params: %s\n", name, snd_strerror(err));
        return err;
    }
    snd_pcm_hw_params_get_period_size(hw, &p, 0);
    snd_pcm_hw_params_get_buffer_size(hw, &b);
    fprintf(stderr, "%s: period=%lu (%lu us) buffer=%lu (%lu us)\n",
            name, p, p * 1000000UL / RATE, b, b * 1000000UL / RATE);
    return 0;
}

static int set_sw_params(snd_pcm_t *pcm, snd_pcm_uframes_t start)
{
    snd_pcm_sw_params_t *sw;
    snd_pcm_sw_params_alloca(&sw);
    snd_pcm_sw_params_current(pcm, sw);
    snd_pcm_sw_params_set_start_threshold(pcm, sw, start);
    snd_pcm_sw_params_set_avail_min(pcm, sw, start);
    return snd_pcm_sw_params(pcm, sw);
}

/* ========== Threads ========== */

/* Capture thread: snd_pcm_readi → ring buffer */
static void *cap_thread_fn(void *arg)
{
    snd_pcm_t *cap = (snd_pcm_t *)arg;
    int32_t *buf = malloc(period * BYTES_PER_FRAME);
    if (!buf) return NULL;

    while (running) {
        snd_pcm_sframes_t r;
        if (tone) {
            /* tone mode: synthesise on all 8ch directly here so the
             * ring stays fed without needing a real cap.
             */
            static double phase = 0.0;
            const double phase_inc = 2.0 * M_PI * 1000.0 / RATE;
            const int32_t amp = 100000000;
            for (snd_pcm_uframes_t f = 0; f < period; f++) {
                int32_t s = (int32_t)(amp * sin(phase));
                int32_t *frame = buf + f * CHANNELS;
                for (int c = 0; c < CHANNELS; c++) frame[c] = s;
                phase += phase_inc;
                if (phase >= 2.0 * M_PI) phase -= 2.0 * M_PI;
            }
            r = period;
            /* avoid spinning faster than realtime */
            usleep((useconds_t)(period * 1000000UL / RATE));
        } else {
            r = snd_pcm_readi(cap, buf, period);
            if (r < 0) {
                atomic_fetch_add(&stat_xrun_cap, 1);
                r = snd_pcm_recover(cap, r, 1);
                if (r < 0) {
                    fprintf(stderr, "cap recover fail: %s\n", snd_strerror(r));
                    break;
                }
                continue;
            }
        }

        /* Channel reversal if swap enabled */
        if (swap) {
            for (snd_pcm_sframes_t f = 0; f < r; f++) {
                int32_t *frame = buf + f * CHANNELS;
                int32_t t;
                t = frame[0]; frame[0] = frame[7]; frame[7] = t;
                t = frame[1]; frame[1] = frame[6]; frame[6] = t;
                t = frame[2]; frame[2] = frame[5]; frame[5] = t;
                t = frame[3]; frame[3] = frame[4]; frame[4] = t;
            }
        }

        /* Push into ring */
        pthread_mutex_lock(&ring_mtx);
        size_t avail_space = RING_FRAMES - (ring_w - ring_r);
        if (avail_space < (size_t)r) {
            /* Ring full: drop the oldest period to make room */
            ring_r += (size_t)r - avail_space;
            atomic_fetch_add(&stat_overrun_ring, 1);
        }
        for (snd_pcm_sframes_t i = 0; i < r; i++) {
            size_t pos = (ring_w + i) % RING_FRAMES;
            memcpy(&ring_buf[pos * CHANNELS],
                   &buf[i * CHANNELS], BYTES_PER_FRAME);
        }
        ring_w += r;
        atomic_fetch_add(&stat_in, r);
        pthread_cond_signal(&ring_cond_data);
        pthread_mutex_unlock(&ring_mtx);
    }
    free(buf);
    /* Wake play thread if waiting */
    pthread_mutex_lock(&ring_mtx);
    pthread_cond_broadcast(&ring_cond_data);
    pthread_mutex_unlock(&ring_mtx);
    return NULL;
}

/* Playback thread: ring buffer → snd_pcm_writei */
static void *play_thread_fn(void *arg)
{
    snd_pcm_t *play = (snd_pcm_t *)arg;
    int32_t *buf = malloc(period * BYTES_PER_FRAME);
    if (!buf) return NULL;

    while (running) {
        /* Wait until ring has at least 1 period of frames */
        pthread_mutex_lock(&ring_mtx);
        while ((ring_w - ring_r) < period && running) {
            pthread_cond_wait(&ring_cond_data, &ring_mtx);
        }
        if (!running) {
            pthread_mutex_unlock(&ring_mtx);
            break;
        }
        for (snd_pcm_uframes_t i = 0; i < period; i++) {
            size_t pos = (ring_r + i) % RING_FRAMES;
            memcpy(&buf[i * CHANNELS],
                   &ring_buf[pos * CHANNELS], BYTES_PER_FRAME);
        }
        ring_r += period;
        pthread_cond_signal(&ring_cond_space);
        pthread_mutex_unlock(&ring_mtx);

        snd_pcm_sframes_t w = snd_pcm_writei(play, buf, period);
        if (w < 0) {
            atomic_fetch_add(&stat_xrun_play, 1);
            w = snd_pcm_recover(play, w, 1);
            if (w < 0) {
                fprintf(stderr, "play recover fail: %s\n", snd_strerror(w));
                break;
            }
            continue;
        }
        atomic_fetch_add(&stat_out, w);
    }
    free(buf);
    return NULL;
}

/* ========== Main ========== */
int main(int argc, char **argv)
{
    /* Defaults: period=256 (5.33 ms ALSA period, multiple of DSP 2 ms),
     * n_periods=2 (10.66 ms ALSA buffer), ring 384 (8 ms max).
     * Total end-to-end latency ≈ 8-10 ms.
     */
    period = 256;
    int n_periods = 2;
    if (argc > 1) period = (snd_pcm_uframes_t)atoi(argv[1]);
    if (argc > 2) n_periods = atoi(argv[2]);
    if (argc > 3) swap = atoi(argv[3]);
    if (argc > 4) tone = atoi(argv[4]);
    if (n_periods < 2) n_periods = 2;
    buffer = period * n_periods;

    if (period >= RING_FRAMES) {
        fprintf(stderr, "period too large for RING_FRAMES=%d (must be < ring)\n", RING_FRAMES);
        return 1;
    }

    ring_buf = malloc(RING_FRAMES * BYTES_PER_FRAME);
    if (!ring_buf) { perror("malloc ring"); return 1; }

    signal(SIGINT, on_sigint);

    snd_pcm_t *cap = NULL, *play = NULL;
    int err;
    if (!tone) {
        if ((err = snd_pcm_open(&cap, CDEV, SND_PCM_STREAM_CAPTURE, 0)) < 0) {
            fprintf(stderr, "open %s: %s\n", CDEV, snd_strerror(err));
            return 1;
        }
        if (set_hw_params(cap, "capture") < 0) return 1;
        if (set_sw_params(cap, period) < 0) return 1;
    }
    if ((err = snd_pcm_open(&play, PDEV, SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
        fprintf(stderr, "open %s: %s\n", PDEV, snd_strerror(err));
        return 1;
    }
    if (set_hw_params(play, "playback") < 0) return 1;
    if (set_sw_params(play, period) < 0) return 1;

    /* Pre-fill play with ONE period of silence (not full buffer) to
     * minimise startup latency. start_threshold = period → play
     * auto-triggers when this single period is queued.
     * Followed back-to-back by snd_pcm_start(cap).
     */
    int32_t *silence = calloc(period, BYTES_PER_FRAME);
    snd_pcm_sframes_t pf = snd_pcm_writei(play, silence, period);
    if (pf < 0) pf = snd_pcm_recover(play, pf, 1);
    free(silence);

    if (cap) {
        if ((err = snd_pcm_start(cap)) < 0) {
            fprintf(stderr, "start cap: %s\n", snd_strerror(err));
            return 1;
        }
    }

    /* Pre-fill ring buffer with `period` silence frames so play_thread can
     * pop immediately on start, avoiding initial drops while cap_thread
     * spins up. Subsequent steady-state ring fill stabilises around
     * RING_FRAMES/2 once cap and play settle into BCLK lockstep.
     */
    memset(ring_buf + ring_w * CHANNELS, 0, period * BYTES_PER_FRAME);
    ring_w += period;

    /* Spawn the 2 threads */
    pthread_t tid_cap = 0, tid_play;
    if (cap) pthread_create(&tid_cap, NULL, cap_thread_fn, cap);
    pthread_create(&tid_play, NULL, play_thread_fn, play);

    fprintf(stderr,
            "loop running. period=%lu n_periods=%d buffer=%lu (~%lu us per side)\n"
            " swap=%d tone=%d  ring=%d frames\n"
            "Ctrl+C to stop.\n",
            period, n_periods, buffer, buffer * 1000000UL / RATE,
            swap, tone, RING_FRAMES);

    /* Stats loop in main thread */
    long last_in = 0, last_out = 0;
    while (running) {
        sleep(1);
        long in_now = atomic_load(&stat_in);
        long out_now = atomic_load(&stat_out);
        long xc = atomic_load(&stat_xrun_cap);
        long xp = atomic_load(&stat_xrun_play);
        long ovr = atomic_load(&stat_overrun_ring);
        fprintf(stderr,
                "in=%ld (+%ld f/s) out=%ld (+%ld f/s) "
                "xrun cap=%ld play=%ld ring_drop=%ld ring_fill=%zu\n",
                in_now, in_now - last_in,
                out_now, out_now - last_out, xc, xp, ovr,
                ring_w - ring_r);
        last_in = in_now;
        last_out = out_now;
    }

    /* Wake threads if waiting */
    pthread_mutex_lock(&ring_mtx);
    pthread_cond_broadcast(&ring_cond_data);
    pthread_cond_broadcast(&ring_cond_space);
    pthread_mutex_unlock(&ring_mtx);

    if (tid_cap) pthread_join(tid_cap, NULL);
    pthread_join(tid_play, NULL);

    fprintf(stderr,
            "\nstopped. in=%ld out=%ld xrun cap=%ld play=%ld ring_drop=%ld\n",
            (long)atomic_load(&stat_in),
            (long)atomic_load(&stat_out),
            (long)atomic_load(&stat_xrun_cap),
            (long)atomic_load(&stat_xrun_play),
            (long)atomic_load(&stat_overrun_ring));

    snd_pcm_drain(play);
    snd_pcm_close(play);
    if (cap) snd_pcm_close(cap);
    free(ring_buf);
    return 0;
}
