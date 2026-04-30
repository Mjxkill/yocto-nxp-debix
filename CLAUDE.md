# CLAUDE.md

# PROTOCOL OBLIGATOIR DE TRAVAIL
lit le fichier REGLES.md dans ce même repertoire

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Is

Yocto BSP build for the Debix Model AB board (NXP i.MX 8M Plus). The image targets an embedded audio/multimedia workstation with Flutter UI, Ardour DAW, DSP firmware, and ML inference (TFLite/NPU). Based on NXP's L6.12.3 release (Yocto 5.0 Scarthgap).

## Build Commands

Initialize the build environment (required once per shell session):
```bash
EULA=1 DISTRO=fsl-imx-xwayland MACHINE=imx8mpevk source imx-setup-release.sh -b Model_AB_Infinity
```

Build the full image:
```bash
bitbake imx-image-full
```

Rebuild a single recipe (e.g. after editing a .bb or .bbappend):
```bash
bitbake <recipe>                  # e.g. bitbake flutter-embedded-runner
bitbake -c cleanall <recipe>      # full clean before rebuild
bitbake -c devshell <recipe>      # interactive debug shell in recipe sysroot
```

Generate the cross-compilation SDK (includes Flutter + Dart host tools):
```bash
bitbake imx-image-full -c populate_sdk
```

Image artifacts land in `Model_AB_Infinity/tmp/deploy/images/imx8mpevk/`.

## Architecture

### Layer stack

- **`sources/`** -- upstream vendor layers (poky, meta-imx, meta-openembedded, meta-freescale, etc.). Do not edit; treat as read-only.
- **`meta-local/`** -- all project customizations live here. Highest priority layer (BBFILE_PRIORITY = 1). Must mirror standard Yocto directory layout (`recipes-<category>/<package>/`).
- **`sources/meta-musicians/`** -- git submodule (`schnitzeltony/meta-musicians`) providing the OE audio/DAW recipe ecosystem (LV2, Jack, etc.).

`bblayers.conf` references ~40 layers. Only `meta-local/` should be modified for project work.

### Key custom recipes (meta-local/)

| Path | What it does |
|------|-------------|
| `recipes-core/images/imx-image-full.bbappend` | Adds Flutter, Dart, Ardour to the image |
| `recipes-fsl/images/imx-image-full.bbappend` | Adds audio tools (sox, ALSA, DSP firmware), ML stack |
| `recipes-kernel/linux/linux-imx_%.bbappend` | Audio/MIPI board patch + SPDIF config fragment |
| `recipes-bsp/u-boot-imx/` | U-Boot SPL FIT load address patch |
| `recipes-graphics/flutter/` | Flutter embedder (Sony), SDK, systemd runner |
| `recipes-devtools/flutter/` and `dart/` | Flutter SDK 3.13.9, Dart SDK 3.7.0 |
| `recipes-musicians/` | Ardour 6.9 + full dependency chain (LV2, aubio, rubberband, etc.) |

### Build directory

`Model_AB_Infinity/` is the active build directory. `conf/local.conf` sets MACHINE=imx8mpevk, DISTRO=fsl-imx-xwayland, Debian packaging. `conf/bblayers.conf` defines the full layer stack.

### Target hardware

NXP i.MX 8M Plus (Cortex-A53 + Cortex-M7, Vivante GPU, ISP, NPU). Audio interfaces: SPDIF, I2S/SAI. Display: MIPI-DSI.

## Conventions

- Commit messages: `area: imperative action` (e.g. `meta-local: add Flutter/Dart integration`)
- BitBake variables: `UPPER_SNAKE_CASE`; functions/tasks: `lower_snake_case`
- Recipe files: `<package>_<version>.bb`; overrides: `<package>_%.bbappend`
- Patches go in a `files/` subdirectory alongside the recipe
- Indent with 4 spaces; align continued lines with trailing `\`

## Key Constraints

- The `downloads/` and `sstate-cache/` directories are untracked and large; never commit them.
- `flash.bin` at root is a patched U-Boot binary that must stay in sync with u-boot-imx recipe changes.
- Ardour is built ALSA-only (PulseAudio backend disabled via patch) since PulseAudio is removed from this NXP release.
- Flutter SDK recipes support offline builds: pre-cached engine artifacts can be placed in `downloads/`.
- Expect >120 GB disk usage for a full build; raise `ulimit -n 4098` for Chromium-based stacks.
