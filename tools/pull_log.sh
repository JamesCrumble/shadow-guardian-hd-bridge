#!/bin/sh
# Copies the phone's game log to logs/phone.log
P=${SG_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}
A="$P/tools/adb"
D=/sdcard/BridgeGames/com.jamescrumble.shadowguardian
[ -n "$($A shell ls $D/sg.log 2>/dev/null)" ] || D=/sdcard/Android/data/com.jamescrumble.shadowguardian/files
$A shell cat $D/sg.log | tr -d '\r' > "$P/logs/phone.log"
wc -l "$P/logs/phone.log"
