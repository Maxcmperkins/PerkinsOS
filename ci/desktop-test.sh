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
	# window_is shown|gone: wait until a lone window at 40,40 is on screen,
	# or has left it. Its pixel 300,200 is dark terminal; without it, it
	# is bright sky. pidof is not enough either way: a program can be
	# running seconds before its window shows (no KVM), and it can still
	# be listed after it exits (a zombie, if its parent never waits).
	window_is() {
		local i want=0
		[ "$1" = gone ] && want=1
		for i in $(seq 40); do
			python3 "$(dirname "$0")/screenshot.py" "$MONITOR" "$WORK/shown.png" > /dev/null
			python3 - "$WORK/shown.png" $want <<'PY' && return 0
import sys
sys.path.insert(0, "ci")
from gif import read_png
w, h, rgb = read_png(sys.argv[1])
dark = max(rgb[(200 * w + 300) * 3:(200 * w + 300) * 3 + 3]) < 60
sys.exit(0 if dark == (sys.argv[2] == "0") else 1)
PY
			sleep 0.5
		done
		fail "the window at 40,40 is not $1"
	}

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

	# Launcher: a held key repeats. Type five letters that match nothing,
	# then hold Backspace: one press would delete one letter, so "sys" can
	# only find System Info if the key repeated and cleared the rest.
	press alt-p
	sleep 2
	for _ in 1 2 3 4 5; do press z; done
	hold backspace 4000
	press s; press y; press s
	python3 "$(dirname "$0")/screenshot.py" "$MONITOR" output/repeat-test.png > /dev/null
	press ret
	count repeated perkinsos-sysinfo
	grep -aq '^repeated=1' "$LOG" || fail "holding Backspace in the launcher did not repeat"
	echo "ok: a held key repeats in the launcher"
	press alt-q
	count repeatclosed perkinsos-sysinfo

	# Launcher: clicking a result starts it. The image has four entries
	# (TERMINAL, SYSTEM INFO, SHUT DOWN, RESTART), so the launcher is 248
	# tall, centred in the 720 pixels above the dock, and the second
	# result is centred on y 368. Any extra entry, such as foot's own
	# duplicates of TERMINAL, would move it: this checks those are hidden.
	press alt-p
	sleep 2
	mouse_move 512 368
	sleep 1
	python3 "$(dirname "$0")/screenshot.py" "$MONITOR" output/launcher-click-test.png > /dev/null
	mouse_click 512 368
	count resultclick perkinsos-sysinfo
	grep -aq '^resultclick=1' "$LOG" || fail "clicking a result in the launcher did not start it"
	count resultgone perkinsos-launcher
	grep -aq '^resultgone=0' "$LOG" || fail "the launcher stayed open after a result was clicked"
	echo "ok: clicking a launcher result started it"
	press alt-q
	count resultclosed perkinsos-sysinfo

	# Quitting the desktop takes Alt+Shift+Escape (checked at the end);
	# Alt+Escape alone must not do it.
	press alt-esc
	send 'sleep 2; kill -0 $(cat /run/desktop.pid); echo altesc=$?'
	wait_for '^altesc='
	grep -aq '^altesc=0' "$LOG" || fail "Alt+Escape alone quit the desktop"
	echo "ok: Alt+Escape alone does not quit the desktop"

	# Mouse: clicking a dock button starts its program. (TERMINAL is the
	# second button; see button_x() in shell/dock/main.c.)
	mouse_click 196 744
	count dockclick foot
	grep -aq '^dockclick=1' "$LOG" || fail "clicking the dock's TERMINAL button did not open a terminal"
	echo "ok: clicking TERMINAL on the dock opened a terminal"
	window_is shown
	press alt-q
	# Check the screen, not pidof: the dock does not wait() for what it
	# starts, so the closed terminal lingers as a zombie.
	window_is gone
	echo "ok: Alt+Q closed the terminal opened from the dock"

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

	# The cockpit's centre display: its buttons change the page (see pages.c
	# in shell/wallpaper, and key_position() in scene.c). SYSTEM INFO is the
	# third along the top, and shows a page, not a window. That page's only
	# button along the bottom, WINDOW, opens the window; HOME has no app
	# there, so the window opening also proves that the page changed.
	mouse_click 512 535
	count cockpitpage perkinsos-sysinfo
	grep -aq '^cockpitpage=0' "$LOG" || fail "the cockpit's SYSTEM INFO key opened a window, not a page"
	mouse_click 319 652
	count cockpitkey perkinsos-sysinfo
	grep -aq '^cockpitkey=1' "$LOG" || fail "the cockpit's SYSTEM page did not open, or its WINDOW key did not start the app"
	echo "ok: SYSTEM INFO on the cockpit panel showed a page, and WINDOW on it started the app"
	press alt-q
	count cockpitclosed perkinsos-sysinfo

	# APPS, the first button, lists every app. The second in the list, below
	# TERMINAL, is SYSTEM INFO, which shows the page again; then WINDOW.
	# (WINDOW just now went back to HOME, or the first click here would be
	# on that page's HOME button.)
	mouse_click 319 535
	sleep 1
	mouse_click 356 587
	sleep 1
	mouse_click 319 652
	count cockpitapp perkinsos-sysinfo
	grep -aq '^cockpitapp=1' "$LOG" || fail "the cockpit's list of apps did not work"
	echo "ok: the cockpit's APPS page lists apps that can be clicked"
	press alt-q
	count cockpitappclosed perkinsos-sysinfo

	# Maximize: a maximized window fills the screen except the dock. The
	# terminal reports its own size in character cells once it has been
	# resized. A normal foot window is 80x24 whatever the font, so
	# "maximized" means at least half as big again both ways. (A fixed row
	# count would depend on the font: VT323's cells are tall and narrow.)
	send 'WAYLAND_DISPLAY=wayland-0 foot --maximized sh -c "sleep 2; stty size > /tmp/maxsize; sleep 60" > /dev/null 2>&1 &'
	send 'sleep 4; echo cells=$(tr " " x < /tmp/maxsize)'
	wait_for '^cells=[0-9]'
	python3 "$(dirname "$0")/screenshot.py" "$MONITOR" output/maximize-test.png > /dev/null
	CELLS=$(grep -a '^cells=' "$LOG" | tr -d '\r' | cut -d= -f2)
	ROWS=${CELLS%x*}; COLS=${CELLS#*x}
	[ "$ROWS" -ge 36 ] && [ "$COLS" -ge 120 ] || fail "maximized terminal is only $CELLS cells"
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

	# Close it with the X on the title bar the compositor draws for foot
	# (foot speaks xdg-decoration). Maximized, the bar is the top 24
	# pixels of the screen and the 16-pixel close button sits 4 pixels in
	# from the right: 1004 to 1020 across, 4 to 20 down.
	# $! is that terminal's process (the last one started with &); other
	# terminals may still be running, so check this one. It must exit:
	# foot hangs up its shell first, which without KVM can take seconds.
	# Each command stays under 80 characters: the serial console wraps
	# longer ones, and a wrapped "xgone=" would look like the answer.
	mouse_click 1012 12
	send 'for i in $(seq 30); do kill -0 $! 2>/dev/null || break; sleep .5; done'
	send 'kill -0 $! 2>/dev/null; echo xgone=$?'
	wait_for '^xgone='
	[ "$(grep -a '^xgone=' "$LOG" | tr -d '\r' | cut -d= -f2)" = 1 ] ||
		fail "clicking the title bar's X did not close the terminal"
	echo "ok: the title bar's X closed the window"

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

	# Drag a window by its title bar, no Alt. A lone new terminal opens
	# at 40,40, so its bar runs from y 16 to 40. Drag it 200 right and
	# 200 down; then 100,100 shows the sky and 400,400 is still terminal.
	# (Not 60,60 as above: with no Alt+drag first, the cursor block is there.)
	send 'WAYLAND_DISPLAY=wayland-0 foot > /dev/null 2>&1 &'
	sleep 4
	python3 "$(dirname "$0")/screenshot.py" "$MONITOR" output/titlebar1-test.png > /dev/null
	# The window's name is on the bar, left of the buttons: some of its
	# pixels are the bright green of the text. ("foot" is the default.)
	python3 - output/titlebar1-test.png <<'PY' || fail "the title bar does not show the window's name"
import sys
sys.path.insert(0, "ci")
from gif import read_png
w, h, rgb = read_png(sys.argv[1])
bright = sum(1 for y in range(18, 38) for x in range(44, 240)
             if rgb[(y * w + x) * 3 + 1] > 150)
sys.exit(0 if bright > 20 else 1)
PY
	echo "ok: the title bar shows the window's name"
	# Grab the bar by the name itself: clicks must go through the text.
	mouse_drag 56 28 256 228 left
	mouse_move 512 150
	sleep 1
	python3 "$(dirname "$0")/screenshot.py" "$MONITOR" output/titlebar2-test.png > /dev/null
	python3 - output/titlebar1-test.png output/titlebar2-test.png <<'PY' || fail "dragging the title bar did not move the window"
import sys
sys.path.insert(0, "ci")
from gif import read_png
def brightest(path, x, y):
    w, h, rgb = read_png(path)
    return max(rgb[(y * w + x) * 3:(y * w + x) * 3 + 3])
before, after = brightest(sys.argv[1], 100, 100), brightest(sys.argv[2], 100, 100)
inside = brightest(sys.argv[2], 400, 400)
sys.exit(0 if before < 60 and after > 120 and inside < 60 else 1)
PY
	echo "ok: dragging the title bar moved the window"
	press alt-q
	sleep 1

	# Minimise with the title bar's button, then get the window back with
	# Alt+Tab. A lone terminal opens at 40,40 and is 698 wide, so its
	# close button is centred on x 726 (as in the screenshots) and the
	# minimise button, one button-and-gap further left, on x 706.
	send 'WAYLAND_DISPLAY=wayland-0 foot > /dev/null 2>&1 &'
	window_is shown
	mouse_click 706 28
	mouse_move 512 150
	window_is gone
	send 'kill -0 $! 2>/dev/null; echo minrunning=$?'
	wait_for '^minrunning='
	grep -aq '^minrunning=0' "$LOG" || fail "minimising the window ended the app"
	echo "ok: the title bar's minimise button hid the window"
	press alt-tab
	window_is shown
	echo "ok: Alt+Tab brought the minimised window back"
	press alt-q
	window_is gone

	# Fullscreen: the window covers the dock too. Going to another window
	# shows the dock again, coming back hides it, and leaving fullscreen
	# (F11, bound here in foot) puts the window at 40,40 like any new one.
	# dock_is shown|hidden: wait for the top edge of the dock's buttons,
	# a run of green in the bottom 48 pixels, to be there or not.
	dock_is() {
		local i
		for i in $(seq 40); do
			python3 "$(dirname "$0")/screenshot.py" "$MONITOR" output/fullscreen-$2-test.png > /dev/null
			python3 - output/fullscreen-$2-test.png "$1" <<'PY' && return 0
import sys
sys.path.insert(0, "ci")
from gif import read_png
w, h, rgb = read_png(sys.argv[1])
green = sum(1 for y in range(h - 46, h) for x in range(0, 384)
            if rgb[(y * w + x) * 3 + 1] > 80)
sys.exit(0 if (green > 100) == (sys.argv[2] == "shown") else 1)
PY
			sleep 0.5
		done
		fail "fullscreen: the dock is not $1 ($2)"
	}
	dock_is shown before
	send 'WAYLAND_DISPLAY=wayland-0 foot -F -o key-bindings.fullscreen=F11 >/dev/null 2>&1 &'
	dock_is hidden on
	echo "ok: a fullscreen window covers the dock"
	press alt-p
	# The launcher is 420 wide and centred: its outline is at x 302.
	LAUNCHER=no
	for _ in $(seq 40); do
		python3 "$(dirname "$0")/screenshot.py" "$MONITOR" output/fullscreen-launcher-test.png > /dev/null
		python3 - output/fullscreen-launcher-test.png <<'PY' && LAUNCHER=yes && break
import sys
sys.path.insert(0, "ci")
from gif import read_png
w, h, rgb = read_png(sys.argv[1])
sys.exit(0 if max(rgb[(360 * w + x) * 3 + 1] for x in (302, 303)) > 150 else 1)
PY
		sleep 0.5
	done
	[ "$LAUNCHER" = yes ] || fail "the launcher did not open over a fullscreen window"
	echo "ok: the launcher opens over a fullscreen window"
	press s; press y; press s; press ret
	dock_is shown other
	echo "ok: opening another window over it shows the dock"
	press alt-tab
	dock_is hidden back
	echo "ok: Alt+Tab back to it hides the dock again"
	press f11
	dock_is shown off
	press alt-q
	sleep 2
	press alt-q
	window_is gone
	echo "ok: leaving fullscreen put the window and the dock back"

	# Alt+Shift+Escape quits the desktop. Start it again for the last test.
	press alt-shift-esc
	send 'for i in $(seq 30); do pidof perkinsos-compositor || break; sleep .5; done'
	send 'pidof perkinsos-compositor; echo quit=$?'
	wait_for '^quit='
	grep -aq '^quit=1' "$LOG" || fail "Alt+Shift+Escape did not quit the desktop"
	echo "ok: Alt+Shift+Escape quit the desktop"
	send '/etc/init.d/S99desktop start'
	check restarted 'pidof perkinsos-dock > /dev/null' "the desktop did not start again"
	sleep 5

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
