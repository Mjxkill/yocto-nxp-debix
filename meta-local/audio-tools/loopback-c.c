/*
 * loopback-c — minimal low-latency 8ch ALSA loopback for SAI7
 *
 * capture hw:2,0 -> playback hw:2,1
 * Format S32_LE 48000 Hz 8 channels (V4.2 SOF native).
 *
 * Channels are remixed 8 7 6 5 4 3 2 1 (input ch1 -> output ch8, etc.).
 * Pass --no-swap as first arg (or set arg1 = 0) to disable the remix.
 *
 * Single thread, blocking RW. Uses small period and buffer to minimise
 * latency. Locks memory, raises scheduler to SCHED_FIFO.
 *
 * Usage:
 *   loopback-c [period_frames] [n_periods] [swap] [tone] [gain]
 *     swap=1 (default) : channels reversed (ch1<->ch8, ch2<->ch7, ...)
 *     swap=0           : direct mapping ch_i -> ch_i
 *     tone=1           : ignore capture, generate 1 kHz sine on all 8 ch
 *                        (sanity check that the writei path is reaching HP)
 *     gain=N           : multiply each sample by N before writei (default 1).
 *                        Use gain=100 to boost a weak mic signal (debug).
 * Defaults: period=64 frames (1.33 ms @ 48k), n_periods=2 (buffer=2.66 ms),
 *           swap=1, tone=0, gain=1.
 *
 * Build (native gcc on board with libasound headers installed):
 *   gcc -O2 -Wall loopback-c.c -lasound -o loopback-c
 *
 * Stop with Ctrl+C.
 */

#include <alsa/asoundlib.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/mman.h>
#include <sched.h>

#define CHANNELS 8
#define RATE     48000
#define FORMAT   SND_PCM_FORMAT_S32_LE
#define CDEV     "hw:2,0"
#define PDEV     "hw:2,0"  /* PCM_DUPLEX_ADD : 1 seul device duplex */
#define BYTES_PER_FRAME (CHANNELS * 4)

static volatile sig_atomic_t running = 1;
static void on_sigint(int sig) { (void)sig; running = 0; }

static int set_hw_params(snd_pcm_t *pcm, const char *name,
                         snd_pcm_uframes_t period, snd_pcm_uframes_t buffer)
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
        fprintf(stderr, "%s: format: %s\n", name, snd_strerror(err)); return err;
    }
    if ((err = snd_pcm_hw_params_set_channels(pcm, hw, CHANNELS)) < 0) {
        fprintf(stderr, "%s: channels: %s\n", name, snd_strerror(err)); return err;
    }
    unsigned int rate = RATE;
    if ((err = snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, 0)) < 0) {
        fprintf(stderr, "%s: rate: %s\n", name, snd_strerror(err)); return err;
    }
    if ((err = snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, 0)) < 0) {
        fprintf(stderr, "%s: period: %s\n", name, snd_strerror(err)); return err;
    }
    if ((err = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer)) < 0) {
        fprintf(stderr, "%s: buffer: %s\n", name, snd_strerror(err)); return err;
    }
    if ((err = snd_pcm_hw_params(pcm, hw)) < 0) {
        fprintf(stderr, "%s: hw_params: %s\n", name, snd_strerror(err)); return err;
    }
    snd_pcm_hw_params_get_period_size(hw, &period, 0);
    snd_pcm_hw_params_get_buffer_size(hw, &buffer);
    fprintf(stderr, "%s: period=%lu frames (%lu us), buffer=%lu frames (%lu us)\n",
            name, period, period * 1000000 / RATE,
            buffer, buffer * 1000000 / RATE);
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

static void vu_bar(double db, char *out, int width)
{
    if (db < -90.0) db = -90.0;
    if (db >  0.0)  db =  0.0;
    int n = (int)((db + 90.0) / 90.0 * width);
    int hot = width * 9 / 10;
    int i;
    for (i = 0; i < n; i++) out[i] = (i < hot) ? '#' : '!';
    for (; i < width; i++) out[i] = '.';
    out[width] = 0;
}

int main(int argc, char **argv)
{
    /* Defaults validated 2026-04-29 on Debix/i.MX8MP DMA 2ms:
     * period=256 frames (5.33ms ALSA, multiple of DSP 2ms period),
     * n_periods=2 (buffer 10.66ms), swap=0 (direct mapping).
     * Smaller period (e.g. 64) blocks at start ~80% of the time due
     * to ALSA-vs-DSP period mismatch races on PCM_DUPLEX.
     */
    snd_pcm_uframes_t period = 256;
    int n_periods = 2;
    int swap = 0;
    int tone = 0;
    if (argc > 1) period = (snd_pcm_uframes_t)atoi(argv[1]);
    if (argc > 2) n_periods = atoi(argv[2]);
    if (argc > 3) swap = atoi(argv[3]);
    if (argc > 4) tone = atoi(argv[4]);
    if (n_periods < 2) n_periods = 2;
    snd_pcm_uframes_t buffer = period * n_periods;

    signal(SIGINT, on_sigint);
    /* No mlockall / SCHED_FIFO: those can interact badly with DMA buffer
     * pinning on this kernel — sox doesn't use them either. */

    snd_pcm_t *cap = NULL, *play = NULL;
    int err;
    if (!tone) {
        if ((err = snd_pcm_open(&cap, CDEV, SND_PCM_STREAM_CAPTURE, 0)) < 0) {
            fprintf(stderr, "open %s: %s\n", CDEV, snd_strerror(err)); return 1;
        }
    }
    if ((err = snd_pcm_open(&play, PDEV, SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
        fprintf(stderr, "open %s: %s\n", PDEV, snd_strerror(err)); return 1;
    }
    if (cap && set_hw_params(cap, "capture", period, buffer) < 0) return 1;
    if (set_hw_params(play, "playback", period, buffer) < 0) return 1;
    /* playback starts as soon as one period is queued (low-latency mode) */
    set_sw_params(play, period);
    /* capture wakeup every period */
    if (cap) set_sw_params(cap, period);

    int32_t *buf = malloc(period * BYTES_PER_FRAME);
    if (!buf) { perror("malloc"); return 1; }

    /* Start capture FIRST. On this SOF setup, pre-filling/starting playback
     * before capture broke the capture pipeline (silent samples). */
    if (cap) {
        if (snd_pcm_link(cap, play) < 0)
            fprintf(stderr, "snd_pcm_link not supported (continuing unlinked)\n");
        snd_pcm_start(cap);
    }

    /* Pre-fill playback with the full buffer of silence so it never starves
     * waiting for the first capture frames. Retry on transient -EPIPE
     * because cap+play start race may put play in XRUN before pre-fill
     * gets to push data. */
    int32_t *silence = calloc(buffer, BYTES_PER_FRAME);
    snd_pcm_sframes_t pf = -1;
    for (int retry = 0; retry < 5 && pf < 0; retry++) {
        pf = snd_pcm_writei(play, silence, buffer);
        if (pf < 0) {
            int recover = snd_pcm_recover(play, pf, 1);
            if (recover < 0) {
                fprintf(stderr, "pre-fill recover: %s\n", snd_strerror(recover));
                break;
            }
        }
    }
    if (pf < 0)
        fprintf(stderr, "pre-fill writei after retry: %s\n", snd_strerror(pf));
    free(silence);
    if (!cap) snd_pcm_start(play);  /* tone mode: no capture, start play here */

    fprintf(stderr, "loop running. period=%lu, n_periods=%d, buffer=%lu (~%lu us per side)  swap=%d tone=%d\n",
            period, n_periods, buffer, buffer * 1000000 / RATE, swap, tone);

    long frames_in = 0, frames_out = 0;
    long xrun_cap = 0, xrun_play = 0;

    /* Sine generator state for tone mode */
    double phase = 0.0;
    const double phase_inc = 2.0 * 3.14159265358979 * 1000.0 / RATE; /* 1 kHz */
    const int32_t amplitude = 100000000; /* ~ -27 dBFS at 1.0 normalised */

    /* VU meter accumulators (refresh every 50 ms) */
    double vu_peak[CHANNELS] = {0}, vu_rms_acc[CHANNELS] = {0};
    long vu_frames = 0;
    const long vu_refresh_frames = RATE / 20;   /* 50 ms */
    int vu_printed = 0;

    while (running) {
        snd_pcm_sframes_t r;
        if (tone) {
            /* Synthesize one period of 1 kHz sine on all 8 channels. */
            for (snd_pcm_uframes_t f = 0; f < period; f++) {
                int32_t s = (int32_t)(amplitude * sin(phase));
                int32_t *frame = buf + f * CHANNELS;
                for (int c = 0; c < CHANNELS; c++) frame[c] = s;
                phase += phase_inc;
                if (phase >= 2.0 * 3.14159265358979) phase -= 2.0 * 3.14159265358979;
            }
            r = period;
            frames_in += r;
        } else {
            r = snd_pcm_readi(cap, buf, period);
            if (r < 0) {
                xrun_cap++;
                r = snd_pcm_recover(cap, r, 1);
                if (r < 0) { fprintf(stderr, "cap recover fail: %s\n", snd_strerror(r)); break; }
                continue;
            }
            frames_in += r;
        }

        /* Accumulate VU stats from what we just read (before remix). This
         * happens for every successful readi, independent of the writei
         * outcome, so xrun on playback don't freeze the display. */
        for (snd_pcm_sframes_t f = 0; f < r; f++) {
            int32_t *frame = buf + f * CHANNELS;
            for (int c = 0; c < CHANNELS; c++) {
                double v = (double)frame[c] / 2147483648.0;
                double a = v < 0 ? -v : v;
                if (a > vu_peak[c]) vu_peak[c] = a;
                vu_rms_acc[c] += v * v;
            }
        }
        vu_frames += r;

        /* In-place channel reversal: ch0<->ch7, ch1<->ch6, ch2<->ch5, ch3<->ch4 */
        if (swap && !tone) {
            for (snd_pcm_sframes_t f = 0; f < r; f++) {
                int32_t *frame = buf + f * CHANNELS;
                int32_t t;
                t = frame[0]; frame[0] = frame[7]; frame[7] = t;
                t = frame[1]; frame[1] = frame[6]; frame[6] = t;
                t = frame[2]; frame[2] = frame[5]; frame[5] = t;
                t = frame[3]; frame[3] = frame[4]; frame[4] = t;
            }
        }

        snd_pcm_sframes_t w = snd_pcm_writei(play, buf, r);
        if (w < 0) {
            xrun_play++;
            w = snd_pcm_recover(play, w, 1);
            if (w < 0) { fprintf(stderr, "play recover fail: %s\n", snd_strerror(w)); break; }
            /* After recovery the stream is in PREPARED state — restart it
             * so playback keeps flowing. */
            snd_pcm_start(play);
            continue;
        }
        frames_out += w;

        /* Refresh VU display every 50 ms (in place, 9 lines overwrite). */
        if (vu_frames >= vu_refresh_frames) {
            char b_pk[33], b_rms[33];
            if (vu_printed) printf("\033[9A");  /* up 9 lines */
            for (int c = 0; c < CHANNELS; c++) {
                double pk_db = vu_peak[c] > 0 ? 20.0 * log10(vu_peak[c]) : -120.0;
                double rms   = sqrt(vu_rms_acc[c] / vu_frames);
                double rm_db = rms        > 0 ? 20.0 * log10(rms)        : -120.0;
                vu_bar(pk_db, b_pk, 20);
                vu_bar(rm_db, b_rms, 20);
                printf("\033[2K ch%d  pk %6.1f %s  rms %6.1f %s\n",
                       c + 1, pk_db, b_pk, rm_db, b_rms);
                vu_peak[c] = 0; vu_rms_acc[c] = 0;
            }
            printf("\033[2K xrun cap=%ld play=%ld  in=%ld out=%ld\n",
                   xrun_cap, xrun_play, frames_in, frames_out);
            fflush(stdout);
            vu_frames = 0;
            vu_printed = 1;
        }
    }

    fprintf(stderr, "\nstopped. in=%ld out=%ld xrun cap=%ld play=%ld\n",
            frames_in, frames_out, xrun_cap, xrun_play);

    snd_pcm_drain(play);
    snd_pcm_close(play);
    if (cap) snd_pcm_close(cap);
    free(buf);
    return 0;
}
