#!/bin/bash
# Boot once at full HD and save a screenshot of the desktop to
# docs/screenshot.png. Run by `make demo` after the GIF is recorded.
set -u
SCREEN=1920x1080
. "$(dirname "$0")/lib.sh"

start_vm
login_root
send 'for i in $(seq 50); do pidof perkinsos-dock >/dev/null && break; sleep 0.2; done; echo dock-ready'
wait_for '^dock-ready'
sleep 6                          # let the wallpaper draw and the radar fill
python3 "$(dirname "$0")/screenshot.py" "$MONITOR" docs/screenshot.png || fail "screenshot failed"
stop_vm
ls -l docs/screenshot.png
