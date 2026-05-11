# Test Fiche : V7.0 — E7.2 (GUI premium Apple-class + strip gain + SAI TX FIFO alignment fix)

**Date** : 2026-05-12
**Statut** : **GO** — RX OK, TX aligné (slot N = ch_N sur 8 slots), validation board confirmée
**Commits** :
- SOF : `4bea8e59d` (branche `feature/v7.0-multiband-drc-tap`, fix `sai.c`)
- meta-local mixer-pro + mixer-gui-http : `e3f3d7f6` (branche `feature/audio-platform-v2`)
- meta-local audio-tools : `3b474ae6` (tap_channel_monitor.py)

## Référentiel

| Champ | Valeur |
|---|---|
| Phase | V7.0 — E7.2 GUI Apple-class redesign + strip gain + SAI TX FIFO align fix |
| Préalable | E7.1 (peak meters + SSE 30 Hz) commit `d055b946` |
| SOF firmware | md5 `743e2e249cc3ea6a291b975a614514f0` (post-fix `4bea8e59d`) |
| mixer-pro version | `v7.0-e7.2` |
| Topology | `sof-imx8mp-tac5212.tplg` (V7.0 commit `a535405ca`) |

## Travaux exécutés

### 1. mixer-pro `v7.0-e7m → v7.0-e7.2`

- Nouveau champ `input_gain[N_INPUT_TOTAL]` + `input_target[N_INPUT_TOTAL]` (26
  inputs : 18 réels + 8 returns) dans struct mixer_state
- Nouvelle op JSON `set_input_gain {src, gain}` (lock atomic)
- Smoothing 64-frame ramp partagé avec master/sends (`smooth_gains()`)
- Appliqué dans `mix_frame()` step 1 (sends) ET step 3 (master matrix)
  → comportement DAW post-fader
- Init à 1.0 (unity) au boot + sur op `reset`
- Doc protocole JSON mise à jour en tête `mixer-pro.c`

### 2. mixer-gui-http GUI E7.2 redesign

- **Drop Tailwind**, custom CSS dark glassmorphism (rgba semi-transparent +
  backdrop-filter blur 14 px), Inter font (Google Fonts CDN), tabular numerals
- **6 sections groupées** en grid responsive minmax 440 px :
  - DSP MICS ×8 (M1–M8)
  - USB ASIO IN ×8 (U1–U8)
  - PHONE ×2 (P1–P2)
  - RETURNS FX ×8 (R1–R8 avec sublabel FX1L/1R/2L/…)
  - BUS FX SEND ×4 stéréo (knobs SVG placeholder pour E7.3)
  - OUTPUTS ×18 (DSP play / USB ASIO out / Phone out, sous-groupes)
- **Faders verticaux** 140 px : thumb métallique brossé (gradient gris) +
  groove + tick d'unité 0 dB ; meter side-by-side gradient
  vert→jaune→orange→rouge
- Header sticky avec version pill + frames/xrun/lat/iter + pills
  Connected/SSE 30Hz
- Fader callback : `set_input_gain` (au lieu de `set_master out=0`) →
  contrôle uniformément toutes les routes de l'input
- Transitions ease 180 ms cubic-bezier

### 3. tap_channel_monitor.py (diag tool)

- Lit `/dev/imx-audio-tap-out` (mmap shared ring écrit par firmware SOF
  `dai-legacy.c:174-219` à chaque dma_cb du DAI playback)
- Calcule RMS dBFS par canal sur fenêtre 100 ms (4800 frames @ 48 kHz)
- Affiche `active=[i,j,...]` pour les canaux > seuil (-50 dBFS par défaut)
- Permet d'isoler DSP-side routing (TAP) vs SAI/DMA aval

### 4. SOF firmware `sai.c` TX FIFO alignment fix (commit `4bea8e59d`)

**Cause racine du décalage TDM TX intermittent** :
`sai_set_config` primait 1 seul zéro dans la TX FIFO avant `TERE=1`. SAI
clocke 1 zéro puis underflow tous les slots 1..7, slot counter ↔ FIFO read
pointer phase indéterminée jusqu'au 1er FRDE=1 → décalage permanent.

**Patch** : prime `tdm_slots` zéros (= 1 frame TDM complète = 8 zéros pour
TAC5212) AVANT TERE=1.

```c
- dai_write(dai, REG_SAI_TDR0, 0x0);
+ for (i_prime = 0; i_prime < sai->params.tdm_slots; i_prime++)
+     dai_write(dai, REG_SAI_TDR0, 0x0);
```

### 5. Tentative ratée (revert)

Patch initial sur `sai_start` path `configured=true` (TCSR.FR + re-prime +
FRDE) a causé un side-effect inattendu sur RX FIFO (décalage +3 slots).
Hypothèse : `CSR_FR` bit ou `dai_write(TDR0)` pendant TERE actif perturbe
état SAI partagé. **Reverted** ; fix limité à `sai_set_config` (boot-time
seulement, ne touche pas le path runtime).

## Tests T7.2.X — résultats board

| Test | Description | Cible | Résultat |
|---|---|---|---|
| T7.2.1 | Build SOF + sign | Reef magic, md5 change | ✓ `743e2e24` |
| T7.2.2 | Boot DSP firmware | dmesg "Firmware info: version" | ✓ `2:10:0-6ee84` |
| T7.2.3 | mixer-pro v7.0-e7.2 boot OK | xrun=0 init | ✓ |
| T7.2.4 | RX : mic1+mic2 → in[0]+in[1] | TAC0 slots 0+1 visibles | ✓ `in[0]=43704, in[1]=45148` |
| T7.2.5 | TX : voice 1 → S1, voice 2 → S2 | identity 0→0+1→1 | ✓ **user confirmé** |
| T7.2.6 | TX sweep `out=N` N=0..7 | tap_channel_monitor active=[N] | ✓ DSP-side routing OK |
| T7.2.7 | TX restart mixer-pro 5× | alignment stable | ✓ pas de regression |
| T7.2.8 | set_input_gain src=1 gain=0.1 | out[1] = in[1] × 0.1 | ✓ `in[1]=33161, out[1]=3763` |
| T7.2.9 | GUI render | 6 sections grouped, faders verticaux, meters animés | ✓ |
| T7.2.10 | GUI fader M2 | contrôle volume S2 (route in[1]→out[1]) | ✓ |

## Validation utilisateur

> User 2026-05-12 : "oui voix 1 sur S1 et voix 2 sur S2 sans décalage"

→ **GO**.

## Bénéfices E7.2 vs E7.1

| Métrique | E7.1 | E7.2 |
|---|---|---|
| Layout | strips horizontaux plats | 6 sections groupées (DSP/ASIO/Phone/Returns/FX/Outputs) |
| Theme | flat | dark glassmorphism + Inter font |
| Faders | range horizontal | verticaux 140 px thumb métallique + tick 0 dB |
| Strip gain | `set_master out=0` (bug : ne contrôle que out 0) | `set_input_gain` (toutes routes uniformes, DAW correct) |
| TDM TX wire | décalage intermittent slot 1=ch7 | aligné slot N=ch_N déterministe |

## Suite

- **E7.3** : panels effets DSP complets (params compressor/reverb/delay/EQ)
- **E7.4** : effets TAC5212 via kcontrols ALSA
- **E7.5** : spectre + phase FFT canvas
