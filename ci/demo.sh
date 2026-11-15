#!/bin/bash
# Record the demo: boot headless, use the desktop with injected key presses,
# screenshot each step, and join the screenshots into docs/demo.gif.
# Run with `make demo`. Frames are kept in output/demo/.
set -u
. "$(dirname "$0")/lib.sh"

FRAMES=output/demo
rm -rf "$FRAMES"
mkdir -p "$FRAMES" docs
N=0

# frame: wait for the screen to settle, then save the next numbered frame.
frame() {
	sleep "${1:-1.5}"
	N=$((N + 1))
	python3 "$(dirname "$0")/screenshot.py" "$MONITOR" \
		"$(printf '%s/%02d.png' "$FRAMES" "$N")" > /dev/null || fail "screenshot failed"
}

# type_text TEXT: press the key for each character (letters, digits, space,
# dash and slash are enough for the demo).
type_text() {
	local i char
	for ((i = 0; i < ${#1}; i++)); do
		char=${1:i:1}
		case "$char" in
			' ') press spc ;;
			'-') press minus ;;
			'/') press slash ;;
			*) press "$char" ;;
		esac
	done
}

start_vm
login_root
send 'for i in $(seq 50); do pidof perkinsos-dock >/dev/null && break; sleep 0.2; done; echo dock-ready'
wait_for '^dock-ready'

frame 3                 # 1: the desktop: wallpaper and dock

press alt-p
frame                   # 2: launcher open, all apps listed
type_text sys
frame                   # 3: filtered to System Info
press ret
frame 2.5               # 4: System Info window

press alt-ret
frame 3                 # 5: a terminal opens on top, focused (blue border)
type_text 'uname -a'
press ret
frame                   # 6: a command typed into the terminal

press alt-f1
frame                   # 7: Alt+F1 switches focus back to System Info

stop_vm

python3 "$(dirname "$0")/gif.py" docs/demo.gif 1.6 "$FRAMES"/*.png
cp "$FRAMES/06.png" docs/screenshot.png
ls -l docs/demo.gif docs/screenshot.png
