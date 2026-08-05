/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * V16 — voice-clean : daemon de nettoyage de la voix (CPU3, FIFO 60).
 *
 * Consomme le ring SHM de mixer-pro ([voix, réf musique] 48 kHz par blocs
 * de 96), applique le mode choisi et repousse la voix traitée :
 *   VC_DTLN    : resample 48→16 k, DTLN 2 étages (TFLite CPU, états LSTM
 *                persistants), 16→48 k. Modèle parole pré-entraîné —
 *                l'écoute de Michael juge (bench 2026-08-05 : repisse
 *                −15,7 dB, chant −5,0 dB).
 *   VC_SPECSUB : soustraction spectrale de PUISSANCE 48 k native
 *                (STFT 512/50 %, α par bande appris quand la voix se
 *                tait — la cohérence de phase n'est PAS requise, c'est
 *                ce qui la rend possible là où la soustraction de forme
 *                d'onde a échoué en 2026-07).
 *   VC_GTCRN   : NON CHARGÉ (3 chemins de conversion en échec,
 *                fiche BENCH — bit modes_avail absent, le moteur refuse).
 *
 * Priorité SOUS les threads UAC2 (FIFO 95) du même cœur : ils préemptent
 * net. Garde-fou de campagne : delta xruns UAC2 = 0 exigé.
 * ARCHI_V16_VOICE_CLEAN.md.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <fftw3.h>
#include <tensorflow/lite/c/c_api.h>

#include "voice_clean_shm.h"

#define DTLN1_PATH "/usr/share/voice-clean/dtln_1.tflite"
#define DTLN2_PATH "/usr/share/voice-clean/dtln_2.tflite"

static volatile sig_atomic_t g_stop;
static void on_sig(int s) { (void)s; g_stop = 1; }

static struct vc_shm *g_shm;
static uint32_t g_dbg_tx, g_dbg_rx;
static float g_dbg_hopmax;
static uint32_t g_tx_rd;

/* ================= resampler 48k <-> 16k (FIR sinc-hann 45 taps) ====== */
#define RS_TAPS 45
static float g_rs_fir[RS_TAPS];
static float g_dec_hist[RS_TAPS];
static float g_int_hist[RS_TAPS / 3 + 2];

static void rs_init(void)
{
	/* passe-bas fc = 0.9 * (8 kHz) pour décimation/interpolation ×3 */
	const double fc = 0.9 / 3.0;   /* × Nyquist 48k */
	for (int i = 0; i < RS_TAPS; i++) {
		double n = i - (RS_TAPS - 1) / 2.0;
		double s = (n == 0.0) ? 2.0 * fc
			 : sin(2.0 * M_PI * fc * n) / (M_PI * n);
		double w = 0.5 - 0.5 * cos(2.0 * M_PI * i / (RS_TAPS - 1));
		g_rs_fir[i] = (float)(s * w);
	}
}

/* décime n échantillons 48k → n/3 @16k (n multiple de 3) */
static void rs_down(const float *in, int n, float *out)
{
	static float buf[VC_PERIOD * 8 + RS_TAPS];
	memcpy(buf, g_dec_hist, (RS_TAPS - 1) * sizeof(float));
	memcpy(buf + RS_TAPS - 1, in, n * sizeof(float));
	for (int o = 0; o < n / 3; o++) {
		float acc = 0;
		const float *p = buf + o * 3;
		for (int t = 0; t < RS_TAPS; t++)
			acc += p[t] * g_rs_fir[t];
		out[o] = acc;
	}
	memcpy(g_dec_hist, buf + n, (RS_TAPS - 1) * sizeof(float));
}

/* interpole n échantillons 16k → n*3 @48k (zéro-stuffing + même FIR ×3) */
static void rs_up(const float *in, int n, float *out)
{
	static float buf[VC_PERIOD * 4 + RS_TAPS];
	const int h = RS_TAPS / 3 + 1;
	memcpy(buf, g_int_hist, h * sizeof(float));
	memcpy(buf + h, in, n * sizeof(float));
	for (int o = 0; o < n * 3; o++) {
		/* phase polyphase : out[o] = Σ in[(o-t)/3] fir[t] pour t≡o mod 3 */
		float acc = 0;
		int ph = o % 3, base = o / 3;
		for (int t = ph, k = 0; t < RS_TAPS; t += 3, k++)
			acc += buf[h + base - k - 1 + 1] * g_rs_fir[t];
		out[o] = acc * 3.0f;
	}
	memcpy(g_int_hist, buf + n, h * sizeof(float));
}

/* ================= DTLN (TFLite C API) ================= */
static TfLiteInterpreter *g_it1, *g_it2;
static float g_st1[512], g_st2[512];   /* états LSTM (tailles réelles lues) */
static int g_st1_n, g_st2_n;
static float g_dtln_in[512], g_dtln_out[512];   /* fenêtres 512 @16k */
static fftwf_plan g_dt_fft, g_dt_ifft;
static float g_dt_re[512];
static fftwf_complex g_dt_cx[257];

static TfLiteInterpreter *dtln_load(const char *path)
{
	TfLiteModel *m = TfLiteModelCreateFromFile(path);
	if (!m)
		return NULL;
	TfLiteInterpreterOptions *o = TfLiteInterpreterOptionsCreate();
	TfLiteInterpreterOptionsSetNumThreads(o, 1);
	TfLiteInterpreter *it = TfLiteInterpreterCreate(m, o);
	if (it && TfLiteInterpreterAllocateTensors(it) != kTfLiteOk) {
		TfLiteInterpreterDelete(it);
		it = NULL;
	}
	return it;
}

static int dtln_init(void)
{
	g_it1 = dtln_load(DTLN1_PATH);
	g_it2 = dtln_load(DTLN2_PATH);
	if (!g_it1 || !g_it2)
		return -1;
	/* états = 2e entrée de chaque modèle */
	const TfLiteTensor *s1 = TfLiteInterpreterGetInputTensor(g_it1, 1);
	const TfLiteTensor *s2 = TfLiteInterpreterGetInputTensor(g_it2, 1);
	g_st1_n = (int)(TfLiteTensorByteSize(s1) / sizeof(float));
	g_st2_n = (int)(TfLiteTensorByteSize(s2) / sizeof(float));
	if (g_st1_n > 512 || g_st2_n > 512)
		return -1;
	g_dt_fft = fftwf_plan_dft_r2c_1d(512, g_dt_re, g_dt_cx, FFTW_ESTIMATE);
	g_dt_ifft = fftwf_plan_dft_c2r_1d(512, g_dt_cx, g_dt_re, FFTW_ESTIMATE);
	fprintf(stderr, "vc: DTLN chargé (états %d/%d)\n", g_st1_n, g_st2_n);
	return 0;
}

static void dtln_reset(void)
{
	memset(g_st1, 0, sizeof(g_st1));
	memset(g_st2, 0, sizeof(g_st2));
	memset(g_dtln_in, 0, sizeof(g_dtln_in));
	memset(g_dtln_out, 0, sizeof(g_dtln_out));
}

/* un hop DTLN : 128 nouveaux échantillons 16k → 128 traités (latence 512) */
static void dtln_hop(const float *in128, float *out128)
{
	memmove(g_dtln_in, g_dtln_in + 128, 384 * sizeof(float));
	memcpy(g_dtln_in + 384, in128, 128 * sizeof(float));

	memcpy(g_dt_re, g_dtln_in, 512 * sizeof(float));
	fftwf_execute(g_dt_fft);
	float mag[257], phr[257], phi[257];
	for (int b = 0; b < 257; b++) {
		float re = g_dt_cx[b][0], im = g_dt_cx[b][1];
		mag[b] = sqrtf(re * re + im * im);
		float m = mag[b] > 1e-12f ? mag[b] : 1.0f;
		phr[b] = re / m;
		phi[b] = im / m;
	}
	/* étage 1 : masque spectral */
	TfLiteTensor *i0 = TfLiteInterpreterGetInputTensor(g_it1, 0);
	TfLiteTensor *i1 = TfLiteInterpreterGetInputTensor(g_it1, 1);
	TfLiteTensorCopyFromBuffer(i0, mag, 257 * sizeof(float));
	TfLiteTensorCopyFromBuffer(i1, g_st1, g_st1_n * sizeof(float));
	TfLiteInterpreterInvoke(g_it1);
	const TfLiteTensor *o0 = TfLiteInterpreterGetOutputTensor(g_it1, 0);
	const TfLiteTensor *o1 = TfLiteInterpreterGetOutputTensor(g_it1, 1);
	float mask[257];
	TfLiteTensorCopyToBuffer(o0, mask, 257 * sizeof(float));
	TfLiteTensorCopyToBuffer(o1, g_st1, g_st1_n * sizeof(float));
	for (int b = 0; b < 257; b++) {
		float mm = mag[b] * mask[b];
		g_dt_cx[b][0] = mm * phr[b];
		g_dt_cx[b][1] = mm * phi[b];
	}
	fftwf_execute(g_dt_ifft);
	float est[512];
	for (int i = 0; i < 512; i++)
		est[i] = g_dt_re[i] / 512.0f;
	/* étage 2 : raffinement temporel */
	i0 = TfLiteInterpreterGetInputTensor(g_it2, 0);
	i1 = TfLiteInterpreterGetInputTensor(g_it2, 1);
	TfLiteTensorCopyFromBuffer(i0, est, 512 * sizeof(float));
	TfLiteTensorCopyFromBuffer(i1, g_st2, g_st2_n * sizeof(float));
	TfLiteInterpreterInvoke(g_it2);
	o0 = TfLiteInterpreterGetOutputTensor(g_it2, 0);
	o1 = TfLiteInterpreterGetOutputTensor(g_it2, 1);
	float blk[512];
	TfLiteTensorCopyToBuffer(o0, blk, 512 * sizeof(float));
	TfLiteTensorCopyToBuffer(o1, g_st2, g_st2_n * sizeof(float));
	/* overlap-add décalé de 128 */
	memmove(g_dtln_out, g_dtln_out + 128, 384 * sizeof(float));
	memset(g_dtln_out + 384, 0, 128 * sizeof(float));
	for (int i = 0; i < 512; i++)
		g_dtln_out[i] += blk[i];
	memcpy(out128, g_dtln_out, 128 * sizeof(float));
}

/* ================= SPECSUB 48k (512/50 %, α par bande) ================= */
#define SS_N   512
#define SS_H   256
#define SS_NB  (SS_N / 2 + 1)
static float g_ss_win[SS_N];
static float g_ss_inv[SS_N], g_ss_inr[SS_N];  /* fenêtres glissantes voix/réf */
static float g_ss_ola[SS_N];
static float g_ss_alpha[SS_NB];               /* transfert réf→micro (puiss.) */
static float g_ss_pr[SS_NB];                  /* réf lissée */
static fftwf_plan g_ss_fft, g_ss_ifft, g_ss_fftr;
static float g_ss_re[SS_N];
static fftwf_complex g_ss_cx[SS_NB], g_ss_cxr[SS_NB];

static void specsub_init(void)
{
	for (int i = 0; i < SS_N; i++)
		g_ss_win[i] = sqrtf(0.5f - 0.5f * cosf(2.0f * (float)M_PI * i
						       / SS_N));
	g_ss_fft = fftwf_plan_dft_r2c_1d(SS_N, g_ss_re, g_ss_cx, FFTW_ESTIMATE);
	g_ss_fftr = fftwf_plan_dft_r2c_1d(SS_N, g_ss_re, g_ss_cxr, FFTW_ESTIMATE);
	g_ss_ifft = fftwf_plan_dft_c2r_1d(SS_N, g_ss_cx, g_ss_re, FFTW_ESTIMATE);
}

static void specsub_reset(void)
{
	memset(g_ss_inv, 0, sizeof(g_ss_inv));
	memset(g_ss_inr, 0, sizeof(g_ss_inr));
	memset(g_ss_ola, 0, sizeof(g_ss_ola));
	memset(g_ss_pr, 0, sizeof(g_ss_pr));
	for (int b = 0; b < SS_NB; b++)
		g_ss_alpha[b] = 1.0f;
}

/* un hop : 256 nouveaux échantillons voix + réf → 256 traités */
static void specsub_hop(const float *v256, const float *r256, float *out256)
{
	memmove(g_ss_inv, g_ss_inv + SS_H, SS_H * sizeof(float));
	memcpy(g_ss_inv + SS_H, v256, SS_H * sizeof(float));
	memmove(g_ss_inr, g_ss_inr + SS_H, SS_H * sizeof(float));
	memcpy(g_ss_inr + SS_H, r256, SS_H * sizeof(float));

	for (int i = 0; i < SS_N; i++)
		g_ss_re[i] = g_ss_inr[i] * g_ss_win[i];
	fftwf_execute(g_ss_fftr);
	for (int i = 0; i < SS_N; i++)
		g_ss_re[i] = g_ss_inv[i] * g_ss_win[i];
	fftwf_execute(g_ss_fft);

	/* énergies larges bandes pour la détection « voix absente » */
	double pv = 0, pr = 0;
	float Pv[SS_NB], Pr[SS_NB];
	for (int b = 0; b < SS_NB; b++) {
		Pv[b] = g_ss_cx[b][0] * g_ss_cx[b][0]
		      + g_ss_cx[b][1] * g_ss_cx[b][1];
		Pr[b] = g_ss_cxr[b][0] * g_ss_cxr[b][0]
		      + g_ss_cxr[b][1] * g_ss_cxr[b][1];
		g_ss_pr[b] += 0.3f * (Pr[b] - g_ss_pr[b]);
		pv += Pv[b];
		pr += Pr[b];
	}
	/* voix absente ≈ micro ≤ 2× ce que la repisse expliquerait →
	 * apprendre α par bande (mu lent). Jamais appris si réf muette. */
	if (pr > 1e-10 && pv < 2.0 * pr) {
		for (int b = 0; b < SS_NB; b++)
			if (g_ss_pr[b] > 1e-12f) {
				float a = Pv[b] / g_ss_pr[b];
				if (a > 4.0f) a = 4.0f;
				g_ss_alpha[b] += 0.05f * (a - g_ss_alpha[b]);
			}
	}
	/* gain spectral : floor −20 dB (β=0.1 en amplitude) */
	for (int b = 0; b < SS_NB; b++) {
		float est = Pv[b] - g_ss_alpha[b] * g_ss_pr[b];
		float g = est > 0.0f && Pv[b] > 1e-12f
			? sqrtf(est / Pv[b]) : 0.1f;
		if (g < 0.1f) g = 0.1f;
		g_ss_cx[b][0] *= g;
		g_ss_cx[b][1] *= g;
	}
	fftwf_execute(g_ss_ifft);
	memmove(g_ss_ola, g_ss_ola + SS_H, SS_H * sizeof(float));
	memset(g_ss_ola + SS_H, 0, SS_H * sizeof(float));
	for (int i = 0; i < SS_N; i++)
		g_ss_ola[i] += g_ss_re[i] * g_ss_win[i] / SS_N;
	memcpy(out256, g_ss_ola, SS_H * sizeof(float));
}

/* ================= push RX ================= */
static void rx_push(const float *x, int n)
{
	g_dbg_rx += n;
	uint32_t wr = atomic_load_explicit(&g_shm->rx_wr,
					   memory_order_relaxed);
	for (int i = 0; i < n; i++)
		g_shm->rx[(wr + i) & VC_RING_MASK] = x[i];
	atomic_store_explicit(&g_shm->rx_wr, wr + n,
			      memory_order_release);
}

int main(void)
{
	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);

	/* CPU3 (demande Michael), FIFO 60 — SOUS l'UAC2 (95) qui préempte */
	cpu_set_t cs;
	CPU_ZERO(&cs);
	CPU_SET(3, &cs);
	sched_setaffinity(0, sizeof(cs), &cs);
	struct sched_param sp = { .sched_priority = 60 };
	if (sched_setscheduler(0, SCHED_FIFO, &sp) < 0)
		fprintf(stderr, "vc: SCHED_FIFO refusé (%m) — SCHED_OTHER\n");
	mlockall(MCL_CURRENT | MCL_FUTURE);

	int fd = -1;
	while (!g_stop && (fd = shm_open(VC_SHM_NAME, O_RDWR, 0)) < 0)
		sleep(2);
	if (g_stop)
		return 0;
	g_shm = mmap(NULL, sizeof(struct vc_shm), PROT_READ | PROT_WRITE,
		     MAP_SHARED, fd, 0);
	close(fd);
	if (g_shm == MAP_FAILED || g_shm->magic != VC_MAGIC) {
		fprintf(stderr, "vc: SHM invalide\n");
		return 1;
	}

	rs_init();
	specsub_init();
	uint32_t avail_modes = 1u << VC_SPECSUB;
	if (dtln_init() == 0)
		avail_modes |= 1u << VC_DTLN;
	else
		fprintf(stderr, "vc: DTLN indisponible (modèles ?)\n");
	/* VC_GTCRN : volontairement absent (conversion en échec, cf. bench) */
	atomic_store(&g_shm->modes_avail, avail_modes);
	fprintf(stderr, "voice-clean V16: CPU3 FIFO60, modes_avail=0x%x, "
		"JAMAIS d'écriture TAC\n", avail_modes);

	g_tx_rd = atomic_load(&g_shm->tx_wr);
	uint32_t last_mode = VC_OFF, beat = 0;
	/* fifos de travail */
	static float v48[VC_PERIOD * 8], r48[VC_PERIOD * 8];
	static float f16[1024];
	int f16_n = 0;
	static float o16[1024];
	int o16_n = 0;
	static float vbuf[SS_H * 4], rbuf[SS_H * 4];
	int ss_n = 0;

	while (!g_stop) {
		struct timespec ts = { 0, 1000000 };   /* 1 ms */
		nanosleep(&ts, NULL);
		if (++beat % 1024 == 0)
			atomic_fetch_add(&g_shm->daemon_alive, 1);
		/* diag débit : TX consommé vs RX produit (déficit = famines) */
		if (beat % 5120 == 0 && g_dbg_tx)
			fprintf(stderr, "vc: 5s tx=%u rx=%u (delta %d) "
				"hop_max=%.2fms\n", g_dbg_tx, g_dbg_rx,
				(int)g_dbg_rx - (int)g_dbg_tx, g_dbg_hopmax);
		if (beat % 5120 == 0) { g_dbg_tx = g_dbg_rx = 0; g_dbg_hopmax = 0; }

		uint32_t mode = atomic_load(&g_shm->mode);
		if (mode != last_mode) {
			dtln_reset();
			specsub_reset();
			f16_n = o16_n = ss_n = 0;
			g_tx_rd = atomic_load(&g_shm->tx_wr);
			last_mode = mode;
		}
		if (mode == VC_OFF)
			continue;

		uint32_t wr = atomic_load_explicit(&g_shm->tx_wr,
						   memory_order_acquire);
		uint32_t avail = wr - g_tx_rd;   /* compteurs libres */
		while (avail >= VC_PERIOD) {
			for (int f = 0; f < VC_PERIOD; f++) {
				uint32_t idx = (g_tx_rd + f) & VC_RING_MASK;
				v48[f] = g_shm->tx[idx * 2];
				r48[f] = g_shm->tx[idx * 2 + 1];
			}
			g_tx_rd += VC_PERIOD;
			avail -= VC_PERIOD;
			g_dbg_tx += VC_PERIOD;

			if (mode == VC_DTLN) {
				/* 96@48k → 32@16k, hop DTLN à 128 */
				rs_down(v48, VC_PERIOD, f16 + f16_n);
				f16_n += VC_PERIOD / 3;
				while (f16_n >= 128) {
					struct timespec h0, h1;
					clock_gettime(CLOCK_MONOTONIC, &h0);
					dtln_hop(f16, o16 + o16_n);
					clock_gettime(CLOCK_MONOTONIC, &h1);
					float ms = (h1.tv_sec - h0.tv_sec) * 1e3f
						 + (h1.tv_nsec - h0.tv_nsec) / 1e6f;
					if (ms > g_dbg_hopmax) g_dbg_hopmax = ms;
					memmove(f16, f16 + 128,
						(f16_n - 128) * sizeof(float));
					f16_n -= 128;
					o16_n += 128;
				}
				while (o16_n >= 32) {
					float out96[VC_PERIOD];
					rs_up(o16, 32, out96);
					memmove(o16, o16 + 32,
						(o16_n - 32) * sizeof(float));
					o16_n -= 32;
					rx_push(out96, VC_PERIOD);
				}
			} else if (mode == VC_SPECSUB) {
				memcpy(vbuf + ss_n, v48,
				       VC_PERIOD * sizeof(float));
				memcpy(rbuf + ss_n, r48,
				       VC_PERIOD * sizeof(float));
				ss_n += VC_PERIOD;
				while (ss_n >= SS_H) {
					float out256[SS_H];
					specsub_hop(vbuf, rbuf, out256);
					memmove(vbuf, vbuf + SS_H,
						(ss_n - SS_H) * sizeof(float));
					memmove(rbuf, rbuf + SS_H,
						(ss_n - SS_H) * sizeof(float));
					ss_n -= SS_H;
					rx_push(out256, SS_H);
				}
			} else {
				rx_push(v48, VC_PERIOD);   /* mode inconnu */
			}
		}
	}
	fprintf(stderr, "vc: stop\n");
	return 0;
}
