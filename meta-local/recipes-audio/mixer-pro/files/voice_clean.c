// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * voice_clean — côté moteur (voir voice_clean.h). V16.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "state.h"
#include "util.h"
#include "strip_dyn.h"     /* N_EXP_CH */
#include "automix.h"       /* g_bmx.role (voies voix / réf musique) */
#include "voice_clean_shm.h"
#include "voice_clean.h"
#include "control.h"

struct vc_state g_vc = { .src = -1 };

void vc_init(void)
{
	int fd = shm_open(VC_SHM_NAME, O_CREAT | O_RDWR, 0666);
	if (fd < 0) {
		mlog("vc: shm_open ÉCHEC — voice-clean indisponible");
		return;
	}
	if (ftruncate(fd, sizeof(struct vc_shm)) < 0) {
		mlog("vc: ftruncate ÉCHEC");
		close(fd);
		return;
	}
	void *m = mmap(NULL, sizeof(struct vc_shm), PROT_READ | PROT_WRITE,
		       MAP_SHARED, fd, 0);
	close(fd);
	if (m == MAP_FAILED) {
		mlog("vc: mmap ÉCHEC");
		return;
	}
	g_vc.shm = m;
	memset(m, 0, sizeof(struct vc_shm));
	g_vc.shm->period = VC_PERIOD;
	atomic_store(&g_vc.shm->mode, VC_OFF);
	g_vc.shm->magic = VC_MAGIC;   /* magic en dernier (segment prêt) */
	mlog("vc: SHM %s prêt (%zu Ko)", VC_SHM_NAME,
	     sizeof(struct vc_shm) >> 10);
}

/* audio_thread, SOUS target_lock, après exp_render */
void vc_process(float in_block[N_INPUT_REAL][PERIOD_FRAMES])
{
	struct vc_shm *s = g_vc.shm;
	int mode = atomic_load_explicit(&g_vc.mode, memory_order_relaxed);
	int src = atomic_load_explicit(&g_vc.src, memory_order_relaxed);
	if (!s || mode == VC_OFF || src < 0 || src >= N_EXP_CH)
		return;

	/* réf musique = somme des voies rôle musique (kick..line) actives
	 * au sens simple (le daemon fait sa propre détection fine) */
	static float ref[PERIOD_FRAMES];
	memset(ref, 0, sizeof(ref));
	for (int i = 0; i < N_EXP_CH; i++) {
		int r = g_bmx.role[i];
		if (i == src || r == BR_OFF || r == BR_LEAD || r == BR_CHOIR)
			continue;
		for (int f = 0; f < PERIOD_FRAMES; f++)
			ref[f] += in_block[i][f];
	}

	/* push TX interleavé [voix, réf] */
	uint32_t wr = atomic_load_explicit(&s->tx_wr, memory_order_relaxed);
	for (int f = 0; f < PERIOD_FRAMES; f++) {
		uint32_t idx = (wr + f) & VC_RING_MASK;
		s->tx[idx * 2]     = in_block[src][f];
		s->tx[idx * 2 + 1] = ref[f];
	}
	atomic_store_explicit(&s->tx_wr, wr + PERIOD_FRAMES,
			      memory_order_release);

	/* pop RX → remplace la voie. AMORÇAGE : on ne consomme qu'à partir
	 * de VC_PRIME périodes d'avance (marge de jitter du daemon) ; une
	 * famine ré-amorce. Le silence d'amorçage EST la latence assumée. */
	#define VC_PRIME (12 * PERIOD_FRAMES)
	uint32_t rxw = atomic_load_explicit(&s->rx_wr, memory_order_acquire);
	uint32_t avail = rxw - g_vc.rx_rd;   /* compteurs libres */
	if (!g_vc.primed) {
		if (avail < VC_PRIME) {
			memset(in_block[src], 0,
			       PERIOD_FRAMES * sizeof(float));
			return;
		}
		g_vc.primed = 1;
	}
	if (avail >= PERIOD_FRAMES) {
		for (int f = 0; f < PERIOD_FRAMES; f++)
			in_block[src][f] =
				s->rx[(g_vc.rx_rd + f) & VC_RING_MASK];
		g_vc.rx_rd += PERIOD_FRAMES;
		/* fondu d'engagement 2 blocs (anti-clic au switch) */
		if (g_vc.ramp < 2 * PERIOD_FRAMES) {
			for (int f = 0; f < PERIOD_FRAMES; f++) {
				float g = (float)(g_vc.ramp + f)
					  / (2.0f * PERIOD_FRAMES);
				in_block[src][f] *= g > 1.0f ? 1.0f : g;
			}
			g_vc.ramp += PERIOD_FRAMES;
		}
	} else {
		memset(in_block[src], 0, PERIOD_FRAMES * sizeof(float));
		atomic_fetch_add_explicit(&g_vc.famines, 1,
					  memory_order_relaxed);
		g_vc.primed = 0;   /* ré-amorçage (reprend la marge complète) */
	}
}

int voice_clean_handle_op(int fd, const char *line)
{
	if (json_has_op(line, "voice_clean")) {
		/* {"op":"voice_clean","src":N,"mode":0..3} — src doit être une
		 * voie VOIX (lead/chœurs) ; mode non chargé par le daemon =
		 * refus explicite (jamais silencieux). */
		int src = -1, mode = -1;
		(void)json_get_int(line, "src", &src);
		(void)json_get_int(line, "mode", &mode);
		if (src < 0 || src >= N_EXP_CH || mode < 0 || mode > 3) {
			dprintf(fd, "{\"ok\":false,\"err\":\"bad args\"}\n");
			return 1;
		}
		int r = g_bmx.role[src];
		if (mode != VC_OFF && r != BR_LEAD && r != BR_CHOIR) {
			dprintf(fd, "{\"ok\":false,\"err\":\"pas une voie voix\"}\n");
			return 1;
		}
		if (!g_vc.shm) {
			dprintf(fd, "{\"ok\":false,\"err\":\"shm indisponible\"}\n");
			return 1;
		}
		uint32_t avail = atomic_load(&g_vc.shm->modes_avail);
		if (mode != VC_OFF && !(avail & (1u << mode))) {
			dprintf(fd, "{\"ok\":false,\"err\":\"mode %d indisponible "
				"(daemon)\"}\n", mode);
			return 1;
		}
		pthread_mutex_lock(&g_st.target_lock);
		atomic_store(&g_vc.src, mode == VC_OFF ? -1 : src);
		g_vc.rx_rd = g_vc.shm ?
			atomic_load(&g_vc.shm->rx_wr) : 0;   /* resynchro */
		g_vc.ramp = 0;
		g_vc.primed = 0;
		atomic_store(&g_vc.famines, 0);
		atomic_store(&g_vc.mode, mode);
		if (g_vc.shm)
			atomic_store(&g_vc.shm->mode, mode);
		pthread_mutex_unlock(&g_st.target_lock);
		mlog("vc: src=%d mode=%d", src, mode);
		dprintf(fd, "{\"ok\":true,\"op\":\"voice_clean\",\"src\":%d,"
			"\"mode\":%d}\n", src, mode);
		return 1;
	}
	if (json_has_op(line, "voice_clean_status")) {
		struct vc_shm *s = g_vc.shm;
		dprintf(fd, "{\"ok\":true,\"src\":%d,\"mode\":%d,"
			"\"famines\":%u,\"daemon\":%u,\"modes_avail\":%u,"
			"\"latency_ms\":48}\n",
			atomic_load(&g_vc.src), atomic_load(&g_vc.mode),
			atomic_load(&g_vc.famines),
			s ? atomic_load(&s->daemon_alive) : 0,
			s ? atomic_load(&s->modes_avail) : 0);
		return 1;
	}
	return 0;
}
