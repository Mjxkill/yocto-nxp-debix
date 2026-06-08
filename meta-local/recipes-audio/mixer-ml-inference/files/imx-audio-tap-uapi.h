/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * imx-audio-tap UAPI — shared between kernel module and userspace.
 *
 * V7.0-E4 dual-tap (i.MX8MP) :
 *   tap_in_buffer  @ 0x94270000, 256 KB : signal SAI7 RX brut (post-DAI cap,
 *                                          pre-multiband_drc), exposé via
 *                                          /dev/imx-audio-tap-in
 *   tap_out_buffer @ 0x942B0000, 256 KB : signal post-effets DSP play
 *                                          (anciennement V3.2.2 npu_tap), exposé
 *                                          via /dev/imx-audio-tap-out (E5)
 *
 * R2/R5 — DOIT correspondre à sof/src/include/sof/audio/npu_tap.h.
 *   Yocto recipe `do_configure:prepend` fait un diff au build entre les 2 paires
 *   d'adresses et `bbfatal` en cas de divergence (single source of truth).
 *
 * Layout d'un buffer (identique IN / OUT) :
 *   [base+0x0000 .. base+0x007F]    header (128 B, full HiFi4 cache line, M2)
 *   [base+0x0080 .. base+0x3FFFF]   ring data (NPU_TAP_DATA_SIZE_MAX = 262016 B)
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
 */
#ifndef __IMX_AUDIO_TAP_UAPI_H__
#define __IMX_AUDIO_TAP_UAPI_H__

#include <linux/types.h>

#define NPU_TAP_MAGIC          0x5441504EU   /* "NPAT" little-endian */
#define NPU_TAP_RING_SIZE      0x40000U      /* 256 KB total (header + data) */
#define NPU_TAP_HDR_SIZE       128U          /* M2 : full HiFi4 cache line */
#define NPU_TAP_DATA_SIZE_MAX  (NPU_TAP_RING_SIZE - NPU_TAP_HDR_SIZE)  /* 262016 */

/*
 * V7.0-E4 — adresses fixes des 2 carves reserved-memory (référence + cross-check
 * Yocto build). Le module kernel récupère l'adresse runtime depuis le DT
 * (memory-region phandle), il ne se fie PAS à ces constantes au probe.
 */
#define NPU_TAP_IN_PHYS_ADDR   0x94270000U   /* signal capture brut (E4) */
#define NPU_TAP_OUT_PHYS_ADDR  0x942B0000U   /* signal play post-effets (E5) */

/*
 * M4 : NPU_TAP_DATA_SIZE_MAX est DEPRECATED pour le ring wrap.
 *      Userspace DOIT lire hdr->ring_size (runtime, R6) — qui est aligné sur
 *      un multiple de period_bytes par le firmware.
 *      Pour V7.0 (period_bytes=3072) : ring_size = 261120 (85 périodes).
 */

struct npu_tap_hdr {
	__u32 magic;          /* @0   : NPU_TAP_MAGIC, écrit en DERNIER (M5) */
	__u32 version;        /* @4   : 5 (V7.0-E4 dual-tap) */
	__u32 ring_size;      /* @8   : R6 — runtime aligned */
	__u32 hdr_size;       /* @12  : 128 */
	__u32 epoch;          /* @16  : R1 — DSP increments par dai_common_params */
	__u32 write_idx;      /* @20  : DSP, wrap mod ring_size */
	__u32 read_idx;       /* @24  : A53, wrap mod ring_size */
	__u32 period_bytes;   /* @28  : 3072 nominal */
	__u32 sample_rate;    /* @32  : 48000 */
	__u32 channels;       /* @36  : 8 */
	__u32 frame_fmt;      /* @40  : SOF_IPC_FRAME_S32_LE */
	__u32 direction;      /* @44  : 0=playback (tap-out), 1=capture (tap-in) */
	__u32 reserved[18];   /* @48..@124 : zero-init, padding 128 B (M2) */
} __attribute__((packed, aligned(128)));    /* M1 : HiFi4 cache line */

#endif /* __IMX_AUDIO_TAP_UAPI_H__ */
