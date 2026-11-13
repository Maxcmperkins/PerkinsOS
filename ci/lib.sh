# Shared helpers for the CI tests. Source this file; don't run it.
#
# How it works: `make run` wires the VM's serial console to stdin/stdout.
# We feed stdin from a FIFO so we can "type" into the VM, and send stdout
# to a log file that we poll for the text we expect. QEMU's monitor is on a
# unix socket so tests can ask QEMU itself for things like screenshots.

TIMEOUT=${TIMEOUT:-180}          # seconds allowed per step (slow without KVM)
WORK=$(mktemp -d)
LOG=$WORK/serial.log
MONITOR=$WORK/monitor.sock

# start_vm: boot the image with no window and wait for the login prompt.
start_vm() {
	mkfifo "$WORK/in"
	make -s run QEMU_DISPLAY=none \
		QEMU_EXTRA="-monitor unix:$MONITOR,server,nowait" \
		< "$WORK/in" > "$LOG" 2>&1 &
	QEMU_PID=$!
	exec 3> "$WORK/in"           # keep the FIFO open for writing
	START=$(date +%s.%N)
	wait_for 'perkinsos login:'
	echo "ok: login prompt after $(echo "$(date +%s.%N) - $START" | bc)s"
}

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

# send TEXT: type a line into the VM's serial console.
send() {
	echo "$1" >&3
}

# press KEYS: press a key combination on the VM's keyboard, as if typed in
# the QEMU window. KEYS uses QEMU's names joined by dashes, e.g. alt-ret.
press() {
	python3 -c '
import socket, sys, time
s = socket.socket(socket.AF_UNIX)
s.connect(sys.argv[1])
s.sendall(("sendkey " + sys.argv[2] + "\n").encode())
time.sleep(0.5)' "$MONITOR" "$1"
}

# login_root: log in at the prompt and wait for a shell.
login_root() {
	send root
	wait_for '^# '
	echo "ok: logged in as root"
}

# stop_vm: power off cleanly and check QEMU exits without error.
stop_vm() {
	send poweroff
	wait "$QEMU_PID" || fail "QEMU exited with an error"
	echo "ok: clean power off"
	rm -rf "$WORK"
}
