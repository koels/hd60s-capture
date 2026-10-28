#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p .deps
if [ ! -d .deps/obs/libobs ]; then
 git clone --depth 1 --branch 33.0.0 https://github.com/obsproject/obs-studio.git .deps/obs
fi
if [ ! -d .deps/simde/simde ]; then
 git clone --depth 1 --branch v0.8.2 https://github.com/simd-everywhere/simde.git .deps/simde
fi
# Sparkle ships a prebuilt framework. Pinned by version and checked by hash,
# because this one ends up inside the signed app and runs the update installer.
SPARKLE=2.10.1
SPARKLE_SHA=1a856d2a9edb8e628de88121d7be04cf0e2167f8641650b78fca00b14244cf27
if [ ! -d .deps/sparkle/Sparkle.framework ]; then
 mkdir -p .deps/sparkle-download .deps/sparkle
 archive=.deps/sparkle-download/Sparkle-$SPARKLE.tar.xz
 [ -f "$archive" ] || curl -fsSL -o "$archive" \
  "https://github.com/sparkle-project/Sparkle/releases/download/$SPARKLE/Sparkle-$SPARKLE.tar.xz"
 echo "$SPARKLE_SHA  $archive" | shasum -a 256 -c -
 tar -xf "$archive" -C .deps/sparkle
fi
