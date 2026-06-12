#!/usr/bin/env python3
"""Génère ml_features_v3_tables.h depuis features_v3.py (parité C/Python).

Tables exportées :
  - MEL44 : bin ranges (start, count) du filterbank court terme (FFT 1024)
  - MELBF : bin ranges du filterbank BF (FFT 8192)
  - DCT   : matrice 20×44
  - NORM  : carto de normalisation 125 (calculée sur bruit rose seedé — non
            recalculable en C)
"""
import sys
import numpy as np
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
import features_v3 as fv3

OUT = Path('/home/michael/yocto-nxp-debix/meta-local/recipes-audio/mixer-ml-inference/files/ml_features_v3_tables.h')

def ranges_from_fb(fb):
    """Filterbank rectangulaire → (start, count) par bande. Vérifie contiguïté."""
    out = []
    for m in range(fb.shape[0]):
        nz = np.nonzero(fb[m])[0]
        if len(nz) == 0:
            out.append((0, 0))
            continue
        start, end = nz[0], nz[-1]
        assert np.all(nz == np.arange(start, end + 1)), f'bande {m} non contiguë'
        # poids uniforme 1/count vérifié
        assert np.allclose(fb[m, nz], 1.0 / len(nz)), f'bande {m} poids non uniforme'
        out.append((int(start), int(len(nz))))
    return out

mel44 = ranges_from_fb(fv3._MEL_SHORT)
melbf = ranges_from_fb(fv3._MEL_BF)

# Force le calcul de la carto NORM
_ = fv3.compute_features_v3(np.zeros(48000, dtype=np.float32))
norm = fv3._NORM
assert norm is not None and len(norm) == fv3.N_FEATURES_V3

dct = fv3._DCT   # (20, 44)

with open(OUT, 'w') as f:
    f.write('/* AUTO-GÉNÉRÉ par gen_ml_features_v3_tables.py — NE PAS ÉDITER.\n')
    f.write(' * Tables de parité avec training/features_v3.py (V5.20).\n */\n')
    f.write('#ifndef ML_FEATURES_V3_TABLES_H\n#define ML_FEATURES_V3_TABLES_H\n\n')
    f.write(f'#define MLF3_N_MEL_SHORT {fv3.N_MEL_SHORT}\n')
    f.write(f'#define MLF3_N_MFCC      {fv3.N_MFCC}\n')
    f.write(f'#define MLF3_N_BF        {fv3.N_BF}\n')
    f.write(f'#define MLF3_N_FEATURES  {fv3.N_FEATURES_V3}\n')
    f.write(f'#define MLF3_FRAME_SIZE  {fv3.FRAME_SIZE}\n')
    f.write(f'#define MLF3_FFT_SHORT   {fv3.FFT_SHORT}\n')
    f.write(f'#define MLF3_FFT_LONG    {fv3.FFT_LONG}\n')
    f.write(f'#define MLF3_WIN_LONG    {fv3.WIN_LONG}\n\n')

    f.write('static const int MLF3_MEL44_START[MLF3_N_MEL_SHORT] = {\n    ')
    f.write(', '.join(str(s) for s, c in mel44))
    f.write('\n};\n')
    f.write('static const int MLF3_MEL44_COUNT[MLF3_N_MEL_SHORT] = {\n    ')
    f.write(', '.join(str(c) for s, c in mel44))
    f.write('\n};\n\n')

    f.write('static const int MLF3_MELBF_START[MLF3_N_BF] = {\n    ')
    f.write(', '.join(str(s) for s, c in melbf))
    f.write('\n};\n')
    f.write('static const int MLF3_MELBF_COUNT[MLF3_N_BF] = {\n    ')
    f.write(', '.join(str(c) for s, c in melbf))
    f.write('\n};\n\n')

    f.write('static const float MLF3_DCT[MLF3_N_MFCC][MLF3_N_MEL_SHORT] = {\n')
    for k in range(dct.shape[0]):
        f.write('    {' + ', '.join(f'{v:.9e}f' for v in dct[k]) + '},\n')
    f.write('};\n\n')

    f.write('static const float MLF3_NORM[MLF3_N_FEATURES] = {\n    ')
    f.write(',\n    '.join(', '.join(f'{v:.9e}f' for v in norm[i:i+4]) for i in range(0, len(norm), 4)))
    f.write('\n};\n\n#endif /* ML_FEATURES_V3_TABLES_H */\n')

print(f'→ {OUT}')
print(f'  MEL44 : {len(mel44)} bandes, bins {mel44[0]} .. {mel44[-1]}')
print(f'  MELBF : {len(melbf)} bandes, bins {melbf[0]} .. {melbf[-1]}')
print(f'  NORM  : {len(norm)} valeurs')
