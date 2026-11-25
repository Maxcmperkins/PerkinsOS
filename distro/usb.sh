#!/bin/bash
# Stage 2 of docs/PLAN-DAILY-DRIVER.md (docs/ROADMAP.md, M4): an image of
# Ubuntu with the PerkinsOS desktop, to write to a USB stick and boot the
# laptop from.
#
#   distro/usb.sh build    build output/usb/perkinsos-usb.img (slow: a whole OS)
#   distro/usb.sh test     boot the image in QEMU as the laptop would (UEFI,
#                          no help from this machine) and log in
#   distro/usb.sh clean    delete the build VM and the image
#
# It is the stage 1 test VM built a second time with distro/vm.sh, in its
# own directory, then changed from a test machine into one to carry around:
#
# - Hardware: the full kernel (linux-generic: the VM's linux-virtual leaves
#   out most drivers), firmware for the Intel Arc graphics and BE200 wifi
#   (linux-firmware), and NetworkManager for wifi.
# - Safety: no automatic login, no ssh server, and sudo asks for the
#   password. The user is "max" with password "perkinsos": change it
#   after the first login (open a terminal, run passwd). Anyone who has
#   read this repo knows it until then. It cannot be forced at the login
#   screen: greetd does not handle PAM asking for a new password, and an
#   expired one just fails to log in (tested). The stick is not encrypted,
#   so the password guards the keyboard, not the files on it.
#
# Write the image to a USB stick of 32 GB or more from Windows with Rufus
# or balenaEtcher (it is a whole-disk image: pick "DD image" if asked). It
# is at \\wsl$\Ubuntu\home\<you>\PerkinsOS\output\usb\perkinsos-usb.img.
# Everything on the stick is erased.
set -eu
cd "$(dirname "$0")/.."

# For distro/vm.sh: a second VM beside the test one, with its own ssh
# port, booting through UEFI like the laptop. 24G fits a "32 GB" stick
# (about 29.8 real GB) with room to spare.
export VM=output/usb/build SSH_PORT=2223 SEED_PORT=8766 DISK_SIZE=24G \
	VM_HOSTNAME=perkinsos UEFI=1
IMG=output/usb/perkinsos-usb.img
HARDWARE="linux-generic linux-firmware network-manager wpasupplicant pciutils rfkill"
PASSWORD=perkinsos               # the same as the test VM's (distro/vm.sh)

running() {
	[ -f "$VM/qemu.pid" ] && kill -0 "$(cat "$VM/qemu.pid")" 2>/dev/null
}

# press KEY / type TEXT: keys on the VM's keyboard, through QEMU's monitor.
# TEXT may use lower-case letters and digits only.
press() {
	python3 -c '
import socket, sys, time
s = socket.socket(socket.AF_UNIX)
s.connect(sys.argv[1])
s.sendall(("sendkey " + sys.argv[2] + "\n").encode())
time.sleep(0.4)' "$1" "$2"
}
type_text() {
	local i
	for ((i = 0; i < ${#2}; i++)); do press "$1" "${2:i:1}"; done
}

build() {
	[ -e "$VM/disk.qcow2" ] || distro/vm.sh create
	HEADLESS=1 distro/vm.sh start
	distro/vm.sh install
	distro/vm.sh ssh "sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -q $HARDWARE &&
		{ snap list firefox > /dev/null 2>&1 || sudo snap install firefox; }"

	# The rest runs as root in one go, because it takes away what this
	# script uses to get in (ssh, and sudo without a password), and ends
	# by switching the VM off. That drops the ssh connection, so ssh
	# reports an error even when all went well: check that it switched off.
	distro/vm.sh ssh "sudo bash -s" <<EOF || true
set -eu
# Wifi and wired networks through NetworkManager, instead of the cloud
# image's one wired connection (systemd-networkd, set up by cloud-init).
cat > /etc/netplan/01-perkinsos.yaml <<'NETPLAN'
network:
  version: 2
  renderer: NetworkManager
NETPLAN
chmod 600 /etc/netplan/01-perkinsos.yaml
rm -f /etc/netplan/50-cloud-init.yaml
# cloud-init set the VM up; on the laptop it would only slow the boot,
# looking for settings that are not there.
touch /etc/cloud/cloud-init.disabled
# The real login screen, without the test VM's automatic login.
make -C /home/max/PerkinsOS/distro install-login PREFIX=/usr
# The password (see the top of distro/usb.sh: change it after logging in).
echo 'max:$PASSWORD' | chpasswd
# No way in from the network: the build's ssh key and the ssh server go.
rm -f /home/max/.ssh/authorized_keys
systemctl disable ssh.service ssh.socket 2> /dev/null || true
# Leave the laptop's NVIDIA card alone (docs/PLAN-DAILY-DRIVER.md): the
# screen is on the Intel graphics. Without this the kernel loads its own
# NVIDIA drivers, and wlroots sees two graphics cards. The initramfs, which
# starts the boot, has its own copy of these settings, so rebuild it.
cat > /etc/modprobe.d/perkinsos-no-nvidia.conf <<'MODPROBE'
blacklist nouveau
blacklist nova_core
blacklist nova_drm
MODPROBE
update-initramfs -u -k all
apt-get clean
# Last: sudo asks for the password from now on (cloud-init had made it
# passwordless for max).
rm -f /etc/sudoers.d/90-cloud-init-users
systemctl poweroff
EOF
	for _ in $(seq 120); do running || break; sleep 1; done
	running && { echo "The VM did not switch off."; exit 1; }

	# A plain whole-disk image: the qcow2 holds only the changes from
	# Ubuntu's download, and USB writers want every byte.
	qemu-img convert -p -O raw "$VM/disk.qcow2" "$IMG"
	ls -lh "$IMG"
	echo "Next: distro/usb.sh test, then write $IMG to a USB stick."
}

# Boot the image the way the laptop will: UEFI firmware, no cloud-init
# seed, no ssh. snapshot=on keeps the image unchanged. Then log in at the
# login screen, open a terminal to see the network, and screenshot each
# step into output/usb/test/.
test_image() {
	[ -e "$IMG" ] || { echo "No image yet. Run: distro/usb.sh build"; exit 1; }
	local dir=output/usb/test accel=
	rm -rf "$dir"; mkdir -p "$dir"
	cp /usr/share/OVMF/OVMF_VARS_4M.fd "$dir/OVMF_VARS.fd"
	if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
		accel="-enable-kvm -cpu host"
	else
		echo "WARNING: no access to /dev/kvm, using slow software emulation."
		accel="-accel tcg,thread=multi -cpu max"
	fi
	# shellcheck disable=SC2086
	qemu-system-x86_64 -M pc -m 4096 -smp 4 $accel \
		-drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
		-drive if=pflash,format=raw,file="$dir/OVMF_VARS.fd" \
		-drive file="$IMG",format=raw,if=virtio,snapshot=on \
		-netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
		-device virtio-vga,xres=1280,yres=800 \
		-device virtio-keyboard-pci -device virtio-tablet-pci \
		-display none -serial file:"$dir/serial.log" \
		-monitor unix:"$dir/monitor.sock",server,nowait > "$dir/qemu.log" 2>&1 &
	local qemu=$! mon=$dir/monitor.sock
	shot() { python3 ci/screenshot.py "$mon" "$dir/$1.png" > /dev/null && echo "ok: $dir/$1.png"; }

	# The serial console shows a login prompt once the boot is done.
	for _ in $(seq 180); do
		grep -aq 'perkinsos login:' "$dir/serial.log" 2> /dev/null && break
		kill -0 $qemu 2> /dev/null || { echo "QEMU exited:"; cat "$dir/qemu.log"; exit 1; }
		sleep 5
	done
	grep -aq 'perkinsos login:' "$dir/serial.log" || { echo "No login prompt."; kill $qemu; exit 1; }
	sleep 15
	shot 1-login

	type_text "$mon" max; press "$mon" ret; sleep 2
	type_text "$mon" "$PASSWORD"; press "$mon" ret
	sleep 60
	shot 2-desktop

	press "$mon" alt-ret; sleep 10      # a terminal: is the network up?
	type_text "$mon" "nmcli"; press "$mon" ret; sleep 5
	shot 3-network
	kill $qemu
}

case "${1:-}" in
build) build ;;
test) test_image ;;
clean)
	running && kill "$(cat "$VM/qemu.pid")"
	rm -rf output/usb
	echo "Deleted output/usb."
	;;
*) sed -n '2,9p' "$0" | sed 's/^# \{0,1\}//'; exit 1 ;;
esac
