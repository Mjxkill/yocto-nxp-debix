/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * imx-audio-tap UAPI — shared between kernel module and userspace.
 *
 * V3.2.2 NPU tap : capture du buffer audio post-effets DSP (post-MBDRC/PGA/DRC)
 * vers une zone DRAM partagée DSP↔A53 pour analyse ML temps réel par NPU.
 *
 * R2/R5 — DOIT correspondre à sof/src/include/sof/audio/npu_tap.h.
 *   Yocto recipe `do_configure_prepend` fait un diff au build entre les 2
 *   headers et `bbfatal` si NPU_TAP_PHYS_ADDR diverge.
 *   Triple sécurité : compile static_assert + Yocto build diff + DT runtime check.
 *
 * Adresse fixe 0x942B0000 (256 KB no-map carve dans dsp_reserved_heap),
 * hors SOF MEMORY{} (au-delà de 0x93400000), accessible DSP via cacheattr
 * region 4 = write-through cacheable.
 *
 * Layout :
 *   [0x942B0000 .. 0x942B007F]   header (128 B, full HiFi4 cache line, M2)
 *   [0x942B0080 .. 0x942EFFFF]   ring data (NPU_TAP_DATA_SIZE_MAX = 262016 B)
 *
 * Usage A53 (R4 seqcount-style read pattern) :
 *   uint32_t magic = atomic_load_acquire(&hdr->magic);
 *   if (magic != NPU_TAP_MAGIC) retry;            // M5 boot handshake
 *   do {
 *     e1 = atomic_load_acquire(&hdr->epoch);
 *     w  = atomic_load_acquire(&hdr->write_idx);
 *     read data from ring (modulo hdr->ring_size, R6)
 *     e2 = atomic_load_acquire(&hdr->epoch);
 *   } while (e1 != e2);
 *   if (e1 != last_seen_epoch) { reset read_idx ; last_seen_epoch = e1; }
 *
 * M6 — LIMITATION Phase 2 (mono-DAI) :
 *   Sentinelle firmware : un seul DAI playback peut être tapé à la fois.
 *   Si Phase 2 ajoute USB UAC2 playback en parallèle à SAI7 TX, il faudra :
 *   (A) accepter qu'un seul des deux est tapé (sentinelle actuelle), ou
 *   (B) refactor avec per-DAI buffer (4 zones DT séparées).
 */
#ifndef __IMX_AUDIO_TAP_UAPI_H__
#define __IMX_AUDIO_TAP_UAPI_H__

#include <linux/types.h>

#define NPU_TAP_MAGIC          0x5441504EU   /* "NPAT" little-endian */
#define NPU_TAP_RING_SIZE      0x40000U      /* 256 KB total */
#define NPU_TAP_HDR_SIZE       128U          /* M2 : full HiFi4 cache line */
#define NPU_TAP_DATA_SIZE_MAX  (NPU_TAP_RING_SIZE - NPU_TAP_HDR_SIZE)  /* 262016 */
#define NPU_TAP_PHYS_ADDR      0x942B0000U

/*
 * M4 : NPU_TAP_DATA_SIZE_MAX est DEPRECATED pour le ring wrap.
 *      Userspace DOIT lire hdr->ring_size (runtime, R6) — qui est aligné sur
 *      un multiple de period_bytes par le firmware.
 *      Pour V4.2 (period_bytes=3072) : ring_size = 261120 (85 périodes).
 */

struct npu_tap_hdr {
	__u32 magic;          /* @0   : NPU_TAP_MAGIC, écrit en DERNIER (M5) */
	__u32 version;        /* @4   : 4 (V3.2.2) */
	__u32 ring_size;      /* @8   : R6 — runtime aligned */
	__u32 hdr_size;       /* @12  : 128 */
	__u32 epoch;          /* @16  : R1 — DSP increments par dai_common_params */
	__u32 write_idx;      /* @20  : DSP, wrap mod ring_size */
	__u32 read_idx;       /* @24  : A53, wrap mod ring_size */
	__u32 period_bytes;   /* @28  : 3072 nominal */
	__u32 sample_rate;    /* @32  : 48000 */
	__u32 channels;       /* @36  : 8 */
	__u32 frame_fmt;      /* @40  : SOF_IPC_FRAME_S32_LE */
	__u32 reserved[19];   /* @44..@124 : zero-init, padding 128 B (M2) */
} __attribute__((packed, aligned(128)));    /* M1 : HiFi4 cache line */

#endif /* __IMX_AUDIO_TAP_UAPI_H__ */
