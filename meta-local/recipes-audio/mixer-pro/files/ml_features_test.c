/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * V9.5.12 — ml_features_test : test parité Python ↔ C.
 *
 * Lit un wav stéréo 48 kHz S32_LE, calcule frame par frame les 11 features
 * mid-only (hop 512) et écrit un CSV.
 *
 * Build (on board) :
 *   gcc -O2 -Wall -o ml_features_test ml_features_test.c ml_features.c \
 *       -lfftw3f -lm
 *
 * Usage :
 *   ml_features_test <input.wav> <output.csv>
 *
 * Parité attendue avec features.py compute_features_mid_only(audio, sr=48000) :
 *   tolérance |Δ| < 0.1 dB sur band_db, < 5 Hz sur centroid_hz.
 */

#define _POSIX_C_SOURCE 200809L
#include "ml_features.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Minimal WAV reader : assume RIFF/WAVE PCM 16-bit ou 24-bit ou 32-bit stéréo
 * @ 48 kHz. Lit le header, retourne le data interleaved en int32 normalisé.
 */
struct wav_info {
    int n_channels;
    int sample_rate;
    int bits_per_sample;
    long n_samples;          /* frames (per channel) */
    int32_t *data;           /* interleaved [n_samples × n_channels] */
};

static int read_wav(const char *path, struct wav_info *w)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) { perror("fopen"); return -1; }

    char riff[4], wave[4], chunk[4];
    uint32_t chunk_size, fmt_size, data_size;
    uint16_t audio_fmt, n_ch, bits;
    uint32_t sr, byte_rate;
    uint16_t block_align;

    if (fread(riff, 1, 4, fp) != 4) goto err;
    if (memcmp(riff, "RIFF", 4)) { fprintf(stderr, "not RIFF\n"); goto err; }
    if (fread(&chunk_size, 4, 1, fp) != 1) goto err;
    if (fread(wave, 1, 4, fp) != 4) goto err;
    if (memcmp(wave, "WAVE", 4)) { fprintf(stderr, "not WAVE\n"); goto err; }

    /* Parse chunks. */
    while (fread(chunk, 1, 4, fp) == 4) {
        if (fread(&chunk_size, 4, 1, fp) != 1) goto err;
        if (memcmp(chunk, "fmt ", 4) == 0) {
            fmt_size = chunk_size;
            if (fread(&audio_fmt, 2, 1, fp) != 1) goto err;
            if (fread(&n_ch, 2, 1, fp) != 1) goto err;
            if (fread(&sr, 4, 1, fp) != 1) goto err;
            if (fread(&byte_rate, 4, 1, fp) != 1) goto err;
            if (fread(&block_align, 2, 1, fp) != 1) goto err;
            if (fread(&bits, 2, 1, fp) != 1) goto err;
            if (fmt_size > 16) fseek(fp, fmt_size - 16, SEEK_CUR);
        } else if (memcmp(chunk, "data", 4) == 0) {
            data_size = chunk_size;
            w->n_channels      = n_ch;
            w->sample_rate     = sr;
            w->bits_per_sample = bits;
            w->n_samples       = data_size / (n_ch * (bits / 8));
            w->data = malloc(w->n_samples * n_ch * sizeof(int32_t));
            if (!w->data) { fprintf(stderr, "malloc fail\n"); goto err; }
            /* Read sample-by-sample, expand to int32. */
            long total = w->n_samples * n_ch;
            for (long i = 0; i < total; i++) {
                if (bits == 16) {
                    int16_t s; if (fread(&s, 2, 1, fp) != 1) { w->n_samples = i / n_ch; break; }
                    w->data[i] = ((int32_t)s) << 16;
                } else if (bits == 24) {
                    uint8_t b[3]; if (fread(b, 1, 3, fp) != 3) { w->n_samples = i / n_ch; break; }
                    int32_t s = ((int32_t)b[0] << 8) | ((int32_t)b[1] << 16) | ((int32_t)b[2] << 24);
                    w->data[i] = s;
                } else if (bits == 32) {
                    int32_t s; if (fread(&s, 4, 1, fp) != 1) { w->n_samples = i / n_ch; break; }
                    w->data[i] = s;
                } else {
                    fprintf(stderr, "unsupported bps %u\n", bits); goto err;
                }
            }
            fclose(fp);
            return 0;
        } else {
            fseek(fp, chunk_size, SEEK_CUR);
        }
    }
err:
    fclose(fp);
    return -1;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <input.wav> <output.csv>\n", argv[0]);
        return 1;
    }

    struct wav_info w = {0};
    if (read_wav(argv[1], &w) < 0) {
        fprintf(stderr, "failed to read %s\n", argv[1]);
        return 1;
    }
    fprintf(stderr, "wav: %d ch %d Hz %d-bit, %ld samples (%.1f s)\n",
            w.n_channels, w.sample_rate, w.bits_per_sample,
            w.n_samples, (float)w.n_samples / w.sample_rate);

    if (w.n_channels < 2) {
        fprintf(stderr, "expected stereo (2+ ch), got %d\n", w.n_channels);
        free(w.data);
        return 1;
    }
    if (w.sample_rate != ML_FEATURES_SAMPLE_RATE) {
        fprintf(stderr, "expected sr=%d, got %d\n",
                ML_FEATURES_SAMPLE_RATE, w.sample_rate);
        free(w.data);
        return 1;
    }

    if (ml_features_init() < 0) {
        fprintf(stderr, "ml_features_init failed\n");
        free(w.data);
        return 1;
    }

    /* De-interleave L et R sur la fly dans 2 buffers de 1024 samples. */
    int32_t L[ML_FEATURES_FRAME_SIZE], R[ML_FEATURES_FRAME_SIZE];
    FILE *fp = fopen(argv[2], "w");
    if (!fp) { perror("fopen out"); free(w.data); return 1; }

    fprintf(fp, "frame,mid_b0_db,mid_b1_db,mid_b2_db,mid_b3_db,mid_b4_db,"
                "mid_b0_centroid_hz,mid_b1_centroid_hz,mid_b2_centroid_hz,"
                "mid_b3_centroid_hz,mid_b4_centroid_hz,mid_global_db\n");

    const int hop = ML_FEATURES_HOP_SIZE;
    const int N   = ML_FEATURES_FRAME_SIZE;
    long n_frames = (w.n_samples >= N) ? ((w.n_samples - N) / hop + 1) : 0;
    fprintf(stderr, "computing %ld frames (hop=%d)...\n", n_frames, hop);

    ml_features_t feats;
    for (long f = 0; f < n_frames; f++) {
        long i0 = f * hop;
        for (int n = 0; n < N; n++) {
            L[n] = w.data[(i0 + n) * w.n_channels + 0];
            R[n] = w.data[(i0 + n) * w.n_channels + 1];
        }
        ml_features_process_frame(L, R, &feats);
        fprintf(fp, "%ld,", f);
        for (int b = 0; b < ML_FEATURES_N_BANDS; b++)
            fprintf(fp, "%.6f,", feats.mid_band_db[b]);
        for (int b = 0; b < ML_FEATURES_N_BANDS; b++)
            fprintf(fp, "%.6f,", feats.mid_centroid_hz[b]);
        fprintf(fp, "%.6f\n", feats.mid_global_db);
    }
    fclose(fp);
    ml_features_cleanup();
    free(w.data);
    fprintf(stderr, "wrote %ld frames to %s\n", n_frames, argv[2]);
    return 0;
}
