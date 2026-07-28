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
