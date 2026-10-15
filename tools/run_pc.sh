#!/bin/bash
# Runs the port on this PC under the system qemu-arm with software GL (llvmpipe), no window.
# Usage: tools/run_pc.sh [frames]      (default 300; log in logs/pc_run.log)
# A screenshot while it runs:  echo "screenshot NAME" >> ctl/commands   -> ctl/screenshots/NAME.png
P=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$P/ctl" "$P/logs"
: > "$P/ctl/commands"
cd "$P/port" || exit 1
LOADER_TRACE=1 SG_CONTROL_DIR="$P/ctl" SDL_VIDEODRIVER=offscreen LIBGL_ALWAYS_SOFTWARE=1 EGL_PLATFORM=surfaceless \
SDL_AUDIODRIVER=dummy SG_WIDTH=${SG_WIDTH:-800} SG_HEIGHT=${SG_HEIGHT:-480} \
SG_FRAME_LIMIT="${1:-300}" \
    timeout "${SG_TIMEOUT:-900}" qemu-arm -L android/build/sysroot build/shadowguardian "$P/sgdata" > "$P/logs/pc_run.log" 2>&1
echo "exit code $?"
grep -E "summary|run finished|FATAL|unresolved|SIGSEGV|zip mount" "$P/logs/pc_run.log" | head -20
