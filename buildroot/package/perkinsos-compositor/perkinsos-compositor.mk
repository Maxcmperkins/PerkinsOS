################################################################################
#
# perkinsos-compositor
#
################################################################################

# "local" means: no download, copy the source from a folder on this machine.
# Buildroot rsyncs compositor/ into output/build/ every time the package is
# (re)built, so `make compositor` always picks up the latest edits.
PERKINSOS_COMPOSITOR_VERSION = local
PERKINSOS_COMPOSITOR_SITE = $(BR2_EXTERNAL_PERKINSOS_PATH)/../compositor
PERKINSOS_COMPOSITOR_SITE_METHOD = local
PERKINSOS_COMPOSITOR_LICENSE = CC0-1.0
PERKINSOS_COMPOSITOR_DEPENDENCIES = host-pkgconf host-wayland wlroots wayland \
	wayland-protocols libxkbcommon

# compositor/ keeps the Makefile it was forked with from TinyWL, so it is
# built the same way as the stock tinywl package: cross-compiler settings in
# the environment (that Makefile does `CFLAGS += ...`), and WAYLAND_SCANNER
# forced to the host's binary. WLR_PROTOCOLS points at the protocol XML
# files that ship in the wlroots source (wlr-layer-shell is one of them).
define PERKINSOS_COMPOSITOR_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(TARGET_CONFIGURE_OPTS) $(MAKE) -C $(@D) \
		WAYLAND_SCANNER=$(HOST_DIR)/bin/wayland-scanner \
		WLR_PROTOCOLS=$(WLROOTS_DIR)/protocol
endef

# The fork still produces a binary called "tinywl" until its Makefile is
# changed to produce "perkinsos-compositor"; accept either name.
define PERKINSOS_COMPOSITOR_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 \
		$(firstword $(wildcard $(@D)/perkinsos-compositor $(@D)/tinywl)) \
		$(TARGET_DIR)/usr/bin/perkinsos-compositor
endef

$(eval $(generic-package))
