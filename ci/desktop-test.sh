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
fi

stop_vm
echo "PASS"
