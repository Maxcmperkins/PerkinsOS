# Build and install PerkinsOS's own programs on an ordinary Linux distro
# (docs/PLAN-DAILY-DRIVER.md, stage 1). The Buildroot image does not use
# this file: its build rules are in buildroot/package/. Both build the same
# compositor/ and shell/ source.
#
#   make -C distro                 build into distro/build/
#   sudo make -C distro install    install into /usr/local
#   make -C distro clean
#
# Packages needed, by their Ubuntu 26.04 names:
#   build-essential pkg-config libwlroots-0.19-dev libwayland-dev
#   wayland-protocols libxkbcommon-dev libcairo2-dev
# and to run it: foot (the terminal) and fonts-dejavu-core. For apps to be
# usable: fonts-noto-* (other scripts, emoji) and xdg-desktop-portal with
# xdg-desktop-portal-gtk (file open and save dialogs for sandboxed apps).
# For apps that only speak X11: xwayland (the X server the compositor
# starts; Ubuntu's wlroots is built with WLR_HAS_XWAYLAND, Buildroot's is
# not), and x11-apps (xeyes, xclock: the simplest X11 programs, to test it).
# The session's services (docs/ROADMAP.md, M3): mako-notifier and
# libnotify-bin (notifications), pipewire-audio, wireplumber and pavucontrol
# (sound), gnome-keyring and libsecret-tools (saved passwords), and
# mate-polkit and pkexec (password prompts). The login screen
# (install-login): greetd and tuigreet.

PREFIX ?= /usr/local
PKG_CONFIG ?= pkg-config
TOP := $(abspath $(CURDIR)/..)
BUILD := $(CURDIR)/build

# pkg-config tells us where the distro put each tool and protocol file.
WAYLAND_SCANNER := $(shell $(PKG_CONFIG) --variable=wayland_scanner wayland-scanner)
WAYLAND_PROTOCOLS := $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)
# layer-shell is not in wayland-protocols. Buildroot takes its XML from the
# wlroots source. Distros' wlroots packages leave it out (and Ubuntu has no
# wlr-protocols package), so distro/protocol/ holds a copy of that one file
# from wlroots 0.19.2, the version Buildroot pins.
WLR_PROTOCOLS ?= $(CURDIR)/protocol

# Same rule as buildroot/package/perkinsos-shell: every folder shell/NAME/
# with a main.c becomes the program perkinsos-NAME, compiled together with
# everything in shell/common/.
SHELL_PROGRAMS := $(notdir $(patsubst %/main.c,%,$(wildcard $(TOP)/shell/*/main.c)))
SHELL_PKGS := wayland-client xkbcommon cairo
# MODEL_DIR: where the wallpaper looks for its aircraft (see model.c).
SHELL_CFLAGS := -O2 -g -Wall -Werror -I$(BUILD)/protocol -I$(TOP)/shell/common \
	-DMODEL_DIR='"$(PREFIX)/share/perkinsos/models"' \
	$(shell $(PKG_CONFIG) --cflags $(SHELL_PKGS))
SHELL_LIBS := $(shell $(PKG_CONFIG) --libs $(SHELL_PKGS)) -lm
SHELL_PROTOCOLS := $(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml \
	$(WLR_PROTOCOLS)/wlr-layer-shell-unstable-v1.xml

.PHONY: all compositor shell install install-login clean

all: compositor shell

# The compositor has its own Makefile (it came with TinyWL). It writes its
# output next to its source, so build a copy and keep compositor/ clean.
compositor:
	mkdir -p $(BUILD)/compositor
	cp $(TOP)/compositor/Makefile $(TOP)/compositor/*.c $(BUILD)/compositor/
	$(MAKE) -C $(BUILD)/compositor WLR_PROTOCOLS=$(WLR_PROTOCOLS)

# wayland-scanner turns each protocol's XML into a C header and a C file;
# then each program is one compiler call.
shell:
	mkdir -p $(BUILD)/protocol
	for xml in $(SHELL_PROTOCOLS); do \
		name=$$(basename $$xml .xml); \
		$(WAYLAND_SCANNER) client-header $$xml $(BUILD)/protocol/$$name-client-protocol.h || exit 1; \
		$(WAYLAND_SCANNER) private-code $$xml $(BUILD)/protocol/$$name-protocol.c || exit 1; \
	done
	for name in $(SHELL_PROGRAMS); do \
		echo "  CC      perkinsos-$$name"; \
		$(CC) $(SHELL_CFLAGS) $(TOP)/shell/$$name/*.c $(TOP)/shell/common/*.c \
			$(BUILD)/protocol/*.c -o $(BUILD)/perkinsos-$$name $(SHELL_LIBS) || exit 1; \
	done

# The session script, terminal colours and aircraft are the same files the
# Buildroot image uses, taken from its overlay. perkinsos-start is new: it
# does the job the image's init script does.
OVERLAY := $(TOP)/buildroot/overlay
install:
	install -D -m 0755 $(BUILD)/compositor/tinywl $(DESTDIR)$(PREFIX)/bin/perkinsos-compositor
	for name in $(SHELL_PROGRAMS); do \
		install -D -m 0755 $(BUILD)/perkinsos-$$name $(DESTDIR)$(PREFIX)/bin/perkinsos-$$name || exit 1; \
	done
	install -D -m 0755 $(OVERLAY)/usr/bin/perkinsos-session $(DESTDIR)$(PREFIX)/bin/perkinsos-session
	install -D -m 0755 perkinsos-start $(DESTDIR)$(PREFIX)/bin/perkinsos-start
	install -d $(DESTDIR)$(PREFIX)/share/perkinsos/models
	install -m 0644 $(OVERLAY)/usr/share/perkinsos/models/*.mesh $(DESTDIR)$(PREFIX)/share/perkinsos/models/
	install -D -m 0644 $(OVERLAY)/etc/xdg/foot/foot.ini $(DESTDIR)/etc/xdg/foot/foot.ini
	# The pixel font VT323 and its licence; fc-cache tells fontconfig, the
	# library apps ask for fonts, that it is there.
	install -D -m 0644 $(OVERLAY)/usr/share/fonts/vt323/VT323-Regular.ttf \
		$(DESTDIR)$(PREFIX)/share/fonts/vt323/VT323-Regular.ttf
	install -D -m 0644 $(OVERLAY)/usr/share/fonts/vt323/OFL.txt \
		$(DESTDIR)$(PREFIX)/share/fonts/vt323/OFL.txt
	[ -n "$(DESTDIR)" ] || fc-cache $(PREFIX)/share/fonts
	install -D -m 0644 perkinsos-portals.conf \
		$(DESTDIR)$(PREFIX)/share/xdg-desktop-portal/perkinsos-portals.conf
	install -D -m 0644 perkinsos-session.target \
		$(DESTDIR)$(PREFIX)/lib/systemd/user/perkinsos-session.target
	install -D -m 0644 perkinsos-polkit-agent.service \
		$(DESTDIR)$(PREFIX)/lib/systemd/user/perkinsos-polkit-agent.service
	install -D -m 0644 mako.config $(DESTDIR)$(PREFIX)/share/perkinsos/mako.config
	# @PREFIX@ in the drop-in becomes where mako.config was installed.
	mkdir -p $(DESTDIR)$(PREFIX)/lib/systemd/user/mako.service.d
	sed 's|@PREFIX@|$(PREFIX)|' mako-perkinsos.conf \
		> $(DESTDIR)$(PREFIX)/lib/systemd/user/mako.service.d/perkinsos.conf

# The login screen: greetd with tuigreet (greetd.toml says how). Separate
# from `install` because it replaces the machine's login: afterwards the
# machine boots to the PerkinsOS login, not to the console or another
# desktop's login screen. Needs the packages greetd and tuigreet.
install-login:
	mkdir -p $(BUILD)
	sed 's|@PREFIX@|$(PREFIX)|' greetd.toml > $(BUILD)/greetd.toml
	install -D -m 0644 $(BUILD)/greetd.toml $(DESTDIR)/etc/greetd/config.toml
	[ -n "$(DESTDIR)" ] || systemctl enable --force greetd.service

clean:
	rm -rf $(BUILD)
