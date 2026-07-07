# ARCHI V10-FX — Effets DSP 100 % par canal (multiband_drc)

Date : 2026-07-05 · Demande utilisateur : « fix firmware complet (option A) et
tous les paramètres de tous les effets du DSP par canal, pas globaux »

## Constat (mesuré board + code)

| Composant DSP | Params | enabled | Crossover | Emp/Deemp |
|---|---|---|---|---|
| DRC1.0 (D3, chemin generic¹) | par canal ✓ | par canal à re-vérifier² | — | — |
| MULTIBAND_DRC1.0 / 2.0 | par canal ✓ (E2) | **band-global lu sur ch0 ✗** | **global ✗** | global (non exposé GUI → reste global) |
| PGA1.0 / 2.0 | par canal ✓ | — | — | — |

¹ Build firmware sans `__XCC__` (zephyr-sdk gcc) → `SOF_MAX_XCHAL_HIFI=NONE`
→ `drc_generic.c` compilé (les chemins hifi3/hifi4 non-patchés D3 sont morts).
² `drc.c` documente un check per-channel « at gain-application level » — à
confirmer dans drc_generic pendant l'étape 1.

Bug utilisateur reproduit : blob multiband correct (ch0-1 OFF, 3 bandes) mais
le firmware lit `p_band_base[0].enabled` (« band-global, taken from ch 0 »),
et même OFF, la chaîne emphasis→crossover→somme→deemphasis reste active —
bouger XOVER modifie le son effet coupé.

## Cible

1. `enabled` **par canal** dans multiband : un canal OFF = passthrough (delay
   aligné) pour CE canal, les autres continuent.
2. Bypass **bit-transparent** du composant si TOUS les canaux de TOUTES les
   bandes sont OFF (plus d'emphasis/crossover du tout). NB : supprime aussi le
   pre-delay → changement d'alignement au toggle, documenté (pre_delay=0 chez
   nous en pratique, à vérifier au test).
3. **Crossover par canal** : chaque canal a ses fréquences de coupure.
   num_bands reste global (structurel). Emp/deemp reste global (non exposé).

## Blob V3 (extension par arithmétique de taille, pattern E2 éprouvé)

```
sof_multiband_drc_config (fixe, 244 o : size/num_bands/enable/reserved[8]/
                          emp[2]/deemp[2]/crossover[6] — devient le DÉFAUT global)
drc_coef[num_bands × 8]                    (E2, 88 o chacun)
xover_ch[8][6] sof_eq_iir_biquad           (V3, 8 × 168 o = 1344 o)   ← NOUVEAU
```
Détection : `trailing = size - header - num_bands×ppb×88` ; si
`trailing == nch×6×28` → crossover par canal, sinon (0) → global (V2 compat).

Tailles : 3 bandes = 3700 o ✓ ; 4 bandes = 4404 o → **bump 4096 → 6144** :
`SOF_MULTIBAND_DRC_MAX_BLOB_SIZE` (fw) + `CONTROLBYTES_MAX` (topology
pipe-multiband-drc-pga-8ch-capture.m4 + pipe play). Transport IPC3 : les puts
bytes sont fragmentés par le kernel (2468 o passent déjà) — test empirique
6 Ko à l'étape 2 avant toute suite.

## Firmware — points de patch

- `multiband_drc_generic.c` `multiband_drc_s16/s32_process_drc` : le check
  `p_band_base[0].enabled` descend DANS la boucle par canal
  (`DRC_PARAM_FOR_CH(...)->enabled`) pour detector/envelope/compress ;
  `state->processed` devient par canal (les state arrays D3 le sont déjà).
- `multiband_drc.c` : calcul `all_disabled` à la réception de config →
  process = default_pass (bypass transparent).
- Crossover : les états de filtres sont déjà par canal ; introduire un
  résolveur de coefs par canal (xover_ch[ch] si V3, sinon global).

## GUI / codec (étape 3, après validation firmware)

- stripfx.js + beta.html : parse/pack V3 (+ selfTest round-trip étendu) ;
  knobs XOVER opèrent sur le canal courant du drawer ; bouton ACTIF/OFF =
  canal courant sur TOUTES les bandes d'un geste.
- Rémanence : inchangée (miroir dsp-blobs + replay, tailles ≤ 6144).

## Séquence (une étape = build + déploiement + écoute + fiche)

1. **E1-fx** : enabled par canal + processed par canal + bypass transparent
   (blob V2 inchangé) → écoute A/B OFF par canal.
2. **E2-fx** : blob V3 crossover par canal + bump 6144 (fw + 2 topologies)
   → test write 6 Ko + écoute XOVER par canal (blob forgé à la main).
3. **E3-fx** : codec + 2 GUIs → test bout en bout + autotest round-trip.

## Invariants (NON-NÉGOCIABLES)

- DMA 2 ms + SCHEDULE_TIME_DOMAIN_DMA partout.
- Back-compat blobs V1/V2 (size-arithmetic, zéro flag magique).
- Déploiement firmware : cat .ri.xman + .ri, vérifier md5 dans dmesg.
- Toute étape sans amélioration empirique = revert immédiat.

## Diag

- Décodeur blob python (scan sof_drc_params) validé aujourd'hui sur blob 313.
- Écoute : tone générateur TAC possible en mire par canal.
