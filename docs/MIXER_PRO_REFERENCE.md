# MIXER-PRO — Documentation de référence complète

Console A.L.A. (Audio Live Assistant) by Electrosens R&D — daemon audio
temps réel de la table de mixage. Carte Debix Model AB (NXP i.MX 8M Plus),
Yocto Scarthgap, kernel 6.6.36-rt35.

Dernière mise à jour : 2026-07-08 (V13 — scènes).
Source : `meta-local/recipes-audio/mixer-pro/files/mixer-pro.c` (~5 800 lignes).

---

## 1. Vue d'ensemble

mixer-pro est le cœur audio de la console : un daemon C unique qui
capture, mixe, traite et restitue **26 sources vers 18 sorties** par
blocs de **96 frames (2 ms) à 48 kHz**, avec une latence one-way de
l'ordre de quelques millisecondes. Tout le traitement par tranche
(gate, compresseur, unmasking voix), les automatismes (Dugan, keeper,
voix devant), les sources internes (sampleur, loopstation, expandeur
MIDI) et les profils (scènes) vivent dans ce process.

```
                        ┌───────────────────────── mixer-pro ─────────────────────────┐
 8 mics (SAI7/TAC5212) ─┤ in 0-7   │                                                  │
 8 stems USB (gadget)  ─┤ in 8-15  │  gate → comp → voix-devant → sampleur/looper/    │
 2 tél (P1/P2, mortes) ─┤ in 16-17 │  synthé(+P1/P2) → automix/keeper → MATRICE       │─ out 0-7  DSP (SAI TX)
 8 returns FX          ─┤ in 18-25 │  sends 26×8 → 4 bus FX stéréo (LV2/natif) →      │─ out 8-15 USB (gadget)
                        │          │  returns → MASTER 26×18 → insert mastering (0/1) │─ out 16-17 tél
                        └──────────┴──────────────────────────────────────────────────┘
```

### Numérotation des tranches (« src »)

| Index | Source | Nom GUI |
|---|---|---|
| 0–7 | micros DSP (SAI7 ← TAC5212 ×4) | M1–M8 |
| 8–15 | stems USB (gadget UAC2 8×8, PC → console) | U1–U8 |
| 16–17 | téléphone (mortes en `--no-phone`) — reçoivent les **sources internes** : sampleur + loopstation + expandeur MIDI | P1–P2 |
| 18–25 | returns des 4 bus FX stéréo | R1–R8 |

### Sorties (« out »)

| Index | Destination |
|---|---|
| 0–7 | DSP → SAI TX → TAC5212 (sorties analogiques) ; 0/1 = master (insert mastering) |
| 8–15 | USB gadget (console → PC) |
| 16–17 | téléphone |

---

## 2. Threads et temps réel

| Thread | Prio/affinité | Rôle |
|---|---|---|
| **audio_thread** | SCHED_FIFO 99, cores isolés 2-3 (`isolcpus=2,3`) | capture → traitement → mix → restitution, cadencé par le PCM DSP (period 96) |
| **control_thread** | normal | socket UNIX `/run/mixer-pro.sock`, `poll()` multi-clients, protocole JSON ligne |
| **persistence_thread** | normal, 1 Hz | sauvegarde différée (dirty flag), tick bandmix (soundcheck/keeper), mmap ring expandeur MIDI |
| **play_thread** (E6.h) | RT | drainage ring USB play via eventfd |
| threads fx/worker LV2 | normal | plugins LV2 (worker/schedule) |

Règles RT absolues (appliquées partout) :
- **Aucune** allocation, I/O, ou transcendante superflue dans audio_thread.
- Traitement **par bloc** (96 frames), NEON `mac_block_n4` pour les matrices.
- Coefficients de filtres **précalculés** dans les handlers socket (control
  thread), jamais en RT.
- Les configs sont écrites SOUS `target_lock` ; audio_thread tient ce lock
  pendant toute la section mix (V9.3.1 — fix use-after-free LV2).
- Les gains vont dans des **targets** lissés par `smooth_gains()` (anti-zipper).
- Compteurs/meters publiés en **atomics** (relaxed), lus par le contrôle.
- `mlockall(MCL_CURRENT|MCL_FUTURE)`.

### Chaîne d'un bloc (audio_thread, ordre exact)

1. `snd_pcm_readi` DSP (8 ch S32) + pop ring USB cap + (tél)
2. lock `target_lock` → `smooth_gains()` (targets → gains, slew ; automix
   resp_ms ; keeper τ2 s)
3. convert S32→float par canal (remap mics `g_mic_map`)
4. `exp_render` — **gate/expandeur** par tranche (0-15), in-place
5. `cmp_render` — **compresseur** par tranche (0-15), in-place
6. `duck_render` — **« voix devant »** (unmasking spectral sidechainé)
7. `smp_render` — **sampleur** → P1/P2 (+=)
8. `loop_render` — **loopstation** → P1/P2 (+=), master peak
9. `midix_render` — **expandeur MIDI** (ring SHM) → P1/P2 (+=)
10. `automix_update` — cible Dugan par bloc
11. `mix_block` — A: sends 26×8 → bus ; B: FX bus (4× moteurs stéréo) ;
    C: master 26×18 (gain = fader × automix × keeper)
12. tap SHM NPU (in 8/9) pour mixer-ml-inference
13. insert mastering (chaîne LV2/native) sur out 0/1, si actif et non bypass
14. gains de sortie par strip (lissés) ; peaks/RMS publiés ; convert → écritures

---

## 3. Fonctions de traitement (détail)

### 3.1 Gate / expandeur par tranche (V12-EXP)

Downward expander sur les 16 voies réelles, pré-tout (sends, master,
looper, automix et tap voient le signal gaté).
- Params : `on`, `threshold_db` −80..0, `ratio` 1..20, `attack_ms`
  0.5..100, `release_ms` 5..1000, `range_db` 0..80, `hold_ms` 0..500.
- Enveloppe crête par bloc, coefs `ka/kr` précalculés, hold anti-chatter
  (granularité 1 bloc), rampe de gain linéaire intra-bloc, GR publié
  (milli-dB atomic). `off` = zéro coût.

### 3.2 Compresseur par tranche (V13-COMP)

Même patron que le gate, loi inverse : au-dessus du seuil,
`g_db = (thr − env_db) × (1 − 1/ratio)` + makeup.
- Params : `on`, `threshold_db` −60..0, `ratio` 1..20, `attack_ms`
  0.5..250, `release_ms` 5..2000, `makeup_db` 0..24.
- Ordre : gate → comp (console standard).

### 3.3 « Voix devant » (V13-VFOCUS — unmasking spectral)

Dynamic EQ sidechainé : la musique est creusée uniquement dans les bandes
où la voix a de l'énergie, uniquement quand elle chante.
- 5 bandes peaking RBJ fixes 250/500/1k/2k/4k Hz, Q 1,4.
- Sidechain = Σ tranches **rôle bandmix lead/choir** (post-fader) →
  5 passe-bande (coefs fixes) → enveloppes (att 5 ms/rel 180 ms) ;
  activité voix : large bande > −45 dBFS.
- Cut par bande = `max_cut × amount × (énergie bande / bande dominante)`,
  lissé 10/200 ms. Coefs peaking recalculés 1×/bloc (cos/sin précalculés),
  appliqués en cascade **partagée** à toutes les tranches rôle instrument.
- Params : `on`, `amount` 0-100, `max_cut_db` 0-12 (défaut 4,5).

### 3.4 Automix Dugan (V12-AMX — parole)

Partage de gain à budget constant (NOM=1) entre les tranches membres
(bouton « A ») : `part_i = env_i×poids_i / Σ`, gain amplitude = √part,
plancher `floor` (défaut −15 dB), jamais de boost.
- Énergie post-fader **et post-gate**, enveloppe att 10 ms/rel 200 ms.
- Params : `on` global, `resp_ms` 10-2000 (slew), `floor_db` −40..0,
  `weight_db` ±20 par tranche (priorité animateur).
- Usage : PAROLE multi-micros. Pour la musique → bandmix.

### 3.5 Auto-mix musique (V13-BANDMIX — assistant groupe)

Trois temps :
1. **Rôles** par tranche : lead/choir/kick/snare/drums/bass/guitar/keys/line/off.
2. **Soundcheck** : `bandmix_measure` 12 s par tranche (source seule) →
   rms_avg/peak/floor (EWMA τ3 s calculée par l'audio à 500 Hz — jamais
   d'échantillonnage aliasé).
3. **Calcul** : gain staging (rms→−20 dBFS, garde-crête −6), gate posé
   sur le floor mesuré, comp du rôle, faders = cibles de mix relatives
   (voix = 0 dB).
4. **Verrouillage** : capture 30 s des parts de loudness post-fader
   (retouches utilisateur incluses) = référence artistique.
5. **LIVE (keeper)** : correcteur 1 Hz en boucle fermée — parts courantes
   vs référence, zone morte ±1 dB, pas 0,5 dB/tick, **butées ±3 dB**,
   slew audio τ2 s, priorité voix, tranches en pause ignorées.
   `keeper_gain[]` est un trim séparé (jamais les faders).

### 3.6 Sources internes → P1/P2

- **Sampleur (V12-SMP)** : 16 slots WAV de `/var/lib/ala/samples`
  (48 kHz exigé, PCM16/24/32/f32, mono→dup, plafond 256 Mo), lecture
  one-shot, libération différée au reload.
- **Loopstation (V12-LOOP-PRO)** : 6 pistes indépendantes 40 s stéréo
  (88 Mo alloués au boot), horloge maître partagée (1ʳᵉ piste = longueur,
  suivantes alignées), source par piste (M1..USB8), mute/clear par piste,
  peak par piste (aussi en REC) + master. Un seul REC simultané.
- **Expandeur MIDI (V12-MIDIX)** : consumer d'un ring SHM `/ala-midix`
  produit par le daemon midi-expander (fluidsynth GM + moteur M1 maison) ;
  pop non-bloquant, zéros si absent, resync dérive, mmap par
  persistence_thread (1 Hz, invalidation au départ du daemon).

### 3.7 Bus FX et insert mastering

- 4 bus FX stéréo : sends 26×8 → moteur par bus (`set_fx_engine` : LV2
  via lilv ou natif), returns dans la matrice master.
- **Insert mastering** post-master sur out 0/1 : chaîne jusqu'à 8 plugins
  (`set_insert`, spec persisté), process in-place ; **bypass runtime**
  `set_insert_bypass` (chaîne conservée chaude) ; enveloppes ML exposées
  (mastering NPU — daemon mixer-ml-inference lit le tap SHM in 8/9).

### 3.8 Scènes (V13-SCENES)

6 profils complets, format = fichier mixer_state.
- `scene_save` → snapshot vers `/var/lib/mixer-pro/scenes/sceneN` (+ `.name`).
- `scene_recall` → **sans coupure** : fichier lu en mémoire → `fmemopen` →
  application sous `target_lock` (zéro I/O disque sous lock), gains via
  targets (glissement), insert chain ré-initiée hors lock si le spec diffère.
- Scène **complète** via gui-http `/api/scene/save|recall` : + TAC/PGA
  (`alsactl store/restore -f`), blobs DSP (replay), patches + canaux du
  synthé (cmd `reload` du midi-expander). Slot étendu : `sceneN.d/`
  (asound.state, dsp-blobs/, synth-patches.conf, midix-chans.conf).

---

## 4. Protocole socket (JSON ligne sur `/run/mixer-pro.sock`)

Une requête = une ligne JSON `{"op":"...", ...}` ; une réponse = une
ligne JSON `{"ok":true|false, ...}`. Multi-clients (poll). Les setters à
champs optionnels font des **updates partiels** (champ absent = inchangé).

### Mixage / routage
| Op | Champs | Rôle |
|---|---|---|
| `set_input_gain` | src, gain (lin 0..8) | fader de tranche (persisté) |
| `set_mute` | src, mute 0/1 | mute (bitmask) |
| `set_send` | in, bus 0-7, gain | départ vers bus FX (canal impair/pair) |
| `set_master` | in, out, gain | cellule de la matrice 26×18 |
| `set_fx_bus` | bus, gain | niveau de return |
| `get_strip_routing` | src/out | routage + gain + mute + sends d'une tranche |
| `set_output_gain` / `get_output_gain` | out, db | trim final par sortie (lissé) |
| `set_input_map` / `get_input_map` | map[8] | remap des slots TDM mics |
| `set_mute`… `reset` | — | reset global des gains |

### Dynamique / voix
| Op | Rôle |
|---|---|
| `set_expander` / `get_expander` | gate par tranche (updates partiels, gr_db live) |
| `set_comp` / `get_comp` | compresseur par tranche (idem) |
| `set_vfocus` / `get_vfocus` | voix devant (on/amount/max_cut ; active + cuts_db[5]) |

### Automatismes
| Op | Rôle |
|---|---|
| `set_automix` | src, on?, weight_db? (partiels) |
| `set_automix_cfg` | on?, resp_ms?, floor_db? |
| `get_automix` | état + members + gains_db + weights_db |
| `bandmix_role` | src, role (lead/choir/kick/…/line/off) |
| `bandmix_measure` | src (12 s auto-stop ; -1 = annuler) |
| `bandmix_calc` / `bandmix_lock` / `bandmix_live` | calcul / référence 30 s / keeper on-off |
| `bandmix_status` | live, ref_valid, measuring+elapsed, par tranche : role/done/rms/floor/keeper_db |

### Sources internes
| Op | Rôle |
|---|---|
| `sampler_list` / `sampler_trigger` / `sampler_stop` / `sampler_reload` | pads |
| `looper_track_ctl` | track, action rec/play/mute/unmute/clear |
| `looper_track_cfg` | track, src_a, src_b, gain_db |
| `looper_ctl` | play_all / stop_all / clear_all |
| `looper_status` | master_len/pos/run/master_peak + 6 pistes (state/len/muted/src/peak) |
| `get_midix` / `set_midix` | présence ring + underruns + peak / trim |
| `midix_ctl` | proxy → daemon midi-expander : cmd status/prog/gain/panic OU `line` passthrough (engine, inst_list, patch_list/get/set/save, reload) |

### FX / insert / mastering
| Op | Rôle |
|---|---|
| `set_fx_engine` / `set_fx_param` / `get_fx` / `reset_fx` | moteur + params des 4 bus |
| `list_lv2_plugins` | catalogue lilv (≈345 plugins, cat/ports) |
| `set_insert` / `get_insert` | chaîne mastering (spec engine+uri ×8) |
| `set_insert_param` / `set_insert_params_bulk` | params des plugins de la chaîne |
| `set_insert_bypass` / `get_insert_bypass` / `insert_bypass` | bypass runtime (bouton MASTERING) |
| `set_assistant_mode` / `get_assistant` | assistant mastering (source hw/passthrough) |

### Scènes / système / diags
| Op | Rôle |
|---|---|
| `scene_save` / `scene_recall` / `scene_list` | profils (slot 0-5, name) |
| `get_state` | version, frames, xrun, delays, latence, profils us (cap/mix/play/iter), ring |
| `get_meters` / `get_meters_lite` | peaks in[26]/out[18]/fx[8] (+analyzer FFT plein) |
| `get_taps` / `set_tap` | analyseur (voie k, points a/b) |
| `get_drift` / `apply_drift_as_shift` / `reset_drift_stats` | asservissement USB↔DSP (shift ppm) |
| `get_alsa` / `set_alsa` / `get_tac_reg` / `set_tac_reg` | accès ALSA/TAC bas niveau |

---

## 5. Persistance

| Fichier | Contenu | Écrit par |
|---|---|---|
| `/var/lib/mixer-pro/mixer_state` | état complet : version, insert spec, assistant, automix (cfg+membres+poids), mute_mask, input_gains[26], fx_bus[8], master[26×18], expander[16], comp[16], bandmix (rôles+réf+live), vfocus, sends[26×8] (V13.1) | persistence_thread (dirty, 1 s) + shutdown |
| `/var/lib/mixer-pro/scenes/sceneN{,.name,.d/}` | profils (même format + TAC/blobs/synthé dans `.d/`) | scene_save / gui-http |
| presets / mic_map / out_gain | fichiers dédiés V9.5.21 | idem |
| `/var/lib/mixer-pro/dsp-blobs/*.hex` | blobs BYTES DSP (miroir des écritures GUI) | gui-http |
| `/var/lib/ala/samples/` | WAV du sampleur | utilisateur |
| `/var/lib/ala/synth-patches.conf`, `midix-chans.conf` | banque M1 + assignations canaux | midi-expander |

⚠ Règle du parseur (leçon V12-AMX) : tout littéral `fscanf` qui suit une
boucle à compte exact DOIT commencer par `" "` (skip d'espace), et les
mots-clés à préfixe commun se parsent en `fgets`+`sscanf` (le littéral
`"bandmix"` avale le début de `"bandmix_live"`).

### Pièges d'API (campagne de validation 2026-07-08)

- **Gains linéaires partout** : `set_input_gain {"gain":0.5}`,
  `set_send {"gain":0.1}`, `set_master {"gain":...}` ;
  `get_output_gain` renvoie `gains` en millièmes (1000 = unité) mais
  `set_output_gain` prend `{"db":-6.0}`.
- `set_insert_bypass` prend `{"on":1}` (sémantique bouton MASTERING :
  on=1 → chaîne active, bypass=0).
- `set_automix` = adhésion **par tranche** (`src` requis) ;
  l'enable global est `set_automix_cfg {"on":1}`.
- Looper : la fin d'un enregistrement = action **`play`** sur la piste
  (fige la longueur ; piste maîtresse = définit `master_len`).
- `bandmix_measure` exige `src` et lance une mesure 12 s
  (`src:-1` = annuler).
- **Numérotation des cartes ALSA instable au boot** (softac5212tdm vue
  carte 4 puis 5) : toujours `-c softac5212tdm`, jamais un index.
- Au boot, le restore ALSA de la carte TAC peut échouer (carte/DSP pas
  prêts) : `ala-fx-restore.sh` fait retry + **vérification témoin** et
  garde une copie `asound.state.boot` (V13.1).

---

## 6. Écosystème (daemons et services autour)

| Service | Rôle | Lien avec mixer-pro |
|---|---|---|
| `usb-uac2-gadget` | gadget USB composite : UAC2 8×8 S32 48 kHz + **f_midi** (« A.L.A. MIDI ») | rings cap/play USB |
| `midi-expander` | fluidsynth (GM, GeneralUser GS) + **moteur M1 maison** (2 OSC PCM multisamples SF2, VDF sans résonance, VDA, EG ADBSSR, LFO, 16 voix, patches persistés) ; socket `/run/midi-expander.sock` (status/prog/gain/panic/engine/inst_list/patch_*/reload) | ring SHM `/ala-midix` → P1/P2 ; proxy `midix_ctl` |
| `anti-larsen` | détection larsen FFT 8192 sur le tap NPU, notches RBJ posés dans les biquads DAC TAC5212 ; socket status + **enable 0/1 runtime** | indépendant (agit sur le TAC) |
| `mixer-ml-inference` | mastering NPU (TFLite) — lit le tap SHM in 8/9 | enveloppes ML affichées (insert) |
| `mixer-gui-http` | REST/HTTP : `/api/cmd` (passthrough socket), `/api/state|meters|drift|sysload`, `/api/alsa/*`, `/api/dsp/blob/*`, `/api/larsen` (GET/POST), `/api/scene/save|recall`, sert `/beta` (web) | pont web + orchestration scènes complètes |
| `mixer-console` | console native Qt6/eglfs (LCD DSI 800×1280 portrait) — 10 pages | client socket direct + XHR gui-http |
| `ala-fx-restore` | au boot post tac-reset : `alsactl restore` + replay blobs (+ arg répertoire de scène) | TAC/DSP |
| `tac-reset` | reset TAC5212 obligatoire post-boot | — |

### GUI — pages (LCD et web `/beta`, parité totale)

MIXER (bancs de tranches, fader+meter, M/A, F1-F4 en grille 2×2, master),
EFFETS (bus FX), MASTERING (insert + assistant + spectre), PADS (sampleur),
LOOPER (6 pistes + VU + master), EXPANDEUR (16 canaux GM/M1 + éditeur de
patch M1), AUTO MIX (rôles/soundcheck/calc/lock/live + Dugan + voix
devant), SCÈNE (4 gros boutons actifs avec VU signal + master L/R + 6
profils nommés, rappel confirmé 2 taps), ROUTING, SYSTÈME (+ notches
anti-larsen live). Drawer d'effets par tranche (TAC + DSP + onglet GATE).

---

## 7. Constantes clés

| Constante | Valeur | Note |
|---|---|---|
| SAMPLE_RATE / PERIOD_FRAMES | 48 000 / 96 | bloc 2 ms |
| N_INPUT_REAL / N_INPUT_TOTAL | 18 / 26 | +8 returns |
| N_OUTPUT_TOTAL / N_BUS_FX_CH | 18 / 8 | 4 bus stéréo |
| N_EXP_CH | 16 | gate/comp/RMS/bandmix |
| LOOP_TRACKS × LOOP_MAX | 6 × 40 s | 88 Mo RAM |
| SMP_SLOTS / SMP_MAX_TOTAL | 16 / 256 Mo | sampleur |
| VF_BANDS | 5 | voix devant |
| SCENE_SLOTS | 6 | profils |
| FX_CHAIN_MAX | 8 | insert |

## 8. Diagnostic rapide

```sh
printf '{"op":"get_state"}\n'  | socat - UNIX-CONNECT:/run/mixer-pro.sock   # xrun/latence/profils
printf '{"op":"get_meters"}\n' | socat - UNIX-CONNECT:/run/mixer-pro.sock   # niveaux
journalctl -u mixer-pro -f                                                  # logs (mlog)
```
- xrun : delta entre relevés (pas la valeur brute — transitoires d'init).
- Après build : vérifier `md5sum` build vs board ET mtime binaire > mtime
  source (piège cwd/bitbake).
- Déploiement : `systemctl stop` avant scp (Text file busy), `cleansstate`
  jamais `cleanall`.
