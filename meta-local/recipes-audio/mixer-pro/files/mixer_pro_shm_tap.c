/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * V9.5.12 — SHM audio tap : implémentation côté mixer-pro (writer).
 */

#define _GNU_SOURCE
#include "mixer_pro_shm_tap.h"

#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static struct mixer_pro_tap_hdr *g_hdr   = NULL;
static float                    *g_ring  = NULL;
static int                       g_init  = 0;

/* Init au démarrage de mixer-pro : crée /dev/shm/mixer-pro-tap-usb,
 * mmap, écrit header. Idempotent. */
int mixer_pro_shm_tap_init(void)
{
    if (g_init) return 0;

    int fd = shm_open(MIXER_PRO_TAP_SHM_NAME,
                      O_CREAT | O_RDWR, 0666);
    if (fd < 0) {
        fprintf(stderr, "shm_tap: shm_open failed\n");
        return -1;
    }
    if (ftruncate(fd, MIXER_PRO_TAP_TOTAL_SIZE) < 0) {
        fprintf(stderr, "shm_tap: ftruncate failed\n");
        close(fd);
        return -1;
    }
    void *base = mmap(NULL, MIXER_PRO_TAP_TOTAL_SIZE,
                      PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (base == MAP_FAILED) {
        fprintf(stderr, "shm_tap: mmap failed\n");
        return -1;
    }

    g_hdr  = (struct mixer_pro_tap_hdr *)base;
    g_ring = (float *)((char *)base + MIXER_PRO_TAP_HDR_SIZE);

    /* Re-init header (epoch++ pour signaler reset au reader). */
    uint32_t old_epoch = g_hdr->epoch;
    memset(g_hdr, 0, sizeof(*g_hdr));
    g_hdr->magic       = MIXER_PRO_TAP_MAGIC;
    g_hdr->version     = MIXER_PRO_TAP_VERSION;
    g_hdr->ring_frames = MIXER_PRO_TAP_RING_FRAMES;
    g_hdr->hdr_size    = MIXER_PRO_TAP_HDR_SIZE;
    g_hdr->epoch       = old_epoch + 1;
    g_hdr->sample_rate = 48000;
    g_hdr->channels    = 2;
    g_hdr->format      = 1;   /* float32 */
    /* Force visibility avant que les writes audio commencent. */
    atomic_thread_fence(memory_order_release);

    g_init = 1;
    fprintf(stderr, "shm_tap: ready %s (%d frames ring, epoch=%u)\n",
            MIXER_PRO_TAP_SHM_NAME, MIXER_PRO_TAP_RING_FRAMES, g_hdr->epoch);
    return 0;
}

/* Audio thread écrit n frames stéréo (L+R) dans le ring. Lock-free.
 * Appelé depuis audio_thread RT99 — DOIT être minimal et déterministe.
 */
void mixer_pro_shm_tap_write(const float *L, const float *R, int n)
{
    if (!g_init || !g_hdr) return;

    /* Charge write_idx courant en relaxed (single writer = nous). */
    uint32_t w = atomic_load_explicit((_Atomic uint32_t *)&g_hdr->write_idx,
                                       memory_order_relaxed);
    const uint32_t rsz = MIXER_PRO_TAP_RING_FRAMES;
    for (int i = 0; i < n; i++) {
        uint32_t idx = (w + i) & (rsz - 1);   /* rsz power of 2 = 4096 ✓ */
        g_ring[2 * idx + 0] = L[i];
        g_ring[2 * idx + 1] = R[i];
    }
    /* Publish nouveau write_idx (acquire-release sync avec reader). */
    atomic_store_explicit((_Atomic uint32_t *)&g_hdr->write_idx,
                          w + n, memory_order_release);
}
