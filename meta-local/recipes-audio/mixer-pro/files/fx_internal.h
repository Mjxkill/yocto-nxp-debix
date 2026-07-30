// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * fx_internal — helpers privés de la famille effects/ (V14.0 étape 5).
 */
#ifndef MIXER_FX_INTERNAL_H
#define MIXER_FX_INTERNAL_H

#include <math.h>

#define CLAMP(x, lo, hi) ((x) < (lo) ? (lo) : (x) > (hi) ? (hi) : (x))
#define DB2LIN(db)       expf((db) * 0.11512925f)   /* 10^(db/20) = exp(db * ln(10)/20) */

#endif /* MIXER_FX_INTERNAL_H */
