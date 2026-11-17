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

	# Mouse: clicking a dock button starts its program. (TERMINAL is the
	# second button; see button_x() in shell/dock/main.c.)
	mouse_click 196 744
	count dockclick foot
	grep -aq '^dockclick=1' "$LOG" || fail "clicking the dock's TERMINAL button did not open a terminal"
	echo "ok: clicking TERMINAL on the dock opened a terminal"
	press alt-q
	count dockclosed foot

	# Wallpaper: it animates, and clicking a radar contact locks it.
	python3 "$(dirname "$0")/screenshot.py" "$MONITOR" output/radar1-test.png > /dev/null
	sleep 1.7
	python3 "$(dirname "$0")/screenshot.py" "$MONITOR" output/radar2-test.png > /dev/null
	cmp -s output/radar1-test.png output/radar2-test.png && fail "the wallpaper is not animating"
	echo "ok: the wallpaper is animating"
	CONTACT=$(python3 "$(dirname "$0")/find-contact.py" output/radar1-test.png output/radar2-test.png) || fail "no contact on the radar"
	mouse_click $CONTACT
	mouse_move 512 150        # move away: a lock must outlive the hover
	sleep 1.5
	python3 "$(dirname "$0")/screenshot.py" "$MONITOR" output/radar-lock-test.png > /dev/null
	# "TGT LOCKED" and the program's name appear in the data display
	# between the two screens. Count full-brightness green pixels there:
	# with no target it only holds dim hint text.
	python3 - output/radar2-test.png output/radar-lock-test.png <<'PY' || fail "clicking a radar contact did not lock it"
import sys
sys.path.insert(0, "ci")
from gif import read_png
def bright(path):
    w, h, rgb = read_png(path)
    return sum(1 for y in range(560, 600) for x in range(276, 500)
               if rgb[(y * w + x) * 3 + 1] > 200)
sys.exit(0 if bright(sys.argv[1]) < 10 and bright(sys.argv[2]) > 60 else 1)
PY
	echo "ok: clicking a radar contact at $CONTACT locked it"

	# Cockpit keys: the buttons on the instrument panel start apps too.
	# SYSTEM INFO is the third button along the top of the centre display
	# (see key_position() in scene.c); click between the button and its name.
	mouse_click 512 535
	count cockpitkey perkinsos-sysinfo
	grep -aq '^cockpitkey=1' "$LOG" || fail "clicking the cockpit's SYSTEM INFO key did not start the app"
	echo "ok: clicking SYSTEM INFO on the cockpit panel started the app"
	press alt-q
	count cockpitclosed perkinsos-sysinfo

	# Maximize: a maximized window fills the screen except the dock. The
	# terminal reports its own size in character cells once it has been
	# resized (a normal foot window is 80x24).
	send 'WAYLAND_DISPLAY=wayland-0 foot --maximized sh -c "sleep 2; stty size > /tmp/maxsize; sleep 60" > /dev/null 2>&1 &'
	send 'sleep 4; echo cells=$(tr " " x < /tmp/maxsize)'
	wait_for '^cells=[0-9]'
	python3 "$(dirname "$0")/screenshot.py" "$MONITOR" output/maximize-test.png > /dev/null
	CELLS=$(grep -a '^cells=' "$LOG" | tr -d '\r' | cut -d= -f2)
	ROWS=${CELLS%x*}; COLS=${CELLS#*x}
	[ "$ROWS" -ge 40 ] && [ "$COLS" -ge 120 ] || fail "maximized terminal is only $CELLS cells"
	# The dock must still be visible: its top edge line and a button
	# outline are the same pixels as in the first screenshot.
	python3 - "$SHOT" output/maximize-test.png <<'PY' || fail "the maximized window covers the dock"
import sys
sys.path.insert(0, "ci")
from gif import read_png
(w, h, before), (_, _, after) = read_png(sys.argv[1]), read_png(sys.argv[2])
row = lambda rgb, y: rgb[y * w * 3:(y * w + 400) * 3]      # left 400 px: the buttons
sys.exit(0 if all(row(before, y) == row(after, y) for y in range(h - 48, h)) else 1)
PY
	echo "ok: a maximized window fills the screen ($CELLS cells) and stops at the dock"
	press alt-q
	sleep 1

	# Alt+drag: with Alt held, the right button resizes the window under
	# the pointer and the left button moves it. With no other window open,
	# a new one appears at 40,40 (see xdg_toplevel_map() in tinywl.c) and a
	# terminal is about 700x490 pixels. Drag its bottom-right quarter
	# outwards. The terminal writes down its size in character cells
	# before and after, and it must have grown both ways. (The first
	# reading waits two seconds: a terminal is 24x80 until its window has
	# been given its real size.)
	send 'WAYLAND_DISPLAY=wayland-0 foot sh -c "sleep 2; stty size > /tmp/size0; sleep 9; stty size > /tmp/size1; sleep 60" > /dev/null 2>&1 &'
	sleep 4
	python3 "$(dirname "$0")/screenshot.py" "$MONITOR" output/drag1-test.png > /dev/null
	mouse_alt_drag 600 420 700 500 right
	send 'sleep 9; echo dragcells=$(tr " " x < /tmp/size0)to$(tr " " x < /tmp/size1)'
	wait_for '^dragcells=[0-9]*x[0-9]*to[0-9]'
	CELLS=$(grep -a '^dragcells=' "$LOG" | tr -d '\r' | cut -d= -f2)
	BEFORE=${CELLS%to*}; AFTER=${CELLS#*to}
	[ "${AFTER%x*}" -gt "${BEFORE%x*}" ] && [ "${AFTER#*x}" -gt "${BEFORE#*x}" ] ||
		fail "Alt+right-drag did not make the terminal bigger ($BEFORE to $AFTER, rows x columns)"
	echo "ok: Alt+right-drag resized the window ($BEFORE to $AFTER, rows x columns)"

	# Now move it right and down. The point 60,60 was inside the window
	# (dark terminal background); afterwards the sky shows there.
	mouse_alt_drag 200 200 400 300 left
	mouse_move 512 150
	sleep 1
	python3 "$(dirname "$0")/screenshot.py" "$MONITOR" output/drag2-test.png > /dev/null
	python3 - output/drag1-test.png output/drag2-test.png <<'PY' || fail "Alt+left-drag did not move the window"
import sys
sys.path.insert(0, "ci")
from gif import read_png
def brightest(path, x, y):
    w, h, rgb = read_png(path)
    return max(rgb[(y * w + x) * 3:(y * w + x) * 3 + 3])
before, after = brightest(sys.argv[1], 60, 60), brightest(sys.argv[2], 60, 60)
inside = brightest(sys.argv[2], 300, 200)       # still covered after the move
sys.exit(0 if before < 60 and after > 120 and inside < 60 else 1)
PY
	echo "ok: Alt+left-drag moved the window"
	press alt-q
	sleep 1

	# Shut down from the launcher: SHUT DOWN is one of its last entries.
	press alt-p
	sleep 1
	press s; press h; press u; press t
	python3 "$(dirname "$0")/screenshot.py" "$MONITOR" output/shutdown-test.png > /dev/null
	press ret
	for _ in $(seq 120); do
		kill -0 "$QEMU_PID" 2>/dev/null || break
		sleep 0.5
	done
	kill -0 "$QEMU_PID" 2>/dev/null && fail "SHUT DOWN in the launcher did not power the machine off"
	wait "$QEMU_PID" || fail "QEMU exited with an error"
	echo "ok: SHUT DOWN in the launcher powered the machine off"
	rm -rf "$WORK"
	echo "PASS"
	exit 0
fi

stop_vm
echo "PASS"
