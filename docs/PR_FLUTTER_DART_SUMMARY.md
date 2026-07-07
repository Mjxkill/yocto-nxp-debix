Title: Add Flutter/Dart SDK + Flutter Embedded on i.MX8MP (Yocto Model_AB_Infinity)

Overview
- Integrates Dart SDK, Flutter SDK, and Sony Flutter Embedded for i.MX8MP (Wayland/EGL).
- Provides a systemd unit to autostart the demo binary on Wayland.
- Makes the integration reproducible/offline-friendly by allowing engine artifacts from ${DL_DIR}.

Key Changes
- meta-local/recipes-devtools/flutter/flutter-sdk_3.13.9.bb
  - Pre-caches the ARM64 engine matching Flutter’s engine.version.
  - Stages engine to sysroot via `/opt` so dependent recipes can link.
  - Prunes x86_64 host artifacts and bundled host dart-sdk from target package.
  - Keeps only `libflutter_engine.so` for `linux-arm64-release`.
  - QA: `INSANE_SKIP: already-stripped` and `INHIBIT_PACKAGE_STRIP = "1"` for upstream prebuilt.
  - Runtime deps: adds `fontconfig` (required by engine).

- meta-local/recipes-devtools/dart/dart-sdk_3.7.0.bb
  - Installs Dart SDK under `/opt/dart-sdk` with `dart` and `pub` symlinks.
  - QA: `INSANE_SKIP: already-stripped ldflags` (snapshots are not ELF).

- meta-local/recipes-graphics/flutter/flutter-embedded_git.bb
  - Uses Flutter SDK engine from sysroot: `${RECIPE_SYSROOT}/opt/flutter-sdk/bin/cache/artifacts/engine/linux-arm64-release/libflutter_engine.so`.
  - DEPENDS: adds `fontconfig` to satisfy engine link symbols.
  - Installs built binary as `/usr/bin/flutter-embedded` (handles both `flutter-client` and `embedding_demo`).

- meta-local/recipes-graphics/flutter/flutter-embedded-runner_1.0.bb
  - New systemd unit recipe to autostart `flutter-embedded`.
  - `SRC_URI = file://flutter-embedded.service`
  - `SYSTEMD_AUTO_ENABLE = "enable"`

- meta-local/recipes-graphics/flutter/files/flutter-embedded.service
  - Unit for Wayland session:
    - `Environment=WAYLAND_DISPLAY=wayland-0`
    - `Environment=XDG_RUNTIME_DIR=/run/user/0`
    - `ExecStart=/usr/bin/flutter-embedded`

- meta-local/recipes-core/images/imx-image-full.bbappend
  - `IMAGE_INSTALL += " flutter-sdk dart-sdk flutter-embedded flutter-embedded-runner"`

Offline/Repro Support
- flutter-sdk `do_compile` tries ${DL_DIR} before network for engine artifacts.
- Recognized local filenames:
  - `elinux-arm64-release.zip` (Sony prebuilt)
  - `linux-arm64-embedder-<engine>.zip`, `linux-arm64-embedder.zip`, `linux-arm64-flutter-gtk.zip`

Build Instructions
1) Setup
   - `source ./setup-environment Model_AB_Infinity`

2) Optional offline prep
   - Place `downloads/elinux-arm64-release.zip` (Sony prebuilt) into `${DL_DIR}`.

3) Build SDKs and embedder
   - `bitbake -c cleansstate dart-sdk flutter-sdk flutter-embedded`
   - `bitbake dart-sdk`
   - `bitbake flutter-sdk`
   - `bitbake flutter-embedded`

4) Image
   - `bitbake imx-image-full`

Runtime Notes
- The `flutter-embedded` binary is installed to `/usr/bin/flutter-embedded`.
- The `flutter-embedded-runner` unit will auto-start after Weston.
- If Weston runs as a non-root user or different display name, adjust the unit to match your session (e.g., set `XDG_RUNTIME_DIR`, `WAYLAND_DISPLAY`).

QA Decisions and Rationale
- `already-stripped` and `ldflags` ignored for upstream prebuilt/Snapshot files to avoid false positives and preserve upstream binaries.
- Target package prunes host tools and x86_64 artifacts to avoid architecture QA failures.

Next Steps (Optional)
- Split a minimal `flutter-engine` runtime package and reserve `flutter-sdk` for host tools only (smaller image, cleaner deps).
- Provide user-scope systemd unit if Weston is not run as root.

