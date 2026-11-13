#!/bin/bash
# Headless desktop test: boot, start the compositor with a terminal in it,
# screenshot the VM's screen and check something was drawn.
# Usage: desktop-test.sh [COMPOSITOR]   (default: tinywl)
# The screenshot is left in output/desktop-test.png for a human to look at.
set -u
. "$(dirname "$0")/lib.sh"

COMPOSITOR=${1:-tinywl}
SHOT=output/desktop-test.png

start_vm
login_root

# -s runs a command once the compositor is up; the log goes to a file so
# it doesn't mix with our serial console.
send "$COMPOSITOR -s foot > /tmp/compositor.log 2>&1 &"
# check NAME COMMAND: poll COMMAND inside the VM for up to 10 s, then print
# NAME=yes or NAME=no. On "no", dump the compositor log and fail.
check() {
	send "for i in \$(seq 50); do $2 && break; sleep 0.2; done; $2 && echo $1=yes || echo $1=no"
	wait_for "^$1="
	grep -aq "^$1=yes" "$LOG" || {
		send 'tail -20 /tmp/compositor.log'; sleep 2
		fail "$3"
	}
}

check socket '[ -S $XDG_RUNTIME_DIR/wayland-0 ]' "$COMPOSITOR did not create a Wayland socket"
echo "ok: $COMPOSITOR is running (wayland-0 socket exists)"

check client 'pidof foot >/dev/null' "foot did not start"
echo "ok: foot connected and is running"

sleep 3                          # let the first frames reach the screen
RESULT=$(python3 "$(dirname "$0")/screenshot.py" "$MONITOR" "$SHOT") || fail "screenshot failed"
echo "ok: screenshot $SHOT ($RESULT)"

# A blank screen has one colour. Background + window + text has many.
COLOURS=${RESULT##*colours=}
[ "$COLOURS" -ge 3 ] || fail "screen looks blank ($COLOURS colours)"

stop_vm
echo "PASS"
