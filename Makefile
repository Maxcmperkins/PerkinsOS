# PerkinsOS top-level Makefile.
#   make image          build the kernel + root filesystem with Buildroot
#   make run            boot them in QEMU (quit with Ctrl-A then X)
#   make test           boot headless and check we can log in
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

.PHONY: image run test menuconfig savedefconfig clean

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

# Boot the image:
#   -kernel/-append  hand the kernel straight to QEMU: no bootloader needed
#   root=/dev/vda    the ext4 image, attached as the first virtio disk
#   console=ttyS0    kernel messages and the login prompt go to the serial port
#   -nographic       ...and the serial port is this terminal (no window yet)
run:
	@[ -f $(IMAGES)/bzImage ] || { echo "No image yet. Run 'make image' first."; exit 1; }
	@[ -n "$(KVM)" ] || echo "WARNING: no access to /dev/kvm, using slow software emulation."
	qemu-system-x86_64 -M pc -m 512 -smp 2 \
		$(if $(KVM),-enable-kvm -cpu host) \
		-kernel $(IMAGES)/bzImage \
		-append "root=/dev/vda rw console=ttyS0" \
		-drive file=$(IMAGES)/rootfs.ext4,if=virtio,format=raw \
		-netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
		-nographic

# Boot headless, log in over the serial console, power off. See the script.
test:
	bash ci/boot-test.sh

menuconfig: $(OUTPUT)/.config
	$(BR_MAKE) menuconfig

# Write output/.config back to the repo as a minimal defconfig.
savedefconfig: $(OUTPUT)/.config
	$(BR_MAKE) savedefconfig BR2_DEFCONFIG=$(DEFCONFIG)

clean:
	rm -rf $(OUTPUT)
