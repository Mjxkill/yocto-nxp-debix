# Plateforme Mastering Live Debix — Plan de travail

**Board** : Debix Model AB (NXP i.MX8MP, HiFi4 DSP + Vivante NPU 2.3 TOPS)
**Audio front-end** : 4× TAC5212 daisy-chain SAI7, TDM 8ch × 32-bit @ 48 kHz ASYNC
**OS** : Yocto 5.0 (L6.12.3), kernel 6.6.36-imx, userspace Ardour/JACK/Flutter
**Architecture DSP retenue** : **Archi C — Équilibrée** (validée 2026-04-21, voir `DSP_ARCHITECTURES.pdf`)
**Dernière mise à jour** : 2026-04-21

---

## 1. Objectif

Réaliser une plateforme de **mastering live "lite"** pilotée par le NPU :

- **Entrées** : 8 mics/lines (TAC5212) + 8 retours ASIO (USB1) + 2 retours iOS (USB2) = **18 sources**
- **Sorties** : 8 monitor/HP (TAC5212) + 8 envois ASIO (USB1) + 2 envois iOS (USB2) = **18 destinations**
- **Bus master stéréo** : multiband DRC + exciter + harmonizer + limiter, paramètres pilotés en temps réel par le NPU (~10 Hz update)
- **4 effets send** (Reverb / Chorus / Flanger / Delay) : **hors DSP**, dans JACK/LV2 sur Cortex-A53
- **Routage flexible** : matrice N×M interne DSP

## 2. Contraintes

| Contrainte | Valeur |
|---|---|
| Fréquence d'échantillonnage | 48 kHz fixe (pour l'instant) |
| Format | s32le 8 ch TDM côté SAI |
| Latence chemin principal (USB→DSP→USB aller-retour) | **≤ 10 ms** |
| Latence effets send | non critique (~20-50 ms acceptable) |
| DSP period SOF | 2 ms (pipeline) |
| Règle architecturale | **Minimiser le contenu du DSP** : routing + dynamics master + NPU tap uniquement |

## 3. Architecture macro

```
MICS ──► 4×TAC5212 ──► SAI7 8ch TDM ──►┐
                      (AGC, 3 biquads, HPF, gain/phase cal per-ch, DVC)
                                        │
   USB1 ret (8ch, DAW) ──────────────────┤
   USB2 ret (2ch, iOS) ──────────────────┤    ┌─── SOF DSP (HiFi4) ───┐
   FX returns 4×2ch (from A53 JACK) ─────┤    │  - Fine EQ per-input  │
                                        │    │  - Routing matrix     │
                                        ├───►│  - Master bus chain   │
                                        │    │    (mbdrc, vol, lim)  │
                                        │    │  - NPU tap            │
                                        │    └───┬──────┬──────┬─────┘
                                        │        │      │      │
   USB1 send (8ch to DAW) ◄──────────────┤       │      │      │
   USB2 send (2ch to iOS) ◄──────────────┤       │      │      │
   FX sends  4×2ch (to A53 JACK) ◄───────┘       │      │      │
                                                 ▼      ▼      ▼
                                            SAI7 out  NPU tap  (ALSA)
                                            (TAC DAC) (→ A53)
                                                 │
                                              4× TAC5212 DAC
                                                 │
                                            HP / monitors

A53 userspace :
  - JACK graph : FX sends → LV2 plugins → FX returns  (Phase 3)
  - NPU service : lit NPU tap, infère, écrit ALSA controls (Phase 4)
  - Flutter UI : lit/écrit ALSA controls, preset manager
  - USB gadget UAC2 : 8in/8out (USB1) + 2in/2out (USB2)  (Phase 2)
```

## 4. Phasage

### Phase 1 — Structure DSP minimale ⬅ **ICI**

- Topology SOF : 8 in + matrice N×M + master bus stéréo
- Configuration TAC5212 per-channel (AGC/biquads/gain-cal) via I²C au boot
- ALSA controls exposés pour chaque cellule de la matrice + params master
- Validation mix interne stéréo, NPU tap fonctionnel
- **Livrables** : 1 topology `.m4`, 1 service `tac5212-init`, 1 test bench

### Phase 2 — USB gadgets

- UAC2 8IN/8OUT (USB1 → ASIO DAW)
- UAC2 2IN/2OUT (USB2 → iOS)
- Bridges ALSA entre USB gadget et DSP (cible `alsaloop` ou bridge C custom selon latence mesurée)
- **État** : déjà testé dans une session précédente, à reconsolider

### Phase 3 — Effets send (A53 userspace)

- JACK2 + 4 plugins LV2 : Reverb / Chorus / Flanger / Delay
- Bridges ALSA : DSP FX-send captures → JACK inputs / JACK outputs → DSP FX-return playbacks
- Sélection plugins à faire parmi LV2 existants (calf, x42, dragonfly, etc.)
- **Trade-off** : +10-20 ms latence sur les sends, acceptable

### Phase 4 — NPU mastering

- Dataset + training modèle TFLite (from scratch, pas de modèle pré-existant)
- Input features : FFT master bus (2048 pts, fenêtre 20 ms) → loudness par bande, balance, crest, corrélation stéréo
- Output : gains EQ 8-10 bandes + seuils multiband DRC + amount exciter
- Service Python/C++ : lit NPU tap (ALSA capture), infère, écrit controls via `sof-ctl`
- Update rate ~10 Hz

### Phase 5 — Harmonizer + polish

- Harmonizer : `rubberband` LV2 en A53 (chain master A53 après DSP)
- Exciter : plugin LV2 ou module SOF custom selon les mesures de latence
- Presets, scenes, Flutter UI avancé

---

## 5. Détail Phase 1 — Structure DSP

### 5.1 Principe directeur

**Le DSP ne fait que ce qu'il DOIT faire en bas latence** :
1. Conditionnement fin des entrées (EQ paramétrique per-channel, corrige ce que TAC5212 ne fait pas)
2. **Matrice de routage** (crossbar mixin_mixout)
3. **Chaîne master stéréo** : multiband_drc + volume + limiter
4. **Tap NPU** sur le master post-chaîne

Tout le reste (effets send, harmonizer complexe, UI, analyse offline) est en A53.

### 5.2 Inventaire des PCMs (côté ALSA host = A53)

| PCM | Type | Ch | Rôle | Phase |
|---|---|---|---|---|
| `SAI_Capture` | capture | 8 | Mics raw post-TAC5212 → entre dans matrice | 1 |
| `SAI_Playback` | playback | 8 | Monitor/HP via TAC5212 DAC ← sort de matrice | 1 |
| `Master_Tap` | capture | 2 | Master post-chaîne, pour NPU + monitoring A53 | 1 |
| `USB1_Send` | capture | 8 | Sortie matrice vers USB gadget 1 (DAW) | 2 |
| `USB1_Ret` | playback | 8 | Retour USB gadget 1 dans matrice | 2 |
| `USB2_Send` | capture | 2 | Sortie matrice vers USB gadget 2 (iOS) | 2 |
| `USB2_Ret` | playback | 2 | Retour USB gadget 2 dans matrice | 2 |
| `FX1_Send` .. `FX4_Send` | capture | 2 ×4 | Sorties vers JACK pour reverb/chorus/flanger/delay | 3 |
| `FX1_Ret` .. `FX4_Ret` | playback | 2 ×4 | Retours depuis JACK | 3 |

**Phase 1 minimum** : juste les 3 premiers (SAI_Capture, SAI_Playback, Master_Tap). Les autres PCMs seront ajoutés incrémentalement en Phase 2 et 3 pour éviter d'exploser la topology d'un coup.

### 5.3 Pipelines SOF (Phase 1)

```
                     ┌────────────── SOF DSP ──────────────┐
                     │                                     │
  SAI7 RX (8ch) ────►│ PIPE_1 : SAI capture + fine EQ     │
                     │   DAI ─► buf ─► eq_iir ─► buf ─► mixin_A
                     │                                     │
   Host PCM0 (cap) ◄─│ PIPE_2 : dry mics to host (SAI_Capture)
                     │   mixin_A ─► buf ─► HOST             │
                     │                                     │
                     │ PIPE_3 : matrix bus (MASTER L+R)    │
                     │   mixin_A → mixout_M (2ch) → buf    │
                     │     → multiband_drc → volume → drc  │
                     │     → buf → mixin_B                  │
                     │                                     │
   Host PCM1 (cap) ◄─│ PIPE_4 : master tap to host (Master_Tap)
                     │   mixin_B → buf → HOST (2ch)        │
                     │                                     │
  SAI7 TX (8ch) ◄────│ PIPE_5 : SAI playback ← matrix out  │
                     │   mixin_B → spread 2→8 → buf → DAI  │
                     │   (mixin_A pour direct-outs aussi)  │
                     │                                     │
                     └─────────────────────────────────────┘
```

**Choix d'implémentation** :
- `mixin_mixout` (IPC4 API) plutôt que `mixer` legacy (IPC3), car permet vraiment N-to-M avec gain par connexion → idéal pour la matrice
- Si SOF Zephyr sur i.MX8MP ne supporte pas encore mixin_mixout en IPC3, **fallback** : plusieurs `mixer` en cascade. À vérifier avant le code.
- EQ IIR : 1 instance per-channel d'entrée (8 instances sur SAI capture). 3 biquads par défaut, coefficients modifiables via ALSA bytes control.

### 5.4 Chaîne master stéréo (détail)

```
Matrix bus L,R (2ch) ──► multiband_drc ──► volume ──► drc ──► output L,R
                        (3 bandes,         (DVC      (final
                        linked stereo)     gain)     limiter
                                                     ceiling
                                                     -0.1 dBFS)
```

| Composant | Kconfig | Paramètres exposés |
|---|---|---|
| `multiband_drc` | `COMP_MULTIBAND_DRC` (requires IIR+CROSSOVER+DRC) | bytes blob : crossover freqs, DRC params par bande |
| `volume` | (natif) | gain dB per-ch, soft-ramp |
| `drc` | `COMP_DRC` | bytes blob : threshold/ratio/attack/release |

Tous pilotables par ALSA (user-UI manuel OU NPU).

### 5.5 Configuration TAC5212 per-channel (I²C au boot)

Hors DSP, dans les 4 codecs TAC5212 (adresses 0x50-0x53) :

| Fonction | Registres | Valeur initiale Phase 1 |
|---|---|---|
| HPF ADC | P0_R114_D[5:4] | 12 Hz (0b10) |
| AGC | Section 8.2.12/13 | **désactivé** par défaut, activable par preset |
| 3 biquads ADC per-ch | P8/P9 register banks | flat par défaut |
| Gain calibration | P0_R83/R88/R93/R96 | 0.0 dB |
| Phase calibration | P0_R84/R89/R94/R97 | 0 cycles |
| DVC | P0_R82/R87/R91/R95 | 0 dB |

Service systemd **`tac5212-init.service`** au boot : applique les registres via `i2cset` ou un petit binaire C, à partir d'un fichier de preset `/etc/tac5212/default.conf`.

### 5.6 ALSA controls exposés (mapping NPU + UI)

Principe : **un control = un paramètre**, nommage explicite pour `amixer`.

| Control | Type | Plage | Action |
|---|---|---|---|
| `SAI In N EQ Band M Gain` | integer TLV dB | -24..+24 | gain biquad |
| `SAI In N EQ Band M Freq` | integer | 20..20000 Hz | freq centre |
| `SAI In N EQ Band M Q` | integer Q6.2 | 0.1..10 | Q |
| `Matrix Cell In=X Out=Y` | integer TLV dB | -inf..+12 | gain cellule |
| `Master MBDRC Band K Threshold` | integer TLV dB | -60..0 | seuil bande K |
| `Master MBDRC Band K Ratio` | integer Q8.8 | 1..20 | ratio |
| `Master Volume` | integer TLV dB | -inf..+6 | gain master |
| `Master Limiter Threshold` | integer TLV dB | -3..0 | plafond |

Format : `amixer -c softac5212tdm cset name='Master Volume' 0dB`

### 5.7 Livrables Phase 1

| Fichier | Où | Statut |
|---|---|---|
| `sof-imx8mp-tac5212-mix.m4` | `sof/tools/topology/topology1/` | à créer |
| `pipe-master-chain.m4` (custom local) | `meta-local/recipes-kernel/linux/files/` | à créer |
| `tac5212-init.c` + systemd unit | `meta-local/recipes-support/tac5212-init/` | à créer |
| `/etc/tac5212/default.conf` | config preset | à créer |
| Test bench `test-dsp-phase1.sh` | `meta-local/recipes-support/tests/` | à créer |

### 5.8 Critères de validation Phase 1

1. `aplay -D plughw:2,<USB1Ret> file.wav` → audible sur HP via master chain (si matrice USB1→Master activée)
2. `arecord -D plughw:2,<SAI_Capture>` → fichier WAV 8 ch contenant les mics secs
3. `arecord -D plughw:2,<Master_Tap>` → fichier stéréo post master-chain (ce qu'entend le NPU)
4. `amixer -c softac5212tdm` liste tous les controls attendus
5. Modifier un control pendant le stream → effet audible immédiat
6. Latence mesurée main path ≤ 10 ms
7. Pas de xrun sur 1h de lecture continue

### 5.9 Risques identifiés Phase 1

| Risque | Probabilité | Mitigation |
|---|---|---|
| ~~`mixin_mixout` pas dispo en IPC3~~ **CONFIRMÉ IPC3 utilisé** | n/a | **Utilisation du composant `mux` (IPC3-compatible) pour le merge master+monitor sur SAI TX** |
| Matrice N×M trop lourde pour 2 ms period | faible | bench CPU DSP, fallback pipeline 4 ms |
| Topology SOF trop complexe → IPC timeout au boot | moyenne | découpage incremental, tester après chaque ajout |
| TAC5212 AGC interfère avec chaîne DSP | faible | AGC désactivé par défaut Phase 1 |

### 5.10 Vérifications préalables confirmées (2026-04-21)

| Point | Preuve |
|---|---|
| IPC3 effectif sur notre build | default Kconfig, pas d'override dans `imx8mp_evk_mimx8ml8_adsp.conf` |
| `multiband_drc` bypass bit-perfect | `multiband_drc_default_pass` = `audio_stream_copy` (src/audio/multiband_drc/multiband_drc_generic.c:14) |
| `drc` bypass bit-perfect | `drc_default_pass` = idem (src/audio/drc/drc_generic.c:473) |
| Mixer legacy multi-channel | exige même ch count source/sink (src/audio/mixer/mixer_generic.c:109 mix_n_s32) |
| `mux` permet routage (in_stream, in_ch) → out_ch | src/audio/mux/mux.c lookup table, IPC3 compat |
| DAI RX partagé entre pipelines | pattern KWD : `PIPELINE_SCHED_COMP_N` + DAPM `dapm(PIPELINE_SINK_X, PIPELINE_SOURCE_Y)` |

### 5.11 Composants SOF mobilisés en Phase 1a

| Composant | Kconfig | Rôle |
|---|---|---|
| `eq_iir` | `COMP_IIR` | EQ fine 8 canaux sur la capture (flat par défaut) |
| `mux` | `COMP_MUX` | Merge 2ch master + 6ch monitor sur SAI TX 8ch |
| `multiband_drc` | `COMP_MULTIBAND_DRC` | Master bus (bypass bit-perfect dispo) |
| `volume` | (natif) | DVC master (0 dB bit-perfect) |
| `drc` | `COMP_DRC` | Final limiter (bypass bit-perfect dispo) |

---

## 6. Protocole de travail

Conformément à `CLAUDE.md` et `REGLES.md` :
- **Aucune modification de source SOF sans critic_analyze préalable**
- Une étape = un commit, validation utilisateur avant le suivant
- Jamais toucher `sof/tools/topology/topology1/sof/*.m4` upstream → nos pipelines custom vont dans `meta-local/`
- Backups systématiques avant tout changement risqué
- Build topology : `m4 -I . -I m4 -I common -I platform/common -I sof ...` puis `alsatplg`
- Deploy : scp vers `/lib/firmware/imx/sof-tplg/sof-imx8mp-tac5212.tplg` + `systemctl reboot`

---

## 7. Prochaine étape immédiate

→ **Valider cette structure DSP (section 5) avec l'utilisateur**
→ **Soumettre le design à `critic_analyze`**
→ **Itérer jusqu'à approbation**
→ **Démarrer l'implémentation Phase 1a** (topology minimale 3 PCMs : SAI_Capture, SAI_Playback, Master_Tap)
