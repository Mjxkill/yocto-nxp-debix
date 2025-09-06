#!/usr/bin/env bash
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LAYER_DIR="$REPO_DIR/sources"

clone_layer() {
  local repo=$1
  local dest=$2
  if [ ! -d "$dest" ]; then
    git clone "$repo" "$dest"
  else
    echo "$dest already exists, skipping clone"
  fi
}

clone_layer https://github.com/kraj/meta-clang.git "$LAYER_DIR/meta-clang"
clone_layer https://github.com/sony/meta-flutter.git "$LAYER_DIR/meta-flutter"

cat <<'EOM'
meta-flutter and its dependency meta-clang have been cloned into the sources directory.
After sourcing your build environment run:
  bitbake-layers add-layer ../sources/meta-clang ../sources/meta-flutter
and append the following to conf/local.conf to include Dart and Flutter in images:
  IMAGE_INSTALL:append = " flutter-engine flutter-embedded-linux"
The flutter-sdk recipe also provides the dart command-line tools on the host.
EOM
