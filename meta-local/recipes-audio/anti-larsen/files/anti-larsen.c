/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * V11-AL E1 — anti-larsen automatique (AFS) pour la console A.L.A.
 *
 * Lit le tap FX (/dev/imx-audio-tap-out : play post-effets 8ch S32, le
 * signal exact qui part aux HP), détecte les raies de larsen par
 * heuristique classique (seuil + PNR + persistance/croissance +
 * non-harmonicité) et pose des notchs RBJ étroits dans les biquads DAC
 * du TAC5212 (slots BQ 7-12 réservés AFS, 1-6 utilisateur).
 *
 * AUCUNE modification de mixer-pro/SOF/kernel : mmap lecture seule
 * (curseur privé, coexiste avec ml-inference) + contrôles ALSA BYTES.
 * ARCHI/ARCHI_V11_ANTILARSEN.md (validé critic, 2 itérations).
 */
#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <errno.h>
#include <fcntl.h>
#include <fftw3.h>
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

#define TAP_DEV        "/dev/imx-audio-tap-out"
#define TAP_TOTAL      0x40000u
#define TAP_HDR        128u
#define TAP_MAGIC      0x5441504Eu
#define FS             48000
#define NCH            8
#define NFFT           8192
#define NBINS          (NFFT / 2 + 1)
#define HZ_PER_BIN     ((double)FS / NFFT)

#define CONF_PATH      "/etc/mixer-pro/anti-larsen.conf"
#define STATUS_SOCK    "/run/anti-larsen.sock"
#define CARD_NAME      "softac5212tdm"

#define N_PAIRS        4          /* TAC0-3, une paire de sorties chacun */
/* BQ 7..11 — le BQ12 est inaccessible tant que le driver kernel a
 * TAC5212_MAX_REG=0x7E (off-by-one, dernier octet du BQ12 au reg 0x7F
 * rejeté par le regmap → EIO). Passera à 6 quand le fix kernel sera
 * déployé. */
#define SLOTS_PER_PAIR 5
#define SLOT_BASE_BQ   7

/* ---- config (défauts = ARCHI) ---- */
static struct {
    int   enable;                  /* master enable */
    int   pair_en[N_PAIRS];        /* par paire de sorties */
    float thresh_db;               /* seuil absolu candidat */
    float pnr_db;                  /* peak-to-neighbour ratio min */
    int   persist_n;               /* analyses consécutives requises */
    float notch_q;
    float depth_start_db;          /* négatif */
    float depth_step_db;           /* négatif */
    float depth_max_db;            /* négatif */
    int   release_s;               /* libération notch inactif */
    int   coef_halved;             /* format TAC : N1/D1 stockés /2 (E0c) */
    int   analysis_hz;             /* cadence d'analyse */
} g_cfg = {
    .enable = 0, .pair_en = {1, 1, 1, 1},
    .thresh_db = -45.0f, .pnr_db = 25.0f, .persist_n = 4,
    .notch_q = 30.0f, .depth_start_db = -9.0f, .depth_step_db = -3.0f,
    .depth_max_db = -18.0f, .release_s = 60, .coef_halved = 0,
    .analysis_hz = 10,
};

/* ---- état ---- */
struct notch {
    int    used;
    double f_hz;
    float  depth_db;
    time_t posed_at;
    time_t last_hit;
};
struct candidate {
    int    bin;
    int    count;
    double last_mag;
};
#define MAX_CAND 16
static struct notch g_notch[N_PAIRS][SLOTS_PER_PAIR];
static struct candidate g_cand[N_PAIRS][MAX_CAND];

static volatile sig_atomic_t g_stop;
static void on_sig(int s) { (void)s; g_stop = 1; }

/* ================= tap FX ================= */
static volatile uint8_t *g_tap;
static uint32_t g_ring_size, g_rd;

static int tap_open(void)
{
    int fd = open(TAP_DEV, O_RDONLY);
    if (fd < 0)
        return -1;
    void *m = mmap(NULL, TAP_TOTAL, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED)
        return -1;
    g_tap = m;
    return 0;
}

static inline uint32_t tap_u32(uint32_t off)
{
    return *(volatile uint32_t *)(g_tap + off);
}

/* lit jusqu'à max frames BRUTES (8ch S32) — curseur privé, jamais
 * bloquant. Conversion float différée par canal (spectrum_pair), après
 * le gate RMS — V10-N8 : ne jamais payer pour du silence. */
static int tap_read(int32_t (*dst)[NCH], int max)
{
    if (tap_u32(0) != TAP_MAGIC)
        return 0;
    g_ring_size = tap_u32(8);
    uint32_t wr = tap_u32(20);
    const uint32_t fsz = NCH * 4;
    uint32_t avail = (wr - g_rd) % g_ring_size;
    int n = (int)(avail / fsz);
    if (n > max)
        n = max;
    for (int i = 0; i < n; i++) {
        uint32_t off = TAP_HDR + (g_rd + i * fsz) % g_ring_size;
        memcpy(dst[i], (const void *)(g_tap + off), fsz);
    }
    g_rd = (g_rd + n * fsz) % g_ring_size;
    return n;
}

/* gate : RMS rapide (1 échantillon sur 16) — un canal sous le seuil de
 * détection ne peut pas porter un larsen candidat, on saute sa FFT */
static int ch_active(int32_t (*frames)[NCH], int ch, float gate_db)
{
    double acc = 0;
    int n = 0;
    for (int i = 0; i < NFFT; i += 16, n++) {
        double v = frames[i][ch] * (1.0 / 2147483648.0);
        acc += v * v;
    }
    double rms_db = 10.0 * log10(acc / n + 1e-24);
    return rms_db > gate_db;
}

/* ================= notch RBJ → blob TAC (port bit-exact stripfx.js) ==== */
static void q31be(double x, uint8_t *out)
{
    double c = x < -1.0 ? -1.0 : (x > 0.9999999995 ? 0.9999999995 : x);
    int64_t v = llround(c * 2147483648.0);
    if (v < -2147483648LL) v = -2147483648LL;
    if (v > 2147483647LL)  v = 2147483647LL;
    uint32_t u = (uint32_t)v;
    out[0] = u >> 24; out[1] = u >> 16; out[2] = u >> 8; out[3] = u;
}

static void notch_blob(double f_hz, double q, uint8_t blob[20])
{
    double w0 = 2.0 * M_PI * f_hz / FS;
    double cw = cos(w0), sw = sin(w0), al = sw / (2.0 * q);
    double a0 = 1.0 + al;
    double N0 = 1.0 / a0, N1 = -2.0 * cw / a0, N2 = 1.0 / a0;
    double D1 = 2.0 * cw / a0, D2 = -(1.0 - al) / a0;
    if (g_cfg.coef_halved) {
        N1 /= 2.0;
        D1 /= 2.0;
    }
    q31be(N0, blob); q31be(N1, blob + 4); q31be(N2, blob + 8);
    q31be(D1, blob + 12); q31be(D2, blob + 16);
}

static const uint8_t FLAT_BLOB[20] = { 0x7F, 0xFF, 0xFF, 0xFF, 0 };

/* ================= écriture contrôle ALSA BYTES ================= */
static snd_ctl_t *g_ctl;

static int ctl_open(void)
{
    char dev[64];
    snprintf(dev, sizeof(dev), "hw:%s", CARD_NAME);
    return snd_ctl_open(&g_ctl, dev, 0);
}

static int bq_write(int tac, int bq, const uint8_t blob[20])
{
    char name[64];
    snprintf(name, sizeof(name), "TAC%d DAC BQ%d Coefs", tac, bq);
    snd_ctl_elem_id_t *id;
    snd_ctl_elem_value_t *val;
    snd_ctl_elem_id_alloca(&id);
    snd_ctl_elem_value_alloca(&val);
    snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
    snd_ctl_elem_id_set_name(id, name);
    snd_ctl_elem_value_set_id(val, id);
    for (int i = 0; i < 20; i++)
        snd_ctl_elem_value_set_byte(val, i, blob[i]);
    int r = snd_ctl_elem_write(g_ctl, val);
    if (r < 0)
        fprintf(stderr, "al: cset %s: %s\n", name, snd_strerror(r));
    return r;
}

/* ================= détection ================= */
static float *g_fft_in;
static fftwf_complex *g_fft_out;
static fftwf_plan g_plan;
static float g_win[NFFT];
static float g_spec_db[N_PAIRS][NBINS];

/* retourne le nb de canaux actifs analysés (0 = paire silencieuse) */
static int spectrum_pair(int32_t (*frames)[NCH], int pair)
{
    /* max(|L|,|R|) de la paire — un larsen sur un seul canal suffit.
     * Gate RMS : la FFT n'est payée que pour les canaux qui portent du
     * signal au-dessus du seuil de détection (marge 10 dB). */
    int done = 0;
    for (int half = 0; half < 2; half++) {
        int ch = pair * 2 + half;
        if (!ch_active(frames, ch, g_cfg.thresh_db - 10.0f))
            continue;
        for (int i = 0; i < NFFT; i++)
            g_fft_in[i] = (float)frames[i][ch] * (1.0f / 2147483648.0f)
                          * g_win[i];
        fftwf_execute(g_plan);
        for (int b = 0; b < NBINS; b++) {
            float re = g_fft_out[b][0], im = g_fft_out[b][1];
            float db = 10.0f * log10f((re * re + im * im) /
                                      ((float)NFFT * NFFT / 16.0f) + 1e-24f);
            if (!done || db > g_spec_db[pair][b])
                g_spec_db[pair][b] = db;
        }
        done++;
    }
    return done;
}

static float band_median_db(const float *sp, int center, int lo_excl, int hi)
{
    /* médiane approx (moyenne tronquée) de ±hi bins hors ±lo_excl */
    float acc = 0;
    int n = 0;
    for (int d = lo_excl + 1; d <= hi; d++) {
        int a = center - d, b = center + d;
        if (a >= 0)     { acc += sp[a]; n++; }
        if (b < NBINS)  { acc += sp[b]; n++; }
    }
    return n ? acc / n : -160.0f;
}

static struct notch *slot_for(int pair, double f_hz)
{
    /* notch existant proche (±3 bins) ? */
    for (int s = 0; s < SLOTS_PER_PAIR; s++) {
        struct notch *nt = &g_notch[pair][s];
        if (nt->used && fabs(nt->f_hz - f_hz) < 3 * HZ_PER_BIN)
            return nt;
    }
    for (int s = 0; s < SLOTS_PER_PAIR; s++)
        if (!g_notch[pair][s].used)
            return &g_notch[pair][s];
    /* saturation : réutilise le plus ancien */
    struct notch *old = &g_notch[pair][0];
    for (int s = 1; s < SLOTS_PER_PAIR; s++)
        if (g_notch[pair][s].posed_at < old->posed_at)
            old = &g_notch[pair][s];
    return old;
}

static void apply_notch(int pair, struct notch *nt)
{
    int slot = (int)(nt - g_notch[pair]);
    uint8_t blob[20];
    notch_blob(nt->f_hz, g_cfg.notch_q, blob);
    if (bq_write(pair, SLOT_BASE_BQ + slot, blob) == 0)
        fprintf(stderr, "al: pair %d slot %d NOTCH %.0f Hz %.0f dB\n",
                pair, slot, nt->f_hz, nt->depth_db);
}

static void clear_notch(int pair, struct notch *nt)
{
    int slot = (int)(nt - g_notch[pair]);
    if (bq_write(pair, SLOT_BASE_BQ + slot, FLAT_BLOB) == 0)
        fprintf(stderr, "al: pair %d slot %d LIBÉRÉ (%.0f Hz)\n",
                pair, slot, nt->f_hz);
    memset(nt, 0, sizeof(*nt));
}

static void analyse_pair(int pair)
{
    const float *sp = g_spec_db[pair];
    time_t now = time(NULL);

    /* 1. candidats de cette analyse */
    int hits[MAX_CAND], n_hits = 0;
    for (int b = 8; b < NBINS - 8 && n_hits < MAX_CAND; b++) {
        if (sp[b] < g_cfg.thresh_db)
            continue;
        if (sp[b] < sp[b - 1] || sp[b] < sp[b + 1])
            continue;                          /* pas un max local */
        float med = band_median_db(sp, b, 1, 10);
        if (sp[b] - med < g_cfg.pnr_db)
            continue;                          /* pas une raie pure */
        /* non-harmonicité : un partiel comparable à 2f ou 3f = musical */
        int h2 = b * 2, h3 = b * 3, musical = 0;
        for (int d = -2; d <= 2; d++) {
            if (h2 + d < NBINS && sp[h2 + d] > sp[b] - 12.0f) musical = 1;
            if (h3 + d < NBINS && sp[h3 + d] > sp[b] - 12.0f) musical = 1;
        }
        if (musical)
            continue;
        hits[n_hits++] = b;
    }

    /* 2. mise à jour des suivis (persistance + croissance) */
    for (int c = 0; c < MAX_CAND; c++) {
        struct candidate *cd = &g_cand[pair][c];
        if (!cd->count)
            continue;
        int found = -1;
        for (int h = 0; h < n_hits; h++)
            if (abs(hits[h] - cd->bin) <= 2) { found = h; break; }
        if (found < 0) {
            cd->count = 0;                    /* raie disparue */
            continue;
        }
        int b = hits[found];
        if (sp[b] >= cd->last_mag - 1.0f)     /* non décroissante */
            cd->count++;
        cd->bin = b;
        cd->last_mag = sp[b];
        hits[found] = -1;

        if (cd->count >= g_cfg.persist_n) {
            double f = cd->bin * HZ_PER_BIN;
            struct notch *nt = slot_for(pair, f);
            if (nt->used && fabs(nt->f_hz - f) < 3 * HZ_PER_BIN) {
                /* raie toujours là malgré le notch → approfondir */
                if (nt->depth_db > g_cfg.depth_max_db &&
                    now - nt->posed_at >= 1) {
                    nt->depth_db += g_cfg.depth_step_db;
                    if (nt->depth_db < g_cfg.depth_max_db)
                        nt->depth_db = g_cfg.depth_max_db;
                    apply_notch(pair, nt);
                }
                nt->last_hit = now;
            } else {
                if (nt->used)                  /* réutilisation : ancien */
                    clear_notch(pair, nt);
                nt->used = 1;
                nt->f_hz = f;
                nt->depth_db = g_cfg.depth_start_db;
                nt->posed_at = nt->last_hit = now;
                apply_notch(pair, nt);
            }
            cd->count = 0;                     /* re-armé */
        }
    }

    /* 3. nouveaux suivis */
    for (int h = 0; h < n_hits; h++) {
        if (hits[h] < 0)
            continue;
        for (int c = 0; c < MAX_CAND; c++) {
            struct candidate *cd = &g_cand[pair][c];
            if (cd->count)
                continue;
            cd->bin = hits[h];
            cd->count = 1;
            cd->last_mag = sp[hits[h]];
            break;
        }
    }

    /* 4. libération des notchs inactifs */
    for (int s = 0; s < SLOTS_PER_PAIR; s++) {
        struct notch *nt = &g_notch[pair][s];
        if (nt->used && now - nt->last_hit > g_cfg.release_s)
            clear_notch(pair, nt);
    }
}

/* ================= statut socket ================= */
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

static void status_serve(void)
{
    int c = accept(g_status_fd, NULL, NULL);
    if (c < 0)
        return;
    char buf[2048];
    int n = snprintf(buf, sizeof(buf),
                     "{\"ok\":true,\"enable\":%d,\"notches\":[", g_cfg.enable);
    int first = 1;
    time_t now = time(NULL);
    for (int p = 0; p < N_PAIRS; p++)
        for (int s = 0; s < SLOTS_PER_PAIR; s++) {
            struct notch *nt = &g_notch[p][s];
            if (!nt->used)
                continue;
            n += snprintf(buf + n, sizeof(buf) - n,
                          "%s{\"pair\":%d,\"bq\":%d,\"freq\":%.0f,"
                          "\"depth\":%.0f,\"age\":%ld}",
                          first ? "" : ",", p, SLOT_BASE_BQ + s,
                          nt->f_hz, nt->depth_db,
                          (long)(now - nt->posed_at));
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
        else if (!strcmp(k, "notch_q")) g_cfg.notch_q = v;
        else if (!strcmp(k, "depth_start_db")) g_cfg.depth_start_db = v;
        else if (!strcmp(k, "depth_max_db")) g_cfg.depth_max_db = v;
        else if (!strcmp(k, "release_s")) g_cfg.release_s = (int)v;
        else if (!strcmp(k, "coef_halved")) g_cfg.coef_halved = (int)v;
        else if (!strcmp(k, "pair0")) g_cfg.pair_en[0] = (int)v;
        else if (!strcmp(k, "pair1")) g_cfg.pair_en[1] = (int)v;
        else if (!strcmp(k, "pair2")) g_cfg.pair_en[2] = (int)v;
        else if (!strcmp(k, "pair3")) g_cfg.pair_en[3] = (int)v;
    }
    fclose(f);
}

int main(void)
{
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    conf_load();
    fprintf(stderr, "anti-larsen E1: enable=%d thresh=%.0f pnr=%.0f "
            "persist=%d q=%.0f halved=%d\n",
            g_cfg.enable, g_cfg.thresh_db, g_cfg.pnr_db,
            g_cfg.persist_n, g_cfg.notch_q, g_cfg.coef_halved);

    if (ctl_open() < 0) {
        fprintf(stderr, "al: carte %s introuvable\n", CARD_NAME);
        return 1;
    }
    /* slots AFS rendus flat au démarrage (état connu) */
    for (int p = 0; p < N_PAIRS; p++)
        for (int s = 0; s < SLOTS_PER_PAIR; s++)
            bq_write(p, SLOT_BASE_BQ + s, FLAT_BLOB);

    if (!g_cfg.enable) {
        fprintf(stderr, "al: enable=0 — slots libérés, sortie\n");
        return 0;
    }
    while (tap_open() < 0 && !g_stop) {
        fprintf(stderr, "al: tap indisponible, retry 5 s\n");
        sleep(5);
    }
    if (g_stop)
        return 0;
    g_rd = tap_u32(20);   /* partir du présent */

    g_fft_in = fftwf_alloc_real(NFFT);
    g_fft_out = fftwf_alloc_complex(NBINS);
    g_plan = fftwf_plan_dft_r2c_1d(NFFT, g_fft_in, g_fft_out, FFTW_ESTIMATE);
    for (int i = 0; i < NFFT; i++)
        g_win[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / (NFFT - 1));

    status_open();

    static int32_t frames[NFFT][NCH];
    int have = 0;
    /* Boucle 100 ms : CONTRAINTE par le ring du tap (261120 o ≈ 170 ms
     * de 8ch S32) — plus lent = débordement et fenêtres déchirées.
     * L'économie CPU vient du gate RMS (pas de FFT sur le silence) et
     * de la conversion différée par canal — pas de la cadence. */
    const long period_ns = 100000000L;

    while (!g_stop) {
        struct timespec ts = { 0, period_ns };
        nanosleep(&ts, NULL);
        status_serve();

        /* fenêtre : consomme le flux, analyse quand 8192 frames prêtes */
        int n = tap_read(frames + have, NFFT - have);
        have += n;
        if (have < NFFT)
            continue;

        for (int p = 0; p < N_PAIRS; p++) {
            if (!g_cfg.pair_en[p])
                continue;
            if (spectrum_pair(frames, p))
                analyse_pair(p);
        }
        have = 0;   /* fenêtre fraîche (pas de recouvrement — 5 Hz suffit) */
    }

    /* sortie propre : slots rendus */
    for (int p = 0; p < N_PAIRS; p++)
        for (int s = 0; s < SLOTS_PER_PAIR; s++)
            if (g_notch[p][s].used)
                clear_notch(p, &g_notch[p][s]);
    fprintf(stderr, "al: stop\n");
    return 0;
}
