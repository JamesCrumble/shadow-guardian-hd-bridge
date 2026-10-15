#!/bin/bash
# Builds a release bundle in release/<version>/ (git-ignored):
#   ShadowGuardian-Bridge-<v>.apk          release-signed APK (not debuggable)
#   ShadowGuardian-Bridge-<v>-src.zip      source of the committed HEAD
#   INSTALL.txt, SHA256SUMS
# The game's own APK and cache are the player's and are never part of a release.
# The release key is port/android/build/release.keystore (created on first use, git-ignored): keep a copy of it,
# a different key means the app has to be uninstalled before an update.
# Usage: tools/make_release.sh   (run from WSL; commit first - the source zip is HEAD)
set -euo pipefail
P=${SG_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}
source "$P/env.sh"
cd "$P"
Q="$P/qemu/out/qemu-arm-${SG_QEMU_VARIANT:-F}.stripped"
VER=$(grep -o 'versionName="[^"]*"' port/android/AndroidManifest.xml | head -1 | cut -d'"' -f2)
OUT="$P/release/$VER"
rm -rf "$OUT"; mkdir -p "$OUT"
bash build.sh > logs/build.log 2>&1
(cd port && QEMU_BIN="$Q" RELEASE=1 bash android/build_apk.sh > "$P/logs/apk_release.log" 2>&1)
tail -2 logs/apk_release.log
mv "port/ShadowGuardian-Bridge-$VER.apk" "$OUT/"
git archive --format=zip --prefix="ShadowGuardian-Bridge-$VER/" -o "$OUT/ShadowGuardian-Bridge-$VER-src.zip" HEAD
cp tools/release_install.txt "$OUT/INSTALL.txt"
(cd "$OUT" && sha256sum * > SHA256SUMS)
ls -la "$OUT"
