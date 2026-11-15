#!/bin/bash
# Headless desktop test: boot, check the desktop came up by itself (see
# buildroot/overlay/etc/init.d/S99desktop), screenshot the VM's screen and
# check something was drawn.
# The screenshot is left in output/desktop-test.png for a human to look at.
set -u
. "$(dirname "$0")/lib.sh"

SHOT=output/desktop-test.png

start_vm
login_root

# check NAME COMMAND MESSAGE: poll COMMAND inside the VM for up to 10 s,
# then print NAME=yes or NAME=no. On "no", dump the desktop log and fail.
check() {
	send "for i in \$(seq 50); do $2 && break; sleep 0.2; done; $2 && echo $1=yes || echo $1=no"
	wait_for "^$1="
	grep -aq "^$1=yes" "$LOG" || {
		send 'tail -20 /var/log/desktop.log'; sleep 2
		fail "$3"
	}
}

check socket '[ -S $XDG_RUNTIME_DIR/wayland-0 ]' "no compositor running (no wayland-0 socket)"
send 'echo compositor=$(cat /proc/$(cat /run/desktop.pid)/comm)'
wait_for '^compositor='
echo "ok: desktop started at boot ($(grep -a '^compositor=' "$LOG" | tr -d '\r'))"

# Any one of the clients the session script may start.
check client 'pidof foot perkinsos-dock perkinsos-launcher >/dev/null' "no client connected to the compositor"
echo "ok: a client is running"

sleep 3                          # let the first frames reach the screen
RESULT=$(python3 "$(dirname "$0")/screenshot.py" "$MONITOR" "$SHOT") || fail "screenshot failed"
echo "ok: screenshot $SHOT ($RESULT)"

# A blank screen has one colour. Background + window + text has many.
COLOURS=${RESULT##*colours=}
[ "$COLOURS" -ge 3 ] || fail "screen looks blank ($COLOURS colours)"

# Keybindings only exist in our compositor, not in stock TinyWL.
if grep -aq '^compositor=perkinsos' "$LOG"; then
	# count NAME PROGRAM: print NAME=<number of PROGRAM processes>.
	count() { send "sleep 2; echo $1=\$(pidof $2 | wc -w)"; wait_for "^$1="; }

	count before foot
	BEFORE=$(grep -a '^before=' "$LOG" | tr -dc 0-9)
	press alt-ret
	count opened foot
	grep -aq "^opened=$((BEFORE + 1))" "$LOG" || fail "Alt+Enter did not open a terminal"
	echo "ok: Alt+Enter opened a terminal"

	press alt-q
	count closed foot
	grep -aq "^closed=$BEFORE" "$LOG" || fail "Alt+Q did not close the focused window"
	echo "ok: Alt+Q closed the focused window"

	# Layer-shell: the protocol is advertised, and the dock is using it.
	send 'echo layershell=$(wayland-info | grep -c zwlr_layer_shell_v1)'
	wait_for '^layershell='
	grep -aq '^layershell=1' "$LOG" || fail "compositor does not advertise layer-shell"
	echo "ok: compositor advertises zwlr_layer_shell_v1"

	# Launcher: Alt+P opens it, typing filters, Enter starts the app.
	press alt-p
	count launcher perkinsos-launcher
	grep -aq '^launcher=1' "$LOG" || fail "Alt+P did not open the launcher"
	press s; press y; press s
	python3 "$(dirname "$0")/screenshot.py" "$MONITOR" output/launcher-test.png > /dev/null
	press ret
	count app perkinsos-sysinfo
	grep -aq '^app=1' "$LOG" || fail "launcher did not start System Info"
	count launchergone perkinsos-launcher
	grep -aq '^launchergone=0' "$LOG" || fail "launcher did not exit after launching"
	echo "ok: launcher opened with Alt+P, filtered on 'sys' and started System Info"

	python3 "$(dirname "$0")/screenshot.py" "$MONITOR" output/app-test.png > /dev/null
	press alt-q
	count appclosed perkinsos-sysinfo
	grep -aq '^appclosed=0' "$LOG" || fail "Alt+Q did not close System Info"
	echo "ok: System Info closed on request"

	# Maximize: a maximized window fills the screen except the dock.
	send 'WAYLAND_DISPLAY=wayland-0 foot --maximized > /dev/null 2>&1 &'
	count maximized foot
	grep -aq '^maximized=1' "$LOG" || fail "foot --maximized did not start"
	python3 "$(dirname "$0")/screenshot.py" "$MONITOR" output/maximize-test.png > /dev/null
	# Compare with the first screenshot (empty desktop + dock): the pixel
	# just above the dock in the bottom-right corner must have changed from
	# wallpaper to window, and a pixel inside the dock must be unchanged.
	python3 - "$SHOT" output/maximize-test.png <<'PY' || fail "maximized window does not fill the area above the dock, or covers the dock"
import sys
sys.path.insert(0, "ci")
from gif import read_png
(w, h, before), (_, _, after) = read_png(sys.argv[1]), read_png(sys.argv[2])
def pixel(rgb, x, y):
    return rgb[(y * w + x) * 3:(y * w + x) * 3 + 3]
above_dock = (w - 3, h - 48 - 3)
in_dock = (w // 2, h - 10)
covered = pixel(before, *above_dock) != pixel(after, *above_dock)
dock_intact = pixel(before, *in_dock) == pixel(after, *in_dock)
sys.exit(0 if covered and dock_intact else 1)
PY
	echo "ok: a maximized window fills the screen and stops at the dock"
	press alt-q
fi

stop_vm
echo "PASS"
