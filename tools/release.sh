#!/bin/sh
# Builds, signs and publishes a release to GitHub, where the app and the OBS
# source both look for updates.
#
# Needs, once: packaging/appcast-url (the feed URL, baked into the app) and the
# signing key, made by .deps/sparkle/bin/generate_keys --account hd60s-capture.
# The private half lives in your login Keychain; the public half is
# packaging/appcast-key.pub.
#
# The feed is releases/latest/download/appcast.xml, so it always resolves to the
# newest release. Each release carries an appcast with only its own entry: an
# older entry would point at an older tag, and Sparkle only needs the newest.
set -eu
cd "$(dirname "$0")/.."
version=$(cat VERSION)
tag="v$version"
feed=$(cat packaging/appcast-url 2>/dev/null || true)
[ -n "$feed" ] || { echo "packaging/appcast-url is empty; nothing to release to" >&2; exit 1; }
[ -s packaging/appcast-key.pub ] || { echo "packaging/appcast-key.pub is empty; run .deps/sparkle/bin/generate_keys --account hd60s-capture" >&2; exit 1; }
gh release view "$tag" >/dev/null 2>&1 && { echo "$tag is already released; bump VERSION first" >&2; exit 1; }

# Refuse a different app's Keychain key before building or publishing.
public_key=$(.deps/sparkle/bin/generate_keys --account hd60s-capture -p)
[ "$public_key" = "$(cat packaging/appcast-key.pub)" ] || { echo "Sparkle signing key does not match this app" >&2; exit 1; }

make bundle
make test
make test-shared
codesign --verify --deep --strict "build/HD60 S Capture.app"

out=build/releases/$version
rm -rf "$out"
mkdir -p "$out"
zip="$out/HD60-S-Capture-$version.zip"
# ditto keeps the signature and the symlinks inside the frameworks intact;
# zip(1) flattens them and the update then fails to validate.
ditto -c -k --sequesterRsrc --keepParent "build/HD60 S Capture.app" "$zip"
.deps/sparkle/bin/generate_appcast --account hd60s-capture \
 --download-url-prefix "${feed%/latest/download/appcast.xml}/download/$tag/" "$out"

if [ -z "${RELEASE_NOTES:-}" ]; then printf 'Version %s.\n' "$version" > build/release-notes.md; fi
gh release create "$tag" "$zip" "$out/appcast.xml" --title "HD60 S Capture $version" --notes-file "${RELEASE_NOTES:-build/release-notes.md}"
echo "Released $tag; the feed now serves it."
