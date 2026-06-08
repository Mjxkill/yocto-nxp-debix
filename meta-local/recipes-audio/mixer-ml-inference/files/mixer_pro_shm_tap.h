/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * V9.5.12 — Shared memory audio tap pour daemon mixer-ml-inference.
 *
 * Mixer-pro audio_thread écrit en continu USB IN [8,9] dans une zone
 * shared memory POSIX (/dev/shm/mixer-pro-tap-usb). Le daemon ml-inference
 * (process séparé) lit ce tap pour faire l'inférence NPU.
 *
 * Pourquoi process séparé : TFLite NPU + VX delegate + galcore créent
 * un freeze kernel quand exécutés dans le même process que les threads
 * RT99 ALSA d'audio. Process isolation = stable.
 *
 * Layout (similaire à npu_tap_hdr mais en SHM userspace) :
 *   [0     .. 127]   header (128 B, aligné cache line)
 *   [128   .. END]   ring data, float32 interleaved L,R
 *
 * Pattern read (daemon, R4 seqcount-style) :
 *   epoch1 = atomic_load_acquire(&hdr->epoch);
 *   w       = atomic_load_acquire(&hdr->write_idx);
 *   read data from ring [read_idx .. w) wrap mod ring_size
 *   epoch2 = atomic_load_acquire(&hdr->epoch);
 *   retry if epoch1 != epoch2
 *
 * Pattern write (mixer-pro, audio_thread) :
 *   memcpy data au write_idx (wrap)
 *   atomic_store_release(&hdr->write_idx, new_idx)
 *
 * Ring size par défaut : 4096 frames stéréo float32 = 32 KB ≈ 85 ms buffer.
 */
#ifndef __MIXER_PRO_SHM_TAP_H__
#define __MIXER_PRO_SHM_TAP_H__

#include <stdint.h>

#define MIXER_PRO_TAP_SHM_NAME    "/mixer-pro-tap-usb"
#define MIXER_PRO_TAP_RING_FRAMES 4096           /* 4096 stéréo float = 32 KB */
#define MIXER_PRO_TAP_HDR_SIZE    128            /* aligné cache line A53 */
#define MIXER_PRO_TAP_RING_BYTES  (MIXER_PRO_TAP_RING_FRAMES * 2 * 4)
#define MIXER_PRO_TAP_TOTAL_SIZE  (MIXER_PRO_TAP_HDR_SIZE + MIXER_PRO_TAP_RING_BYTES)

#define MIXER_PRO_TAP_MAGIC       0x4D504150U    /* "MPAP" little-endian */
#define MIXER_PRO_TAP_VERSION     1

struct mixer_pro_tap_hdr {
    uint32_t magic;          /* MIXER_PRO_TAP_MAGIC */
    uint32_t version;        /* MIXER_PRO_TAP_VERSION */
    uint32_t ring_frames;    /* MIXER_PRO_TAP_RING_FRAMES */
    uint32_t hdr_size;
    uint32_t epoch;          /* incrémenté à chaque reset (mixer-pro restart) */
    uint32_t write_idx;      /* frame index (wrap mod ring_frames), écrit par mixer-pro */
    uint32_t sample_rate;    /* 48000 */
    uint32_t channels;       /* 2 (interleaved L, R) */
    uint32_t format;         /* 1 = float32 */
    uint32_t reserved[23];   /* zero, padding 128 B */
} __attribute__((packed, aligned(128)));

#endif /* __MIXER_PRO_SHM_TAP_H__ */
