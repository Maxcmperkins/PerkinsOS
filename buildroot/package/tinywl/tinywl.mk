################################################################################
#
# tinywl
#
################################################################################

# TinyWL lives in the tinywl/ folder of the wlroots source tarball. Taking
# the version, URL and download folder from the wlroots package means the
# two can never drift apart (CLAUDE.md: only use TinyWL from the pinned
# wlroots version), and the tarball is downloaded once.
TINYWL_VERSION = $(WLROOTS_VERSION)
TINYWL_SITE = $(WLROOTS_SITE)
TINYWL_SOURCE = wlroots-$(TINYWL_VERSION).tar.gz
TINYWL_DL_SUBDIR = wlroots
TINYWL_LICENSE = CC0-1.0
TINYWL_LICENSE_FILES = tinywl/LICENSE
TINYWL_DEPENDENCIES = host-pkgconf host-wayland wlroots wayland \
	wayland-protocols libxkbcommon

# TinyWL has its own small Makefile. The cross-compiler settings go in the
# environment, not on the make command line, because that Makefile does
# `CFLAGS += ...` and command-line variables would override it.
# WAYLAND_SCANNER is the exception: the Makefile asks pkg-config for it and
# would get the target's binary, so we force the one built for the host.
define TINYWL_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(TARGET_CONFIGURE_OPTS) $(MAKE) -C $(@D)/tinywl \
		WAYLAND_SCANNER=$(HOST_DIR)/bin/wayland-scanner
endef

define TINYWL_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/tinywl/tinywl $(TARGET_DIR)/usr/bin/tinywl
endef

$(eval $(generic-package))
