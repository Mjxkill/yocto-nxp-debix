// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * voice_clean_shm — layout du segment SHM entre mixer-pro (producteur voix
 * + réf musique, consommateur voix traitée) et le daemon voice-clean
 * (transformateur, CPU3). V16 (ARCHI_V16_VOICE_CLEAN.md).
 *
 * Copié dans la recette voice-clean (même fichier, source unique ici).
 */
#ifndef VOICE_CLEAN_SHM_H
#define VOICE_CLEAN_SHM_H

#include <stdatomic.h>
#include <stdint.h>

#define VC_SHM_NAME   "/ala-voice-clean"
#define VC_MAGIC      0x56434C4Eu   /* "VCLN" */
#define VC_PERIOD     96            /* frames par bloc (2 ms @48 k) */
/* RING = PUISSANCE DE 2 obligatoire : les indices sont des compteurs
 * LIBRES uint32 (wrap 2^32 sain) et l'adressage un masque. Un modulo sur
 * une taille non-2^n casse au wrap (bug V16 mesuré : tx lu à 4× temps
 * réel, 2^32 mod 6144 = 4096). 8192 frames ≈ 170 ms. */
#define VC_RING_FR    8192u
#define VC_RING_MASK  (VC_RING_FR - 1u)

/* modes (écrits par mixer-pro, lus par le daemon) */
enum { VC_OFF = 0, VC_DTLN = 1, VC_GTCRN = 2, VC_SPECSUB = 3 };

struct vc_shm {
	uint32_t magic;
	uint32_t period;               /* = VC_PERIOD */
	_Atomic uint32_t mode;         /* VC_* (daemon reset états au chgt) */
	_Atomic uint32_t daemon_alive; /* heartbeat daemon (incrémenté ~1 Hz) */
	_Atomic uint32_t modes_avail;  /* bitmask des modes chargés (daemon) */
	/* TX : mixer-pro → daemon, frames float32 INTERLEAVÉES [voix, réf] */
	_Atomic uint32_t tx_wr;        /* compteur LIBRE de frames (masque à l'accès) */
	/* RX : daemon → mixer-pro, float32 mono (voix traitée) */
	_Atomic uint32_t rx_wr;
	uint32_t _pad[9];
	float tx[VC_RING_FR * 2];
	float rx[VC_RING_FR];
};

#endif /* VOICE_CLEAN_SHM_H */
