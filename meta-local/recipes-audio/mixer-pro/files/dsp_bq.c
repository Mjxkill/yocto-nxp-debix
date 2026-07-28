// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * dsp_bq — biquads RBJ du moteur : calcul de coefficients (voir dsp_bq.h).
 * Code déplacé tel quel depuis mixer-pro.c (V14.0 étape 0, extraction pure).
 */
#include <math.h>

#include "mixer-pro.h"   /* SAMPLE_RATE */
#include "dsp_bq.h"

void eqx_hpf(struct eqx_bq *q, float fc)
{
	if (fc <= 0.0f) { *q = (struct eqx_bq){ 1, 0, 0, 0, 0 }; return; }
	float w = 2.0f * (float)M_PI * fc / (float)SAMPLE_RATE;
	float cw = cosf(w), sw = sinf(w), al = sw / (2.0f * 0.707f);
	float a0 = 1.0f + al;
	q->b0 = (1.0f + cw) / 2.0f / a0;  q->b1 = -(1.0f + cw) / a0;
	q->b2 = (1.0f + cw) / 2.0f / a0;
	q->a1 = -2.0f * cw / a0;          q->a2 = (1.0f - al) / a0;
}

void rbj_peak_core(struct eqx_bq *q, float cw, float al, float A)
{
	float a0 = 1.0f + al / A;
	q->b0 = (1.0f + al * A) / a0;  q->b1 = -2.0f * cw / a0;
	q->b2 = (1.0f - al * A) / a0;
	q->a1 = -2.0f * cw / a0;        q->a2 = (1.0f - al / A) / a0;
}

void eqx_peak(struct eqx_bq *q, float fc, float gdb, float Q)
{
	if (fc <= 0.0f || gdb == 0.0f) { *q = (struct eqx_bq){ 1, 0, 0, 0, 0 }; return; }
	float A = powf(10.0f, gdb / 40.0f);
	float w = 2.0f * (float)M_PI * fc / (float)SAMPLE_RATE;
	rbj_peak_core(q, cosf(w), sinf(w) / (2.0f * Q), A);
}

void meq_shelf(struct eqx_bq *q, float fc, float gdb, int high)
{
	if (fc <= 0.0f || gdb == 0.0f) { *q = (struct eqx_bq){ 1, 0, 0, 0, 0 }; return; }
	float A  = powf(10.0f, gdb / 40.0f);
	float w  = 2.0f * (float)M_PI * fc / (float)SAMPLE_RATE;
	float cw = cosf(w), sw = sinf(w);
	float al = sw * 0.5f * 1.41421356f;          /* S=1 → alpha = sw/2·√2 */
	float tsa = 2.0f * sqrtf(A) * al;
	float ap1 = A + 1.0f, am1 = A - 1.0f;
	float b0, b1, b2, a0, a1, a2;
	if (high) {
		b0 =  A * (ap1 + am1 * cw + tsa);
		b1 = -2.0f * A * (am1 + ap1 * cw);
		b2 =  A * (ap1 + am1 * cw - tsa);
		a0 =        ap1 - am1 * cw + tsa;
		a1 =  2.0f * (am1 - ap1 * cw);
		a2 =        ap1 - am1 * cw - tsa;
	} else {
		b0 =  A * (ap1 - am1 * cw + tsa);
		b1 =  2.0f * A * (am1 - ap1 * cw);
		b2 =  A * (ap1 - am1 * cw - tsa);
		a0 =        ap1 + am1 * cw + tsa;
		a1 = -2.0f * (am1 + ap1 * cw);
		a2 =        ap1 + am1 * cw - tsa;
	}
	q->b0 = b0 / a0; q->b1 = b1 / a0; q->b2 = b2 / a0;
	q->a1 = a1 / a0; q->a2 = a2 / a0;
}
