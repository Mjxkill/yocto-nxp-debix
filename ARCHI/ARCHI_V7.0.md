# ARCHI V7.0 — DSP indépendant + NPU dual-tap (asymétrique)

**Date** : 2026-05-10
**Auteur** : Michael (cadrage), assisté Claude
**Statut** : DRAFT — à valider avant tout code
**Baseline** : E6.a, commit SOF `0580b5f14`

## 1. Pourquoi V7.0

V6.0 (passthrough firmware-only DAI-to-DAI sans PCM hôte) **abandonné** après 3 semaines : SOF est conçu host-centric, le `pipe_task` body ne s'exécute jamais sans HOST anchor (mailbox 0x570 = 0xFFFFFFFF empiriquement).

V7.0 **repart de E6.a** et change de cap :
- 2 pipelines DSP **strictement indépendantes** (chacune avec son PCM HOST anchor — modèle SOF natif)
- Routing/mixage **délégué à Linux userspace**
- 2 **NPU taps asymétriques** : un sur l'INPUT brut (cap), un sur l'OUTPUT post-effets (play)

## 2. Vue système end-to-end

```
mics ─┐                                                      ┌─ speakers
      │                                                      │
   SAI7 RX ──→ PIPE 1 cap (DSP) ──→ PCM 0 cap ─┐          ┌─ PCM 1 play ──→ PIPE 2 play (DSP) ──→ SAI7 TX
      │             │                          │          │                       │
      │             ↓                          │          │                       ↓
      │       TAP IN (raw)                     │          │                 TAP OUT (post-FX)
      │             │                          │          │                       │
      │             ↓                          │          │                       ↓
      │     reserved-mem tap-in                │          │              reserved-mem tap-out
      │             │                          │          │                       │
      │             ↓                          │          │                       ↓
      │     /dev/imx-audio-tap-in              │          │              /dev/imx-audio-tap-out
      │             ↓                          │          │                       ↓
      │            NPU                          ↓          ↑                     NPU
      │                                       Linux Mixer / Routing / Send FX
      │                                          ↑          ↓
      │                            USB gadget ───┤          ├─→ USB gadget   (8 × 8)
      │                            Téléphone ────┤          ├─→ Téléphone    (2 × 2)
```

## 3. Pipeline 1 — Capture (8 voies)

```
SAI7 RX 8ch
      ├──→ TAP IN (raw, hook dai_dma_cb) ──→ rmem tap-in ──→ /dev/imx-audio-tap-in
      ↓
multiband_drc (8ch, 8 indép, multi-blob)
      ↓
drc D3 (8ch, 8 indép, state arrays)
      ↓
pga (8ch, 8 vols indép)
      ↓
PCM HOST 0 cap (ASIO IN, 8ch S32_LE 48 kHz)  →  /dev/snd/pcmC2D0c
```

| Étage | Component SOF | Paramètres | Origine |
|---|---|---|---|
| Capture DAI | `DAI SAI7 RX` | TDM 8 × S32_LE × 48 kHz | E6.a |
| **TAP IN (raw)** | hook `dai_dma_cb` cap | rmem `tap-in`, signal mic non traité | V7.0 nouveau |
| Compression multibande | `multiband_drc` 8 ch | blob 8 × N (multi-config patché) | V7.0 nouveau |
| Compression dynamique | `drc` D3 patché | state arrays + multi-blob 8 × M | E5.e.2-D3 |
| Volume | `pga` 8 ch | 8 vol kcontrols indép | E5.e.2 step1 |
| HOST PCM | `PCM 0 cap` | 8 ch S32_LE 48 kHz | E6.a |

## 4. Pipeline 2 — Playback (8 voies)

```
PCM HOST 1 play (ASIO OUT, 8ch S32_LE 48 kHz)  ←  /dev/snd/pcmC2D1p
      ↓
multiband_drc (8ch, 8 indép, multi-blob)
      ↓
pga (8ch, 8 vols indép)
      ↓
drc (8ch, 8 indép, limiteur)
      ├──→ TAP OUT (post-FX, hook pre-DAI TX) ──→ rmem tap-out ──→ /dev/imx-audio-tap-out
      ↓
SAI7 TX 8ch (master, BCLK/FSYNC source)
```

| Étage | Component SOF | Paramètres | Origine |
|---|---|---|---|
| HOST PCM | `PCM 1 play` | 8 ch S32_LE 48 kHz | V7.0 simplifié vs E6.a |
| Compression multibande | `multiband_drc` 8 ch | blob 8 × N′ | V7.0 nouveau |
| Volume | `pga` 8 ch | 8 vol kcontrols indép | réutilisé |
| Limiteur | `drc` 8 ch | blob 8 × M′ | réutilisé |
| **TAP OUT (post-FX)** | hook post-strips, pre-DAI TX | rmem `tap-out`, signal speaker post-traité | V7.0 nouveau |
| Playback DAI | `DAI SAI7 TX` | TDM 8 × S32_LE × 48 kHz · master | E6.a |

**Suppressions vs E6.a** : `mixer16`, `deinterleave_8`, `interleave_8`, tous les buffers mono intermédiaires.

## 5. Effets TAC5212 dans la chaîne (NPU pilote tout)

Le TAC5212 n'est PAS un convertisseur transparent. Tous ses effets internes font partie de la chaîne audio V7.0, et le NPU doit pouvoir agir sur l'**ensemble unifié** (TAC + DSP). Source : datasheet SLASF23A § 7.1, p. 28.

**Chaîne audio complète V7.0** :

```
Capture :
mics → [TAC ADC : AGC → HPF → biquads → gain → decim] → SAI7 RX
     → [DSP cap : multiband_drc → drc D3 → pga] → PCM 0

Playback :
PCM 1 → [DSP play : multiband_drc → pga] → SAI7 TX
      → [TAC DAC : interp → biquads → DRC → gain → limiter+foldback] → speakers
```

> **Note V7.0-E3 final (2026-05-11)** : le `drc` limiteur final côté play a été retiré après diag T3.9. Les coeffs default (héritage musique) provoquaient un pattern « tic à l'attack » incompatible avec la voix temps réel ; la double compression (`multiband_drc` + `drc`) était redondante. Un vrai limiteur calibré voix pourra revenir en **E3.b** (futur sprint).

**Effets TAC5212 ADC (capture)** : AGC · HPF · Biquad filters par canal · Gain/Volume · Phase & gain calibration · Decimation filter (linear-phase / low-lat / ultra-low-lat) · Digital channel mixer · Mic bias programmable · PDM mic decimation (jusqu'à 4 mics PDM)

**Effets TAC5212 DAC (playback)** : Interpolation filter (linear-phase / low-lat / ultra-low-lat) · Biquad filters par canal · DRC · Gain/Volume · Distortion limiter · Thermal foldback · Battery guard (brown-out) · Tone generator · VAD (Voice Activity Detection) · UAD (Ultrasonic Activity Detection)

**Pilotage NPU** : le NPU calcule des consignes/coefficients, le GUI test V7.0 (E7) ou un orchestrateur userspace les applique aux kcontrols ALSA. Driver `tac5212.c` doit exposer ces effets en kcontrols (audit E0).

## 6. Devices Linux mixer

Le mixer Linux est connecté à 3 paires symétriques :

| Device | Channels | Direction | Source / sink | Usage |
|---|---|---|---|---|
| **DSP TAC5212** | 8 × 8 | cap + play | PCM 0 cap / PCM 1 play | Capture mics + restitution speakers locaux |
| **USB gadget audio** | 8 × 8 | cap + play | USB 2.0 audio class — vu comme carte son externe par PC host | I/O studio externe (DAW PC ↔ board) |
| **Téléphone** | 2 × 2 | cap + play | Modem voice / VoIP / PCM dédié | Communication voix entrante/sortante |

Le mixer ne fait **aucun appel au DSP**. Il prend N inputs (DSP cap, USB cap, phone cap), produit M outputs (DSP play, USB play, phone play), routing/gain/send FX au choix de l'app userspace.

## 7. Invariants stricts

| # | Invariant | Mémoire |
|---|---|---|
| I1 | TX = master, RX = slave, ASYNC, BCLK continu (FCONT=1) | `feedback_tx_master_drives_all.md` |
| I2 | DMA 2 ms, `SCHEDULE_TIME_DOMAIN_DMA` exclusivement | `feedback_dma_2ms_definitive.md` |
| I3 | 8 ch = 1 component multi-channel, jamais 8 instances | `feedback_pcm_8ch_pas_separes.md` |
| I4 | Pas de cross-pipeline DSP cap ↔ play | nouveau V7.0 |
| I5 | NPU taps présents en permanence (in + out) | `project_npu_non_negotiable.md` |
| I6 | tac-reset avant tout test capture | `tac_reset_required_after_boot.md` |
| I7 | period=2000 µs, S32_LE, 8 slots TDM, 48 kHz | E6.a baseline |
| I8 | Filename firmware = `sof-imx8m.ri` | `sof_firmware_deploy_path.md` |

## 8. Priorités V7.0

1. **Latence end-to-end < 10 ms** (mic → DSP cap → mixer Linux → DSP play → speaker) — non-négo
2. **GUI de test V7.0** (mixer + réglage de TOUS les effets DSP/TAC en direct) — livrable
3. **2 NPU taps** (in raw + out post-FX) — plomberie livrable, NPU consommateur en aval
4. **Ardour n'est PAS un objectif** — pas le DAW cible, pas dans l'image, pas optimisé pour

## 9. Roadmap

| Étape | Objectif | Critère GO |
|---|---|---|
| **E0** | Branche `feature/v7.0-multiband-drc-tap` créée depuis `0580b5f14`, doc V7.0 commit/push, **retrait `apply-v6-always-on.py`**, fiche E0 baseline E6.a re-vérifiée | boot OK, audio loopback OK, kernel sans patches V6.0 |
| **E1** | Topology simplifiée + ALSA low-lat Linux : retirer mixer16/deinterleave/interleave, PCM 1 → SAI7 TX direct, MMAP + SCHED_FIFO + mlockall | latence boucle ALSA mesurée < 10 ms, 0 xrun sur 60 s |
| **E2** | Pipe cap : remplacer `eq_iir` par `multiband_drc` (8 ch multi-blob, patch state arrays) | 8 multibandes indép, latence E1 préservée |
| **E3** | Pipe play : strips OUT `multiband_drc → pga` (8 ch indép) — drc final retiré après diag tic tic | 8 voies play indép, audio OK, latence préservée |
| **E4 ✓** | **Tap IN brut** (PIPE 1) : `apply-npu-tap-dt.py` refactoré 2 carves + 2 nodes, module multi-instance via prop DT `device-name`, hook firmware `dai_dma_cb` capture, `/dev/imx-audio-tap-in` + `/dev/imx-audio-tap-out` exposés | **GO 2026-05-11** — tap-in 1.22 MB/s, tap-out V3.2.2 non régressé, loopback E3 préservé |
| **E5 ✓** | **Tap OUT post-FX** (PIPE 2) : plomberie livrée en E4 (refactor dual-tap), validation empirique en E5 | **GO 2026-05-11** — modulation PGA Strip1 -40 dB visible exactement sur tap-out, tap-in/-out simultanés OK |
| **E6.a ✓** | USB gadget UAC2 8×8 isolé (configfs + systemd) | **GO 2026-05-11** — bidir validé : PC→board sine 880 -4.4 dBFS, board→PC sine 440 -6 dBFS sur 8 voies, format S32_LE 48kHz négocié high-speed, 0 régression DSP |
| **E6.b ✓** | Téléphone 2×2 (snd-aloop simulé, faute de modem hardware) | **GO 2026-05-11** — card 10 `Phone`, sine 1 kHz aloop bidir préservé -6 dBFS |
| **E6.c ✓** | Routing mixer N×M (alsa-route-bridge déclaratif via alsaloop) | **GO 2026-05-11** — route E2E DSP cap → UAC2 → PC `arecord` validée (4.6 MB/3 s, débit nominal). PipeWire dispo en réserve |
| **E7** | **GUI de test V7.0** : app Linux (Qt / Flutter / web) — mixer N×M visuel + sliders pour tous les paramètres effets DSP/TAC (kcontrols ALSA + SOF tplg) | tous effets pilotables en direct, audio reste < 10 ms |

## 10. Patches kernel — audit V6.0 → V7.0

| Patch / fichier | V7.0 | Action |
|---|---|---|
| `0001-imx8mp-evk-audio-mipi.patch` | Garder | Board hardware |
| `spdif.cfg`, `disable-at24.cfg` | Garder | Hors scope audio principal |
| SOF imx-probes (`imx-probes.c`, `apply-imx-probes.py`, `sof-imx-probes.cfg`) | Garder | Debug auxiliaire |
| TAC5212 (`tac5212.c/h`, `apply-tac5212-dt.py`, `tac5212.cfg`) | Obligatoire | Sans ça, pas d'audio |
| `apply-npu-tap-dt.py` (2 reserved-mem V7.0-E4) | **OK V7.0-E4** | `tap_in_buffer@94270000` + `tap_out_buffer@942B0000`, 2 nodes `imx_audio_tap_in/out` (prop `device-name`) |
| `apply-sdram2-dt.py` (8 MB DSP-only) | Évaluer | mixer16 retiré mais multiband_drc/drc blobs peuvent réclamer la mémoire ; confirmer E2/E3 |
| **`apply-v6-always-on.py`** (K0+K1+K2+K4+K5) | **RETIRER (E0)** | V6.0 only ; V7.0 a PCM HOST anchors → start standard ALSA |

## 11. Diag mailbox (`/sys/kernel/debug/sof/debug`)

| Adresse | Compteur | Sens |
|---|---|---|
| `0x500` | marker `0xCAFE0420` | sdma_set_config block ran |
| `0x504` | sdma_set_config calls | nb total per boot |
| `0x510-0x52C` | sdma_chan_type[0..7] | type DMA |
| `0x530-0x54C` | hw_event[0..7] | 12=SAI7 RX, 13=SAI7 TX |
| `0x550-0x56C` | direction[0..7] | sens DMA |
| `0x570 / 0x574` | pipe_task body counter | > 0 sur les 2 pipelines |
| `0x600-0x68C` | sdma_copy + dai_dma_cb | trafic DMA effectif |
| E4/E5 | nouveaux compteurs taps | adresses dédiées, jamais réutilisées |

## 12. Risques

| Risque | Probabilité | Mitigation |
|---|---|---|
| `multiband_drc` SOF stock pas multi-instance | Haute | Patch state arrays + multi-blob (modèle drc D3) |
| 2 NPU taps trop de bande passante DDR | Moyenne | Reserved-mem séparées, mesure SDMA bandwidth E5 |
| ALSA low-latency casse Ardour | Faible | Sprint dédié post-E6, isolation mlockall via systemd |
| `drc` cap + `drc` play simultanés — conflit ABI | Moyenne | Patch déjà multi-instance, re-tester sur 2 pipes |
| Tap brut + tap fx désynchronisés (jitter NPU) | Faible | Timestamps SOF dans rmem header |

## 13. Hors-scope V7.0

- **Send FX userspace avancés** (réverb, delay, sidechain inter-voies) — le GUI test V7.0 (E7) couvre seulement mixer + paramètres d'effets DSP/TAC
- E8 GUI v2 (FFT NPU + visualisations ML) — évolution post-V7.0 du GUI E7
- Code NPU (modèles TFLite/Vela)
- Ardour (jamais cible)
- V6.0 archive (branche `feature/v6-always-on-async` conservée)

## 14. Plan de développement complet

Le plan détaillé (8 étapes E0→E7, tableaux travaux + tests + critère GO + contenu type des fiches `TESTS_V7.0_E<n>.md`) est rendu en section 12 du PDF `ARCHI_V7.0.pdf`. Synthèse par étape :

| Étape | Tag git | Livrable | Fiche | Préalable |
|---|---|---|---|---|
| E0 | `v7.0-e0` | Branche + audit kernel + audit driver TAC + baseline E6.a | `TESTS_V7.0_E0.md` | doc V7.0 mergée |
| E1 | `v7.0-e1` | Topology simplifiée + ALSA low-lat < 10 ms | `TESTS_V7.0_E1.md` | E0 GO |
| E2 | `v7.0-e2` | multiband_drc CAP 8 ch indép | `TESTS_V7.0_E2.md` | E1 GO |
| E3 | `v7.0-e3` | Strips OUT play 8 ch | `TESTS_V7.0_E3.md` | E2 GO |
| E4 | `v7.0-e4` | Tap IN brut + /dev/imx-audio-tap-in | `TESTS_V7.0_E4.md` | E3 GO |
| E5 | `v7.0-e5` | Tap OUT post-FX + /dev/imx-audio-tap-out | `TESTS_V7.0_E5.md` | E4 GO |
| E6 | `v7.0-e6` | USB gadget 8×8 + téléphone 2×2 | `TESTS_V7.0_E6.md` | E5 GO |
| E7 | `v7.0-e7` | GUI test V7.0 (mixer + tous effets) | `TESTS_V7.0_E7.md` | E6 GO |

**Règle d'or** : aucune étape ne passe à la suivante tant que **tous les tests Tn.X = OK** ET **« Test utilisateur OUI »**. NO-GO = retour analyse + investigation critic, pas de saut.

**Contenu type des fiches** : en-tête (date / statut / commit / md5), référentiel, travaux exécutés, build & deploy, tests automatisés (Tn.X), logs & mailbox, mesure latence, test utilisateur OUI/NON, régression vs étape précédente, conclusion GO/NO-GO, annexes.

## 15. Décision de branchage

| Repo | Source | Nouvelle branche V7.0 |
|---|---|---|
| `sof/` | `feature/audio-platform-v2` @ `0580b5f14` | `feature/v7.0-multiband-drc-tap` |
| `yocto-nxp-debix/` | `feature/audio-platform-v2` (HEAD courant) | `feature/v7.0-multiband-drc-tap` |
| `meta-local/` | idem yocto | idem yocto |
