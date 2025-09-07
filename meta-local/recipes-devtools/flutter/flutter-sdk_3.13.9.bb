DESCRIPTION = "Flutter SDK for Linux"
HOMEPAGE = "https://flutter.dev"
LICENSE = "BSD-3-Clause"
LIC_FILES_CHKSUM = "file://LICENSE;md5=1d84cf16c48e571923f837136633a265"

SRC_URI = "https://storage.googleapis.com/flutter_infra_release/releases/stable/linux/flutter_linux_3.13.9-stable.tar.xz"
SRC_URI:class-native = "https://storage.googleapis.com/flutter_infra_release/releases/stable/linux/flutter_linux_3.13.9-stable.tar.xz"
SRC_URI:class-nativesdk = "https://storage.googleapis.com/flutter_infra_release/releases/stable/linux/flutter_linux_3.13.9-stable.tar.xz"
SRC_URI[sha256sum] = "b6bc6f93423488c67110e0fe56523cd2260f3a4c379ed015cd1c7fab66362739"
SRC_URI:class-native[sha256sum] = "b6bc6f93423488c67110e0fe56523cd2260f3a4c379ed015cd1c7fab66362739"
SRC_URI:class-nativesdk[sha256sum] = "b6bc6f93423488c67110e0fe56523cd2260f3a4c379ed015cd1c7fab66362739"
S = "${WORKDIR}/flutter"

inherit pkgconfig

BBCLASSEXTEND = "native nativesdk"

# Ensure the target sysroot contains the arm64 engine blob so that
# downstream recipes (e.g. flutter-embedded) can link against it during
# do_compile. The Flutter SDK archive only includes linux-x64 engine
# artifacts by default; for aarch64 targets we proactively download the
# linux-arm64 engine bundle that matches the SDK's engine revision.

SYSROOT_DIRS += "/opt"

do_compile() {
    # Pre-cache linux-arm64 engine artifacts into the SDK cache.
    # We avoid relying on host tools like wget/unzip by using Python.
    if [ "${TARGET_ARCH}" = "aarch64" ]; then
        export PYTHONUNBUFFERED=1
        export FLUTTER_S="${S}"
        export FLUTTER_DL_DIR="${DL_DIR}"
        python3 - <<'PY'
import os, sys, zipfile, urllib.request, urllib.error
S = os.environ['FLUTTER_S']
DL_DIR = os.environ.get('FLUTTER_DL_DIR')
engine_file = os.path.join(S,'bin','internal','engine.version')
with open(engine_file,'r') as f:
    engine = f.read().strip()
base = f"https://storage.googleapis.com/flutter_infra_release/flutter/{engine}"
candidates = [
    f"{base}/linux-arm64-embedder.zip",
    f"{base}/linux-arm64-release/linux-arm64-embedder.zip",
    f"{base}/linux-arm64-release/linux-arm64-flutter-gtk.zip",
]
destdir = os.path.join(S,'bin','cache','artifacts','engine','linux-arm64-release')
os.makedirs(destdir, exist_ok=True)
last_err = None
zip_path = os.path.join(destdir,'engine-artifacts.zip')
# 1) Try local pre-downloaded zip in DL_DIR (prefer Sony eLinux engine bundle)
local_candidates = []
if DL_DIR:
    local_candidates = [
        os.path.join(DL_DIR, "elinux-arm64-release.zip"),
        os.path.join(DL_DIR, f"linux-arm64-embedder-{engine}.zip"),
        os.path.join(DL_DIR, "linux-arm64-embedder.zip"),
        os.path.join(DL_DIR, "linux-arm64-flutter-gtk.zip"),
    ]
for lp in local_candidates:
    if os.path.exists(lp):
        print(f"Using local engine zip: {lp}")
        with open(lp, 'rb') as src, open(zip_path, 'wb') as dst:
            dst.write(src.read())
        break
else:
    # 2) Fallback to network download
    for url in candidates:
        try:
            print(f"Trying {url}")
            urllib.request.urlretrieve(url, zip_path)
            print(f"Downloaded {url}")
            break
        except urllib.error.HTTPError as e:
            last_err = e
            if e.code == 404:
                print(f"Not found: {url}")
                continue
            raise
    else:
        print("ERROR: Could not locate a suitable linux-arm64 engine artifact for Flutter.")
        print(f"Engine revision: {engine}")
        if last_err:
            print(f"Last error: {last_err}")
        sys.exit(1)

print("Unpacking engine artifacts...")
with zipfile.ZipFile(zip_path) as z:
    z.extractall(destdir)
os.remove(zip_path)

# Ensure libflutter_engine.so exists at expected location (or move it if nested).
found = None
for root, dirs, files in os.walk(destdir):
    if 'libflutter_engine.so' in files:
        found = os.path.join(root, 'libflutter_engine.so')
        break
if not found:
    print("ERROR: libflutter_engine.so not found in extracted engine artifacts.")
    sys.exit(1)
if os.path.dirname(found) != destdir:
    # Move to destdir for a stable path referenced by downstream builds
    target = os.path.join(destdir, 'libflutter_engine.so')
    if os.path.exists(target):
        os.remove(target)
    os.rename(found, target)
print("Done pre-caching linux-arm64 engine artifacts.")
PY
    fi
}

do_install() {
    install -d ${D}/opt/${PN}
    cp -r ${S}/* ${D}/opt/${PN}/
    install -d ${D}${bindir}
    ln -s /opt/${PN}/bin/flutter ${D}${bindir}/flutter
}

FILES:${PN} += "/opt/${PN}"

# Only bash needed for wrapper scripts; do not drag target dart-sdk via this package
RDEPENDS:${PN} += "bash fontconfig"

# Prebuilt upstream binaries are already stripped; silence QA for them.
INSANE_SKIP:${PN} += "already-stripped"
INHIBIT_PACKAGE_STRIP = "1"

# Prune host/x86_64 and unused artifacts from target package to satisfy QA
do_install:append() {
    if [ "${TARGET_ARCH}" = "aarch64" ]; then
        # Remove host dart-sdk bundled in Flutter SDK (x86_64)
        rm -rf ${D}/opt/${PN}/bin/cache/dart-sdk || true

        # Remove all linux-x64 engine artifacts and host tools
        rm -rf ${D}/opt/${PN}/bin/cache/artifacts/engine/linux-x64 || true
        rm -rf ${D}/opt/${PN}/bin/cache/artifacts/engine/linux-x64-profile || true
        rm -rf ${D}/opt/${PN}/bin/cache/artifacts/engine/linux-x64-release || true
        find ${D}/opt/${PN}/bin/cache/artifacts/engine -type d -path "*/linux-x64*" -prune -exec rm -rf {} + || true
        find ${D}/opt/${PN}/bin/cache/artifacts/engine -type d -path "*/android-*-*/linux-x64" -prune -exec rm -rf {} + || true

        # Keep only the core engine shared library for ARM64
        find ${D}/opt/${PN}/bin/cache/artifacts/engine/linux-arm64-release -maxdepth 1 -type f ! -name 'libflutter_engine.so' -delete || true
        # Drop optional eLinux platform libs to avoid unnecessary target deps
        find ${D}/opt/${PN}/bin/cache/artifacts/engine -type f -name 'libflutter_elinux_*.so' -delete || true
    fi
}
