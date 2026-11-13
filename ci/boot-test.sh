#!/bin/bash
# Headless boot test: boot the image in QEMU, wait for the login prompt,
# log in as root, run a command, power off. Exits non-zero if any step
# doesn't happen in time. Used by `make test` and by CI.
set -u
. "$(dirname "$0")/lib.sh"

start_vm
login_root

send 'echo kernel=$(uname -r)'
wait_for '^kernel=[0-9]'
echo "ok: $(grep -a '^kernel=' "$LOG" | tr -d '\r')"

stop_vm
echo "PASS"
