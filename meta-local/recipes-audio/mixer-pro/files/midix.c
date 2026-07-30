// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * midix — V12-MIDIX : expandeur MIDI, consumer du ring SHM (voir midix.h).
 * Code déplacé tel quel depuis mixer-pro.c (V14.0 étape 1, extraction pure).
 * Seul changement : le struct anonyme g_midix est nommé (midix_state) pour
 * pouvoir être extern — initialisation identique (.gain = 1.0f).
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "util.h"      /* mlog */
#include "midix.h"
#include "control.h"    /* handlers d'ops (V14.0 étape 4) */
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>

#define MIDIX_SHM   "/ala-midix"

struct midix_state g_midix = { .gain = 1.0f };

/* persistence_thread (1 Hz) — tente le mmap tant que le daemon n'est pas
 * là ; invalide si le magic disparaît (arrêt propre du daemon). */
void midix_try_map(void)
{
	struct midix_hdr *h = atomic_load(&g_midix.hdr);
	if (h) {
		if (h->magic != MIDIX_MAGIC) {   /* daemon parti */
			atomic_store(&g_midix.hdr, NULL);
			munmap(h, g_midix.map_sz);
			g_midix.data = NULL;
			mlog("midix: ring invalidé (daemon arrêté)");
		}
		return;
	}
	int fd = shm_open(MIDIX_SHM, O_RDONLY, 0);
	if (fd < 0)
		return;
	struct stat st;
	if (fstat(fd, &st) < 0 || st.st_size < (off_t)sizeof(*h)) {
		close(fd);
		return;
	}
	void *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
	close(fd);
	if (m == MAP_FAILED)
		return;
	h = m;
	if (h->magic != MIDIX_MAGIC || !h->ring_frames ||
	    (off_t)(sizeof(*h) + (size_t)h->ring_frames * 2 * sizeof(float))
	    > st.st_size) {
		munmap(m, (size_t)st.st_size);
		return;
	}
	g_midix.map_sz = (size_t)st.st_size;
	g_midix.data = (float *)((char *)m + sizeof(*h));
	g_midix.ridx = atomic_load(&h->widx);   /* démarre au présent */
	atomic_store_explicit(&g_midix.hdr, h, memory_order_release);
	mlog("midix: ring mappé (%u frames)", h->ring_frames);
}

/* Rendu (audio_thread, SOUS target_lock, après loop_render) */
void midix_render(float in_block[N_INPUT_REAL][PERIOD_FRAMES])
{
	struct midix_hdr *h = atomic_load_explicit(&g_midix.hdr,
						   memory_order_acquire);
	if (!h)
		return;
	uint32_t w = atomic_load_explicit(&h->widx, memory_order_acquire);
	int32_t avail = (int32_t)(w - g_midix.ridx);
	if (avail < PERIOD_FRAMES) {   /* producer en retard → silence */
		atomic_fetch_add_explicit(&g_midix.underruns, 1,
					  memory_order_relaxed);
		return;
	}
	/* dérive/burst : si on traîne trop, on saute au présent */
	if (avail > (int32_t)(h->ring_frames / 2))
		g_midix.ridx = w - PERIOD_FRAMES;

	const int P = N_INPUT_MICS + N_INPUT_STEMS;   /* P1 = 16 */
	const uint32_t ring = h->ring_frames;
	const float g = g_midix.gain;
	float pk = 0.0f;
	for (int f = 0; f < PERIOD_FRAMES; f++) {
		uint32_t idx = (g_midix.ridx + f) % ring;
		float l = g_midix.data[(size_t)idx * 2];
		float r = g_midix.data[(size_t)idx * 2 + 1];
		in_block[P][f]     += l * g;
		in_block[P + 1][f] += r * g;
		float a = l < 0 ? -l : l, b = r < 0 ? -r : r;
		if (a > b) b = a;
		if (b > pk) pk = b;
	}
	g_midix.ridx += PERIOD_FRAMES;
	atomic_store_explicit(&g_midix.peak,
			      (uint32_t)(pk * g * 2147483647.0f),
			      memory_order_relaxed);
}

/* V14.0 étape 4 : ops du module — appelées par le dispatcher control.
 * Corps déplacés tels quels depuis handle_cmd (extraction pure) ;
 * retourne 1 si l'op est traitée, 0 sinon. */
int midix_handle_op(int fd, const char *line)
{
	if (json_has_op(line, "get_midix")) {
		/* V12-MIDIX : présence du module + santé pour la GUI */
		struct midix_hdr *h = atomic_load(&g_midix.hdr);
		dprintf(fd, "{\"ok\":true,\"present\":%d,\"underruns\":%u,"
			"\"peak\":%u,\"gain\":%.2f}\n",
			h ? 1 : 0,
			atomic_load(&g_midix.underruns),
			atomic_load(&g_midix.peak), g_midix.gain);
		return 1;
	}
	if (json_has_op(line, "set_midix")) {
		/* {"op":"set_midix","gain":F} — trim du module dans P1/P2 */
		float g = -1.0f;
		(void)json_get_float(line, "gain", &g);
		if (g >= 0.0f && g <= 4.0f)
			g_midix.gain = g;
		dprintf(fd, "{\"ok\":true,\"op\":\"set_midix\",\"gain\":%.2f}\n",
			g_midix.gain);
		return 1;
	}
	if (json_has_op(line, "midix_ctl")) {
		/* V12-MIDIX-GUI — proxy vers le daemon midi-expander (la GUI
		 * n'a qu'un canal : ce socket). {"op":"midix_ctl","cmd":
		 * "status"|"prog"|"gain"|"panic", chan?, num?, value?}.
		 * Control thread uniquement (jamais l'audio) ; si le daemon
		 * est absent, connect échoue immédiatement (pas de blocage). */
		char cmd[16] = "", raw[192] = "";
		int chan = -1, num = -1;
		float val = -1.0f;
		(void)json_get_str(line, "cmd", cmd, sizeof(cmd));
		(void)json_get_str(line, "line", raw, sizeof(raw));
		(void)json_get_int(line, "chan", &chan);
		(void)json_get_int(line, "num", &num);
		(void)json_get_float(line, "value", &val);
		char req[224];
		if (raw[0])   /* V12-SYNTH : passthrough générique (engine,
			       * inst_list, patch_get/set/save…) */
			snprintf(req, sizeof(req), "%s\n", raw);
		else if (!strcmp(cmd, "status"))
			snprintf(req, sizeof(req), "status\n");
		else if (!strcmp(cmd, "prog") && chan >= 0 && chan < 16 &&
			 num >= 0 && num < 128)
			snprintf(req, sizeof(req), "prog %d %d\n", chan, num);
		else if (!strcmp(cmd, "gain") && val >= 0.0f && val <= 10.0f)
			snprintf(req, sizeof(req), "gain %.3f\n", val);
		else if (!strcmp(cmd, "panic"))
			snprintf(req, sizeof(req), "panic\n");
		else {
			dprintf(fd, "{\"ok\":false,\"err\":\"bad midix cmd\"}\n");
			return 1;
		}
		int s = socket(AF_UNIX, SOCK_STREAM, 0);
		struct sockaddr_un sa = { .sun_family = AF_UNIX };
		snprintf(sa.sun_path, sizeof(sa.sun_path),
			 "/run/midi-expander.sock");
		struct timeval tv = { .tv_sec = 0, .tv_usec = 500000 };
		setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
		/* réponses longues (inst_list ~8 Ko) : lecture en boucle
		 * jusqu'au '\n' final. Thread ctl unique → static ok. */
		static char resp[16384];
		ssize_t rn = 0;
		if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) == 0 &&
		    write(s, req, strlen(req)) > 0) {
			while (rn < (ssize_t)sizeof(resp) - 1) {
				ssize_t k = read(s, resp + rn,
						 sizeof(resp) - 1 - (size_t)rn);
				if (k <= 0)
					break;
				rn += k;
				if (resp[rn - 1] == '\n')
					break;
			}
		}
		close(s);
		if (rn > 0)
			write(fd, resp, (size_t)rn);
		else
			dprintf(fd, "{\"ok\":false,\"err\":\"expander absent\"}\n");
		return 1;
	}
	return 0;
}
