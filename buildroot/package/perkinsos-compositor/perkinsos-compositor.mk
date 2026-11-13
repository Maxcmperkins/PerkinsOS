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
#
# Before the fork exists, compositor/ holds only a README: build nothing,
# and the image keeps using stock TinyWL (see overlay/etc/init.d/S99desktop).
define PERKINSOS_COMPOSITOR_BUILD_CMDS
	if [ -f $(@D)/Makefile ]; then \
		$(TARGET_MAKE_ENV) $(TARGET_CONFIGURE_OPTS) $(MAKE) -C $(@D) \
			WAYLAND_SCANNER=$(HOST_DIR)/bin/wayland-scanner \
			WLR_PROTOCOLS=$(WLROOTS_DIR)/protocol; \
	else \
		echo "compositor/ has no Makefile yet: nothing to build"; \
	fi
endef

# The fork produces a binary called "tinywl" until its Makefile is changed
# to produce "perkinsos-compositor"; accept either name.
define PERKINSOS_COMPOSITOR_INSTALL_TARGET_CMDS
	for bin in $(@D)/perkinsos-compositor $(@D)/tinywl; do \
		if [ -x $$bin ]; then \
			$(INSTALL) -D -m 0755 $$bin \
				$(TARGET_DIR)/usr/bin/perkinsos-compositor; \
			break; \
		fi; \
	done
endef

$(eval $(generic-package))
