/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * V15 — anti-larsen v2 : détection + DÉCISION (la « preuve par la boucle »).
 *
 * Ce daemon ne touche plus JAMAIS au TAC (v1 : écrire les biquads TAC en
 * live = plops, cause racine prouvée 2026-07-28). Il détecte et décide ;
 * l'ACTUATION est le module antilarsen.c de mixer-pro (notchs logiciels
 * par voie flaguée, ops socket). ARCHI_V15_ANTILARSEN_V2.md.
 *
 * Principe (design Michael 2026-08-04) :
 *  1. le larsen ne naît que dans des micros → seules les voies FLAGUÉES
 *     par l'opérateur (larsen_flag, mixer-pro) sont concernées ;
 *  2. verdict EMPIRIQUE : on pose le notch (la boucle casse) puis on
 *     regarde l'ENTRÉE micro — f disparue = larsen (le micro n'entendait
 *     que la sono) → confirmé ; f persiste = vraie source dans la salle
 *     (note tenue) → retrait IMMÉDIAT + blacklist temporaire de f ;
 *  3. confirmé → identification du COUPABLE : ré-ouverture des voies une
 *     à une, celle dont la ré-ouverture fait repartir f garde son notch,
 *     les autres sont libérées ;
 *  4. récidive → approfondissement (step → max) ; release_s sans récidive
 *     → libération.
 *
 * Capteurs : tap FX (/dev/imx-audio-tap-out, sortie post-effets — les
 * candidates) + tap RAW (/dev/imx-audio-tap-in, entrées micros brutes —
 * les verdicts). L'heuristique v1 (seuil + PNR + persistance + non-
 * harmonicité) ne sert plus que de DÉCLENCHEUR de sonde.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <fftw3.h>
#include <math.h>
#include <signal.h>
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

#define TAP_OUT_DEV    "/dev/imx-audio-tap-out"
#define TAP_IN_DEV     "/dev/imx-audio-tap-in"
#define TAP_TOTAL      0x40000u
#define TAP_HDR        128u
#define TAP_MAGIC      0x5441504Eu
#define FS             48000
#define NCHAN          8
#define NFFT           8192
#define NBINS          (NFFT / 2 + 1)
#define HZ_PER_BIN     ((double)FS / NFFT)

#define CONF_PATH      "/etc/mixer-pro/anti-larsen.conf"
#define STATUS_SOCK    "/run/anti-larsen.sock"
#define MIXER_SOCK     "/run/mixer-pro.sock"

/* ---- config (défauts = ARCHI V15) ---- */
static struct {
    int   enable;
    float thresh_db;        /* seuil de raie candidate (sortie) */
    float pnr_db;           /* peak-to-neighbours ratio */
    int   persist_n;        /* cycles consécutifs avant sonde */
    float depth_start_db;   /* profondeur de sonde (−12) */
    float depth_step_db;    /* approfondissement sur récidive (−3) */
    float verdict_ms;       /* fenêtre de verdict (300, réglable) */
    float refine_ms;        /* fenêtre de ré-ouverture par voie (400) */
    float confirm_drop_db;  /* chute à l'entrée = larsen confirmé (15) */
    float in_floor_db;      /* f « présente à l'entrée » au-dessus de (−70) */
    int   blacklist_s;      /* gel de f après verdict INNOCENT (10) */
    int   release_s;        /* libération sans récidive (60) */
} g_cfg = {
    .enable = 0, .thresh_db = -45.0f, .pnr_db = 25.0f, .persist_n = 4,
    .depth_start_db = -12.0f, .depth_step_db = -3.0f,
    .verdict_ms = 300.0f, .refine_ms = 400.0f, .confirm_drop_db = 15.0f,
    .in_floor_db = -70.0f, .blacklist_s = 10, .release_s = 60,
};

static volatile sig_atomic_t g_stop;
static void on_sig(int s) { (void)s; g_stop = 1; }

/* ================= taps (in + out) ================= */
struct tap {
    volatile uint8_t *map;
    uint32_t ring_size, rd;
};
static struct tap g_tout, g_tin;

static int tap_open(struct tap *t, const char *dev)
{
    int fd = open(dev, O_RDONLY);
    if (fd < 0)
        return -1;
    void *m = mmap(NULL, TAP_TOTAL, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED)
        return -1;
    t->map = m;
    return 0;
}

static inline uint32_t tap_u32(struct tap *t, uint32_t off)
{
    return *(volatile uint32_t *)(t->map + off);
}

static int tap_read(struct tap *t, int32_t (*dst)[NCHAN], int max)
{
    if (tap_u32(t, 0) != TAP_MAGIC)
        return 0;
    t->ring_size = tap_u32(t, 8);
    uint32_t wr = tap_u32(t, 20);
    const uint32_t fsz = NCHAN * 4;
    uint32_t avail = (wr - t->rd) % t->ring_size;
    int n = (int)(avail / fsz);
    if (n > max)
        n = max;
    for (int i = 0; i < n; i++) {
        uint32_t off = TAP_HDR + (t->rd + i * fsz) % t->ring_size;
        memcpy(dst[i], (const void *)(t->map + off), fsz);
    }
    t->rd = (t->rd + n * fsz) % t->ring_size;
    return n;
}

/* ================= client socket mixer-pro ================= */
static int mixer_op(const char *req, char *out, size_t out_sz)
{
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0)
        return -1;
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    strncpy(sa.sun_path, MIXER_SOCK, sizeof(sa.sun_path) - 1);
    struct timeval tv = { 0, 500000 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int r = -1;
    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) == 0 &&
        write(s, req, strlen(req)) > 0) {
        ssize_t n = 0, k;
        while (out && n < (ssize_t)out_sz - 1 &&
               (k = read(s, out + n, out_sz - 1 - (size_t)n)) > 0) {
            n += k;
            if (out[n - 1] == '\n')
                break;
        }
        if (out)
            out[n > 0 ? n : 0] = '\0';
        r = 0;
    }
    close(s);
    if (r < 0)
        fprintf(stderr, "al: mixer-pro injoignable (%s)\n", strerror(errno));
    return r;
}

static void op_notch(int src, double f, float depth)
{
    char req[128], rep[256];
    snprintf(req, sizeof(req),
             "{\"op\":\"larsen_notch\",\"src\":%d,\"freq\":%.1f,"
             "\"depth\":%.1f}\n", src, f, depth);
    mixer_op(req, rep, sizeof(rep));
}

static void op_release(int src, double f)
{
    char req[128], rep[256];
    snprintf(req, sizeof(req),
             "{\"op\":\"larsen_release\",\"src\":%d,\"freq\":%.1f}\n",
             src, f);
    mixer_op(req, rep, sizeof(rep));
}

/* flags des voies micros (0..7), lus 1 Hz depuis larsen_status —
 * l'opérateur les pose via la GUI, mixer-pro les persiste. */
static int g_flag[NCHAN];
static int g_engine_enable;

static void poll_flags(void)
{
    char rep[4096];
    if (mixer_op("{\"op\":\"larsen_status\"}\n", rep, sizeof(rep)) < 0)
        return;
    g_engine_enable = strstr(rep, "\"enable\":1") != NULL;
    const char *p = rep;
    for (int i = 0; i < NCHAN; i++) {
        char key[32];
        snprintf(key, sizeof(key), "{\"src\":%d,\"flag\":", i);
        const char *q = strstr(p, key);
        g_flag[i] = q && q[strlen(key)] == '1';
    }
}

/* ================= FFT ================= */
static float *g_fft_in;
static fftwf_complex *g_fft_out;
static fftwf_plan g_plan;
static float g_win[NFFT];
static float g_spec[NBINS];

static int ch_active(int32_t (*frames)[NCHAN], int ch, float gate_db)
{
    double acc = 0;
    int n = 0;
    for (int i = 0; i < NFFT; i += 16, n++) {
        double v = frames[i][ch] * (1.0 / 2147483648.0);
        acc += v * v;
    }
    return 10.0 * log10(acc / n + 1e-24) > gate_db;
}

static void spectrum_ch(int32_t (*frames)[NCHAN], int ch)
{
    for (int i = 0; i < NFFT; i++)
        g_fft_in[i] = (float)frames[i][ch] * (1.0f / 2147483648.0f)
                      * g_win[i];
    fftwf_execute(g_plan);
    for (int b = 0; b < NBINS; b++) {
        float re = g_fft_out[b][0], im = g_fft_out[b][1];
        g_spec[b] = 10.0f * log10f((re * re + im * im) /
                                   ((float)NFFT * NFFT / 16.0f) + 1e-24f);
    }
}

/* énergie (dB) à ±2 bins autour de b pour la voie ch du buffer donné */
static float energy_at(int32_t (*frames)[NCHAN], int ch, int b)
{
    spectrum_ch(frames, ch);
    float m = -160.0f;
    for (int d = -2; d <= 2; d++)
        if (b + d >= 0 && b + d < NBINS && g_spec[b + d] > m)
            m = g_spec[b + d];
    return m;
}

static float band_mean_db(const float *sp, int center, int lo_excl, int hi)
{
    float acc = 0;
    int n = 0;
    for (int d = lo_excl + 1; d <= hi; d++) {
        int a = center - d, b = center + d;
        if (a >= 0)    { acc += sp[a]; n++; }
        if (b < NBINS) { acc += sp[b]; n++; }
    }
    return n ? acc / n : -160.0f;
}

/* ================= détection (déclencheur, heuristique v1) ============ */
struct candidate {
    int    bin;
    int    count;
    double last_mag;
};
#define MAX_CAND 16
static struct candidate g_cand[MAX_CAND];   /* fusion toutes sorties */

/* raies candidates sur UNE voie de sortie (spectre déjà dans g_spec) */
static void detect_out_ch(void)
{
    const float *sp = g_spec;
    int hits[MAX_CAND], n_hits = 0;
    for (int b = 8; b < NBINS - 8 && n_hits < MAX_CAND; b++) {
        if (sp[b] < g_cfg.thresh_db)
            continue;
        if (sp[b] < sp[b - 1] || sp[b] < sp[b + 1])
            continue;
        if (sp[b] - band_mean_db(sp, b, 1, 10) < g_cfg.pnr_db)
            continue;
        int h2 = b * 2, h3 = b * 3, musical = 0;
        for (int d = -2; d <= 2; d++) {
            if (h2 + d < NBINS && sp[h2 + d] > sp[b] - 12.0f) musical = 1;
            if (h3 + d < NBINS && sp[h3 + d] > sp[b] - 12.0f) musical = 1;
        }
        if (musical)
            continue;
        hits[n_hits++] = b;
    }

    for (int c = 0; c < MAX_CAND; c++) {
        struct candidate *cd = &g_cand[c];
        if (!cd->count)
            continue;
        for (int h = 0; h < n_hits; h++)
            if (hits[h] >= 0 && abs(hits[h] - cd->bin) <= 2) {
                if (sp[hits[h]] >= cd->last_mag - 1.0f)
                    cd->count++;
                cd->bin = hits[h];
                cd->last_mag = sp[hits[h]];
                hits[h] = -1;
                break;
            }
    }
    for (int h = 0; h < n_hits; h++) {
        if (hits[h] < 0)
            continue;
        for (int c = 0; c < MAX_CAND; c++)
            if (!g_cand[c].count) {
                g_cand[c].bin = hits[h];
                g_cand[c].count = 1;
                g_cand[c].last_mag = sp[hits[h]];
                break;
            }
    }
}

/* ================= machine d'états (une sonde à la fois) ============== */
enum { P_IDLE, P_PROBE, P_REFINE };
static struct {
    int     state;
    int     bin;
    double  f_hz;
    struct timespec t0;          /* début de la phase courante */
    float   pre_in_db[NCHAN];    /* énergie d'entrée avant sonde */
    int     involved[NCHAN];     /* f présente à l'entrée avant sonde */
    int     culprit[NCHAN];      /* voies confirmées coupables */
    int     refine_ch;           /* voie en cours de ré-ouverture (−1 fini) */
    float   out_pre_db;          /* niveau sortie à f avant ré-ouverture */
} g_probe = { .state = P_IDLE };

/* notchs confirmés (registre daemon : récidive + libération) */
struct held {
    int    used;
    double f_hz;
    float  depth_db;
    int    voices[NCHAN];
    time_t posed_at, last_hit;
};
#define MAX_HELD 8
static struct held g_held[MAX_HELD];

/* blacklist de fréquences jugées INNOCENTES (notes tenues) */
static struct { int bin; time_t until; } g_black[8];

static int blacklisted(int bin)
{
    time_t now = time(NULL);
    for (unsigned i = 0; i < 8; i++)
        if (g_black[i].until > now && abs(g_black[i].bin - bin) <= 3)
            return 1;
    return 0;
}

static void blacklist(int bin)
{
    time_t now = time(NULL);
    for (unsigned i = 0; i < 8; i++)
        if (g_black[i].until <= now) {
            g_black[i].bin = bin;
            g_black[i].until = now + g_cfg.blacklist_s;
            return;
        }
    g_black[0].bin = bin;
    g_black[0].until = now + g_cfg.blacklist_s;
}

static double ms_since(const struct timespec *t0)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - t0->tv_sec) * 1000.0 +
           (now.tv_nsec - t0->tv_nsec) / 1e6;
}

static struct held *held_for(double f)
{
    for (int i = 0; i < MAX_HELD; i++)
        if (g_held[i].used && fabs(g_held[i].f_hz - f) < 3 * HZ_PER_BIN)
            return &g_held[i];
    return NULL;
}

/* démarre une sonde sur la candidate b (pré-conditions déjà vérifiées) */
static void probe_start(int b, int32_t (*in_frames)[NCHAN])
{
    g_probe.state = P_PROBE;
    g_probe.bin = b;
    g_probe.f_hz = b * HZ_PER_BIN;
    clock_gettime(CLOCK_MONOTONIC, &g_probe.t0);
    memset(g_probe.culprit, 0, sizeof(g_probe.culprit));
    for (int ch = 0; ch < NCHAN; ch++) {
        g_probe.pre_in_db[ch] = -160.0f;
        g_probe.involved[ch] = 0;
        if (!g_flag[ch])
            continue;
        g_probe.pre_in_db[ch] = energy_at(in_frames, ch, b);
        g_probe.involved[ch] = g_probe.pre_in_db[ch] > g_cfg.in_floor_db;
    }
    op_notch(-1, g_probe.f_hz, g_cfg.depth_start_db);
    fprintf(stderr, "al: SONDE %.0f Hz (notch %.0f dB sur voies flaguées)\n",
            g_probe.f_hz, g_cfg.depth_start_db);
}

/* verdict après verdict_ms : larsen (f morte à l'entrée) ou note tenue */
static void probe_verdict(int32_t (*in_frames)[NCHAN])
{
    int larsen = 1, checked = 0;
    for (int ch = 0; ch < NCHAN; ch++) {
        if (!g_probe.involved[ch])
            continue;
        checked++;
        float post = energy_at(in_frames, ch, g_probe.bin);
        if (g_probe.pre_in_db[ch] - post < g_cfg.confirm_drop_db)
            larsen = 0;   /* f persiste ici : vraie source acoustique */
    }
    if (!checked)
        larsen = 0;

    if (!larsen) {
        op_release(-1, g_probe.f_hz);
        blacklist(g_probe.bin);
        fprintf(stderr, "al: INNOCENT %.0f Hz (persiste à l'entrée) — "
                "retrait immédiat, blacklist %d s\n",
                g_probe.f_hz, g_cfg.blacklist_s);
        g_probe.state = P_IDLE;
        return;
    }

    fprintf(stderr, "al: CONFIRMÉ %.0f Hz — identification du coupable\n",
            g_probe.f_hz);
    g_probe.state = P_REFINE;
    g_probe.refine_ch = -1;   /* avancé par refine_step */
}

/* ré-ouverture voie par voie : la voie dont la ré-ouverture fait repartir
 * f à la SORTIE est coupable (re-notch) ; sinon elle reste ouverte. */
static void refine_step(int32_t (*out_frames)[NCHAN],
                        int32_t (*in_frames)[NCHAN])
{
    (void)in_frames;
    /* verdict de la voie précédemment ré-ouverte */
    if (g_probe.refine_ch >= 0) {
        float out_now = energy_at(out_frames, 0, g_probe.bin);
        float out_now2 = energy_at(out_frames, 1, g_probe.bin);
        if (out_now2 > out_now)
            out_now = out_now2;
        int regrow = out_now > g_probe.out_pre_db + 6.0f ||
                     out_now > g_cfg.thresh_db;
        if (regrow) {
            g_probe.culprit[g_probe.refine_ch] = 1;
            op_notch(g_probe.refine_ch, g_probe.f_hz,
                     g_cfg.depth_start_db);
            fprintf(stderr, "al: voie %d COUPABLE (%.0f Hz repart) — "
                    "re-notch\n", g_probe.refine_ch, g_probe.f_hz);
        } else {
            fprintf(stderr, "al: voie %d hors de cause (%.0f Hz)\n",
                    g_probe.refine_ch, g_probe.f_hz);
        }
    }
    /* voie suivante à tester */
    int next = -1;
    for (int ch = g_probe.refine_ch + 1; ch < NCHAN; ch++)
        if (g_probe.involved[ch]) { next = ch; break; }
    if (next < 0) {
        /* terminé : registre. Si AUCUNE voie isolée coupable (couplage
         * multi-micros), on garde le notch sur toutes les impliquées. */
        int any = 0;
        for (int ch = 0; ch < NCHAN; ch++)
            any |= g_probe.culprit[ch];
        if (!any) {
            for (int ch = 0; ch < NCHAN; ch++)
                if (g_probe.involved[ch]) {
                    g_probe.culprit[ch] = 1;
                    op_notch(ch, g_probe.f_hz, g_cfg.depth_start_db);
                }
            fprintf(stderr, "al: pas de coupable isolé %.0f Hz — notch "
                    "gardé sur toutes les voies impliquées\n", g_probe.f_hz);
        }
        for (int i = 0; i < MAX_HELD; i++)
            if (!g_held[i].used) {
                g_held[i].used = 1;
                g_held[i].f_hz = g_probe.f_hz;
                g_held[i].depth_db = g_cfg.depth_start_db;
                memcpy(g_held[i].voices, g_probe.culprit,
                       sizeof(g_held[i].voices));
                g_held[i].posed_at = g_held[i].last_hit = time(NULL);
                break;
            }
        g_probe.state = P_IDLE;
        return;
    }
    g_probe.refine_ch = next;
    float o0 = energy_at(out_frames, 0, g_probe.bin);
    float o1 = energy_at(out_frames, 1, g_probe.bin);
    g_probe.out_pre_db = o0 > o1 ? o0 : o1;
    op_release(next, g_probe.f_hz);
    clock_gettime(CLOCK_MONOTONIC, &g_probe.t0);
}

/* récidive sur un notch tenu → approfondissement ; libération sinon */
static void held_maintain(void)
{
    time_t now = time(NULL);
    for (int i = 0; i < MAX_HELD; i++) {
        struct held *h = &g_held[i];
        if (!h->used)
            continue;
        for (int c = 0; c < MAX_CAND; c++)
            if (g_cand[c].count &&
                abs(g_cand[c].bin - (int)(h->f_hz / HZ_PER_BIN)) <= 2) {
                if (h->depth_db + g_cfg.depth_step_db >= -40.0f &&
                    now - h->last_hit >= 1) {
                    h->depth_db += g_cfg.depth_step_db;
                    for (int ch = 0; ch < NCHAN; ch++)
                        if (h->voices[ch])
                            op_notch(ch, h->f_hz, h->depth_db);
                    fprintf(stderr, "al: récidive %.0f Hz → %.0f dB\n",
                            h->f_hz, h->depth_db);
                }
                h->last_hit = now;
                g_cand[c].count = 0;
            }
        if (now - h->last_hit > g_cfg.release_s) {
            for (int ch = 0; ch < NCHAN; ch++)
                if (h->voices[ch])
                    op_release(ch, h->f_hz);
            fprintf(stderr, "al: LIBÉRÉ %.0f Hz (%d s sans récidive)\n",
                    h->f_hz, g_cfg.release_s);
            memset(h, 0, sizeof(*h));
        }
    }
}

/* ================= statut socket (GUI /api/larsen) ================= */
static int g_status_fd = -1;

static void status_open(void)
{
    unlink(STATUS_SOCK);
    g_status_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    strncpy(sa.sun_path, STATUS_SOCK, sizeof(sa.sun_path) - 1);
    if (bind(g_status_fd, (struct sockaddr *)&sa, sizeof(sa)) == 0)
        listen(g_status_fd, 4);
}

static void al_set_enable(int en)
{
    g_cfg.enable = en ? 1 : 0;
    char rep[128];
    char req[64];
    snprintf(req, sizeof(req), "{\"op\":\"larsen_enable\",\"on\":%d}\n",
             g_cfg.enable);
    mixer_op(req, rep, sizeof(rep));   /* off → le moteur retire tout */
    if (!g_cfg.enable) {
        memset(g_held, 0, sizeof(g_held));
        memset(g_cand, 0, sizeof(g_cand));
        g_probe.state = P_IDLE;
    }
    fprintf(stderr, "al: enable=%d\n", g_cfg.enable);
}

static void status_serve(void)
{
    int c = accept(g_status_fd, NULL, NULL);
    if (c < 0)
        return;
    {
        struct timeval tv = { 0, 80000 };
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        char cmd[64];
        ssize_t r = recv(c, cmd, sizeof(cmd) - 1, 0);
        if (r > 0) {
            cmd[r] = '\0';
            int en;
            float v;
            if (sscanf(cmd, "enable %d", &en) == 1)
                al_set_enable(en);
            else if (sscanf(cmd, "verdict_ms %f", &v) == 1 &&
                     v >= 100 && v <= 2000)
                g_cfg.verdict_ms = v;
            else if (sscanf(cmd, "thresh_db %f", &v) == 1 &&
                     v >= -80 && v <= -20)
                g_cfg.thresh_db = v;
        }
    }
    char buf[2048];
    time_t now = time(NULL);
    int n = snprintf(buf, sizeof(buf),
                     "{\"ok\":true,\"enable\":%d,\"engine\":%d,"
                     "\"state\":\"%s\",\"probe_hz\":%.0f,"
                     "\"verdict_ms\":%.0f,\"notches\":[",
                     g_cfg.enable, g_engine_enable,
                     g_probe.state == P_IDLE ? "idle" :
                     g_probe.state == P_PROBE ? "probe" : "refine",
                     g_probe.state != P_IDLE ? g_probe.f_hz : 0.0,
                     g_cfg.verdict_ms);
    int first = 1;
    for (int i = 0; i < MAX_HELD; i++) {
        struct held *h = &g_held[i];
        if (!h->used)
            continue;
        int nv = 0;
        char vs[64] = "";
        for (int ch = 0; ch < NCHAN; ch++)
            if (h->voices[ch])
                nv += snprintf(vs + nv, sizeof(vs) - nv, "%s%d",
                               nv ? "," : "", ch);
        n += snprintf(buf + n, sizeof(buf) - n,
                      "%s{\"freq\":%.0f,\"depth\":%.0f,\"voices\":[%s],"
                      "\"age\":%ld}",
                      first ? "" : ",", h->f_hz, h->depth_db, vs,
                      (long)(now - h->posed_at));
        first = 0;
    }
    n += snprintf(buf + n, sizeof(buf) - n, "]}\n");
    (void)!write(c, buf, n);
    close(c);
}

/* ================= config ================= */
static void conf_load(void)
{
    FILE *f = fopen(CONF_PATH, "r");
    if (!f)
        return;
    char line[128], k[64];
    float v;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n')
            continue;
        if (sscanf(line, "%63s = %f", k, &v) != 2)
            continue;
        if (!strcmp(k, "enable")) g_cfg.enable = (int)v;
        else if (!strcmp(k, "thresh_db")) g_cfg.thresh_db = v;
        else if (!strcmp(k, "pnr_db")) g_cfg.pnr_db = v;
        else if (!strcmp(k, "persist_n")) g_cfg.persist_n = (int)v;
        else if (!strcmp(k, "depth_start_db")) g_cfg.depth_start_db = v;
        else if (!strcmp(k, "depth_step_db")) g_cfg.depth_step_db = v;
        else if (!strcmp(k, "verdict_ms")) g_cfg.verdict_ms = v;
        else if (!strcmp(k, "refine_ms")) g_cfg.refine_ms = v;
        else if (!strcmp(k, "confirm_drop_db")) g_cfg.confirm_drop_db = v;
        else if (!strcmp(k, "in_floor_db")) g_cfg.in_floor_db = v;
        else if (!strcmp(k, "blacklist_s")) g_cfg.blacklist_s = (int)v;
        else if (!strcmp(k, "release_s")) g_cfg.release_s = (int)v;
    }
    fclose(f);
}

int main(void)
{
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    conf_load();
    fprintf(stderr, "anti-larsen V15: enable=%d thresh=%.0f verdict=%.0fms "
            "drop=%.0fdB probe=%.0fdB (actuation mixer-pro, JAMAIS le TAC)\n",
            g_cfg.enable, g_cfg.thresh_db, g_cfg.verdict_ms,
            g_cfg.confirm_drop_db, g_cfg.depth_start_db);

    while ((tap_open(&g_tout, TAP_OUT_DEV) < 0 ||
            tap_open(&g_tin, TAP_IN_DEV) < 0) && !g_stop) {
        fprintf(stderr, "al: taps indisponibles, retry 5 s\n");
        sleep(5);
    }
    if (g_stop)
        return 0;
    g_tout.rd = tap_u32(&g_tout, 20);
    g_tin.rd = tap_u32(&g_tin, 20);

    g_fft_in = fftwf_alloc_real(NFFT);
    g_fft_out = fftwf_alloc_complex(NBINS);
    g_plan = fftwf_plan_dft_r2c_1d(NFFT, g_fft_in, g_fft_out, FFTW_ESTIMATE);
    for (int i = 0; i < NFFT; i++)
        g_win[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / (NFFT - 1));

    status_open();
    if (g_cfg.enable)
        al_set_enable(1);   /* synchronise le moteur au boot */

    static int32_t out_frames[NFFT][NCHAN], in_frames[NFFT][NCHAN];
    int have_out = 0, have_in = 0, tick = 0;
    const long period_ns = 100000000L;   /* 100 ms (ring tap ~170 ms) */

    while (!g_stop) {
        struct timespec ts = { 0, period_ns };
        nanosleep(&ts, NULL);
        status_serve();
        if (++tick % 10 == 0)
            poll_flags();

        have_out += tap_read(&g_tout, out_frames + have_out,
                             NFFT - have_out);
        have_in  += tap_read(&g_tin, in_frames + have_in, NFFT - have_in);
        if (have_out < NFFT || have_in < NFFT)
            continue;

        if (g_cfg.enable && g_engine_enable) {
            switch (g_probe.state) {
            case P_IDLE:
                /* candidates sur les sorties façade (0/1) + retours (2..7) */
                for (int ch = 0; ch < NCHAN; ch++)
                    if (ch_active(out_frames, ch,
                                  g_cfg.thresh_db - 10.0f)) {
                        spectrum_ch(out_frames, ch);
                        detect_out_ch();
                    }
                held_maintain();
                for (int c = 0; c < MAX_CAND; c++) {
                    struct candidate *cd = &g_cand[c];
                    if (cd->count < g_cfg.persist_n)
                        continue;
                    cd->count = 0;
                    if (blacklisted(cd->bin) ||
                        held_for(cd->bin * HZ_PER_BIN))
                        continue;
                    /* pré-condition : f présente dans ≥1 entrée flaguée
                     * (sinon ça ne peut pas être un larsen micro) */
                    int any = 0;
                    for (int ch = 0; ch < NCHAN && !any; ch++)
                        any = g_flag[ch] &&
                              energy_at(in_frames, ch, cd->bin) >
                              g_cfg.in_floor_db;
                    if (any) {
                        probe_start(cd->bin, in_frames);
                        break;   /* une sonde à la fois */
                    }
                }
                break;
            case P_PROBE:
                if (ms_since(&g_probe.t0) >= g_cfg.verdict_ms)
                    probe_verdict(in_frames);
                break;
            case P_REFINE:
                if (g_probe.refine_ch < 0 ||
                    ms_since(&g_probe.t0) >= g_cfg.refine_ms)
                    refine_step(out_frames, in_frames);
                break;
            }
        }
        have_out = have_in = 0;
    }

    if (g_cfg.enable)
        al_set_enable(0);   /* retire tous les notchs au shutdown */
    fprintf(stderr, "al: stop\n");
    return 0;
}
