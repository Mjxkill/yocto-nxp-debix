/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * V11-AL E1b — anti-larsen automatique (AFS) pour la console A.L.A.
 *
 * Lit le tap FX (/dev/imx-audio-tap-out : play post-effets 8ch S32, le
 * signal exact qui part aux HP), détecte les raies de larsen par
 * heuristique classique (seuil + PNR + persistance/croissance +
 * non-harmonicité) et pose des notchs RBJ dans les biquads DAC du
 * TAC5212, PAR CANAL.
 *
 * Mapping biquads (datasheet TAC5212 SLASF23A Table 7-48, validé board
 * 2026-07-06 au casque) : allocation modulo 4 canaux (famille TAC5x1x
 * 4ch) → sur TAC5212 (2 canaux) seuls BQ1/5/9 (canal 1) et BQ2/6/10
 * (canal 2) sont dans le chemin ; BQ3/4/7/8/11/12 pilotent des canaux
 * INEXISTANTS. Le daemon force '3 Biquads/Ch' et utilise BQ5/9 (ch A)
 * et BQ6/10 (ch B) — BQ1/BQ2 restent à l'utilisateur (panneau BIQUADS).
 *
 * Coefficients : N0,N1,N2,D1,D2 en Q1.31 big-endian avec N1 et D1
 * stockés DIVISÉS PAR 2 (validé à l'oreille : le format plein sature
 * D1≈2cos(w0) pour les notchs graves → filtre inopérant).
 *
 * AUCUNE modification de mixer-pro/SOF/kernel.
 * ARCHI/ARCHI_V11_ANTILARSEN.md (critic ×3).
 */
#define _GNU_SOURCE
#include <alsa/asoundlib.h>
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

#define TAP_DEV        "/dev/imx-audio-tap-out"
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
#define CARD_NAME      "softac5212tdm"

#define SLOTS_PER_CH   2
/* slots AFS par canal local du TAC (A=canal impair 1, B=canal 2).
 * BQ6/BQ12 exigent le fix kernel apply-tac5212-bq12-maxreg (MAX_REG
 * 0x7E→0x7F, déployé board 2026-07-06) — sans lui, EIO sur ces slots. */
static const int SLOT_BQ[2][SLOTS_PER_CH] = { { 5, 9 }, { 6, 10 } };

/* ---- config (défauts = ARCHI) ---- */
static struct {
    int   enable;
    int   pair_en[4];              /* par TAC (paire de canaux) */
    float thresh_db;
    float pnr_db;
    int   persist_n;
    float notch_q;
    float depth_start_db;
    float depth_step_db;
    float depth_max_db;
    int   release_s;
    int   coef_halved;             /* 1 = format TI validé board */
} g_cfg = {
    .enable = 0, .pair_en = {1, 1, 1, 1},
    .thresh_db = -45.0f, .pnr_db = 25.0f, .persist_n = 4,
    .notch_q = 30.0f, .depth_start_db = -9.0f, .depth_step_db = -3.0f,
    .depth_max_db = -18.0f, .release_s = 60, .coef_halved = 1,
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
static struct notch g_notch[NCHAN][SLOTS_PER_CH];
static struct candidate g_cand[NCHAN][MAX_CAND];

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

/* lit jusqu'à max frames BRUTES — curseur privé, jamais bloquant */
static int tap_read(int32_t (*dst)[NCHAN], int max)
{
    if (tap_u32(0) != TAP_MAGIC)
        return 0;
    g_ring_size = tap_u32(8);
    uint32_t wr = tap_u32(20);
    const uint32_t fsz = NCHAN * 4;
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

/* gate : RMS rapide (1/16 éch.) — sous le seuil, pas de larsen possible */
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

/* ====== notch RBJ → blob TAC (Q1.31 BE, N1/D1 divisés par 2) ====== */
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

/* ================= contrôles ALSA ================= */
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

/* force '3 Biquads/Ch' (item 3) — sinon BQ9/BQ10 hors chemin.
 * Fallback critic : échec ⇒ on reste sur les slots BQ5/BQ6 seuls. */
static void bq_config3(int tac)
{
    char name[64];
    snprintf(name, sizeof(name), "TAC%d DAC Biquad Config", tac);
    snd_ctl_elem_id_t *id;
    snd_ctl_elem_value_t *val;
    snd_ctl_elem_id_alloca(&id);
    snd_ctl_elem_value_alloca(&val);
    snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
    snd_ctl_elem_id_set_name(id, name);
    snd_ctl_elem_value_set_id(val, id);
    snd_ctl_elem_value_set_enumerated(val, 0, 3);
    if (snd_ctl_elem_write(g_ctl, val) < 0)
        fprintf(stderr, "al: %s -> 3/ch ECHEC (slots BQ9/10 indispo)\n",
                name);
}

/* ================= détection ================= */
static float *g_fft_in;
static fftwf_complex *g_fft_out;
static fftwf_plan g_plan;
static float g_win[NFFT];
static float g_spec[NBINS];

static int spectrum_ch(int32_t (*frames)[NCHAN], int ch)
{
    if (!ch_active(frames, ch, g_cfg.thresh_db - 10.0f))
        return 0;
    for (int i = 0; i < NFFT; i++)
        g_fft_in[i] = (float)frames[i][ch] * (1.0f / 2147483648.0f)
                      * g_win[i];
    fftwf_execute(g_plan);
    for (int b = 0; b < NBINS; b++) {
        float re = g_fft_out[b][0], im = g_fft_out[b][1];
        g_spec[b] = 10.0f * log10f((re * re + im * im) /
                                   ((float)NFFT * NFFT / 16.0f) + 1e-24f);
    }
    return 1;
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

static struct notch *slot_for(int ch, double f_hz)
{
    for (int s = 0; s < SLOTS_PER_CH; s++) {
        struct notch *nt = &g_notch[ch][s];
        if (nt->used && fabs(nt->f_hz - f_hz) < 3 * HZ_PER_BIN)
            return nt;
    }
    for (int s = 0; s < SLOTS_PER_CH; s++)
        if (!g_notch[ch][s].used)
            return &g_notch[ch][s];
    struct notch *old = &g_notch[ch][0];
    for (int s = 1; s < SLOTS_PER_CH; s++)
        if (g_notch[ch][s].posed_at < old->posed_at)
            old = &g_notch[ch][s];
    return old;
}

static int slot_bq(int ch, const struct notch *nt)
{
    return SLOT_BQ[ch & 1][(int)(nt - g_notch[ch])];
}

static void apply_notch(int ch, struct notch *nt)
{
    uint8_t blob[20];
    notch_blob(nt->f_hz, g_cfg.notch_q, blob);
    if (bq_write(ch / 2, slot_bq(ch, nt), blob) == 0)
        fprintf(stderr, "al: ch %d BQ%d NOTCH %.0f Hz %.0f dB\n",
                ch, slot_bq(ch, nt), nt->f_hz, nt->depth_db);
}

static void clear_notch(int ch, struct notch *nt)
{
    if (bq_write(ch / 2, slot_bq(ch, nt), FLAT_BLOB) == 0)
        fprintf(stderr, "al: ch %d BQ%d LIBÉRÉ (%.0f Hz)\n",
                ch, slot_bq(ch, nt), nt->f_hz);
    memset(nt, 0, sizeof(*nt));
}

static void analyse_ch(int ch)
{
    const float *sp = g_spec;
    time_t now = time(NULL);

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
        struct candidate *cd = &g_cand[ch][c];
        if (!cd->count)
            continue;
        int found = -1;
        for (int h = 0; h < n_hits; h++)
            if (hits[h] >= 0 && abs(hits[h] - cd->bin) <= 2) {
                found = h;
                break;
            }
        if (found < 0) {
            cd->count = 0;
            continue;
        }
        int b = hits[found];
        if (sp[b] >= cd->last_mag - 1.0f)
            cd->count++;
        cd->bin = b;
        cd->last_mag = sp[b];
        hits[found] = -1;

        if (cd->count >= g_cfg.persist_n) {
            double f = cd->bin * HZ_PER_BIN;
            struct notch *nt = slot_for(ch, f);
            if (nt->used && fabs(nt->f_hz - f) < 3 * HZ_PER_BIN) {
                if (nt->depth_db > g_cfg.depth_max_db &&
                    now - nt->posed_at >= 1) {
                    nt->depth_db += g_cfg.depth_step_db;
                    if (nt->depth_db < g_cfg.depth_max_db)
                        nt->depth_db = g_cfg.depth_max_db;
                    apply_notch(ch, nt);
                }
                nt->last_hit = now;
            } else {
                if (nt->used)
                    clear_notch(ch, nt);
                nt->used = 1;
                nt->f_hz = f;
                nt->depth_db = g_cfg.depth_start_db;
                nt->posed_at = nt->last_hit = now;
                apply_notch(ch, nt);
            }
            cd->count = 0;
        }
    }

    for (int h = 0; h < n_hits; h++) {
        if (hits[h] < 0)
            continue;
        for (int c = 0; c < MAX_CAND; c++) {
            struct candidate *cd = &g_cand[ch][c];
            if (cd->count)
                continue;
            cd->bin = hits[h];
            cd->count = 1;
            cd->last_mag = sp[hits[h]];
            break;
        }
    }

    for (int s = 0; s < SLOTS_PER_CH; s++) {
        struct notch *nt = &g_notch[ch][s];
        if (nt->used && now - nt->last_hit > g_cfg.release_s)
            clear_notch(ch, nt);
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

static void clear_notch(int ch, struct notch *nt);

static void status_serve(void)
{
    int c = accept(g_status_fd, NULL, NULL);
    if (c < 0)
        return;
    /* V13-SCENES : commande optionnelle avant la réponse — « enable 0|1 »
     * (toggle runtime depuis la GUI). Clients existants n'envoient rien :
     * timeout court puis status comme avant. */
    {
        struct timeval tv = { 0, 80000 };   /* 80 ms */
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        char cmd[32];
        ssize_t r = recv(c, cmd, sizeof(cmd) - 1, 0);
        if (r > 0) {
            cmd[r] = '\0';
            int en;
            if (sscanf(cmd, "enable %d", &en) == 1) {
                g_cfg.enable = en ? 1 : 0;
                if (!g_cfg.enable)
                    for (int ch2 = 0; ch2 < NCHAN; ch2++)
                        for (int s2 = 0; s2 < SLOTS_PER_CH; s2++)
                            if (g_notch[ch2][s2].used)
                                clear_notch(ch2, &g_notch[ch2][s2]);
                fprintf(stderr, "al: enable=%d (runtime)\n", g_cfg.enable);
            }
        }
    }
    char buf[2048];
    int n = snprintf(buf, sizeof(buf),
                     "{\"ok\":true,\"enable\":%d,\"notches\":[", g_cfg.enable);
    int first = 1;
    time_t now = time(NULL);
    for (int ch = 0; ch < NCHAN; ch++)
        for (int s = 0; s < SLOTS_PER_CH; s++) {
            struct notch *nt = &g_notch[ch][s];
            if (!nt->used)
                continue;
            n += snprintf(buf + n, sizeof(buf) - n,
                          "%s{\"ch\":%d,\"bq\":%d,\"freq\":%.0f,"
                          "\"depth\":%.0f,\"age\":%ld}",
                          first ? "" : ",", ch, slot_bq(ch, nt),
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
    fprintf(stderr, "anti-larsen E1b: enable=%d thresh=%.0f pnr=%.0f "
            "persist=%d q=%.0f halved=%d slots/ch=%d\n",
            g_cfg.enable, g_cfg.thresh_db, g_cfg.pnr_db,
            g_cfg.persist_n, g_cfg.notch_q, g_cfg.coef_halved,
            SLOTS_PER_CH);

    if (ctl_open() < 0) {
        fprintf(stderr, "al: carte %s introuvable\n", CARD_NAME);
        return 1;
    }
    /* état connu : slots AFS flat sur les 4 TAC */
    for (int tac = 0; tac < 4; tac++)
        for (int l = 0; l < 2; l++)
            for (int s = 0; s < SLOTS_PER_CH; s++)
                bq_write(tac, SLOT_BQ[l][s], FLAT_BLOB);

    if (!g_cfg.enable)
        /* V13-SCENES : on RESTE résident (activable depuis la GUI via
         * le socket) — la boucle saute l'analyse tant que enable=0. */
        fprintf(stderr, "al: enable=0 — en veille (activable runtime)\n");
    /* BQ9/BQ10 exigent '3 Biquads/Ch' */
    for (int tac = 0; tac < 4; tac++)
        if (g_cfg.pair_en[tac])
            bq_config3(tac);

    while (tap_open() < 0 && !g_stop) {
        fprintf(stderr, "al: tap indisponible, retry 5 s\n");
        sleep(5);
    }
    if (g_stop)
        return 0;
    g_rd = tap_u32(20);

    g_fft_in = fftwf_alloc_real(NFFT);
    g_fft_out = fftwf_alloc_complex(NBINS);
    g_plan = fftwf_plan_dft_r2c_1d(NFFT, g_fft_in, g_fft_out, FFTW_ESTIMATE);
    for (int i = 0; i < NFFT; i++)
        g_win[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / (NFFT - 1));

    status_open();

    static int32_t frames[NFFT][NCHAN];
    int have = 0;
    /* 100 ms : contraint par le ring du tap (~170 ms) */
    const long period_ns = 100000000L;

    while (!g_stop) {
        struct timespec ts = { 0, period_ns };
        nanosleep(&ts, NULL);
        status_serve();

        int n = tap_read(frames + have, NFFT - have);
        have += n;
        if (have < NFFT)
            continue;

        for (int ch = 0; ch < NCHAN; ch++) {
            if (!g_cfg.enable)   /* V13-SCENES : veille runtime */
                break;
            if (!g_cfg.pair_en[ch / 2])
                continue;
            if (spectrum_ch(frames, ch))
                analyse_ch(ch);
        }
        have = 0;
    }

    for (int ch = 0; ch < NCHAN; ch++)
        for (int s = 0; s < SLOTS_PER_CH; s++)
            if (g_notch[ch][s].used)
                clear_notch(ch, &g_notch[ch][s]);
    fprintf(stderr, "al: stop\n");
    return 0;
}
