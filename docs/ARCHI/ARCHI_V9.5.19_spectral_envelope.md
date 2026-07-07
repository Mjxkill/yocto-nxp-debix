# ARCHI V9.5.19 — Mastering ML par enveloppe spectrale fine

**Date** : 2026-06-10
**Statut** : DESIGN — en attente de validation utilisateur
**Remplace** : approche "EQ 16 bandes ISO" (v5.14 → v5.18)

## 1. Motivation (constat utilisateur, écoute v5.18 ep5)

- L'EQ 16 bandes (Q=1, larges) ne peut PAS être propre : chaque boost ramasse
  tout le contenu de la bande, bruit compris → sifflement 14-16 kHz audible
  sur tout le morceau.
- Le master humain (target) est "propre" : basses reformées sans baver,
  l'excitation harmonique fait le travail HF plutôt qu'un boost EQ large.
- → Le modèle doit produire une **enveloppe de correction spectrale fine**
  (toutes les fréquences de la FFT), pas 16 boutons.

## 2. Architecture cible

```
                       ┌──────────────────────────────────────────┐
   audio (mono/canal)  │  ENCODER v3 (inchangé)                   │
   ──────────────────► │  Mel 44 + MFCC 20 + BF 16 + ΔMel 44 + RMS│
   fenêtre 100 ms      │  = 125 features × 10 trames              │
   hop 10 ms           └──────────────┬───────────────────────────┘
                                      ▼
                       ┌──────────────────────────────────────────┐
                       │  MODÈLE XXL_conv2d (~6.6M params)        │
                       │  sortie 74 = 64 enveloppe + 4 exciter    │
                       │             + 6 limiter                  │
                       └──────────────┬───────────────────────────┘
                                      ▼
            ┌─────────────────────────┼─────────────────────────┐
            ▼                         ▼                         ▼
   ENVELOPPE 64 pts          EXCITER (calibré)           LIMITER
   gains dB par bande        amount/drive/freq/ceil      th/ceil/at/rt/in/out
   log 20 Hz - 20 kHz
   range ±12 dB
            │
            ▼
   ┌────────────────────┐
   │ FIR 256 taps        │  ← recalculée à chaque refresh (100 Hz)
   │ phase linéaire      │     latence +2.67 ms (128 taps de group delay)
   │ (ifft de l'enveloppe│     lissage temporel des coefs (cross-fade)
   │  interpolée)        │
   └────────────────────┘
```

### 2.1 Sortie modèle : 74 valeurs

| Plage | Contenu | Range denorm |
|---|---|---|
| [0..63] | enveloppe spectrale, 64 bandes log 20 Hz → 20 kHz | ±12 dB |
| [64..67] | exciter : amount, drive, freq_hz, ceiling | inchangé v5.17 |
| [68..73] | limiter : th, ceil, at, rt, input (0..18), output | inchangé v5.17 |

L'EQ paramétrique 16 bandes DISPARAÎT (remplacé par l'enveloppe).
Le modèle reste MONO, appliqué par canal (2 invocations NPU / 10 ms).

### 2.2 Application board : FIR 256 taps phase linéaire

Choix : **FIR** plutôt qu'OLA-FFT (latence 21 ms ❌ vs budget < 10 ms)
et plutôt que biquads (interaction entre bandes, pas de phase linéaire).

- Construction : enveloppe 64 pts → interpolation log → réponse |H(f)| sur
  129 pts (rfft 256) → phase linéaire → ifft → fenêtre Hann → 256 taps.
- Recalcul à chaque refresh params (100 Hz). Cross-fade des coefficients
  sur 1 période (10 ms) pour éviter les clics (double FIR pendant le fade
  OU interpolation linéaire des taps — à trancher au bench).
- Coût CPU par canal : 256 taps × 48000 = 12.3 M MAC/s → NEON ~1.5 M cycles/s
  ≈ 0.1% CPU. Construction FIR (ifft 256) : 100 Hz × ~10 µs = négligeable.
- Latence ajoutée : (256/2)/48000 = **2.67 ms**. Budget total chaîne à
  vérifier vs < 10 ms NON-NÉGO.
- Implémentation mixer-pro : nouveau moteur natif `fx_spectral_env`
  (effects.c), lock-free comme para_eq_x16 : le control_thread écrit les
  64 gains cible, l'audio_thread interpole/reconstruit.

### 2.3 Training : surrogate PARFAITEMENT fidèle

Énorme avantage : le surrogate de l'enveloppe est un simple masque
fréquentiel — mathématiquement identique au FIR du board (à la résolution
près). Fini le mismatch surrogate/réel (cause racine n°2 du diag).

```python
# surrogate_spectral_env.py (différentiable)
X = rfft(x)                                # (B, n_bins)
H = interp_log(envelope_64, n_bins)        # gains linéaires par bin
y = irfft(X * H)
```

(Au training : application par bloc entier de 100 ms, phase zéro — la
différence avec le FIR 256 phase linéaire est négligeable pour la loss.)

### 2.4 Loss v5.19

Inchangée v5.18 (pondération fiabilité × air, tilt, Huber RMS, compression,
Mel, anti-collapse) SAUF :
- L'enveloppe étant fine, ajouter une **régularisation de douceur** :
  `L_smooth = mean((env[b+1] − env[b])²) / SCALE_SMOOTH`
  → évite les enveloppes en peigne (musical noise), favorise les courbes
  douces comme un vrai matching EQ.
- (option) pénalité L1 sur l'enveloppe → préfère ne rien faire que sur-corriger.

### 2.5 Anti-souffle silences (constat écoute : sifflement sur l'intro)

Gate applicatif côté daemon (PAS dans le modèle) :
```
si RMS fenêtre < -50 dBFS : params neutres (env=0 dB, exciter off,
limiter input=0) avec slew 100 ms
```
→ règle dure, robuste, 10 lignes. Le modèle garde son rôle sur le signal.

## 3. Chaîne board V5.19

```
cap → mix → [fx_spectral_env (FIR 256)] → [Calf Exciter] → [LSP Limiter] → out
              ↑ 64 gains @100 Hz            ↑ 4 params       ↑ 6 params
              └────────── daemon ml-inference (NPU, 2 canaux) ──────────┘
```

- StereoTools retiré (modèle mono par canal).
- Calf Exciter et LSP Limiter restent en LV2 (validés à 10 Hz ; à re-valider
  à 100 Hz de push — sinon réduire le refresh de leurs params à 10 Hz et
  garder 100 Hz pour l'enveloppe seule).

## 4. Plan d'exécution

| # | Étape | Durée estim |
|---|---|---|
| 1 | `surrogate_spectral_env.py` + test parité masque/FIR | 45 min |
| 2 | `model.py` : sortie 74, denorm v5_19 | 20 min |
| 3 | `train_v5_19.py` : loss + L_smooth, cache v3 réutilisé | 30 min |
| 4 | Smoke 3 paires → full 30 epochs | ~5h |
| 5 | Éval écoute 5 morceaux complets (streaming, FIR réel simulé) | 30 min |
| 6 | Si GO écoute : fx_spectral_env (effects.c) + ml_features.c v3 + daemon 74 params + export TFLite | 1-2 j |

## 5. Risques

| Risque | Mitigation |
|---|---|
| Musical noise (enveloppe qui zigzague dans le temps) | L_smooth + slew temporel des gains côté board (tau ~50 ms) |
| Latence +2.67 ms dépasse le budget total | mesurer la chaîne complète ; si besoin FIR 128 taps (+1.3 ms) |
| Enveloppe ±12 dB insuffisante (le master demande +14 dB d'air) | l'exciter complète ; sinon élargir à ±15 dB |
| 64 outputs d'enveloppe = mode collapse possible | anti-collapse loss déjà en place (L_mc) |
