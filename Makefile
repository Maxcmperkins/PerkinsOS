# PerkinsOS top-level Makefile.
#   make image          build the kernel + root filesystem with Buildroot
#   make run            boot them in QEMU (quit with Ctrl-A then X)
#   make test           boot headless and check we can log in
#   make test-desktop   boot headless and screenshot the desktop
#   make demo           record docs/demo.gif from a headless run
#   make compositor     rebuild only compositor/ and repack the image (fast)
#   make shell          rebuild only shell/ and repack the image (fast)
#   make fork-tinywl    one-off: copy stock TinyWL into compositor/
#   make menuconfig     change the image config interactively
#   make savedefconfig  write the changed config back into the repo
#   make clean          delete build output (keeps downloads)

# Pinned Buildroot release (2026.02.x is the LTS). This also pins every
# package version in the image, including wlroots 0.19.2.
# The sha256 is from buildroot.org's signed .sign file for this release.
BUILDROOT_VERSION := 2026.02.3
BUILDROOT_SHA256  := 5a59e7501b0b4ec52c41f4bfa79412320e0b37eae5f719605a258e8d0c6fc7fb
BUILDROOT_TARBALL := dl/buildroot-$(BUILDROOT_VERSION).tar.xz
BUILDROOT_DIR     := .buildroot/buildroot-$(BUILDROOT_VERSION)

OUTPUT    := $(CURDIR)/output
IMAGES    := $(OUTPUT)/images
DEFCONFIG := $(CURDIR)/buildroot/configs/perkinsos_defconfig

# WSL adds the Windows PATH (C:\Program Files\...) to the Linux one, and
# Buildroot refuses to run when PATH contains spaces. Drop the /mnt/ entries.
export PATH := $(shell echo "$$PATH" | tr ':' '\n' | grep -v '^/mnt/' | paste -sd:)

# How we call Buildroot:
#   O=            build out-of-tree into ./output, so .buildroot/ stays pristine
#   BR2_EXTERNAL= our own tree (configs now, packages later)
#   BR2_DL_DIR=   keep downloads in ./dl so `make clean` doesn't re-download
# No -j here: Buildroot parallelises each package itself.
BR_MAKE := $(MAKE) -C $(BUILDROOT_DIR) O=$(OUTPUT) \
	BR2_EXTERNAL=$(CURDIR)/buildroot BR2_DL_DIR=$(CURDIR)/dl

# Use KVM (hardware virtualisation) when we're allowed to; it's far faster.
KVM := $(shell [ -r /dev/kvm ] && [ -w /dev/kvm ] && echo yes)

.PHONY: image run test test-desktop demo compositor shell fork-tinywl menuconfig savedefconfig clean

image: $(OUTPUT)/.config
	$(BR_MAKE)

# Download the release tarball and refuse it if the checksum is wrong.
$(BUILDROOT_TARBALL):
	mkdir -p dl
	wget -O $@.tmp https://buildroot.org/downloads/$(notdir $@)
	echo "$(BUILDROOT_SHA256)  $@.tmp" | sha256sum -c -
	mv $@.tmp $@

# Unpack it. The Makefile inside stands in for "the tree is unpacked".
$(BUILDROOT_DIR)/Makefile: $(BUILDROOT_TARBALL)
	mkdir -p .buildroot
	tar -xf $< -C .buildroot
	touch $@

# Expand our short defconfig into the full output/.config.
# Re-runs whenever perkinsos_defconfig changes.
$(OUTPUT)/.config: $(DEFCONFIG) $(BUILDROOT_DIR)/Makefile
	$(BR_MAKE) perkinsos_defconfig

# Where the VM's screen goes: a GTK window if this terminal has a desktop
# (WSLg sets DISPLAY), otherwise nowhere. Override with QEMU_DISPLAY=none.
QEMU_DISPLAY ?= $(if $(DISPLAY)$(WAYLAND_DISPLAY),gtk,none)
comma := ,
# Extra QEMU arguments, e.g. a monitor socket for the CI test.
QEMU_EXTRA ?=
# The VM's screen resolution, WIDTHxHEIGHT. Full HD by default; the tests
# use 1024x768. Try `make run SCREEN=1280x720` if 1080p is too big or slow.
SCREEN ?= 1920x1080
SCREEN_WIDTH := $(word 1,$(subst x, ,$(SCREEN)))
SCREEN_HEIGHT := $(word 2,$(subst x, ,$(SCREEN)))
# In a window, let QEMU shrink the picture to fit if the window is smaller
# than the VM's screen (a 1080p VM on a 1080p laptop, minus the title bar).
QEMU_DISPLAY_OPTIONS := $(if $(filter gtk,$(QEMU_DISPLAY)),$(comma)zoom-to-fit=on)

# Boot the image:
#   -kernel/-append     hand the kernel straight to QEMU: no bootloader needed
#   root=/dev/vda       the ext4 image, attached as the first virtio disk
#   console=ttyS0       kernel messages and the login prompt go to the serial port
#   -serial mon:stdio   ...and the serial port is this terminal
#   virtio-vga          the virtio-gpu graphics card the compositor draws to;
#                       xres/yres is the resolution it tells the VM to use
#   virtio-keyboard/-tablet  input devices; a tablet reports absolute
#                       positions, so the pointer isn't captured by the window
run:
	@[ -f $(IMAGES)/bzImage ] || { echo "No image yet. Run 'make image' first."; exit 1; }
	@[ -n "$(KVM)" ] || echo "WARNING: no access to /dev/kvm, using slow software emulation."
	qemu-system-x86_64 -M pc -m 1024 -smp 2 \
		$(if $(KVM),-enable-kvm -cpu host) \
		-kernel $(IMAGES)/bzImage \
		-append "root=/dev/vda rw console=ttyS0" \
		-drive file=$(IMAGES)/rootfs.ext4,if=virtio,format=raw \
		-netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
		-device virtio-vga,xres=$(SCREEN_WIDTH),yres=$(SCREEN_HEIGHT) \
		-device virtio-keyboard-pci -device virtio-tablet-pci \
		-display $(QEMU_DISPLAY)$(QEMU_DISPLAY_OPTIONS) \
		-serial mon:stdio $(QEMU_EXTRA)

# Boot headless, log in over the serial console, power off. See the script.
test:
	bash ci/boot-test.sh

# Start the compositor headless and screenshot it to output/desktop-test.png.
test-desktop:
	bash ci/desktop-test.sh

# Record docs/demo.gif: boot headless, drive the desktop with key presses
# and mouse clicks, screenshot each step. Then save a full-HD still to
# docs/screenshot.png.
demo:
	bash ci/demo.sh
	bash ci/screenshot-hd.sh

# Fast edit loop: rebuild only our own package, then repack the image.
# Takes seconds, not minutes. `-rebuild` makes Buildroot copy the folder
# again and recompile it; `all` regenerates rootfs.ext4.
compositor: $(OUTPUT)/.config
	$(BR_MAKE) perkinsos-compositor-rebuild all

shell: $(OUTPUT)/.config
	$(BR_MAKE) perkinsos-shell-rebuild all

# One-off, week 3: start compositor/ as an exact copy of stock TinyWL, taken
# from the wlroots tarball Buildroot already downloaded (so it matches the
# pinned wlroots version). Refuses to overwrite an existing fork.
WLROOTS_TARBALL = $(firstword $(wildcard dl/wlroots/wlroots-*.tar.gz))
fork-tinywl:
	@[ ! -e compositor/tinywl.c ] || { echo "compositor/tinywl.c already exists."; exit 1; }
	@[ -n "$(WLROOTS_TARBALL)" ] || { echo "No wlroots tarball in dl/. Run 'make image' first."; exit 1; }
	tar -xzf $(WLROOTS_TARBALL) -C compositor --strip-components=2 --wildcards \
		'wlroots-*/tinywl/tinywl.c' 'wlroots-*/tinywl/Makefile' 'wlroots-*/tinywl/LICENSE'
	@echo "Forked TinyWL from $(WLROOTS_TARBALL) into compositor/."
	@echo "Next: git add compositor && git commit, then 'make compositor && make run'."

menuconfig: $(OUTPUT)/.config
	$(BR_MAKE) menuconfig

# Write output/.config back to the repo as a minimal defconfig.
savedefconfig: $(OUTPUT)/.config
	$(BR_MAKE) savedefconfig BR2_DEFCONFIG=$(DEFCONFIG)

clean:
	rm -rf $(OUTPUT)
