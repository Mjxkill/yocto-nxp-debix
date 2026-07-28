// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * dsp_block — helpers NEON par bloc (chemin RT).
 *
 * V9.3.2 : NEON intrinsics pour les boucles inner du mix.
 * aarch64 a NEON nativement (toujours dispo). PERIOD_FRAMES=96 = multiple de 4
 * → pas de tail handling. Gain attendu × 3-4 sur les matrices send + master.
 *
 * Extraction V14.0 (étape 2, ARCHI_V14_RESTRUCTURATION.md) depuis
 * mixer-pro.c — code déplacé tel quel. Utilisé par mix_block (mixer-pro.c,
 * → audio_loop à l'étape 3) et vspat_render (voice.c).
 */
#ifndef MIXER_DSP_BLOCK_H
#define MIXER_DSP_BLOCK_H

#include <stdint.h>
#include <arm_neon.h>

/* Helper inline : dst[f] += src[f] * g pour f=0..N-1, N multiple de 4.
 * vmlaq_f32(a, b, c) = a + b * c (multiply-accumulate sur 4 floats). */
static inline void mac_block_n4(float *dst, const float *src, float g, uint32_t N)
{
	float32x4_t vg = vdupq_n_f32(g);
	for (uint32_t f = 0; f < N; f += 4) {
		float32x4_t vs = vld1q_f32(src + f);
		float32x4_t vd = vld1q_f32(dst + f);
		vd = vmlaq_f32(vd, vs, vg);
		vst1q_f32(dst + f, vd);
	}
}

/* dst[f] = src[f] * g pour f=0..N-1 (multiply, pas accumulate). */
static inline void mul_block_n4(float *dst, const float *src, float g, uint32_t N)
{
	float32x4_t vg = vdupq_n_f32(g);
	for (uint32_t f = 0; f < N; f += 4) {
		float32x4_t vs = vld1q_f32(src + f);
		vst1q_f32(dst + f, vmulq_f32(vs, vg));
	}
}

#endif /* MIXER_DSP_BLOCK_H */
