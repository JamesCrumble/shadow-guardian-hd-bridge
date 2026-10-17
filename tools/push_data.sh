#!/bin/sh
# Copies the game's APK and the cache zip (as it is, not unpacked) into the app's folder on the phone
# and opens the folders/files to the app (adb push makes them shell-owned).
# Usage: push_data.sh [original|community|all]   (default all)
#   original  gamebin/origin/    (*.apk, GloftSGHP.zip)
#   community gamebin/comunity/  (*.apk, *.zip)
# The app tells the editions apart by content, so the names do not matter; both can lie in the folder at once.
P=$(cd "$(dirname "$0")/.." && pwd)
A=$P/tools/adb
D=/sdcard/Android/data/com.jamescrumble.shadowguardian/files
$A shell mkdir -p $D
push_dir() {
  for f in "$1"/*.apk "$1"/*.zip; do
    [ -f "$f" ] || continue
    $A push "$(wslpath -w "$f")" "$D/$(basename "$f")" || exit 1
  done
}
case "${1:-all}" in
  original)  push_dir "$P/gamebin/origin" ;;
  community) push_dir "$P/gamebin/comunity" ;;
  all)       push_dir "$P/gamebin/origin"; push_dir "$P/gamebin/comunity" ;;
  *) echo "usage: $0 [original|community|all]"; exit 2 ;;
esac
$A shell "chmod 2777 /sdcard/Android/data/com.jamescrumble.shadowguardian $D; chmod 666 $D/*"
$A shell ls -la $D
