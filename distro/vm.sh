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
# that. The user in the VM is "max", password "perkinsos" (VM_PASSWORD
# below); sudo needs no password. After `install`, the VM logs max in to
# the desktop by itself when it boots; quit the desktop (Alt+Escape) to
# see the login screen. It is a throwaway test VM, reachable only from
# this machine (ssh on 127.0.0.1:2222, with a key, not the password).
#
# distro/usb.sh builds a second VM with this script, by setting VM,
# SSH_PORT, SEED_PORT, DISK_SIZE, VM_HOSTNAME and UEFI=1 (boot through
# UEFI firmware, as a laptop does, instead of the old BIOS way).
set -eu
cd "$(dirname "$0")/.."

RELEASE=26.04
CODENAME=resolute
IMAGE_NAME=ubuntu-$RELEASE-server-cloudimg-amd64.img
IMAGE_URL=https://cloud-images.ubuntu.com/releases/$CODENAME/release
IMAGE=dl/ubuntu/$IMAGE_NAME
VM=${VM:-output/distro-vm}     # everything about this VM; output/ is not committed
SSH_PORT=${SSH_PORT:-2222}
SEED_PORT=${SEED_PORT:-8765}
DISK_SIZE=${DISK_SIZE:-20G}
VM_HOSTNAME=${VM_HOSTNAME:-perkinsos-ubuntu}
# UEFI firmware for QEMU (Ubuntu's ovmf package): the code is shared and
# read-only; each VM keeps its own copy of the settings (boot entries).
OVMF_CODE=/usr/share/OVMF/OVMF_CODE_4M.fd
OVMF_VARS=/usr/share/OVMF/OVMF_VARS_4M.fd
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
	xdg-desktop-portal xdg-desktop-portal-gtk \
	xwayland x11-apps \
	mako-notifier libnotify-bin pipewire-audio wireplumber pavucontrol \
	gnome-keyring libsecret-tools mate-polkit pkexec greetd tuigreet"
# The test VM's password for "max", for the login screen and password
# prompts. Not a secret: the VM is a throwaway, ssh into it takes the key
# only, and nothing outside this machine can reach it.
VM_PASSWORD=perkinsos

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
	# stays untouched. DISK_SIZE is its maximum size, not what it uses now.
	qemu-img create -q -f qcow2 -b "$PWD/$IMAGE" -F qcow2 "$VM/disk.qcow2" "$DISK_SIZE"
	# First-boot settings, read by cloud-init inside the image.
	echo "instance-id: perkinsos-$(basename "$VM")-1" > "$VM/seed/meta-data"
	cat > "$VM/seed/user-data" <<EOF
#cloud-config
hostname: $VM_HOSTNAME
users:
  - name: max
    gecos: Max
    shell: /bin/bash
    sudo: "ALL=(ALL) NOPASSWD:ALL"
    # In the sudo group, as a normal install would be: password prompts
    # (polkit) ask for a member of it, and with none the agent crashes.
    groups: [sudo]
    lock_passwd: false
    plain_text_passwd: $VM_PASSWORD
    ssh_authorized_keys:
      - $(cat "$VM/id_ed25519.pub")
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
	# A sound card for the VM. Its sound goes nowhere unless we have a
	# window: then QEMU plays it through PulseAudio, which on WSL is the
	# server WSLg provides (PULSE_SERVER), so it comes out of Windows.
	audio=none
	if [ -z "${HEADLESS:-}" ] && [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then
		display=gtk,zoom-to-fit=on
		[ -n "${PULSE_SERVER:-}" ] && audio="pa,server=$PULSE_SERVER"
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

	firmware=
	if [ -n "${UEFI:-}" ]; then
		[ -e "$VM/OVMF_VARS.fd" ] || cp "$OVMF_VARS" "$VM/OVMF_VARS.fd"
		firmware="-drive if=pflash,format=raw,readonly=on,file=$OVMF_CODE"
		firmware="$firmware -drive if=pflash,format=raw,file=$VM/OVMF_VARS.fd"
	fi

	# Same virtual hardware as `make run`, plus more memory for a browser
	# and port 2222 on this machine forwarded to the VM's ssh.
	# setsid: run QEMU in a session of its own. Otherwise it is shut down
	# (by a "hang up" signal) when the terminal that started it closes.
	# shellcheck disable=SC2086
	setsid qemu-system-x86_64 -M pc -m 4096 -smp 4 $accel $firmware \
		-drive file="$VM/disk.qcow2",if=virtio \
		-netdev user,id=net0,hostfwd=tcp:127.0.0.1:$SSH_PORT-:22 \
		-device virtio-net-pci,netdev=net0 \
		-device virtio-vga,xres=1280,yres=800 \
		-device virtio-keyboard-pci -device virtio-tablet-pci \
		-audiodev "$audio",id=snd0 -device intel-hda -device hda-duplex,audiodev=snd0 \
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
	remove_old_install
	# Build the package and install it as apt would any other. The copy
	# has no .git, so the version comes from here. --reinstall: while
	# testing uncommitted changes the version stays the same.
	local version
	version=$(make -s -C distro version 2>/dev/null)
	vm_ssh "cd PerkinsOS && make -C distro deb VERSION='$version' &&
		sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -q --reinstall \
			./distro/build/deb/perkinsos_*.deb"
	# The user's systemd reads unit files when it starts; tell it to read
	# them again, or a running session misses new ones (mako's drop-in).
	vm_ssh 'XDG_RUNTIME_DIR=/run/user/$(id -u) systemctl --user daemon-reload'
	login_screen
	echo "Installed. Restart the desktop with: distro/vm.sh ssh sudo systemctl restart greetd"
}

# VMs set up before the package had PerkinsOS in /usr/local, put there by
# `make install`, and foot's colours over the foot package's own file.
# Remove both once, so the package's copy in /usr is the one that runs.
remove_old_install() {
	vm_ssh '[ -e /usr/local/bin/perkinsos-compositor ] || exit 0
		sudo rm -rf /usr/local/bin/perkinsos-* /usr/local/share/perkinsos \
			/usr/local/share/fonts/vt323 \
			/usr/local/share/xdg-desktop-portal/perkinsos-portals.conf \
			/usr/local/lib/systemd/user/perkinsos-* \
			/usr/local/lib/systemd/user/mako.service.d
		if cmp -s /etc/xdg/foot/foot.ini PerkinsOS/buildroot/overlay/etc/xdg/foot/foot.ini; then
			sudo rm /etc/xdg/foot/foot.ini
			sudo apt-get install -y -q --reinstall \
				-o Dpkg::Options::=--force-confmiss foot
		fi'
}

# The login screen (distro/greetd.toml), plus one thing only the test VM
# has: greetd's initial_session logs max in once at boot, so the VM comes
# up on the desktop for tests and screenshots. Quitting the desktop shows
# the login screen. VMs made before the login screen logged in through
# the console instead (a getty drop-in and ~/.profile); remove that.
login_screen() {
	vm_ssh "sudo passwd -S max | grep -q ' P ' || echo 'max:$VM_PASSWORD' | sudo chpasswd"
	vm_ssh 'sudo rm -f /etc/systemd/system/getty@tty1.service.d/autologin.conf
		sed -i "/^# PerkinsOS: start the desktop/,/^fi$/d" ~/.profile
		cd PerkinsOS && sudo make -C distro install-login PREFIX=/usr &&
		sudo tee -a /etc/greetd/config.toml > /dev/null <<"EOF"

# Test VM only (distro/vm.sh): log max in once at boot, without a password.
[initial_session]
command = "/usr/bin/perkinsos-start"
user = "max"
EOF
		sudo systemctl daemon-reload'
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
