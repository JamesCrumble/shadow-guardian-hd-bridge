#!/bin/sh
# Copies the game's APK and the cache zip (as it is, not unpacked) into the app's folder on the phone
# and opens the folders/files to the app (adb push makes them shell-owned).
P=$(cd "$(dirname "$0")/.." && pwd)
A=$P/tools/adb
D=/sdcard/Android/data/com.jamescrumble.shadowguardian/files
$A shell mkdir -p $D
for f in "$P/gamebin/"*.apk "$P/gamebin/GloftSGHP.zip"; do
  $A push "$(wslpath -w "$f")" "$D/$(basename "$f")" || exit 1
done
$A shell "chmod 2777 /sdcard/Android/data/com.jamescrumble.shadowguardian $D; chmod 666 $D/*"
$A shell ls -la $D
