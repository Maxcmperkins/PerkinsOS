#!/bin/bash
# Headless boot test: boot the image in QEMU, wait for the login prompt,
# log in as root, run a command, power off. Exits non-zero if any step
# doesn't happen in time. Used by `make test` and by CI.
#
# How it works: QEMU's serial console is wired to stdin/stdout (-nographic).
# We feed stdin from a FIFO so we can "type" into the VM, and send stdout
# to a log file that we poll for the text we expect.
set -u

TIMEOUT=${TIMEOUT:-180}          # seconds allowed per step (slow without KVM)
WORK=$(mktemp -d)
LOG=$WORK/serial.log
mkfifo "$WORK/in"

make -s run < "$WORK/in" > "$LOG" 2>&1 &
QEMU_PID=$!
exec 3> "$WORK/in"               # keep the FIFO open for writing
START=$(date +%s.%N)

fail() {
	echo "FAIL: $1"
	echo "--- last 30 lines of serial log ---"
	tail -30 "$LOG"
	kill "$QEMU_PID" 2>/dev/null
	exit 1
}

# wait_for PATTERN: block until PATTERN shows up in the serial log.
wait_for() {
	local deadline=$(( $(date +%s) + TIMEOUT ))
	until grep -aq "$1" "$LOG"; do
		kill -0 "$QEMU_PID" 2>/dev/null || fail "QEMU exited while waiting for '$1'"
		[ "$(date +%s)" -lt "$deadline" ] || fail "timed out waiting for '$1'"
		sleep 0.2
	done
}

wait_for 'perkinsos login:'
BOOT_SECS=$(echo "$(date +%s.%N) - $START" | bc)
echo "ok: login prompt after ${BOOT_SECS}s"

echo root >&3
wait_for '^# '
echo "ok: logged in as root"

echo 'echo kernel=$(uname -r)' >&3
wait_for '^kernel=[0-9]'
echo "ok: $(grep -a '^kernel=' "$LOG" | tr -d '\r')"

echo poweroff >&3
wait "$QEMU_PID" || fail "QEMU exited with an error"
echo "ok: clean power off"
echo "PASS"
rm -rf "$WORK"
