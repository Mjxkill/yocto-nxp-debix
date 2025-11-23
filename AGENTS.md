# Repository Guidelines

## Project Structure & Module Organization
- Root holds build helpers such as `build.sh`, generated images like `flash.bin`, and project notes.
- `sources/` mirrors upstream Yocto layers (`poky`, `meta-imx`, `meta-openembedded`) alongside vendor drops; avoid editing unless upgrading the vendor baseline.
- `meta-local/` contains custom recipes, images, patches, and must mirror Yocto layout (e.g., `recipes-graphics/flutter/flutter-embedded_%.bbappend`).
- `Model_AB_Infinity/` is the active build directory with `conf/`, `sstate-cache/`, `tmp/`, and `deploy/` outputs; target images land in `Model_AB_Infinity/tmp/deploy/images/imx8mpevk/`.
- `downloads/` caches shared source tarballs and should remain untracked.

## Build, Test, and Development Commands
- `EULA=1 DISTRO=fsl-imx-xwayland MACHINE=imx8mpevk source imx-setup-release.sh -b Model_AB_Infinity` initializes the build environment.
- `bitbake imx-image-full` produces the reference image; artifacts appear under `tmp/deploy/images/imx8mpevk/`.
- `bitbake <recipe>` rebuilds a single component (for example `bitbake flutter-embedded-runner`); prefer `-c cleanall` before iterative fixes.
- `bitbake <recipe> -c devshell` spawns a debug shell inside the recipe sysroot.
- `bitbake imx-image-full -c populate_sdk` creates the Flutter/Dart SDK installer.

## Coding Style & Naming Conventions
- BitBake variables use `UPPER_SNAKE_CASE`; functions and tasks use `lower_snake_case`.
- Indent with four spaces; align continued commands with a trailing `\`.
- Recipe files follow `<package>_<version>.bb`; overrides live in `<package>_%.bbappend`.
- Place patches inside a local `files/` directory and reference them via `SRC_URI += "file://<patch>"`.

## Testing Guidelines
- After changes, rebuild the touched recipe and the image; fail builds should block merges.
- Validate deployable images on target hardware, confirming critical features such as the Flutter demo launch.
- Optional: execute relevant Poky selftests with `oe-selftest -r <suite>` when modifying core tooling.

## Commit & Pull Request Guidelines
- Commit subjects follow `area: imperative action` (e.g., `meta-local: add Flutter/Dart integration`); wrap bodies at ~72 characters and explain rationale plus key results.
- Reference issues or downstream tickets, and include build logs or screenshots when behavior changes.
- Pull requests must declare target `MACHINE`/`DISTRO`, test steps, and note any image size or boot-time impact.

## Security & Configuration Tips
- Set `EULA=1` before sourcing NXP scripts; never commit credentials or SDK installers.
- Expect large disk usage (>120 GB) and raise `ulimit -n 4098` when building Chromium-based stacks.
- Keep the patched `flash.bin` and `f_uac2.c` updates in sync so the deployed image carries the required u-boot and USB audio configuration.
