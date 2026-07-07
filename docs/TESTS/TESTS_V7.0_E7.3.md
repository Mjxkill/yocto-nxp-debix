# Test Fiche : V7.0 — E7.3 (Panels effets DSP complets — FX bus compressor/reverb/delay/EQ)

**Date** : 2026-05-11
**Statut** : **GO** — tous params modifiables en live
**Commits** :
- `edf6dd50 E7.3a: detail panel sidebar + routage exhaustif per-strip`
- `0d0f844f E7.3b: ALSA kcontrols proxy + per-strip effects panel + outputs clickable`

## Référentiel

| Champ | Valeur |
|---|---|
| Phase | V7.0 — E7.3 FX bus panels + per-strip detail sidebar |
| Préalable | E7.2 GO (GUI Apple-class + strip gain) |
| Backend | `mixer-pro` `op:set_fx_param`, `op:reset_fx`, `op:get_strip_routing` (déjà en place depuis E6.e) |
| Frontend | `mixer-gui-http` `index.html` rev E7.3 |

## Travaux exécutés

### E7.3a — Detail panel sidebar per-strip

- Sidebar `.detail-panel` qui s'ouvre au clic sur une voie d'entrée (M, U, P) ou de sortie (S, U, P)
- Routage exhaustif per-strip :
  - Pour input : 8 mini-faders L/R par bus FX × 4 (sends) + 18 mini-faders master (toward S1..S8, U1..U8, P1..P2)
  - Pour output : informationnel uniquement (kcontrols TAC en E7.4)
- Stéréo link entre voies paires/impaires (toggleStereoLink), routage symétrique automatique

### E7.3b — ALSA kcontrols proxy + outputs clickable

- Backend `mixer-gui-http` : nouvelles routes
  - `GET /api/alsa/contents` → exec `amixer contents`
  - `POST /api/alsa/set` → exec `amixer cset numid=N value`
- Frontend : section `EFFETS` dans la sidebar listant les kcontrols ALSA pertinents
  - Filtrage par strip courant (TAC0 pour M1/M2, TAC1 pour M3/M4, etc.)
  - Sliders auto-générés pour INTEGER/BOOLEAN/ENUMERATED
  - Placeholder pour BYTES (sera étendu en E7.4 avec biquads RBJ)
- Output strips deviennent clickables (avant : décoratifs only)

### FX bus panels (compressor / reverb / delay / EQ)

- Modal `.fxbus-panel` s'ouvre au clic sur "Edit effect ▸" d'une bus card
- 4 effets natifs C (déjà présents depuis E6.e) avec leurs paramètres exposés :
  - **Compressor** : threshold, ratio, attack, release, makeup
  - **Reverb** : size, damp, wet
  - **Delay** : time, feedback, wet
  - **EQ 3-band** : freq L/M/H, gain L/M/H, Q
- Sliders avec debounce 50 ms, `op:set_fx_param` envoyé live
- `fxParamSpec(type)` retourne le schéma de params par type d'effet
- `fxState[4]` mirror des paramètres actuels (initialisé par `get_state` au load)

## Tests T7.3.X — exécutés board

| Test | Résultat |
|---|---|
| T7.3.1 clic M1 ouvre sidebar | ✓ |
| T7.3.2 mini-fader send FX1 L → écho mixer-pro `set_send` | ✓ |
| T7.3.3 mini-fader master → S1 → audio change live | ✓ |
| T7.3.4 stereo link M1+M2 → routage miroir M2 automatique | ✓ |
| T7.3.5 sidebar EFFETS liste kcontrols TAC0 ADC | ✓ après ajout `amixer contents` proxy |
| T7.3.6 modal FX bus 1 (Compressor) ouvre avec 5 params | ✓ |
| T7.3.7 slider threshold compressor → audio compresse audiblement | ✓ |
| T7.3.8 reset bus FX 1 → params défaut restaurés | ✓ |
| T7.3.9 EQ band gain → balance fréquentielle audible | ✓ |
| T7.3.10 GUI version v7.0-e7.3b | ✓ |

## Validation utilisateur

> Validation perceptuelle effectuée — sliders réactifs, audio change immédiatement

→ **GO**.

## Limites connues à fin E7.3

- Les kcontrols `TYPE=BYTES` (biquads TAC, blobs DRC) ne sont pas encore éditables — traité en E7.4
- Pas encore d'inverse extraction pour les paramètres dérivés depuis les coefs binaires
- L'éditeur DRC est encore à l'état raw hex (non décodé) — Phase B en E7.4.c

## Suite

E7.4 = biquads RBJ programmables sur les 8 voies × 24 par TAC + paged-blob effets TAC + Phase B DRC editor + crossover éditable + layout grid + sidebar tabs.
