################################################################################
#
# perkinsos-shell
#
################################################################################

# Built from the shell/ folder of this repository (see perkinsos-compositor
# for what "local" means).
PERKINSOS_SHELL_VERSION = local
PERKINSOS_SHELL_SITE = $(BR2_EXTERNAL_PERKINSOS_PATH)/../shell
PERKINSOS_SHELL_SITE_METHOD = local
PERKINSOS_SHELL_DEPENDENCIES = host-pkgconf host-wayland wayland \
	wayland-protocols wlroots libxkbcommon cairo

# The rule: every folder shell/NAME/ that contains .c files is compiled
# into one program, /usr/bin/perkinsos-NAME. All .c files in the folder are
# compiled together. There is no Makefile to maintain inside shell/.

# Wayland protocols the clients may use. wayland-scanner turns each XML file
# into a C header (the functions a client calls) and a C file (the message
# tables behind them), which are compiled into every program.
#   xdg-shell:        normal application windows (apps)
#   wlr-layer-shell:  surfaces anchored to a screen edge (dock, launcher)
PERKINSOS_SHELL_PROTOCOLS = \
	$(STAGING_DIR)/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml \
	$(WLROOTS_DIR)/protocol/wlr-layer-shell-unstable-v1.xml

# Libraries every program is linked against:
#   wayland-client  talk to the compositor
#   xkbcommon       turn key codes into characters
#   cairo           2D drawing (shapes, text, PNG icons) into a pixel buffer
PERKINSOS_SHELL_PKGS = wayland-client xkbcommon cairo

define PERKINSOS_SHELL_BUILD_CMDS
	mkdir -p $(@D)/protocol
	for xml in $(PERKINSOS_SHELL_PROTOCOLS); do \
		name=$$(basename $$xml .xml); \
		$(HOST_DIR)/bin/wayland-scanner client-header $$xml \
			$(@D)/protocol/$$name-client-protocol.h || exit 1; \
		$(HOST_DIR)/bin/wayland-scanner private-code $$xml \
			$(@D)/protocol/$$name-protocol.c || exit 1; \
	done
	for dir in $(@D)/*/; do \
		name=$$(basename $$dir); \
		[ "$$name" != protocol ] || continue; \
		ls $$dir*.c > /dev/null 2>&1 || continue; \
		echo "  CC      perkinsos-$$name"; \
		$(TARGET_MAKE_ENV) $(TARGET_CC) $(TARGET_CFLAGS) -Wall -g \
			-I$(@D)/protocol \
			$$($(PKG_CONFIG_HOST_BINARY) --cflags $(PERKINSOS_SHELL_PKGS)) \
			$$dir*.c $(@D)/protocol/*.c \
			-o $(@D)/perkinsos-$$name \
			$(TARGET_LDFLAGS) \
			$$($(PKG_CONFIG_HOST_BINARY) --libs $(PERKINSOS_SHELL_PKGS)) \
			-lm || exit 1; \
	done
endef

define PERKINSOS_SHELL_INSTALL_TARGET_CMDS
	for bin in $(@D)/perkinsos-*; do \
		[ -x $$bin ] || continue; \
		$(INSTALL) -D -m 0755 $$bin $(TARGET_DIR)/usr/bin/$$(basename $$bin); \
	done
endef

$(eval $(generic-package))
