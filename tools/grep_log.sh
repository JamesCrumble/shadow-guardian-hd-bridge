#!/bin/sh
# grep -E "$1" over the phone's game log (last $2 lines, default 40).
P=${SG_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}
A=$P/tools/adb
D=/sdcard/BridgeGames/com.jamescrumble.shadowguardian
[ -n "$($A shell ls $D/sg.log 2>/dev/null)" ] || D=/sdcard/Android/data/com.jamescrumble.shadowguardian/files
F=$D/sg.log
$A shell "grep -aE '$1' $F | tail -${2:-40}"
