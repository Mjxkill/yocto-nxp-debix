/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * V9.5.12 — mixer-ml-inference : daemon ML mastering NPU isolé.
 *
 * Pourquoi un process séparé :
 *   TFLite NPU (via VX delegate libvx_delegate.so + galcore driver) crashe
 *   le kernel quand exécuté dans le même process que les threads RT99 de
 *   mixer-pro (audio_thread + ALSA SDMA). Hypothèse : conflit IRQ NPU vs
 *   SDMA sur les cores non-isolcpus + galcore wait_event() qui bloque trop
 *   longtemps face à l'IRQ storm ALSA.
 *
 *   Bench Python tflite_runtime (process Python isolé) marche parfaitement
 *   (0.38 ms/invoke NPU INT8) avec mixer-pro tournant en parallèle.
 *
 * Architecture :
 *   loop @ 50 Hz (sleep 20 ms) :
 *     1. poll mixer-pro socket get_assistant pour source/mode
 *     2. lit audio :
 *        - source=hw  → mmap /dev/imx-audio-tap-in   (8ch S32, ch 0+1)
 *        - source=usb → mmap /dev/shm/mixer-pro-tap-usb (2ch float32)
 *     3. accumulate 512 samples → ml_features.c → 11 floats
 *     4. quand 19 frames disponibles → invoke TFLite NPU
 *     5. push 62 params via set_insert_params_bulk
 *
 *   Crash daemon → systemd restart, mixer-pro intact.
 */

#define _GNU_SOURCE
#include "ml_features.h"
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
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <tensorflow/lite/c/c_api.h>
#include <tensorflow/lite/delegates/external/external_delegate.h>

#define MIXER_PRO_SOCK_PATH     "/run/mixer-pro.sock"
#define MODEL_PATH_DEFAULT      "/etc/mixer-pro/mastering_v5_12_int8.tflite"
#define VX_DELEGATE_SO          "/usr/lib/libvx_delegate.so"
#define NPU_TAP_IN_DEV          "/dev/imx-audio-tap-in"

#define N_PARAMS                62
#define N_FRAMES                19      /* 19 × 10.7 ms = 200 ms context */
#define LOOP_PERIOD_NS          (20 * 1000 * 1000)   /* 20 ms = 50 Hz */
#define POLL_STATE_NS           (200 * 1000 * 1000)  /* 200 ms = 5 Hz poll */

#define SRC_PASSTHROUGH         0
#define SRC_HW_IN               1
#define SRC_USB_IN              2

/* ---------- PARAM_RANGES (mirroir training/model.py V5.12) ---------- */

typedef struct { float lo, hi; } range_t;

static const range_t PARAM_RANGES[N_PARAMS] = {
    /* eq.freq [0..15] */
    {20,20000},{20,20000},{20,20000},{20,20000},
    {20,20000},{20,20000},{20,20000},{20,20000},
    {20,20000},{20,20000},{20,20000},{20,20000},
    {20,20000},{20,20000},{20,20000},{20,20000},
    /* eq.gain_db [16..31] */
    {-12,12},{-12,12},{-12,12},{-12,12},{-12,12},{-12,12},{-12,12},{-12,12},
    {-12,12},{-12,12},{-12,12},{-12,12},{-12,12},{-12,12},{-12,12},{-12,12},
    /* eq.q [32..47] */
    {0.3,4},{0.3,4},{0.3,4},{0.3,4},{0.3,4},{0.3,4},{0.3,4},{0.3,4},
    {0.3,4},{0.3,4},{0.3,4},{0.3,4},{0.3,4},{0.3,4},{0.3,4},{0.3,4},
    /* exciter */
    {0,1},        /* 48 amount */
    {1,6},        /* 49 drive */
    {5000,12000}, /* 50 freq_hz */
    {0.5,1},      /* 51 ceiling */
    /* stereo */
    {-0.2,0.2},   /* 52 balance */
    {0.7,1.4},    /* 53 mid_gain */
    {0.5,1.7},    /* 54 side_gain */
    {0,0.3},      /* 55 sm_swap */
    /* limiter */
    {-12,0},      /* 56 threshold_db */
    {0.85,0.99},  /* 57 ceiling_lin */
    {0.3,10},     /* 58 attack_ms */
    {10,200},     /* 59 release_ms */
    {0,12},       /* 60 input_db */
    {-6,0},       /* 61 output_db */
};

/* Force drive max + balance neutre (validés écoute PC v5.12). */
#define FORCE_DRIVE   6.0f
#define FORCE_BALANCE 0.0f

/* ---------- Globals ---------- */

static volatile int g_should_exit = 0;
static int g_current_source = SRC_PASSTHROUGH;

static TfLiteModel              *g_tfl_model    = NULL;
static TfLiteDelegate           *g_tfl_delegate = NULL;
static TfLiteInterpreterOptions *g_tfl_opts     = NULL;
static TfLiteInterpreter        *g_tfl_itp      = NULL;

/* HW IN : mmap NPU tap. */
static struct npu_tap_hdr *g_hw_hdr   = NULL;
static int32_t            *g_hw_ring  = NULL;
static uint32_t            g_hw_ring_bytes = 0;
static uint32_t            g_hw_read_idx_bytes = 0;
static uint32_t            g_hw_last_epoch = 0;

/* USB IN : mmap SHM tap depuis mixer-pro. */
static struct mixer_pro_tap_hdr *g_usb_hdr  = NULL;
static float                    *g_usb_ring = NULL;
static uint32_t                  g_usb_read_idx = 0;
static uint32_t                  g_usb_last_epoch = 0;

/* Features ring (19 frames × 11 floats). */
static float g_feat_ring[N_FRAMES][ML_FEATURES_N_FEATURES];
static int   g_feat_head = 0;
static int   g_feat_count = 0;

/* Sample history (window 1024). */
static float g_hist_L[ML_FEATURES_FRAME_SIZE];
static float g_hist_R[ML_FEATURES_FRAME_SIZE];
static int   g_hist_idx = 0;
static int   g_samples_since_frame = 0;

/* ---------- Signal handlers ---------- */

static void on_signal(int sig) { (void)sig; g_should_exit = 1; }

/* ---------- Socket Unix to mixer-pro ---------- */

static int sock_send_recv(const char *req, char *resp, int resp_size)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    strncpy(sa.sun_path, MIXER_PRO_SOCK_PATH, sizeof(sa.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd); return -1;
    }
    int len = (int)strlen(req);
    if (write(fd, req, len) != len) { close(fd); return -1; }
    int n = (int)read(fd, resp, resp_size - 1);
    if (n > 0) resp[n] = '\0';
    close(fd);
    return n;
}

/* Poll mixer-pro get_assistant → set g_current_source. */
static int poll_assistant_state(void)
{
    char resp[512];
    if (sock_send_recv("{\"op\":\"get_assistant\"}\n", resp, sizeof(resp)) < 0)
        return SRC_PASSTHROUGH;
    /* Cheap parse : check substrings. */
    int mastering = (strstr(resp, "\"mode\":\"mastering\"") != NULL);
    if (!mastering) return SRC_PASSTHROUGH;
    if (strstr(resp, "\"source\":\"usb\""))  return SRC_USB_IN;
    if (strstr(resp, "\"source\":\"hw\""))   return SRC_HW_IN;
    return SRC_PASSTHROUGH;
}

/* ---------- HW IN tap (NPU TAP IN) ---------- */

static int hw_tap_open(void)
{
    if (g_hw_hdr) return 0;
    int fd = open(NPU_TAP_IN_DEV, O_RDONLY);
    if (fd < 0) { fprintf(stderr, "ml-inf: open %s: %s\n", NPU_TAP_IN_DEV, strerror(errno)); return -1; }
    void *base = mmap(NULL, NPU_TAP_RING_SIZE, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (base == MAP_FAILED) { fprintf(stderr, "ml-inf: mmap tap_in failed\n"); return -1; }
    g_hw_hdr  = (struct npu_tap_hdr *)base;
    g_hw_ring = (int32_t *)((char *)base + NPU_TAP_HDR_SIZE);
    g_hw_ring_bytes = NPU_TAP_RING_SIZE - NPU_TAP_HDR_SIZE;
    g_hw_read_idx_bytes = 0;
    g_hw_last_epoch = 0;
    fprintf(stderr, "ml-inf: HW tap open OK (ring %u B)\n", g_hw_ring_bytes);
    return 0;
}

/* Pop n_frames samples mono mid de HW tap (8ch interleaved S32, ch 0+1).
 * Returns frames read.
 */
static int hw_tap_read(float *L, float *R, int max_frames)
{
    if (!g_hw_hdr) return 0;
    uint32_t magic = atomic_load_explicit((_Atomic uint32_t *)&g_hw_hdr->magic,
                                           memory_order_acquire);
    if (magic != NPU_TAP_MAGIC) return 0;
    uint32_t epoch1, w, epoch2;
    uint32_t ring_size = atomic_load_explicit((_Atomic uint32_t *)&g_hw_hdr->ring_size,
                                               memory_order_relaxed);
    int n_read = 0;
    do {
        epoch1 = atomic_load_explicit((_Atomic uint32_t *)&g_hw_hdr->epoch,
                                       memory_order_acquire);
        w = atomic_load_explicit((_Atomic uint32_t *)&g_hw_hdr->write_idx,
                                  memory_order_acquire);
        if (epoch1 != g_hw_last_epoch) {
            g_hw_read_idx_bytes = 0;
            g_hw_last_epoch = epoch1;
        }
        epoch2 = atomic_load_explicit((_Atomic uint32_t *)&g_hw_hdr->epoch,
                                       memory_order_acquire);
    } while (epoch1 != epoch2);

    const int n_ch       = 8;
    const int frame_size = n_ch * 4;   /* S32_LE per ch */

    /* avail bytes (modular). */
    uint32_t r = g_hw_read_idx_bytes;
    uint32_t avail_bytes = (w >= r) ? (w - r) : (ring_size - (r - w));
    int avail_frames = (int)(avail_bytes / frame_size);
    int to_read = avail_frames < max_frames ? avail_frames : max_frames;
    for (int i = 0; i < to_read; i++) {
        uint32_t off = (r + i * frame_size) % ring_size;
        int32_t s_l = g_hw_ring[off / 4 + 0];
        int32_t s_r = g_hw_ring[off / 4 + 1];
        L[i] = (float)s_l * (1.0f / 2147483648.0f);
        R[i] = (float)s_r * (1.0f / 2147483648.0f);
    }
    g_hw_read_idx_bytes = (r + to_read * frame_size) % ring_size;
    n_read = to_read;
    return n_read;
}

/* ---------- USB IN tap (SHM from mixer-pro) ---------- */

static int usb_tap_open(void)
{
    if (g_usb_hdr) return 0;
    int fd = shm_open(MIXER_PRO_TAP_SHM_NAME, O_RDONLY, 0);
    if (fd < 0) { fprintf(stderr, "ml-inf: shm_open USB: %s\n", strerror(errno)); return -1; }
    void *base = mmap(NULL, MIXER_PRO_TAP_TOTAL_SIZE, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (base == MAP_FAILED) { fprintf(stderr, "ml-inf: mmap USB SHM failed\n"); return -1; }
    g_usb_hdr  = (struct mixer_pro_tap_hdr *)base;
    g_usb_ring = (float *)((char *)base + MIXER_PRO_TAP_HDR_SIZE);
    g_usb_read_idx = 0;
    g_usb_last_epoch = 0;
    fprintf(stderr, "ml-inf: USB tap open OK\n");
    return 0;
}

static int usb_tap_read(float *L, float *R, int max_frames)
{
    if (!g_usb_hdr) return 0;
    uint32_t magic = atomic_load_explicit((_Atomic uint32_t *)&g_usb_hdr->magic,
                                           memory_order_acquire);
    if (magic != MIXER_PRO_TAP_MAGIC) return 0;
    uint32_t epoch = atomic_load_explicit((_Atomic uint32_t *)&g_usb_hdr->epoch,
                                           memory_order_acquire);
    if (epoch != g_usb_last_epoch) {
        g_usb_read_idx = atomic_load_explicit((_Atomic uint32_t *)&g_usb_hdr->write_idx,
                                               memory_order_relaxed);
        g_usb_last_epoch = epoch;
    }
    uint32_t w = atomic_load_explicit((_Atomic uint32_t *)&g_usb_hdr->write_idx,
                                       memory_order_acquire);
    uint32_t avail = w - g_usb_read_idx;
    uint32_t rsz = MIXER_PRO_TAP_RING_FRAMES;
    if (avail > rsz) {
        /* Overflow : skip to most recent half. */
        g_usb_read_idx = w - rsz / 2;
        avail = rsz / 2;
    }
    int n = (avail < (uint32_t)max_frames) ? (int)avail : max_frames;
    for (int i = 0; i < n; i++) {
        uint32_t idx = (g_usb_read_idx + i) & (rsz - 1);
        L[i] = g_usb_ring[2 * idx + 0];
        R[i] = g_usb_ring[2 * idx + 1];
    }
    g_usb_read_idx += n;
    return n;
}

/* ---------- TFLite load + invoke ---------- */

static int tflite_load(const char *model_path)
{
    g_tfl_model = TfLiteModelCreateFromFile(model_path);
    if (!g_tfl_model) { fprintf(stderr, "ml-inf: model load failed\n"); return -1; }
    g_tfl_opts = TfLiteInterpreterOptionsCreate();
    TfLiteInterpreterOptionsSetNumThreads(g_tfl_opts, 1);
    TfLiteExternalDelegateOptions vx_opts =
        TfLiteExternalDelegateOptionsDefault(VX_DELEGATE_SO);
    g_tfl_delegate = TfLiteExternalDelegateCreate(&vx_opts);
    if (g_tfl_delegate) {
        TfLiteInterpreterOptionsAddDelegate(g_tfl_opts, g_tfl_delegate);
        fprintf(stderr, "ml-inf: VX NPU delegate loaded\n");
    }
    g_tfl_itp = TfLiteInterpreterCreate(g_tfl_model, g_tfl_opts);
    if (!g_tfl_itp) return -1;
    if (TfLiteInterpreterAllocateTensors(g_tfl_itp) != kTfLiteOk) return -1;
    fprintf(stderr, "ml-inf: tflite ready (in=11×19, out=62)\n");
    return 0;
}

static int tflite_invoke(float *params_out)
{
    static float input_tensor[ML_FEATURES_N_FEATURES * N_FRAMES];
    int oldest = g_feat_head;
    for (int t = 0; t < N_FRAMES; t++) {
        int frame_idx = (oldest + t) % N_FRAMES;
        const float *src = g_feat_ring[frame_idx];
        for (int f = 0; f < ML_FEATURES_N_FEATURES; f++)
            input_tensor[f * N_FRAMES + t] = src[f];
    }
    TfLiteTensor *in = TfLiteInterpreterGetInputTensor(g_tfl_itp, 0);
    if (TfLiteTensorCopyFromBuffer(in, input_tensor, sizeof(input_tensor)) != kTfLiteOk)
        return -1;
    if (TfLiteInterpreterInvoke(g_tfl_itp) != kTfLiteOk) return -1;
    const TfLiteTensor *out = TfLiteInterpreterGetOutputTensor(g_tfl_itp, 0);
    return TfLiteTensorCopyToBuffer(out, params_out, N_PARAMS * sizeof(float))
              == kTfLiteOk ? 0 : -1;
}

/* ---------- Build + push set_insert_params_bulk ---------- */

static void push_params(const float *params_norm)
{
    /* Denormalize. */
    float P[N_PARAMS];
    for (int i = 0; i < N_PARAMS; i++) {
        P[i] = PARAM_RANGES[i].lo
             + params_norm[i] * (PARAM_RANGES[i].hi - PARAM_RANGES[i].lo);
    }
    P[49] = FORCE_DRIVE;
    P[52] = FORCE_BALANCE;

    /* Build JSON bulk : ~76 entries. Buffer 4 KB suffit. */
    static char buf[4096];
    int n = snprintf(buf, sizeof(buf), "{\"op\":\"set_insert_params_bulk\",\"params\":[");
    for (int b = 0; b < 16; b++) {
        n += snprintf(buf + n, sizeof(buf) - n,
                      "[0,\"ft_%d\",1],[0,\"f_%d\",%.2f],[0,\"g_%d\",%.4f],[0,\"q_%d\",%.3f],",
                      b, b, P[0 + b], b, powf(10.0f, P[16 + b] / 20.0f), b, P[32 + b]);
    }
    n += snprintf(buf + n, sizeof(buf) - n,
                  "[1,\"amount\",%.4f],[1,\"drive\",%.2f],[1,\"freq\",%.1f],[1,\"ceil\",%.4f],"
                  "[2,\"balance_in\",%.4f],[2,\"mlev\",%.4f],[2,\"slev\",%.4f],"
                  "[3,\"th\",%.4f],[3,\"g_in\",%.4f],[3,\"g_out\",%.4f],"
                  "[3,\"at\",%.2f],[3,\"rt\",%.1f]]}\n",
                  P[48], P[49], P[50], P[51],
                  P[52], P[53], P[54],
                  powf(10.0f, P[56] / 20.0f), powf(10.0f, P[60] / 20.0f),
                  powf(10.0f, P[61] / 20.0f), P[58], P[59]);
    char resp[256];
    sock_send_recv(buf, resp, sizeof(resp));
}

/* ---------- Process samples → features → infer → push ---------- */

static void process_samples(const float *L, const float *R, int n)
{
    for (int i = 0; i < n; i++) {
        g_hist_L[g_hist_idx] = L[i];
        g_hist_R[g_hist_idx] = R[i];
        g_hist_idx = (g_hist_idx + 1) % ML_FEATURES_FRAME_SIZE;
        g_samples_since_frame++;
    }
    while (g_samples_since_frame >= ML_FEATURES_HOP_SIZE) {
        static float win_L[ML_FEATURES_FRAME_SIZE];
        static float win_R[ML_FEATURES_FRAME_SIZE];
        int start = g_hist_idx;
        for (int i = 0; i < ML_FEATURES_FRAME_SIZE; i++) {
            int idx = (start + i) % ML_FEATURES_FRAME_SIZE;
            win_L[i] = g_hist_L[idx];
            win_R[i] = g_hist_R[idx];
        }
        ml_features_t feat;
        ml_features_process_frame_f32(win_L, win_R, &feat);
        float *dst = g_feat_ring[g_feat_head];
        for (int b = 0; b < 5; b++) dst[b] = feat.mid_band_db[b];
        for (int b = 0; b < 5; b++) dst[5 + b] = feat.mid_centroid_hz[b];
        dst[10] = feat.mid_global_db;
        g_feat_head = (g_feat_head + 1) % N_FRAMES;
        if (g_feat_count < N_FRAMES) g_feat_count++;
        g_samples_since_frame -= ML_FEATURES_HOP_SIZE;
    }
}

/* ---------- Main loop ---------- */

int main(int argc, char **argv)
{
    const char *model_path = MODEL_PATH_DEFAULT;
    if (argc >= 2) model_path = argv[1];

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    if (ml_features_init() < 0) { fprintf(stderr, "ml-inf: features init failed\n"); return 1; }
    if (tflite_load(model_path) < 0) { fprintf(stderr, "ml-inf: tflite load failed\n"); return 1; }

    fprintf(stderr, "ml-inf: daemon running (model %s)\n", model_path);

    struct timespec poll_last = {0};
    static float Lbuf[ML_FEATURES_HOP_SIZE * 4];
    static float Rbuf[ML_FEATURES_HOP_SIZE * 4];

    while (!g_should_exit) {
        struct timespec ts = {0, LOOP_PERIOD_NS};
        nanosleep(&ts, NULL);

        /* Poll mixer-pro state à 5 Hz (économise socket calls). */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long dt_ns = (now.tv_sec - poll_last.tv_sec) * 1000000000L
                   + (now.tv_nsec - poll_last.tv_nsec);
        if (dt_ns > POLL_STATE_NS) {
            int new_src = poll_assistant_state();
            if (new_src != g_current_source) {
                fprintf(stderr, "ml-inf: source change %d -> %d\n",
                        g_current_source, new_src);
                g_current_source = new_src;
                g_hist_idx = 0;
                g_samples_since_frame = 0;
                g_feat_count = 0;
                g_feat_head = 0;
            }
            poll_last = now;
        }

        if (g_current_source == SRC_PASSTHROUGH) continue;

        /* Open tap (idempotent) + read available samples. */
        int n_read = 0;
        if (g_current_source == SRC_HW_IN) {
            if (hw_tap_open() == 0)
                n_read = hw_tap_read(Lbuf, Rbuf, sizeof(Lbuf) / sizeof(Lbuf[0]));
        } else if (g_current_source == SRC_USB_IN) {
            if (usb_tap_open() == 0)
                n_read = usb_tap_read(Lbuf, Rbuf, sizeof(Lbuf) / sizeof(Lbuf[0]));
        }
        if (n_read <= 0) continue;

        process_samples(Lbuf, Rbuf, n_read);
        if (g_feat_count < N_FRAMES) continue;

        float params[N_PARAMS];
        if (tflite_invoke(params) < 0) {
            fprintf(stderr, "ml-inf: invoke failed\n");
            continue;
        }
        push_params(params);
    }

    fprintf(stderr, "ml-inf: daemon stopping\n");
    if (g_tfl_itp)      TfLiteInterpreterDelete(g_tfl_itp);
    if (g_tfl_delegate) TfLiteExternalDelegateDelete(g_tfl_delegate);
    if (g_tfl_opts)     TfLiteInterpreterOptionsDelete(g_tfl_opts);
    if (g_tfl_model)    TfLiteModelDelete(g_tfl_model);
    ml_features_cleanup();
    return 0;
}
