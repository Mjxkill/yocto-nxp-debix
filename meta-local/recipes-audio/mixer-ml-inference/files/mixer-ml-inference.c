/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * V9.5.20 — mixer-ml-inference : daemon mastering ML enveloppe spectrale.
 *
 * Process isolé (TFLite NPU + galcore incompatibles avec le process RT99
 * de mixer-pro — diag 2026-06-08). Pipeline par cycle de 10 ms :
 *
 *   1. lit 480 samples × 2 canaux (SHM tap USB ou NPU tap HW)
 *   2. features v3 par canal (FFT 1024 + FFT 8192 + Mel/MFCC/BF/ΔMel)
 *   3. 2 invocations NPU (modèle mono 125×10 → 74 outputs par canal)
 *   4. décode : enveloppe 64 (±12 dB, cap ±6 dB > 8 kHz) par canal,
 *      exciter 4 + limiter 6 (moyenne L/R — plugins stéréo à params communs)
 *   5. gate silences : RMS fenêtre < -50 dBFS → params neutres (slew)
 *   6. push bulk → mixer-pro : slot 0 = spectral_env (l_gN / r_gN),
 *      slot 1 = Calf Exciter, slot 2 = LSP Limiter
 *
 * NPU bench : 0.81 ms/invoke → 2 canaux = 1.6 ms / 10 ms = 16 % NPU.
 * Features : ~250 µs/canal (FFTW NEON).
 */

#define _GNU_SOURCE
#include "ml_features_v3.h"
#include "mixer_pro_shm_tap.h"
#include "imx-audio-tap-uapi.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <tensorflow/lite/c/c_api.h>
#include <tensorflow/lite/delegates/external/external_delegate.h>

#define MIXER_PRO_SOCK_PATH  "/run/mixer-pro.sock"
#define MODEL_PATH_DEFAULT   "/etc/mixer-pro/mastering_v5_20_int8.tflite"
#define VX_DELEGATE_SO       "/usr/lib/libvx_delegate.so"
#define NPU_TAP_IN_DEV       "/dev/imx-audio-tap-in"

#define N_OUT       74
#define N_ENV       64
#define CYCLE_NS    (10 * 1000 * 1000)      /* 10 ms = 100 Hz (chaîne 100% native lock-free) */
#define POLL_STATE_NS (500 * 1000 * 1000)

#define SRC_PASSTHROUGH 0
#define SRC_HW_IN       1
#define SRC_USB_IN      2

/* Gate silences (le modèle n'a jamais vu de silence au training) */
#define GATE_RMS_DB     -65.0f   /* -50 coupait la musique calme (intro mesurée -56 dB) */
#define GATE_SLEW       0.05f

/* Ranges (parité model.py PARAM_RANGES_V5_17 + ENV ±12) */
static const float R_EXC[4][2] = {{0,1},{1,6},{5000,12000},{0.5f,1}};
static const float R_LIM[6][2] = {{-12,0},{0.85f,0.99f},{0.3f,10},{10,200},{0,18},{-6,0}};
#define ENV_RANGE_DB 12.0f
#define ENV_HF_CAP_DB 6.0f
#define ENV_HF_FIRST_BAND 55       /* bandes 55..63 > 8 kHz (20×1000^(b/63)) */

static volatile int g_should_exit = 0;
static int g_source = -1;   /* -1 = inconnu → le 1er poll déclenche la "transition" (et neutralise si passthrough) */
static float g_gate = 1.0f;        /* 1 = signal, 0 = silence (params neutres) */

static TfLiteModel              *g_model = NULL;
static TfLiteDelegate           *g_delegate = NULL;
static TfLiteInterpreterOptions *g_opts = NULL;
static TfLiteInterpreter        *g_itp = NULL;

static mlf3_state_t g_feat[2];     /* L, R */

/* --- taps --- */
static struct npu_tap_hdr *g_hw_hdr = NULL;
static int32_t *g_hw_ring = NULL;
static uint32_t g_hw_read_bytes = 0, g_hw_epoch = 0;

static struct mixer_pro_tap_hdr *g_usb_hdr = NULL;
static float *g_usb_ring = NULL;
static uint32_t g_usb_read = 0, g_usb_epoch = 0;

static void on_signal(int sig) { (void)sig; g_should_exit = 1; }

static int sock_send_recv(const char *req, char *resp, int resp_size)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    strncpy(sa.sun_path, MIXER_PRO_SOCK_PATH, sizeof(sa.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) { close(fd); return -1; }
    int len = (int)strlen(req);
    if (write(fd, req, len) != len) { close(fd); return -1; }
    int n = (int)read(fd, resp, resp_size - 1);
    if (n > 0) resp[n] = '\0';
    close(fd);
    return n;
}

static int poll_assistant_state(void)
{
    char resp[512];
    if (sock_send_recv("{\"op\":\"get_assistant\"}\n", resp, sizeof(resp)) < 0)
        return SRC_PASSTHROUGH;
    if (!strstr(resp, "\"mode\":\"mastering\"")) return SRC_PASSTHROUGH;
    if (strstr(resp, "\"source\":\"usb\"")) return SRC_USB_IN;
    if (strstr(resp, "\"source\":\"hw\""))  return SRC_HW_IN;
    return SRC_PASSTHROUGH;
}

/* --- HW IN : mmap NPU tap (8ch S32, ch0/1 = L/R) --- */
static int hw_tap_open(void)
{
    if (g_hw_hdr) return 0;
    int fd = open(NPU_TAP_IN_DEV, O_RDONLY);
    if (fd < 0) return -1;
    void *base = mmap(NULL, NPU_TAP_RING_SIZE, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (base == MAP_FAILED) return -1;
    g_hw_hdr = (struct npu_tap_hdr *)base;
    g_hw_ring = (int32_t *)((char *)base + NPU_TAP_HDR_SIZE);
    fprintf(stderr, "ml-inf: HW tap open\n");
    return 0;
}

static int hw_tap_read(float *L, float *R, int max_frames)
{
    if (!g_hw_hdr) return 0;
    if (atomic_load_explicit((_Atomic uint32_t *)&g_hw_hdr->magic,
                              memory_order_acquire) != NPU_TAP_MAGIC) return 0;
    uint32_t ring_size = atomic_load_explicit(
        (_Atomic uint32_t *)&g_hw_hdr->ring_size, memory_order_relaxed);
    uint32_t epoch = atomic_load_explicit((_Atomic uint32_t *)&g_hw_hdr->epoch,
                                           memory_order_acquire);
    uint32_t w = atomic_load_explicit((_Atomic uint32_t *)&g_hw_hdr->write_idx,
                                       memory_order_acquire);
    if (epoch != g_hw_epoch) { g_hw_read_bytes = 0; g_hw_epoch = epoch; }
    const int fsz = 8 * 4;
    uint32_t r = g_hw_read_bytes;
    uint32_t avail = (w >= r) ? (w - r) : (ring_size - (r - w));
    int n = (int)(avail / fsz);
    if (n > max_frames) n = max_frames;
    for (int i = 0; i < n; i++) {
        uint32_t off = (r + i * fsz) % ring_size;
        L[i] = (float)g_hw_ring[off / 4 + 0] * (1.0f / 2147483648.0f);
        R[i] = (float)g_hw_ring[off / 4 + 1] * (1.0f / 2147483648.0f);
    }
    g_hw_read_bytes = (r + n * fsz) % ring_size;
    return n;
}

/* --- USB IN : SHM tap mixer-pro (stéréo float32) --- */
static int usb_tap_open(void)
{
    if (g_usb_hdr) return 0;
    int fd = shm_open(MIXER_PRO_TAP_SHM_NAME, O_RDONLY, 0);
    if (fd < 0) return -1;
    void *base = mmap(NULL, MIXER_PRO_TAP_TOTAL_SIZE, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (base == MAP_FAILED) return -1;
    g_usb_hdr = (struct mixer_pro_tap_hdr *)base;
    g_usb_ring = (float *)((char *)base + MIXER_PRO_TAP_HDR_SIZE);
    fprintf(stderr, "ml-inf: USB tap open\n");
    return 0;
}

static int usb_tap_read(float *L, float *R, int max_frames)
{
    if (!g_usb_hdr) return 0;
    if (atomic_load_explicit((_Atomic uint32_t *)&g_usb_hdr->magic,
                              memory_order_acquire) != MIXER_PRO_TAP_MAGIC) return 0;
    uint32_t epoch = atomic_load_explicit((_Atomic uint32_t *)&g_usb_hdr->epoch,
                                           memory_order_acquire);
    if (epoch != g_usb_epoch) {
        g_usb_read = atomic_load_explicit(
            (_Atomic uint32_t *)&g_usb_hdr->write_idx, memory_order_relaxed);
        g_usb_epoch = epoch;
    }
    uint32_t w = atomic_load_explicit((_Atomic uint32_t *)&g_usb_hdr->write_idx,
                                       memory_order_acquire);
    uint32_t avail = w - g_usb_read;
    const uint32_t rsz = MIXER_PRO_TAP_RING_FRAMES;
    if (avail > rsz) { g_usb_read = w - rsz / 2; avail = rsz / 2; }
    int n = avail < (uint32_t)max_frames ? (int)avail : max_frames;
    for (int i = 0; i < n; i++) {
        uint32_t idx = (g_usb_read + i) & (rsz - 1);
        L[i] = g_usb_ring[2 * idx + 0];
        R[i] = g_usb_ring[2 * idx + 1];
    }
    g_usb_read += n;
    return n;
}

/* --- TFLite --- */
static int tflite_load(const char *path)
{
    g_model = TfLiteModelCreateFromFile(path);
    if (!g_model) return -1;
    g_opts = TfLiteInterpreterOptionsCreate();
    TfLiteInterpreterOptionsSetNumThreads(g_opts, 1);
    if (!getenv("ML_NO_NPU")) {
        TfLiteExternalDelegateOptions vx =
            TfLiteExternalDelegateOptionsDefault(VX_DELEGATE_SO);
        g_delegate = TfLiteExternalDelegateCreate(&vx);
        if (g_delegate) {
            TfLiteInterpreterOptionsAddDelegate(g_opts, g_delegate);
            fprintf(stderr, "ml-inf: VX NPU delegate loaded\n");
        }
    } else {
        fprintf(stderr, "ml-inf: ML_NO_NPU=1 — CPU only\n");
    }
    g_itp = TfLiteInterpreterCreate(g_model, g_opts);
    if (!g_itp || TfLiteInterpreterAllocateTensors(g_itp) != kTfLiteOk) return -1;
    {
        int ni = TfLiteInterpreterGetInputTensorCount(g_itp);
        int no = TfLiteInterpreterGetOutputTensorCount(g_itp);
        fprintf(stderr, "ml-inf: tflite ready — %d inputs, %d outputs\n", ni, no);
        for (int i = 0; i < ni; i++) {
            TfLiteTensor *t = TfLiteInterpreterGetInputTensor(g_itp, i);
            fprintf(stderr, "  in[%d] '%s' dims:", i, TfLiteTensorName(t));
            for (int d = 0; d < TfLiteTensorNumDims(t); d++)
                fprintf(stderr, " %d", TfLiteTensorDim(t, d));
            fprintf(stderr, " bytes=%zu\n", TfLiteTensorByteSize(t));
        }
    }
    return 0;
}

static int tflite_invoke(const float *tensor, float *out74)
{
    TfLiteTensor *in = TfLiteInterpreterGetInputTensor(g_itp, 0);
    if (TfLiteTensorCopyFromBuffer(in, tensor,
            MLF3_N_FEATURES * MLF3_N_WINDOW * sizeof(float)) != kTfLiteOk)
        return -1;
    if (TfLiteInterpreterInvoke(g_itp) != kTfLiteOk) return -1;
    const TfLiteTensor *out = TfLiteInterpreterGetOutputTensor(g_itp, 0);
    return TfLiteTensorCopyToBuffer(out, out74, N_OUT * sizeof(float))
           == kTfLiteOk ? 0 : -1;
}

/* Décode + gate : pn (74 normalisé) → env_db[64] + exc[4] + lim[6] réels. */
static void decode_params(const float *pn, float g,
                           float *env_db, float *exc, float *lim)
{
    for (int b = 0; b < N_ENV; b++) {
        float v = (pn[b] * 2.0f - 1.0f) * ENV_RANGE_DB;
        if (b >= ENV_HF_FIRST_BAND) {     /* cap anti-souffle (parité training) */
            if (v > ENV_HF_CAP_DB)  v = ENV_HF_CAP_DB;
            if (v < -ENV_HF_CAP_DB) v = -ENV_HF_CAP_DB;
        }
        env_db[b] = v * g;                /* gate → 0 dB */
    }
    static const float exc_neutral[4] = {0.0f, 1.0f, 8000.0f, 1.0f};
    static const float lim_neutral[6] = {0.0f, 0.99f, 4.0f, 100.0f, 0.0f, 0.0f};
    for (int i = 0; i < 4; i++) {
        float v = pn[N_ENV + i] * (R_EXC[i][1] - R_EXC[i][0]) + R_EXC[i][0];
        exc[i] = exc_neutral[i] + (v - exc_neutral[i]) * g;
    }
    for (int i = 0; i < 6; i++) {
        float v = pn[N_ENV + 4 + i] * (R_LIM[i][1] - R_LIM[i][0]) + R_LIM[i][0];
        lim[i] = lim_neutral[i] + (v - lim_neutral[i]) * g;
    }
}

/* Push bulk : slot 0 = spectral_env (l_gN / r_gN), slot 1 = exciter,
 * slot 2 = limiter. ~3.5 KB, buffer socket mixer-pro = 16 KB. */
static void push_params(const float *envL, const float *envR,
                         const float *exc, const float *lim,
                         int with_dyn)
{
    /* with_dyn=0 : enveloppe seule (fx_spectral_env, set_param trivial).
     * with_dyn=1 : + exciter/limiter LV2 — le LSP Limiter recalcule son
     * état interne à chaque set_param → cadence réduite (10 Hz) sinon
     * l'audio_thread dépasse son budget (xruns mesurés à 50 Hz). */
    static char buf[12288];
    int n = snprintf(buf, sizeof(buf),
                     "{\"op\":\"set_insert_params_bulk\",\"params\":[");
    for (int b = 0; b < N_ENV; b++)
        n += snprintf(buf + n, sizeof(buf) - n, "%s[0,\"l_g%d\",%.2f]",
                      b ? "," : "", b, envL[b]);
    for (int b = 0; b < N_ENV; b++)
        n += snprintf(buf + n, sizeof(buf) - n, ",[0,\"r_g%d\",%.2f]",
                      b, envR[b]);
    if (with_dyn)
        n += snprintf(buf + n, sizeof(buf) - n,
                  ",[1,\"amount\",%.4f],[1,\"drive\",%.2f]"
                  ",[1,\"freq\",%.1f],[1,\"ceil\",%.4f]"
                  ",[2,\"th\",%.4f],[2,\"g_in\",%.4f],[2,\"g_out\",%.4f]"
                  ",[2,\"at\",%.2f],[2,\"rt\",%.1f],[2,\"ceil\",%.4f]",
                  exc[0], exc[1], exc[2], exc[3],
                  powf(10.0f, lim[0] / 20.0f),
                  powf(10.0f, lim[4] / 20.0f),
                  powf(10.0f, lim[5] / 20.0f),
                  lim[2], lim[3], lim[1]);
    n += snprintf(buf + n, sizeof(buf) - n, "]}\n");
    char resp[256];
    sock_send_recv(buf, resp, sizeof(resp));
}

int main(int argc, char **argv)
{
    const char *model_path = argc >= 2 ? argv[1] : MODEL_PATH_DEFAULT;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    mlf3_init(&g_feat[0]);
    mlf3_init(&g_feat[1]);
    if (tflite_load(model_path) < 0) {
        fprintf(stderr, "ml-inf: tflite load failed\n");
        return 1;
    }
    fprintf(stderr, "ml-inf: v5.20 daemon running (%s)\n", model_path);

    struct timespec poll_last = {0};
    static float bufL[4096], bufR[4096];
    static float pendL[MLF3_FRAME_SIZE], pendR[MLF3_FRAME_SIZE];
    int pend = 0;
    static float tensor[MLF3_N_FEATURES * MLF3_N_WINDOW];
    float pnL[N_OUT], pnR[N_OUT];
    float envL[N_ENV], envR[N_ENV];
    float excL[4], excR[4], limL[6], limR[6];
    float exc[4], lim[6];

    while (!g_should_exit) {
        struct timespec ts = {0, CYCLE_NS};
        nanosleep(&ts, NULL);

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long dt = (now.tv_sec - poll_last.tv_sec) * 1000000000L
                + (now.tv_nsec - poll_last.tv_nsec);
        if (dt > POLL_STATE_NS) {
            int s = poll_assistant_state();
            if (s != g_source) {
                fprintf(stderr, "ml-inf: source %d -> %d\n", g_source, s);
                g_source = s;
                mlf3_init(&g_feat[0]);
                mlf3_init(&g_feat[1]);
                pend = 0;
                if (s == SRC_PASSTHROUGH) {
                    /* passthrough = VRAI bypass : pousse les params neutres
                     * (sinon le dernier mastering reste figé dans la chaîne) */
                    float env0[N_ENV] = {0};
                    const float exc0[4] = {0.0f, 1.0f, 8000.0f, 1.0f};
                    const float lim0[6] = {0.0f, 0.99f, 4.0f, 100.0f, 0.0f, 0.0f};
                    push_params(env0, env0, exc0, lim0, 1);
                    g_gate = 1.0f;
                    fprintf(stderr, "ml-inf: passthrough -> params neutres\n");
                }
            }
            poll_last = now;
        }
        if (g_source == SRC_PASSTHROUGH) continue;

        int n = 0;
        if (g_source == SRC_HW_IN) {
            if (hw_tap_open() == 0) n = hw_tap_read(bufL, bufR, 4096);
        } else {
            if (usb_tap_open() == 0) n = usb_tap_read(bufL, bufR, 4096);
        }

        /* assemble en trames de 480 ; toutes alimentent les features
         * (continuité ΔMel/ring100), la prédiction se fait une fois par
         * cycle sur l'état le plus frais */
        int did_frame = 0;
        int i = 0;
        while (i < n) {
            int take = MLF3_FRAME_SIZE - pend;
            if (take > n - i) take = n - i;
            memcpy(pendL + pend, bufL + i, take * sizeof(float));
            memcpy(pendR + pend, bufR + i, take * sizeof(float));
            pend += take;
            i += take;
            if (pend == MLF3_FRAME_SIZE) {
                mlf3_push_frame(&g_feat[0], pendL);
                mlf3_push_frame(&g_feat[1], pendR);
                pend = 0;
                did_frame = 1;
            }
        }
        if (!did_frame || g_feat[0].n_frames < 1) continue;

        /* gate silences : RMS de la fenêtre 100 ms (sous-échantillonné ×4) */
        {
            float acc = 0.0f;
            for (int k = 0; k < MLF3_WIN_LONG; k += 4) {
                const float l = g_feat[0].ring100[k], r = g_feat[1].ring100[k];
                acc += 0.25f * (l + r) * (l + r);
            }
            const float rms = sqrtf(acc / (MLF3_WIN_LONG / 4) + 1e-12f);
            const float rms_db = 20.0f * log10f(rms + 1e-12f);
            const float target = rms_db < GATE_RMS_DB ? 0.0f : 1.0f;
            g_gate += (target - g_gate) * (GATE_SLEW * 4.0f);   /* ~50 ms */
            if (g_gate < 0.0f) g_gate = 0.0f;
            if (g_gate > 1.0f) g_gate = 1.0f;
        }

        /* 2 invocations NPU (1 par canal) */
        mlf3_fill_tensor(&g_feat[0], tensor);
        if (tflite_invoke(tensor, pnL) < 0) continue;
        mlf3_fill_tensor(&g_feat[1], tensor);
        if (tflite_invoke(tensor, pnR) < 0) continue;

        decode_params(pnL, g_gate, envL, excL, limL);
        decode_params(pnR, g_gate, envR, excR, limR);
        /* exciter + limiter : plugins stéréo → params communs (moyenne L/R) */
        for (int k = 0; k < 4; k++) exc[k] = 0.5f * (excL[k] + excR[k]);
        for (int k = 0; k < 6; k++) lim[k] = 0.5f * (limL[k] + limR[k]);

        /* V5.20 — moyenne glissante 250 ms (25 cycles) sur les params du
         * limiteur : les prédictions brutes à 100 Hz sautent trop vite
         * (pompage audible, demande utilisateur 2026-06-11). */
        {
            #define LIM_AVG_N 25   /* 250 ms (réglage écoute 2026-06-12) */
            static float lim_hist[LIM_AVG_N][6];
            static int   lim_pos = 0, lim_fill = 0;
            memcpy(lim_hist[lim_pos], lim, sizeof(lim));
            lim_pos = (lim_pos + 1) % LIM_AVG_N;
            if (lim_fill < LIM_AVG_N) lim_fill++;
            for (int k = 0; k < 6; k++) {
                float acc = 0.0f;
                for (int j = 0; j < lim_fill; j++) acc += lim_hist[j][k];
                lim[k] = acc / lim_fill;
            }
        }

        push_params(envL, envR, exc, lim, 1);   /* natifs lock-free → 100 Hz OK */
    }

    fprintf(stderr, "ml-inf: stopping\n");
    if (g_itp) TfLiteInterpreterDelete(g_itp);
    if (g_delegate) TfLiteExternalDelegateDelete(g_delegate);
    if (g_opts) TfLiteInterpreterOptionsDelete(g_opts);
    if (g_model) TfLiteModelDelete(g_model);
    return 0;
}
