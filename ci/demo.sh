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

frame 4                 # 1: the desktop: formation, radar, RWR, dock
frame 0.6               # 2: a moment later, to show it moving

# Lock a radar contact with the mouse.
CONTACT=$(python3 "$(dirname "$0")/find-contact.py" "$FRAMES/01.png" "$FRAMES/02.png") || fail "no radar contact"
mouse_click $CONTACT
mouse_move 420 330
frame                   # 3: TGT LOCKED with the program's details

press alt-p
frame                   # 4: launcher open, all apps listed
type_text sys
frame                   # 5: filtered to System Info
press ret
frame 2.5               # 6: System Info window

mouse_click 196 744     # the dock's TERMINAL button
frame 3                 # 7: a terminal opens on top, focused
type_text 'uname -a'
press ret
frame                   # 8: a command typed into the terminal

press alt-f1
frame                   # 9: Alt+F1 switches focus back to System Info

stop_vm

python3 "$(dirname "$0")/gif.py" docs/demo.gif 1.5 "$FRAMES"/*.png
cp "$FRAMES/08.png" docs/screenshot.png
ls -l docs/demo.gif docs/screenshot.png
