# Revue de Code — Plateforme A.L.A. Debix (Yocto-nxp-debix)

**Date de révision** : 2026-07-28  
**Branche active** : `L6.6.36-2.1.0-debix_model_ab` (HEAD `19b46b1b`)  
**Branche SOF** : `feature/v7.0-multiband-drc-tap` (HEAD `356b490cd`)  
**Scope** : `meta-local/`, `meta-local/recipes-kernel/linux/files/`, `sof/`, `docs/`  
**Méthode** : Analyse statique + lecture git log + comparison specs/archi/test fiches  

---

## TABLE DES MATIÈRES

1. [Architecture & structure du dépôt](#1-architecture--structure-du-dépôt)
2. [Recettes Yocto par domaine](#2-recettes-yocto-par-domaine)
   - [2.1 Kernel (linux-imx bbappend)](#21-kernel-linux-imx-bbappend)
   - [2.2 U-Boot / imx-boot / flash.bin](#22-u-boot--imx-boot--flashbin)
   - [2.3 Module NPU tap (imx-audio-tap)](#23-module-npu-tap-imx-audio-tap)
   - [2.4 TAC5212 driver + reset](#24-tac5212-driver--reset)
   - [2.5 mixer-pro daemon (recette + code)](#25-mixer-pro-daemon-recette--code)
   - [2.6 Services systemd (mixer-pro, ML, anti-larsen, IRQ RT, etc.)](#26-services-systemd)
   - [2.7 mixer-gui-http + ala-fx-restore](#27-mixer-gui-http--ala-fx-restore)
   - [2.8 Console native Qt6 (mixer-console)](#28-console-native-qt6-mixer-console)
   - [2.9 Flutter/Dart SDK](#29-flutterdart-sdk)
   - [2.10 Plugins LV2 + Ardour](#210-plugins-lv2--ardour)
   - [2.11 BSP services (USB gadget, snd-aloop, alsa-route-bridge, boot-script)](#211-bsp-services)
3. [SOF Firmware (sof/ fork)](#3-sof-firmware-sof-fork)
4. [Patches kernel & topology](#4-patches-kernel--topology)
5. [Fichiers sources kernel](#5-fichiers-sources-kernel)
6. [Code mixer-pro.c (6961 lignes)](#6-code-mixerproc-6961-lignes)
7. [Documentation & traçabilité](#7-documentation--traçabilité)
8. [Findings critiques (priorités)](#8-findings-critiques-priorités)

---

## 1. Architecture & Structure du Dépôt

### 1.1 Topologie git

```
yocto-nxp-debix/
├── meta-local/                    [CUSTOM LAYER — code projet, 47 recettes]
├── sources/                       [UPSTREAM LAYERS — lecture seule, ~40 layers]
├── Model_AB_Infinity/             [BUILD DIR — non versionné]
├── sof/                           [FORK DSP — non versionné, ~50 commits en avant 4bea8e59d]
├── docs/                          [Documentation maître : ARCHI/, TESTS/, plans]
├── AGENTS.md / CLAUDE.md / REGLES.md
└── build.sh / imx-setup-release.sh
```

- **Branche active** : `L6.6.36-2.1.0-debix_model_ab` (HEAD `19b46b1b`, 1 commit depuis 6da9847e/V13.9)
- **Upstream principal** : `origin/L6.12.3-debix_model_ab` (plus récent, commit `886877f7`, ~32 commits en avance)
- **Feature** : `feature/v7.0-multiband-drc-tap` (1 commit derrière, `b8593028`)
- **417 commits** depuis 2025-01-01, **274 dans `meta-local/`**, **147 depuis 2026-07-01**
- **1 seul submodule** : `sources/meta-musicians`

### 1.2 Configuration build

- `Model_AB_Infinity/conf/local.conf` : `MACHINE=imx8mpevk`, `DISTRO=fsl-imx-xwayland`, `PACKAGE_CLASSES=package_deb`
- ✅ Workaround tar 1.35 documenté (`ASSUME_PROVIDED:remove = "tar-native"`)
- `Model_AB_Infinity/conf/bblayers.conf` : ~40 layers upstream + `meta-local` (priorité 1)
- ⚠️ `meta-browser` (chromium) **requis par mixer-kiosk mais NON dans bblayers.conf** — ajout manuel requis (CLAUDE.md §10-P4b)

### 1.3 Conventions

- ✅ BitBake : `UPPER_SNAKE_CASE`, fonctions `lower_snake_case`, indent 4 spaces
- ✅ Recettes : `<package>_<version>.bb`, overrides `<package>_%.bbappend`
- ✅ Patches dans `files/` + `SRC_URI += "file://..."`

---

## 2. Recettes Yocto par Domaine

### 2.1 Kernel (linux-imx bbappend)

**Fichier** : `meta-local/recipes-kernel/linux/linux-imx_%.bbappend` (147 lines)

#### Stratégie de patching

Applique les patches via **2 mécanismes mixtes** :
1. `SRC_URI += "file://..."` pour patches classiques (PREEMPT_RT, config fragments)
2. **Scripts Python d'application DT** (`apply-*.py`) exécutés dans `do_patch:append()` / `do_configure:append()`

#### 2.1.1 PREEMPT_RT

- Patch `patch-6.6.36-rt35.patch.gz` appliqué **premier** (avant patches custom) — correct
- Config forcée via `sed` dans `do_configure:append()` :
  ```bash
  sed -i 's/^CONFIG_PREEMPT=y/# CONFIG_PREEMPT is not set/' "$cfg"
  sed -i 's/^CONFIG_PREEMPT_DYNAMIC=y/# CONFIG_PREEMPT_DYNAMIC is not set/' "$cfg"
  sed -i 's/^# CONFIG_PREEMPT_RT is not set/CONFIG_PREEMPT_RT=y/' "$cfg"
  ```
- ⚠️ **RISK** : si `CONFIG_PREEMPT=y` n'existe pas dans le defconfig (sinon = déjà RT), le sed est un no-op silencieux. **`olddefconfig` ne garantit pas que PREEMPT_RT est activé** — un changement de defconfig NXP pourrait casser le RT sans erreur.

#### 2.1.2 Patches DT

8 scripts Python `apply-*.py` + 3 patches classiques.

| Script | Fonction | Lignes |
|---|---|---|
| `apply-tac5212-dt.py` | DT son card (fsl,imx-audio-card) + SAI7 config + GPIO | 222 |
| `apply-npu-tap-dt.py` | reserved-memory carves + imx_audio_tap nodes | 106 |
| `apply-sdram2-dt.py` | DSP-only DDR carve @0xA0000000 | 50 |
| `apply-goodix-touch-dt.py` | GT911 tactile DSI 8" | ~80 |
| `apply-panel-max-brightness.py` | brightness=255 au boot | ~40 |
| `apply-imx-probes.py` | SOF probes client kernel (imx-probes.c) | 190 |
| `apply-imx-card-linkid.py` | patch imx-card.c — link->id = 0 (sequential) | ~50 |
| `apply-simple-card-multicodec.py` | **DISABLED (no-op)** | 20 |
| `apply-tac5212-bq12-maxreg.py` | MAX_REG 0x7E→0x7F | ~30 |

#### 2.1.3 ⚠️ Findings kernel bbappend

1. **Dead code actif** : `apply-simple-card-multicodec.py` est dans `SRC_URI` + appelé dans `do_patch:append` mais est un no-op (documenté comme DISABLED).
2. **Script shell redondant** : `apply-tac5212-dt.sh` (120 lines) fait la même chose que `apply-tac5212-dt.py` (222 lines). Le `.sh` est plus ancien (simple-card vs fsl,imx-audio-card). **Dead code** — devrait être supprimé.
3. **Regex DT fragile** : `apply-tac5212-dt.py` utilise `re.sub` avec `re.DOTALL` pour remplacer `sound-dac-out {...}` — un whitespace/cmnt upstream casse le build silencieusement.
4. **Kernel patch via Python** : `apply-imx-card-linkid.py` modifie `sound/soc/fsl/imx-card.c` via sed — **non standard Yocto** (devrait être un `.patch` dans SRC_URI). Difficile à auditer avec `devtool`.
5. **`rm -f || true` incohérent** : `sof-zephyr_%.bbappend` ligne 11-12 n'ont pas `|| true` mais ligne 13 si.
6. **`CONFIG_SND_SOC_TAC5212=m`** forcé via sed dans `do_configure:append` — mais `tac5212.cfg` existe et fait déjà `CONFIG_SND_SOC_TAC5212=m`. **Double setting** → redondant mais inoffensif.

#### 2.1.4 TAC5212 kernel driver (tac5212.c, 1281 lines)

- Driver ASoC pour TI TAC5212 (120dB DAC, 119dB ADC, TDM, I2C)
- `REGCACHE_NONE` (écriture I2C directe) — correct pour le timing
- Cache 20-byte coef blobs pour 12 biquads ADC + 12 DAC (lignes 21-26 de tac5212.h)
- BQ12 coefficients avec `MAX_REG 0x7F` (fix V11-AL, `apply-tac5212-bq12-maxreg.py`)
- 50 kcontrols ALSA (TAC, PGA, biquads, DRC)

**✅ Point fort** : Cache regmap pour les biquads write-only. Driver bien documenté.

---

### 2.2 U-Boot / imx-boot / flash.bin

| Fichier | Description | Status |
|---|---|---|
| `u-boot-imx_%.bbappend` | 1 patch : SPL FIT load address | ⚠️ Patch mais "NE PAS recompiler u-boost" |
| `imx-boot_1.0.bbappend` | Utilise flash.bin prébuildé | ✅ Correct |
| `flash.bin` | Binary patché à la racine | ⚠️ Non versionné (.gitignore) |

**⚠️ Conflit** : Le bbappend u-boot **ajoute un patch** mais le commentaire dit "NE PAS recompiler u-boot — ne boote pas sur cette board." Si `bitbake u-boot-imx` est lancé → patch appliqué → flash.bin binaire incohérent.

**⚠️ Path relatif** : `IMX_BOOT_PREBUILT = "${TOPDIR}/../flash.bin"` — path relatif, fragile si structure changée.

---

### 2.3 Module NPU tap (imx-audio-tap)

**Fichier** : `meta-local/recipes-kernel/imx-audio-tap/imx-audio-tap_0.1.bb` (71 lines)

**Module kernel** : `imx-audio-tap.c` (227 lines) + `imx-audio-tap-uapi.h` (73 lines)

#### Architecture

- Expose `/dev/imx-audio-tap-in` (0x94270000) et `/dev/imx-audio-tap-out` (0x942B0000) via `reserved-memory` DT phandle
- mmap `pgprot_writecombine` → userspace lit ring buffer DSP→A53
- sysfs `phys_addr` + `size` read-only

#### ⚠️ Findings critiques

1. **BUILD-TIME CROSS-CHECK** (R5) : Le `do_configure:prepend` compare `NPU_TAP_IN_PHYS_ADDR`/`NPU_TAP_OUT_PHYS_ADDR` entre UAPI kernel et SOF. `bbfatal` si mismatch. **✅ Single source of truth validé au build.**

2. **PACKAGING DEB MANQUANT** : `REPRODUCIBILITY.md:129` note que `kernel-module-imx-audio-tap` bloque `bitbake imx-image-full -c rootfs`. **Dette connue, non résolue** — le workaround est de commenter `IMAGE_INSTALL:append = " kernel-module-imx-audio-tap"` dans `imx-image-full.bbappend:23`, mais le `.bbappend` l'installe toujours. **Contradiction** : soit la recette est installée soit pas.

3. **PV = 0.1** : Version incohérente avec le projet (V7.0+). Le `imx-audio-tap.bb` installe le module + un meta-package via `RPROVIDES:${PN} = "imx-audio-tap"`.

4. **Makefile hors-standard** : Le Makefile (28 lines) fait du **out-of-tree module** mais Yocto `module.bbclass` gère normalement ça. Le `do_unpack:append` + `do_stage_sources` (Python inline) est **fragile** — ne pas utiliser `inherit module` standard.

---

### 2.4 TAC5212 driver + reset

#### 2.4.1 Driver (tac5212.c, 1281 lines)

- **REGCACHE_NONE** : écriture I2C directe (correct pour le timing TDM)
- **BQ12 programmable biquads** : 12 ADC + 12 DAC biquads, cache 20-byte coef blobs
- **`MAX_REG = 0x7F`** (fix V11-AL — off-by-one)
- Format `dsp_a` TDM 8 slots × 32 bits

#### 2.4.2 tac-reset (deux copies)

| Emplacement | Fichier | Recette |
|---|---|---|
| `recipes-kernel/linux/files/` | tac-reset.sh (200+) | linux-imx bbappend |
| `recipes-support/tac5212-service/files/` | tac-reset.sh (200+) | tac5212-service |

**⚠️ DOUBLE INSTALLATION** : Deux copies du même script + deux `tac-reset.service` identiques → conflit dans le rootfs. Le `IMAGE_INSTALL` au `.bbappend:26` installe `tac5212-service` mais le kernel bbappend installe aussi le service (via `linux-imx-src` debug package ?).

**⚠️ `tac-reset.service` overlap** :
- Service kernel/files : `After=sound.target Wants=sound.target`, `ExecStartPre=/bin/sleep 10`
- Service tac5212-service : identique mais installe `/usr/bin/tac-reset` (pas `/bin/tac-reset`)
- `mixer-pro.service` : `ExecStartPre=/usr/bin/tac-reset analog`

Donc trois mécanismes de reset au boot : le service systemd + l'ExecStartPre. **Double reset + 10s sleep = overhead boot**.

#### 2.4.3 tac-daisy.sh (181 lines, dans kernel/files)

Script de configuration daisy-chain TAC5212. **⚠️ Pas dans aucune recette install** — semble être un outil de debug.

---

### 2.5 mixer-pro daemon

**Fichier source** : `meta-local/recipes-audio/mixer-pro/files/mixer-pro.c` (**6961 lines**)

#### 2.5.1 Recette (mixer-pro_1.0.bb, 68 lines)

| Champ | Valeur |
|---|---|
| `DEPENDS` | `alsa-lib lilv fftw` |
| `RDEPENDS` | `alsa-lib lilv lv2 libfftwf` |
| `SYSTEMD_AUTO_ENABLE` | `enable` |
| `SERVICE` | `mixer-pro.service` |

**✅ Correct** : `RDEPENDS` utilise `libfftwf` (runtime) pas `fftw` (build). Le lien vers `mixerctl` est documenté.

#### 2.5.2 Architecture code (mixer-pro.c)

##### Sections principales (16 blocs numérotés)

| Section | Lignes | Description |
|---|---|---|
| State | 47-385 | Structures, constantes, état global |
| V8.1 UAC2 ISOLATION | 385-1217 | Gestion USB séparée (cap/play threads) |
| Logging/ALSA | 1217-1295 | Utils |
| **Mixer core** | 1295-1494 | `mix_block`, matrice 26×18 |
| V12-SMP | 1309-1494 | Sampleur PADS (16 pads) |
| V12-LOOP-PRO | 1494-1786 | Loopstation multipiste |
| V13.3 STEREO_LINK | 1786- | Linking stéréo paires |
| V12-EXP | 1786-1892 | Gate/expander par tranche |
| V13-COMP | 1892-1995 | Compresseur natif par tranche |
| **V13-BANDMIX** | 1995-2128 | Automix Dugan + keeper + balance auto |
| V13.6 EQ placement | 2128-2140 | EQ par rôle (eqx_bq) |
| **V13.7 MASTER** | 2140-2821 | EQ master + LUFS + makeup LUFS |
| V13-VFOCUS | 2821-2991 | Unmasking spectral sidechainé |
| V12-MIDIX | 2991-3277 | Module MIDI (fluidsynth) |
| **V13.9 SPATIALIZER** | 3277-3349 | Widener Lauridsen compensé |
| Audio loop | 3349-3889 | Boucle audio principale |
| Play thread DSP | 3889-3945 | Thread play via eventfd |
| Control socket | 3945-6131 | Socket UNIX JSON (38 ops) |
| Persistence | 6131-6336 | save/load mixer_state + scènes |
| V13-SCENES | 6336-6718 | Rappel profil sans coupure |
| Signal/MAIN | 6718-6961 | Signal handling + main() |

##### 2.5.3 Chaîne audio (audio_thread, 96 frames / 2ms @ 48kHz)

```
[capture]          ↓
exp_render (gate)     ← V12-EXP (24 ch)
eqx_render (placement) ← V13.6 (16 ch, 2 biquads)
cmp_render (comp)     ← V13-COMP (16 ch, seuil al_ref+offset)
duck_render (vfocus)  ← V13-VFOCUS (dynamic EQ sidechainé)
smp_render (sampler)  ← V12-SMP (P1/P2 outputs)
loop_render (looper)  ← V12-LOOP (6 tracks)
midix_render (MIDI)   ← V12-MIDIX (fluidsynth → P1/P2)
automix_update (Dugan/keeper) ← V12-AMX/V13-BANDMIX
mix_block (26×18)     ← Mixer core
SHM tap write (USB IN) ← NPU tap userspace
vspat_render (spatializer) ← V13.9 (post-mix, pre-master-EQ)
meq_chain (master EQ + makeup) ← V13.7 (si g_master_on)
insert process_block (LV2 chain) ← V13-SCENES
out_gain smoothing
LUFS measurement (BS.1770 K-weighting)
peaks/RMS publication (atomiques)
[playback]           ↑
```

##### 2.5.4 Threads & RT (mixer-pro.h:95-102)

| Thread | Prio | Core | Affinité |
|---|---|---|---|
| audio_thread | FIFO 99 | 2 | audio + DSP play |
| play_thread | FIFO 98 | 2 | drainage ring SPSC |
| cap_uac2_thread | FIFO 95 | 3 | USB capture |
| play_uac2_thread | FIFO 95 | 3 | USB playback |
| analyzer_thread | 60 | 0,1 | FFT + scope |
| control_thread | OTHER | 0,1 | socket IPC |
| persistence_thread | OTHER | 0,1 | save/load état |

**✅ Correct** : isolcpus=2,3 dans bootargs. CPUAffinity=0 1 2 3 (cgroup ouvert, pinning explicite par thread).

##### 2.5.5 bmx_tick (1 Hz, persistence_thread) — Automix LIVE

Logique V13.5-V13.9 (lignes 2363-2740) :

1. **Soundcheck** : mesure RMS/peak/floor par tranche (12s/tranche)
2. **Lock** : moyenne 30 ticks de `lt_ms` (post-fader) → `ref_share[i]`
3. **Autolive continu** (V13.5) :
   - Ancre voix (`Llead`), gel si voix muette
   - Solo auto v2 (`solo_base` EWMA τ60↑/τ20↓, +6dB vs base)
   - Keeper (kdb = ancre + offset_rôle, anti-blast 5s, slew ±1dB/tick)
   - **Balance auto quadrants** (V13.9) : LUFS→-14 + écart voix-musique→+3dB
    - `presence_gain` = gain groupe (voice_db/choir_db/music_db)
    - Queue quadrant : `LUFS<−14 & E<3 → +voix` / `LUFS>−14 & E>3 → +musique`
    - **Gel des montées** dans les creux (`prog_peak - 3dB`)
4. **Gate auto** (V13.9) : seuil = `al_ref[i] − gate_db`
5. **Comp auto** (V13.6) : seuil = `al_ref[i] + COMP_OFF[role]`
6. **Master EQ + makeup LUFS** (V13.7) : seulement si `!balance_on`

**Invariants critiques** (docs/REVUE_2026-07-18.md:57-63) :
- ✅ Pas de signal → aucun gain automatique ne bouge
- ✅ Voie muette → hors mix (mute + off), jamais blend bas
- ✅ Reset n'écrase JAMAIS un réglage opérateur (vfocus, EQ master)
- ✅ Automations OFF par défaut tant qu'elles ne sont pas validées à l'oreille

##### 2.5.6 API socket (38 ops JSON)

```
mixerctl send/master/fx/mute/state/reset/raw
set_send, set_master, set_input_gain, set_mute, set_output_gain
set_input_map, set_alsa, get_alsa
set_fx_engine, set_fx_param, fx_engine_list (V12-AMX)
bandmix_measure, bandmix_autolive, bandmix_solo, bandmix_live
automix_tune, set_balance
set_vfocus, set_vspatial
master_eq
set_insert, set_insert_param, set_insert_params_bulk, insert_bypass
set_assistant_mode, get_assistant (V13.7)
set_expander, sampler_trigger/param, looper_ctl/track_ctl/track_cfg
midix_ctl, set_midix
scene_save
reset
get_state, get_strip_routing, get_tac_reg
```

##### 2.5.7 ⚠️ Findings mixer-pro.c

1. **VERSION DÉSYNCHRONISÉE** : `#define MIXER_VERSION "v9.5.12-slow-smooth"` mais le code contient V13.5-V13.9 + revue 2026-07-18. **4 versions majeures d'écart**. La revue `2026-07-18` ne mentionne pas ce problème — critique pour le debug board.

2. **DUPLICATION COMPRESSEUR** : `cmp_render` (mixer-pro.c ~1930) vs `comp_process_block` (effects.c ~40). Même logique (envelope follower + gain reduction) mais implémentations séparées. Violence DRY (methode-dev §5).

3. **Spatializer avant makeup** : `vspat_render` (ligne 3277) est **après mix_block** mais **avant meq_chain** (master EQ + makeup). Donc le spatializer n'est **pas affecté par le limiting** (slot 2 insert) → le side peut dépasser 0 dBFS. ✅ Documenté mais à vérifier à l'oreille.

4. **Persistence EQ master** : `save_mixer_state` **n'écrit PAS l'EQ master** — `master_eq` est persisté dans un fichier séparé `/var/lib/mixer-pro/master_eq`. `scene_apply` ne restaure pas l'EQ master → **changement de scène = reset EQ master**. Dette connue (REVUE §2.4).

5. **Compresseur `cmp_render` V13-COMP** : 
   - `releasing` flag pour ramp gain→1 sans clic (ligne ~1937) ✅
   - Mais `cmp_configure` n'est pas appelée par `set_comp` (API) — **l'op n'existe pas dans la liste des 38 ops**. Les params comp sont **uniquement auto** (via `bmx_tick`). ✅ Conscient mais documenté comme "réglage manuel futur".

6. **EQ placement `eqx_bq`** (V13.6) : double-buffer atomique (`bank` field) — mais `eqx_config` est appelée par `bmx_tick` (control thread) et lu par `eqx_render` (audio thread). **Le `atomic_load_explicit(&g_eqx.bank[i], ...)`** dans `eqx_render` — mais le double-buffering switch se fait via `atomic_store` → **OK, pas de race**. Mais `eqx_config` écrit `g_eqx.bq[nb][i][...]` dans le buffer non-actif — **correct mais fragile si `nb` est calculé différemment entre config et render**.

7. **LUFS K-weighting** : les constantes `K1_B0`, `K1_A1`, etc. sont hardcodées (ligne ~2193) — **pas de source**. BS.1770-4 K-weighting filter. **À vérifier contre la norme** — si les coeffs sont faits main et erronés, le makeup est faux.

8. **`set_balance` op** (ligne ~5620) : valide `lufs_tgt ∈ [-30,-6]` et `e_tgt ∈ [-6,12]`. ✅ **Mais `c_tgt` (chœurs) n'est pas exposé** — seulement hardcodé (`-14, +3, +1.5` defaults). Le `beta.html` V13.9 expose `c_tgt` via le repeater → **incohérence entre API et GUI**.

---

### 2.6 Services systemd

#### mixer-pro.service (38 lines)

```ini
[Unit]
Description=V8.0-E1 mixer-pro daemon (console DAW SW, 26 in / 4 bus FX / 18 out)
After=sound.target snd-aloop-phone.service usb-uac2-gadget.service
Wants=usb-uac2-gadget.service mixer-ml-inference.service  ← V13.9
...
ExecStartPre=/usr/bin/tac-reset analog
ExecStart=/usr/bin/mixer-pro --no-phone
CPUSchedulingPolicy=fifo
CPUSchedulingPriority=80
CPUAffinity=0 1 2 3
User=root
```

**⚠️ Findings** :

1. **`Wants=mixer-ml-inference`** corrige la régression V13.9 (PartOf ne propage pas START — documenté commit `6da9847e`). ✅
2. **`ExecStartPre=/usr/bin/tac-reset analog`** : si `tac-reset.service` systemd a déjà fait un reset + 10s sleep → **double reset** au lancement de mixer-pro.
3. **`Restart=on-failure`** + `RestartSec=2` : si mixer-pro crash en boucle, le tac-reset est relancé 2x/seconde → **risque corruption I2C**.

#### mixer-ml-inference.service

```ini
PartOf=mixer-pro.service  ← V13.4
Restart=always, RestartSec=5s
CPUAffinity=0
```

**⚠️ POTENTIELLE OSCILLATION** : mixer-pro `Wants` ml-inference + ml-inference `Requires=mixer-pro + PartOf=mixer-pro`. Si mixer-pro restart → ml-inference stop (PartOf) → Wants le relance → mais Requires de ml-inference sur mixer-pro est satisface → **oscillation 2-5s**.

#### anti-larsen.service

```ini
After=mixer-pro.service tac-reset.service ala-fx-restore.service
Wants=mixer-pro.service
```

**⚠️ COUPLAGE CACHÉ** : `After=ala-fx-restore.service` mais ce service est dans la recette **`mixer-gui-http`** — si gui-http n'est pas installé, **deadlock**.

#### irq-prio-rt.service (V9.1)

Daemon mode : poll IRQ kthreads toutes les 5s, bump prio 90. Cache PID pour éviter `pgrep -f` continu.

**✅ Excellent** : cache + retry. **⚠️ Patterns regex fragiles** : `30e60000.mailbox\\[3-0\\]` — si kernel change naming, silent fail.

#### boot-script-rt (V8.33)

`boot.cmd` → `boot.scr` : `isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3 threadirqs`.

**⚠️ `imx8mp-evk.dtb` hardcodé** : pas de support pour un DTB custom Debix.

---

### 2.7 mixer-gui-http + ala-fx-restore

#### mixer-gui-http.c (1654 lines)

- libmicrohttpd + Alpine.js/Tailwind CDN
- Pool 4 sockets Unix persistants (round-robin mutex)
- `/api/cmd` → proxy JSON → mixer-pro.sock

**⚠️ CDN EXTERNAL** : `beta.html` + `index.html` chargent Alpine.js + Tailwind depuis `cdn.jsdelivr.net`. **Console isolée = GUI cassée si pas internet.**

#### ala-fx-restore.sh (V10-N7b)

- Retry ×5 `alsactl restore` avec vérification témoin (`TAC0 ADC Biquad Config`)
- Push DSP blobs via `/api/dsp/blob/set` (HTTP, dépend mixer-gui-http)

**✅ Excellent** : Fix du bug majeur V13.1 (restore TAC auto-destructeur). **⚠️ Dépendance chaînée** : restore → alsactl → GUI HTTP → mixer-pro. Si un maillon casse → échec silencieux.

---

### 2.8 Console native Qt6 (mixer-console)

**Recette** : `recipes-graphics/mixer-console/mixer-console_0.1.bb` (59 lines)
**Sources** : `main.cpp` (164), `mixerclient.cpp` (278), `mixerclient.h` (133), 21 fichiers QML

- Qt6/QML eglfs (DRM/KMS direct, pas de compositor)
- 12-15% CPU vs 85-93%×2 Chromium
- KMS config `{"device": "/dev/dri/card1"}` (DSI)

**⚠️ Coexistence mixer-kiosk + mixer-console** : les deux sont `SYSTEMD_AUTO_ENABLE = "enable"` → **3 composants graphiques au boot** (weston + kiosk + console).

---

### 2.9 Flutter/Dart SDK

#### flutter-sdk_3.13.9.bb (145 lines)

**do_compile() Python heredoc** (lignes 26-108) :
1. Lit `engine.version` depuis le SDK
2. Télécharge `linux-arm64-embedder.zip` (Google Storage)
3. Fallback local (`DL_DIR` : `elinux-arm64-release.zip`, etc.)

**⚠️ Risques** :
- `urllib.request.urlretrieve` **sans timeout** → blocage indefini
- `URLError` non gérée (seulement `HTTPError`) → exception non catchée
- Flutter 3.13.9 + Dart 3.7.0 (standalone) → **incompatibilité pubspec potentielle**

#### dart-sdk_3.7.0.bb (30 lines)

Simple, correct. BBCLASSEXTEND native+nativesdk.

---

### 2.10 Plugins LV2 + Ardour

#### 47 recettes plugins LV2

| Recette | Build system | Notes |
|---|---|---|
| calf | autotools-brokensep | qemu TTL generation |
| lsp-plugins | make | ⚠️ Fix memoïsation `=`→`=` (V9.2-step5g) |
| zam-plugins | make | lv2-turtle-helper |
| x42 family (dpl, darc, fil4, meters, fat1) | make | pkgconfig |
| mda-lv2 | waf | git + patch python3 |
| dragonfly-reverb | make | lv2-ttl-helper |
| artdy-fx | cmake | SSE flags désactivés |
| gxplugins.lv2 | make | SSE_CFLAGS= |
| 10× sjaehn (bchoppr, bslizr, etc.) | make | |
| drobilla stack (suil, lilv, serd, sord, sratom) | waf | |

**⚠️ Points communs** :
- Toutes les recettes x42/sjaehn utilisent `STRIP=echo OPTIMIZATIONS=` — correct
- `REQUIRED_DISTRO_FEATURES = "x11"` sur certaines mais pas `mixer-console` (eglfs) → **conflit potentiel**
- `BBCLASSEXTEND += "native"` sur 4 libs multimedia pour lsp-plugins-native chain

#### ardour6.bb

- `PV = "6.9"` (2021, très ancien)
- 22 DEPENDS, SRCREV fixe sur master
- Patches : `disable-pulseaudio-backend-check.patch`, `fix-missing-iostream-include.patch`
- `BUILD_DIST_TARGET ??= "none"` — commentaire "default tested is ARM, no fpu-optimizations"

**⚠️ Fragilité** : Ardour 6.9 sur master fixe + 22 dépendances = **build brittle**.

---

### 2.11 BSP services

#### usb-uac2-gadget

- ConfigFS : UAC2 8×8 S32_LE 48kHz + MIDI 1×1
- UDC = `38100000.usb`
- Idempotent + graceful skip

**⚠️ `idVendor = 0x1d6b`** (Linux Foundation) — correct mais `idProduct` identique pour tous les boards.

#### snd-aloop-phone

- `modules-load.d` + `modprobe.d` + service systemd
- **Redondant** : 3 mécanismes pour load le module.

#### alsa-route-bridge

- **Disabled by default** ✅
- Parse `/etc/alsa-route-bridge.conf` via `IFS='|'`

**⚠️ Fragile parsing** : `|` dans un chemin ALSA = crash parse.

#### boot-script-rt, goodix-calibration, irq-prio-rt

Voir sections 2.2, 2.4, 2.6.

---

## 3. SOF Firmware (sof/ fork)

### 3.1 État du fork

- **Branche** : `feature/v7.0-multiband-drc-tap` (HEAD `356b490cd`)
- **Remote** : `github.com/Mjxkill/sof` (origin = local download cache)
- **Écart avec artifact vendorisé** : HEAD du fork (`356b490cd`) est **+3 commits** derrière le `4bea8e59d` cité dans PROJECT_STATE.md. Les 2 commits intermédiaires (`6ee842c67` NPU dual-tap, `248e33cf3` DSP load, `356b490cd` V10-FX) **ne sont PAS dans l'image déployée** d'après `REPRODUCIBILITY.md` ("firmware md5 inchangé").

**⚠️ DIVERGENCE FORK/ARTIFACT** : Le firmware `.ri` dans `sof-firmware-custom/files/` correspond à `4bea8e59d` mais le fork avance. **Risque d'incohérence** si quelqu'un rebuild le firmware sans mettre à jour l'artifact vendorisé.

### 3.2 Commits récents significatifs

| Commit | Description | Fichier impacté |
|---|---|---|
| `4bea8e59d` | SAI TX FIFO alignment (prime N zeros) | `src/drivers/imx/sai.c` |
| `6ee842c67` | NPU dual-tap (tap-in + tap-out) | `src/audio/dai-legacy.c`, `npu_tap.h` |
| `248e33cf3` | DSP load → mailbox SW REGs | `zephyr_dma_domain.c` |
| `356b490cd` | multiband_drc V10-FX per-channel | `multiband_drc`, topology |

### 3.3 Dual-tap architecture (V7.0-E4)

| Élément | tap-in (capture) | tap-out (playback) |
|---|---|---|
| Adresse | `0x94270000` | `0x942B0000` |
| Hook | `dai_dma_cb` après `dma_buffer_copy_from` | `dai_dma_cb` après `dma_buffer_copy_to` |
| Sentinelle | `npu_tap_in_owner` | `npu_tap_out_owner` |
| Header dir | `direction=1` (capture) | `direction=0` (playback) |

**✅ Cross-check** : `imx-audio-tap-uapi.h` ↔ `sof/src/include/sof/audio/npu_tap.h` validé au build (Yocto `do_configure:prepend`).

---

## 4. Patches kernel & Topology

### 4.1 Topology V7.0 (`sof-imx8mp-tac5212-V7.0.m4`)

Diff HEAD~5 : PIPE 2 pass de `pipe-multiband-drc-pga-drc-8ch-playback.m4` → `pipe-multiband-pga-8ch-playback.m4`.

**⚠️ MODIFICATION DE PRODUCTION** : Le nouveau fichier est documenté comme **"DIAG"** (isoler le "tic tic" à l'attack du signal). Mais il est **mis comme PIPE 2 par défaut** — si le diag n'est pas validé, le limiter final DRC est perdu en production.

**⚠️ CONTROLBYTES_MAX** : Bumpé de 4096→6144 pour le blob multiband_drc 8ch — **pas de commentaire** sur la taille exacte du blob.

### 4.2 multiband_drc V10-FX (sof HEAD `356b490cd`)

- Per-channel enabled (8 canaux indépendants)
- Bypass bit-transparent
- Crossover par canal (blob V3)
- `MULTIBAND_DRC_VERSION 3` — **incohérent avec V10-FX commit message**

---

## 5. Fichiers sources kernel

### 5.1 imx-probes.c (170 lines)

Client kernel SOF Probes (`snd_sof_imx_probes`).

**⚠️ Bloqué firmware** (PROJECT_STATE.md:157-161) : `sdma.c:588` guard `if (!buf_addr || !buf_xaddr) return 0;` drop silencieusement les transferts AP2AP. **Backend kernel OK, frontend firmware bloqué** — gardé comme infra utilisable.

### 5.2 gpio-monitor.c (146 lines)

Mesure précise FSYNC frequency via interruption GPIO.

**⚠️ Outil debug** : Pas dans une recette install — outil standalone.

### 5.3 test-sof-dsp.sh (144 lines)

Frequency sweep + FFT harmonic analysis.

---

## 6. Code mixer-pro.c (analyse détaillée)

### 6.1 Audio loop ordering critique (lignes ~3530-3540)

```c
exp_render(in_block);      // gate (V12-EXP)
eqx_render(in_block);      // EQ placement (V13.6)
cmp_render(in_block);      // compresseur (V13-COMP)
duck_render(in_block);     // vfocus unmasking (V13-VFOCUS)
smp_render(in_block);      // sampleur (V12-SMP)
loop_render(in_block);     // looper (V12-LOOP)
midix_render(in_block);    // MIDI expandeur (V12-MIDIX)
automix_update(in_block, PERIOD_FRAMES);  // Dugan/keeper/balance (V13-BANDMIX)
mix_block(in_block, out_block, ...);      // matrice 26×18
mixer_pro_shm_tap_write(...);             // NPU tap USB (V9.5.12)
vspat_render(in_block, out_block, ...);   // spatializer (V13.9)
// master EQ + makeup LUFS (V13.7) IF g_master_on
// insert LV2 chain (V13-SCENES)
// out_gain smoothing
// LUFS measurement (BS.1770)
// peaks/RMS publication (atomiques)
```

**⚠️ ORDRE CRITIQUE** : Le spatializer (`vspat_render`) est **après mix_block** mais **avant master EQ/makeup**. Donc :
1. Le NPU USB tap capture **sans spatializer** (OK documenté)
2. Le NPU HW tap (tap-out) capture **après mix + spatializer** (depuis le DAI TX hook)
3. **Incohérence** : tap USB ≠ tap HW si spatializer actif

### 6.2 Biquads & coefficients

- `eqx_render` (V13.6) : 2 biquads/channel × 16 channels = 32 biquads/sample
- `meq_chain` (V13.7) : 3 biquads stereo = 6 biquads/sample  
- `vfocus` (V13-VFOCUS) : 5 biquads dynamic EQ
- Tous en forme II transposée, coefs précalculés

**⚠️ CODE DUPLIQUÉ** : `eqx_bq`, `meq_bq` (via `meq_shelf`) — même structure, implémentations séparées.

### 6.3 Atomic publications

- `g_ms_in[i]` via `union { float f; uint32_t u; }` + `atomic_store_explicit` — **correct** mais hacky
- `g_mk.lufs_c` (atomic int, ×100) publié par `audio_thread`, lu par `bmx_tick`
- `g_insert_bypass`, `g_meq_active`, `g_meq_pending` — atomiques pour cross-thread

---

## 7. Documentation & Traçabilité

### 7.1 Documents maîtres

| Document | Lignes | Version | Statut |
|---|---|---|---|
| `docs/PROJECT_STATE.md` | 440 | 2026-05-13 | Source de vérité |
| `docs/REPRODUCIBILITY.md` | 232 | 2026-05-13 | Procédure rebuild |
| `docs/ARCHI/ARCHI_V7.0.md` | 291 | V7.0 | Architecture DSP |
| `docs/REVUE_2026-07-18.md` | 64 | 2026-07-18 | **Revue projet** |
| `docs/TESTS/TESTS_V13.9_AUTOMIX_LIVE.md` | 65 | V13.9 | **Test fiche** |
| `docs/MIXER_PRO_REFERENCE.md` | 352 | V13 | Guide développeur |

### 7.2 Revue 2026-07-18

**✅ Documentée** : état par sous-système, fiabilisation (7 items), IHM, évolution, propositions, règles de travail. Mentions correctement les known issues (SAI RX slot shift, anti-larsen faux-positifs, etc.).

### 7.3 Documentation ARCHI

25 fichiers ARCHI/V*.md (1931 lines total) — couvrant V6.0, V7.0, V9.5, V10, V11, V12, V13.

**⚠️ GAP** : Pas de `ARCHI_V13.7_MASTER_LUFS_EQ.md` bien que le code V13.7 master EQ soit présent. Le `ARCHI_V13_BANDMIX.md` (114 lines) couvre V13.5 mais pas V13.7/V13.9.

### 7.4 Documentation TESTS

85 fiches TESTS_V*.md — suivant le template `docs/TESTS/TEMPLATE.md`.

**⚠️ VERSIONING** : La fiche V13.9 fait référence au commit `6da9847e` mais mentionne **md5 de mixer-pro**, **mixer-console**, **beta.html** — systematique. ✅ Mais `md5sum` sur le board n'est **pas vérifiable depuis le git** — les artefacts sont sur la board, pas dans le repo.

### 7.5 Manuel utilisateur (341 lines)

`docs/MANUEL_UTILISATEUR.md` — V13 (2026-07-08).

**⚠️ ÉCHEC BUILD** : La revue note "md5 de tous les artefacts consignés (commit 6da9847e)" mais le commit `19b46b1b` (HEAD actuel) est **1 commit après** et ajoute la fiche TESTS. **La revue n'a pas été intégrée dans un commit séparé** — c'est le commit `6da9847e` himself qui contient la revue.

---

## 8. Findings Critiques (Priorités)

### 🔴 CRITIQUE (bloque build ou production)

| # | Finding | Localisation | Impact |
|---|---|---|---|
| 1 | **packaging deb kernel-module-imx-audio-tap manquant** | REPRODUCIBILITY.md:129 | `bitbake imx-image-full -c rootfs` échoue |
| 2 | **`--no-phone` dans mixer-pro** | mixer-pro.service | P1/P2 téléphone toujours mort (délibéré, OK) |
| 3 | **meta-browser NON dans bblayers.conf** | CLAUDE.md:127 | build chromium-ozone-wayland échoue si pas ajouté manuellement |

### 🟠 ÉLEVÉE (risque production / fragilité)

| # | Finding | Localisation |
|---|---|---|
| 4 | Version `MIXER_VERSION` désynchronisée (v9.5.12 vs V13.9 code) | mixer-pro.h:29 |
| 5 | Dead code actif : `apply-simple-card-multicodec.py` no-op dans SRC_URI | linux-imx bbappend:50 |
| 6 | Script shell redondant : `apply-tac5212-dt.sh` + `.py` identiques | kernel/files/ |
| 7 | Double installation tac-reset.sh (kernel/files + tac5212-service/files) | 2 recettes |
| 8 | Divergence fork sof/ (HEAD 356b490cd) vs artifact vendorisé (4bea8e59d) | sof/ vs sof-firmware-custom/files/ |
| 9 | `flash.bin` non versionné mais dépendu par `IMX_BOOT_PREBUILT` | imx-boot bbappend:1 |
| 10 | Coexistence weston + kiosk + console native (3 graphiques au boot) | mixer-kiosk + mixer-console |
| 11 | `apply-tac5212-dt.py` regex fragile sur `sound-dac-out` | 222 lines |
| 12 | `apply-imx-card-linkid.py` patch kernel via sed (non-standard Yocto) | kernel/files/ |

### 🟡 MOYENNE (dette / amélioration)

| # | Finding | Localisation |
|---|---|---|
| 13 | Duplication code compresseur (cmp_render vs comp_process_block) | mixer-pro.c:1930, effects.c |
| 14 | Duplication code biquads (eqx_render vs meq_chain) | mixer-pro.c |
| 15 | Persistence EQ master séparée (scene_apply ne le restore pas) | mixer-pro.c:6331 |
| 16 | Version multiband_drc (`MULTIBAND_DRC_VERSION 3`) vs commit (V10-FX) | sof/multiband_drc |
| 17 | Flutter 3.13.9 + Dart 3.7.0 (incompat pubspec potentielle) | flutter-sdk + dart-sdk |
| 18 | `urllib.request.urlretrieve` sans timeout dans flutter-sdk do_compile | flutter-sdk_3.13.9.bb:70 |
| 19 | Ardour 6.9 (2021) sur master fixe + 22 DEPENDS | ardour6.bb |
| 20 | `K1_*`/`K2_*` LUFS coeffs hardcodés sans source | mixer-pro.c:2193 |
| 21 | `set_balance` API n'expose pas `c_tgt` (chœurs) mais beta.html si | mixer-pro.c, beta.html |
| 22 | 3 mécanismes de backlight (udev + service + ExecStartPre) | goodix-calibration + mixer-console.service |
| 23 | `set_balance` valide `e_tgt ∈ [-6,12]` — **plage étrange** (positif = voix trop forte) | mixer-pro.c:5645 |

### 🟢 BASSE (cosmétique / documentation)

| # | Finding | Localisation |
|---|---|---|
| 24 | `tac-daisy.sh` non installé (outillage debug seul) | kernel/files/ |
| 25 | `gpio-monitor.c` non dans recipe | kernel/files/ |
| 26 | `sof-test-v42-probes.sh` + `vu8.c` + `loopback-c.c` non versionnés dans recipes | audio-tools/ |
| 27 | `mixer-pro_v1_backup.c.bak` dans mixer-ml-inference/files | 442 lines, pas de recette |
| 28 | `weston-project.ini` avec `[launcher]` hardcoded (gopoint) | weston-init |
| 29 | `alsa-route-start.sh` parse fragile `IFS='|'` | 43 lignes |

### ✅ POINTS FORTS (à préserver)

1. **Cross-check build-time UAPI↔SOF** (R5) — exemplaire, single source of truth
2. **Idempotence scripts DT** + **sentinelles mono-DAI** (R7)
3. **Persistence atomic** (tmp + rename) + **rétrocompatibilité format**
4. **Gate auto adaptative** (al_ref − gate_db) + **anti-blast reprise** (5s)
5. **Fondu crossfade EQ master** sans clic
6. **Queue quadrant balance auto** avec gel des montées dans les creux
7. **Retry ×5 + vérification witness** pour alsactl restore (V13.1)
8. **Page-gating QML/Qt** (polling stop sur pages non-visibles)
9. **Documentation exhaustive** (1931 LINES ARCHI + 85 fiches TESTS)
10. **Validation hardware** systématique avec md5 + fiches test

---

*Document généré par revue statique complète du dépôt yocto-nxp-debix@19b46b1b (2026-07-28). Voir `docs/REVUE_2026-07-18.md` pour l'état projet officiel.*
