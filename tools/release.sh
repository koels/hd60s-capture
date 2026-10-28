#!/bin/sh
# Builds a release zip and folds it into the appcast Sparkle reads.
#
# Needs, once:
#   packaging/appcast-url      the https URL the appcast will be served from
#   .deps/sparkle/bin/generate_keys   run once; the private key goes into your
#                                     login Keychain and the public half into
#                                     packaging/appcast-key.pub
# Both are read by the Makefile and baked into the app's Info.plist, so build
# after setting them or the app ships with no feed.
set -eu
cd "$(dirname "$0")/.."
version=$(cat VERSION)
feed=$(cat packaging/appcast-url 2>/dev/null || true)
[ -n "$feed" ] || { echo "packaging/appcast-url is empty; nothing to release to" >&2; exit 1; }
[ -s packaging/appcast-key.pub ] || { echo "packaging/appcast-key.pub is empty; run .deps/sparkle/bin/generate_keys --account hd60s-capture" >&2; exit 1; }

make bundle
out=build/releases
mkdir -p "$out"
zip="$out/HD60-S-Capture-$version.zip"
rm -f "$zip"
# ditto keeps the signature and the symlinks inside the frameworks intact;
# zip(1) flattens them and the update then fails to validate.
ditto -c -k --sequesterRsrc --keepParent "build/HD60 S Capture.app" "$zip"

# generate_appcast signs every zip in the folder and writes appcast.xml beside
# them, reusing the private key from the Keychain.
.deps/sparkle/bin/generate_appcast --account hd60s-capture --download-url-prefix "$(dirname "$feed")/" "$out"

echo
echo "Upload these to $(dirname "$feed")/ :"
ls -1 "$out"/*.zip "$out"/appcast.xml
