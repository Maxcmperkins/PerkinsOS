#!/bin/bash
# Does the compositor's screen locking work? Runs it with no screen
# (wlroots' headless backend), so it needs no VM and no graphics card.
#
#   distro/test-lock.sh [COMPOSITOR]
#
# Skipped until the compositor offers ext-session-lock-v1; from then on it
# runs by itself in CI. STRICT=1 makes "not offered" a failure.
#
# This checks the protocol, not the picture: that no window shows through a
# locked screen has to be looked at in the VM.
set -u
COMPOSITOR=${1:-perkinsos-compositor}
XDG_RUNTIME_DIR=$(mktemp -d)
export XDG_RUNTIME_DIR WLR_BACKENDS=headless WLR_RENDERER=pixman
unset WAYLAND_DISPLAY DISPLAY
LOG=$XDG_RUNTIME_DIR/compositor.log
"$COMPOSITOR" > "$LOG" 2>&1 &
pid=$!
trap 'kill -9 $(jobs -p) 2> /dev/null; rm -rf "$XDG_RUNTIME_DIR"' EXIT

fail() { echo "FAIL: $1"; tail -20 "$LOG"; exit 1; }
for _ in $(seq 50); do
	socket=$(find "$XDG_RUNTIME_DIR" -type s -name 'wayland-*' | head -1)
	[ -n "$socket" ] && break
	kill -0 $pid 2> /dev/null || fail "the compositor did not start"
	sleep 0.1
done
[ -n "$socket" ] || fail "no Wayland socket"
WAYLAND_DISPLAY=$(basename "$socket")
export WAYLAND_DISPLAY

offered=$(timeout 10 wayland-info) || fail "wayland-info could not connect"
if ! echo "$offered" | grep -q ext_session_lock_manager_v1; then
	[ -z "${STRICT:-}" ] || fail "the compositor does not offer ext-session-lock-v1"
	echo "SKIP: the compositor does not offer ext-session-lock-v1 yet"
	exit 0
fi

# lock NAME: start swaylock; succeeds once it reports the screen locked.
# It writes a newline to --ready-fd when the compositor says so.
lock() {
	swaylock --ready-fd 3 3> "$XDG_RUNTIME_DIR/$1" &
	locker=$!
	for _ in $(seq 50); do
		[ -s "$XDG_RUNTIME_DIR/$1" ] && return 0
		kill -0 $locker 2> /dev/null || return 1
		sleep 0.1
	done
	kill $locker
	return 1
}
lock first || fail "swaylock could not lock"
first=$locker
echo "ok: locked"
lock second && fail "a second locker was accepted while locked"
echo "ok: a second locker is refused"
kill -9 "$first"; sleep 1
kill -0 $pid 2> /dev/null || fail "the compositor died with the locker"
echo "ok: the compositor survives the locker crashing"
lock third || fail "a new locker could not take over after a crash"
echo "ok: a new locker can take over"
kill -USR1 "$locker"; sleep 1     # swaylock unlocks on SIGUSR1
kill -0 "$locker" 2> /dev/null && fail "swaylock did not unlock"
lock fourth || fail "could not lock again after unlocking"
echo "ok: unlocked, and locked again"
echo PASS
