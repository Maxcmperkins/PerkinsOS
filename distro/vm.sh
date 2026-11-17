#!/bin/bash
# The stage 1 test machine (docs/PLAN-DAILY-DRIVER.md): Ubuntu 26.04 in
# QEMU, with the PerkinsOS desktop installed on it by distro/Makefile.
#
#   distro/vm.sh create        make the VM's disk from Ubuntu's cloud image (once)
#   distro/vm.sh start         boot it and wait until it answers
#   distro/vm.sh install       copy this repo in, build it, install it
#   distro/vm.sh ssh [CMD]     a shell in the VM, or run CMD there
#   distro/vm.sh shot OUT.png  screenshot of the VM's screen
#   distro/vm.sh stop          shut it down
#   distro/vm.sh destroy       delete the VM's disk (keeps the download)
#
# `start` opens a window if this terminal has a display; HEADLESS=1 stops
# that. The user in the VM is "max": no password, logs in on the screen
# automatically, and sudo needs no password. It is a throwaway test VM,
# reachable only from this machine (ssh on 127.0.0.1:2222).
set -eu
cd "$(dirname "$0")/.."

RELEASE=26.04
CODENAME=resolute
IMAGE_NAME=ubuntu-$RELEASE-server-cloudimg-amd64.img
IMAGE_URL=https://cloud-images.ubuntu.com/releases/$CODENAME/release
IMAGE=dl/ubuntu/$IMAGE_NAME
VM=output/distro-vm            # everything about this VM; output/ is not committed
SSH_PORT=2222
SEED_PORT=8765
# Built up in two steps so the string has single spaces only: rsync splits
# it on spaces, and a tab from a line continuation would end up inside an
# option.
SSH="ssh -i $VM/id_ed25519 -p $SSH_PORT -o StrictHostKeyChecking=no"
SSH="$SSH -o UserKnownHostsFile=$VM/known_hosts -o LogLevel=ERROR -o ConnectTimeout=5"

# Ubuntu's packages for building and running the desktop (see distro/Makefile).
PACKAGES="build-essential pkg-config libwlroots-0.19-dev libwayland-dev \
	wayland-protocols libxkbcommon-dev libcairo2-dev \
	foot fonts-dejavu-core rsync \
	fonts-noto-core fonts-noto-cjk fonts-noto-color-emoji \
	xdg-desktop-portal xdg-desktop-portal-gtk"

# Download the cloud image and refuse it if the checksum is wrong.
fetch() {
	[ -f "$IMAGE" ] && return
	mkdir -p dl/ubuntu
	wget -c -O "$IMAGE.part" "$IMAGE_URL/$IMAGE_NAME"
	wget -q -O dl/ubuntu/SHA256SUMS "$IMAGE_URL/SHA256SUMS"
	expected=$(grep "$IMAGE_NAME\$" dl/ubuntu/SHA256SUMS | cut -d' ' -f1)
	echo "$expected  $IMAGE.part" | sha256sum -c -
	mv "$IMAGE.part" "$IMAGE"
}

create() {
	[ ! -e "$VM/disk.qcow2" ] || { echo "$VM/disk.qcow2 already exists (use: destroy)"; exit 1; }
	fetch
	mkdir -p "$VM/seed"
	# A key pair just for this VM, so nothing of yours is used or changed.
	ssh-keygen -q -t ed25519 -N '' -C perkinsos-vm -f "$VM/id_ed25519"
	# The disk records only differences from the downloaded image, which
	# stays untouched. 20G is its maximum size, not what it uses now.
	qemu-img create -q -f qcow2 -b "$PWD/$IMAGE" -F qcow2 "$VM/disk.qcow2" 20G
	# First-boot settings, read by cloud-init inside the image.
	echo "instance-id: perkinsos-vm-1" > "$VM/seed/meta-data"
	cat > "$VM/seed/user-data" <<EOF
#cloud-config
hostname: perkinsos-ubuntu
users:
  - name: max
    gecos: Max
    shell: /bin/bash
    sudo: "ALL=(ALL) NOPASSWD:ALL"
    lock_passwd: true
    ssh_authorized_keys:
      - $(cat "$VM/id_ed25519.pub")
write_files:
  # Log "max" in on the first console without asking for a password.
  - path: /etc/systemd/system/getty@tty1.service.d/autologin.conf
    content: |
      [Service]
      ExecStart=
      ExecStart=-/sbin/agetty --autologin max --noclear %I \$TERM
EOF
	touch "$VM/first-boot"
	echo "Created $VM/disk.qcow2. Next: distro/vm.sh start"
}

running() {
	[ -f "$VM/qemu.pid" ] && kill -0 "$(cat "$VM/qemu.pid")" 2>/dev/null
}

start() {
	[ -e "$VM/disk.qcow2" ] || { echo "No VM yet. Run: distro/vm.sh create"; exit 1; }
	running && { echo "Already running."; return; }

	# KVM runs the VM on the real CPU. Without it QEMU emulates the CPU in
	# software, which works but is many times slower.
	if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
		accel="-enable-kvm -cpu host"
	else
		echo "WARNING: no access to /dev/kvm, using slow software emulation."
		accel="-accel tcg,thread=multi -cpu max"
	fi
	display=none
	if [ -z "${HEADLESS:-}" ] && [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then
		display=gtk,zoom-to-fit=on
	fi
	# On the first boot only, hand cloud-init its settings: it fetches them
	# from a tiny web server on this machine (10.0.2.2 is "the host" as seen
	# from inside the VM).
	seed=
	if [ -e "$VM/first-boot" ]; then
		python3 -m http.server "$SEED_PORT" --bind 127.0.0.1 --directory "$VM/seed" \
			> "$VM/seed.log" 2>&1 &
		echo $! > "$VM/seed.pid"
		seed="-smbios type=1,serial=ds=nocloud;s=http://10.0.2.2:$SEED_PORT/"
	fi

	# Same virtual hardware as `make run`, plus more memory for a browser
	# and port 2222 on this machine forwarded to the VM's ssh.
	# setsid: run QEMU in a session of its own. Otherwise it is shut down
	# (by a "hang up" signal) when the terminal that started it closes.
	# shellcheck disable=SC2086
	setsid qemu-system-x86_64 -M pc -m 4096 -smp 4 $accel \
		-drive file="$VM/disk.qcow2",if=virtio \
		-netdev user,id=net0,hostfwd=tcp:127.0.0.1:$SSH_PORT-:22 \
		-device virtio-net-pci,netdev=net0 \
		-device virtio-vga,xres=1280,yres=800 \
		-device virtio-keyboard-pci -device virtio-tablet-pci \
		-display $display -serial file:"$VM/serial.log" \
		-monitor unix:"$VM/monitor.sock",server,nowait \
		-qmp unix:"$VM/qmp.sock",server,nowait \
		-pidfile "$VM/qemu.pid" $seed > "$VM/qemu.log" 2>&1 &

	# QEMU writes its pidfile a moment after it starts; `running` needs it.
	for _ in $(seq 20); do [ -f "$VM/qemu.pid" ] && break; sleep 0.5; done

	printf 'Waiting for the VM to answer on ssh'
	for _ in $(seq 240); do
		if $SSH max@127.0.0.1 true 2>/dev/null; then
			echo " ok"
			if [ -e "$VM/first-boot" ]; then
				$SSH max@127.0.0.1 'cloud-init status --wait > /dev/null 2>&1 || true'
				kill "$(cat "$VM/seed.pid")" 2>/dev/null || true
				rm -f "$VM/first-boot" "$VM/seed.pid"
			fi
			return
		fi
		if ! running; then
			echo; echo "QEMU exited:"; cat "$VM/qemu.log"
			if grep -q "gtk initialization failed" "$VM/qemu.log"; then
				echo "QEMU could not open a window. On WSL this means the part that"
				echo "shows Linux windows on Windows (WSLg) is not running. Restart"
				echo "WSL: in PowerShell run 'wsl --shutdown', then open Ubuntu again."
				echo "Or start without a window: HEADLESS=1 distro/vm.sh start"
			fi
			exit 1
		fi
		printf .
		sleep 5
	done
	echo; echo "Timed out. The VM's console output is in $VM/serial.log"; exit 1
}

vm_ssh() {
	# shellcheck disable=SC2029
	$SSH max@127.0.0.1 "$@"
}

install() {
	running || { echo "Not running. Run: distro/vm.sh start"; exit 1; }
	# Only install packages when one is missing: it is the slow part.
	vm_ssh "dpkg -s $PACKAGES > /dev/null 2>&1 || {
		sudo apt-get update -q &&
		sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -q $PACKAGES; }"
	rsync -a --delete -e "$SSH" \
		--exclude output --exclude dl --exclude .buildroot --exclude .git \
		--exclude distro/build ./ max@127.0.0.1:PerkinsOS/
	vm_ssh 'cd PerkinsOS && make -C distro && sudo make -C distro install'
	# Start the desktop when "max" logs in on the first console. Not `exec`:
	# if the compositor fails you are left at a shell prompt, not a login loop.
	vm_ssh 'grep -q perkinsos-start ~/.profile || cat >> ~/.profile <<"EOF"

# PerkinsOS: start the desktop when logging in on the first console.
if [ "$(tty)" = /dev/tty1 ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
	perkinsos-start
fi
EOF'
	echo "Installed. Restart the desktop with: distro/vm.sh ssh sudo systemctl restart getty@tty1"
}

case "${1:-}" in
create) create ;;
start) start ;;
install) install ;;
ssh) shift; vm_ssh "$@" ;;
shot) python3 ci/screenshot.py "$VM/monitor.sock" "$2" ;;
stop)
	running || { echo "Not running."; exit 0; }
	vm_ssh sudo poweroff 2>/dev/null || true
	for _ in $(seq 60); do running || break; sleep 1; done
	running && kill "$(cat "$VM/qemu.pid")"
	echo "Stopped."
	;;
destroy)
	running && kill "$(cat "$VM/qemu.pid")"
	rm -rf "$VM"
	echo "Deleted $VM."
	;;
*) sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'; exit 1 ;;
esac
