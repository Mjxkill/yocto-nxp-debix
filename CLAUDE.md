# CLAUDE.md

# PROTOCOL OBLIGATOIR DE TRAVAIL
lit le fichier REGLES.md dans ce même repertoire

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Is

Yocto BSP build for the Debix Model AB board (NXP i.MX 8M Plus) — the
**A.L.A. console** (Audio Live Assistant, Electrosens R&D) : console de mixage
live autonome. Pile : firmware DSP SOF custom (matrix 16×8, DMA 2 ms), moteur
audio temps réel `mixer-pro` (C, AUTOMIX LIVE), console native Qt6/eglfs sur
écran DSI, GUI web pour PC distants (`mixer-gui-http`, autonome sans internet),
mastering ML sur NPU (TFLite). Based on NXP's L6.6.36 release (Yocto 5.0
Scarthgap), branche `L6.6.36-2.1.0-debix_model_ab`.

Historique : l'UI Flutter et Ardour ont été RETIRÉS (2026-07 — console native
Qt6 = la cible, Ardour n'est pas un objectif). Chromium/kiosk retirés de
l'image (revue 2026-07-28) : l'écran est servi par mixer-console (Qt6), le web
par mixer-gui-http.

## Build Commands

Initialize the build environment (required once per shell session — TOUJOURS
depuis la racine du repo, piège cwd) :
```bash
EULA=1 DISTRO=fsl-imx-xwayland MACHINE=imx8mpevk source imx-setup-release.sh -b Model_AB_Infinity
```

Build the full image:
```bash
bitbake imx-image-full
```

Rebuild a single recipe (e.g. after editing a .bb or .bbappend):
```bash
bitbake <recipe>                  # e.g. bitbake mixer-pro
bitbake -c cleansstate <recipe>   # JAMAIS cleanall (casse les fetch git)
bitbake -c devshell <recipe>      # interactive debug shell in recipe sysroot
```

Image artifacts land in `Model_AB_Infinity/tmp/deploy/images/imx8mpevk/`.
Après build : vérifier md5/mtime du binaire avant scp (piège binaire stale).

## Architecture

### Layer stack

- **`sources/`** -- upstream vendor layers (poky, meta-imx, meta-openembedded, meta-freescale, etc.). Do not edit; treat as read-only.
- **`meta-local/`** -- all project customizations live here. Highest priority layer (BBFILE_PRIORITY = 1). Must mirror standard Yocto directory layout (`recipes-<category>/<package>/`).
- **`sources/meta-musicians/`** -- git submodule (`schnitzeltony/meta-musicians`) providing the OE audio recipe ecosystem (LV2, etc.).

`bblayers.conf` references ~40 layers. Only `meta-local/` should be modified for project work.

### Key custom recipes (meta-local/)

| Path | What it does |
|------|-------------|
| `recipes-audio/mixer-pro/` | Moteur audio RT (26 in / 4 bus FX / 18 out, AUTOMIX LIVE, cores 2-3) |
| `recipes-audio/mixer-gui-http/` | Serveur web GUI (libmicrohttpd, beta.html autonome zéro CDN) |
| `recipes-audio/mixer-ml-inference/` | Daemon mastering NPU (TFLite, PartOf mixer-pro) |
| `recipes-audio/anti-larsen/` | AFS notchs (DISABLED — écritures TAC en live = plops, refonte v2 logicielle à faire) |
| `recipes-graphics/mixer-console/` | Console native Qt6/eglfs (écran DSI, page AUTO MIX) |
| `recipes-fsl/images/imx-image-full.bbappend` | Contenu image (audio, ML, LV2 utilisés par l'insert) |
| `recipes-kernel/linux/linux-imx_%.bbappend` | PREEMPT_RT + patches DT (TAC5212, NPU tap, tactile) avec assertions |
| `recipes-kernel/imx-audio-tap/` | Module kernel tap NPU (/dev/imx-audio-tap-in/-out) |
| `recipes-bsp/imx-mkimage/` | flash.bin prébuildé versionné (garde-fou md5) |
| `recipes-support/tac5212-service/` | tac-reset (propriété unique) |
| `recipes-musicians/` | Plugins LV2 utilisés par l'insert mixer (calf, mda, x42…) |

### Build directory

`Model_AB_Infinity/` is the active build directory. `conf/local.conf` sets MACHINE=imx8mpevk, DISTRO=fsl-imx-xwayland, Debian packaging. `conf/bblayers.conf` defines the full layer stack.

### Target hardware

NXP i.MX 8M Plus (Cortex-A53 ×4 + Cortex-M7, Vivante GPU, NPU). Audio : 4×
TAC5212 (TDM 8 slots via SAI7, piloté par le DSP SOF), USB gadget UAC2 8×8.
Écran : MIPI-DSI 800×1280 (scène Qt 1280×800 rotée). Carte : 192.168.0.198.

## Conventions

- Commit messages: `area: imperative action` (français OK, style `V13.9 : …`)
- BitBake variables: `UPPER_SNAKE_CASE`; functions/tasks: `lower_snake_case`
- Recipe files: `<package>_<version>.bb`; overrides: `<package>_%.bbappend`
- Patches go in a `files/` subdirectory alongside the recipe (`.patch` SRC_URI standard, pas de sed-python sur les sources kernel)
- Indent with 4 spaces; align continued lines with trailing `\`
- `MIXER_VERSION` (mixer-pro.h) DOIT être bumpé à chaque évolution du moteur
- Fiches de test : `docs/TESTS/TESTS_V<x>_*.md` pour chaque étape validée board
- Règles moteur : pas de signal → aucun gain ne bouge ; un reset n'écrase
  jamais un réglage opérateur ; automation non validée = OFF par défaut ;
  effets TAC statiques (jamais écrits pendant le live)

## Key Constraints

- The `downloads/` and `sstate-cache/` directories are untracked and large; never commit them.
- `flash.bin` : source de vérité = `meta-local/recipes-bsp/imx-mkimage/files/flash.bin` (binaire U-Boot patché, md5 vérifié au build). NE PAS recompiler u-boot-imx (ne boote pas sur cette carte).
- Expect >120 GB disk usage for a full build.
