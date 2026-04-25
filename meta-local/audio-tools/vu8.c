/*
 * vu8 — text 8-channel VU-meter for hw:2,0 (SAI_Capture).
 *
 * Reads S32_LE 48kHz 8ch from the SOF capture device and prints peak +
 * RMS dBFS for each channel, refreshed every ~50 ms.
 *
 * Build: gcc -O2 -Wall vu8.c -lasound -lm -o vu8
 * Usage: vu8 [period_ms]    (default 50)
 *        Ctrl+C to stop.
 */

#include <alsa/asoundlib.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CDEV     "hw:2,0"
#define CHANNELS 8
#define RATE     48000

static volatile sig_atomic_t running = 1;
static void on_int(int s) { (void)s; running = 0; }

static void bar(double db, char *out, int width)
{
    /* db in [-90, 0] -> [0, width] */
    if (db < -90.0) db = -90.0;
    if (db > 0.0)  db = 0.0;
    int n = (int)((db + 90.0) / 90.0 * width);
    int i;
    for (i = 0; i < n; i++) out[i] = (i < width * 8 / 10) ? '#' : '!';
    for (; i < width; i++) out[i] = '.';
    out[width] = 0;
}

int main(int argc, char **argv)
{
    int period_ms = 50;
    if (argc > 1) period_ms = atoi(argv[1]);
    if (period_ms < 10) period_ms = 10;

    snd_pcm_uframes_t period = (snd_pcm_uframes_t)(RATE * period_ms / 1000);
    snd_pcm_uframes_t buffer = period * 4;

    signal(SIGINT, on_int);

    snd_pcm_t *cap;
    int err;
    if ((err = snd_pcm_open(&cap, CDEV, SND_PCM_STREAM_CAPTURE, 0)) < 0) {
        fprintf(stderr, "open %s: %s\n", CDEV, snd_strerror(err)); return 1;
    }

    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(cap, hw);
    snd_pcm_hw_params_set_access(cap, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(cap, hw, SND_PCM_FORMAT_S32_LE);
    snd_pcm_hw_params_set_channels(cap, hw, CHANNELS);
    unsigned int rate = RATE;
    snd_pcm_hw_params_set_rate_near(cap, hw, &rate, 0);
    snd_pcm_hw_params_set_period_size_near(cap, hw, &period, 0);
    snd_pcm_hw_params_set_buffer_size_near(cap, hw, &buffer);
    if ((err = snd_pcm_hw_params(cap, hw)) < 0) {
        fprintf(stderr, "hw_params: %s\n", snd_strerror(err)); return 1;
    }

    int32_t *buf = malloc(period * CHANNELS * sizeof(int32_t));
    if (!buf) { perror("malloc"); return 1; }

    double peak[CHANNELS], rms_acc[CHANNELS];
    char b_pk[33], b_rms[33];

    fprintf(stderr, "vu8: period=%lu frames (%d ms), Ctrl+C to stop\n", period, period_ms);
    fprintf(stderr, "scale -90 dBFS .....|.....| 0 dBFS  (# = signal, ! = hot)\n");
    /* hide cursor */
    printf("\033[?25l");
    fflush(stdout);

    snd_pcm_start(cap);

    while (running) {
        snd_pcm_sframes_t r = snd_pcm_readi(cap, buf, period);
        if (r < 0) {
            r = snd_pcm_recover(cap, r, 1);
            if (r < 0) break;
            continue;
        }
        for (int c = 0; c < CHANNELS; c++) { peak[c] = 0.0; rms_acc[c] = 0.0; }
        for (snd_pcm_sframes_t f = 0; f < r; f++) {
            int32_t *frame = buf + f * CHANNELS;
            for (int c = 0; c < CHANNELS; c++) {
                double v = (double)frame[c] / 2147483648.0;
                if (v < 0) v = -v;
                if (v > peak[c]) peak[c] = v;
                rms_acc[c] += v * v;
            }
        }
        /* clear screen + home cursor */
        printf("\033[H\033[2J");
        printf(" ch  peak (dBFS)            rms (dBFS)\n");
        for (int c = 0; c < CHANNELS; c++) {
            double pk_db = peak[c]   > 0 ? 20.0 * log10(peak[c])             : -120.0;
            double rms   = sqrt(rms_acc[c] / r);
            double rm_db = rms       > 0 ? 20.0 * log10(rms)                  : -120.0;
            bar(pk_db, b_pk, 20);
            bar(rm_db, b_rms, 20);
            printf(" %d  %6.1f  %s  %6.1f  %s\n", c + 1, pk_db, b_pk, rm_db, b_rms);
        }
        fflush(stdout);
    }

    /* show cursor again */
    printf("\033[?25h");
    snd_pcm_close(cap);
    free(buf);
    return 0;
}
