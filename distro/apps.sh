#!/bin/bash
# Install everyday apps on an Ubuntu machine running the PerkinsOS desktop
# (docs/ROADMAP.md, M1). Run it on that machine, not on the build host:
#
#   distro/apps.sh             install the apps below
#   distro/apps.sh pin         pin the apps known to work to the dock
#
# In the test VM: distro/vm.sh ssh PerkinsOS/distro/apps.sh
#
# Only apps that can draw through Wayland themselves are here: PerkinsOS has
# no XWayland yet (M3), so apps that only speak X11 (Steam, IntelliJ and
# PyCharm) cannot open a window. docs/APPS.md records how each app did.
set -eu

# The file manager: Thunar, Xfce's. It is light, draws through Wayland
# itself (GTK 3) and does not need the rest of its desktop. gvfs-backends
# gives it the Trash, USB drives and network shares; tumbler draws
# thumbnails; the archive plugin adds "Extract here".
FILE_MANAGER="thunar thunar-archive-plugin gvfs-backends tumbler"
# Apps to open the files you download, chosen to be light GTK 3 apps that
# draw through Wayland themselves, like Thunar: Ristretto (pictures),
# Atril (PDFs), Mousepad (text) and Xarchiver (zip files, and Thunar's
# "Extract here"). Without them, pictures opened in ImageMagick's X11-only
# viewer and PDFs in Inkscape. set_defaults() below makes them the default.
VIEWERS="ristretto atril mousepad xarchiver"
# From Ubuntu's own archive.
APT_PACKAGES="git python3 python3-venv nodejs npm sqlite3 docker.io \
	sqlitebrowser inkscape blender obs-studio $FILE_MANAGER $VIEWERS"
# Snaps, for apps Ubuntu does not package.
SNAPS="discord"
# VS Code comes from Microsoft's .deb, not its snap: the snap's start-up
# script ends with `--ozone-platform=x11`, which forces X11 whatever you
# pass, so it cannot open a window without XWayland.
VSCODE_DEB=https://update.code.visualstudio.com/latest/linux-deb-x64/stable

# The .desktop names (see apps_load_pinned() in shell/common/apps.c) of
# apps that work on PerkinsOS (docs/APPS.md). Only a few: the dock has
# room for about nine buttons on a 1280-pixel screen, and the rest are a
# search away in the launcher.
PINS="thunar firefox_firefox com.microsoft.VSCode discord_discord blender org.inkscape.Inkscape"

# Discord is Electron, which uses X11 unless told otherwise, and ignores
# the ELECTRON_OZONE_PLATFORM_HINT variable. A copy of its .desktop file
# in ~/.local/share/applications takes the place of the snap's (the
# launcher reads that folder first) and adds the flag.
discord_on_wayland() {
	from=/var/lib/snapd/desktop/applications/discord_discord.desktop
	to="${XDG_DATA_HOME:-$HOME/.local/share}/applications/discord_discord.desktop"
	mkdir -p "$(dirname "$to")"
	sed 's|^Exec=/snap/bin/discord|Exec=/snap/bin/discord --ozone-platform=wayland|' \
		"$from" > "$to"
}

# Which app opens which kind of file or link: what happens when you
# double-click in Thunar, open a download in Firefox, or click a link in
# Discord. Stored per user in ~/.config/mimeapps.list.
set_defaults() {
	xdg-mime default thunar.desktop inode/directory
	xdg-mime default org.xfce.ristretto.desktop \
		image/png image/jpeg image/gif image/webp image/bmp image/svg+xml
	xdg-mime default atril.desktop application/pdf
	xdg-mime default org.xfce.mousepad.desktop text/plain text/markdown
	xdg-mime default xarchiver.desktop application/zip application/x-tar \
		application/gzip application/x-7z-compressed
	# Links: http, https and web pages go to Firefox. Not `xdg-settings set
	# default-web-browser`: it looks for the .desktop file in XDG_DATA_DIRS,
	# which over ssh lacks snap's folder, so it fails (exit 2) and stops
	# this script. xdg-mime writes the same lines to mimeapps.list.
	xdg-mime default firefox_firefox.desktop \
		x-scheme-handler/http x-scheme-handler/https text/html
}

install_apps() {
	# shellcheck disable=SC2086
	if ! dpkg -s $APT_PACKAGES > /dev/null 2>&1; then
		sudo apt-get update -q
		sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -q $APT_PACKAGES
	fi
	for snap in $SNAPS; do
		snap list "$snap" > /dev/null 2>&1 || sudo snap install "$snap"
	done
	if ! dpkg -s code > /dev/null 2>&1; then
		# Microsoft's package also adds Microsoft's apt repository, so
		# VS Code updates with the rest of the system.
		deb=$(mktemp --suffix=.deb)
		curl -fsSL -o "$deb" "$VSCODE_DEB"
		sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -q "$deb"
		rm -f "$deb"
	fi
	discord_on_wayland
	set_defaults
	echo "Installed. Open them from the launcher (Alt+P)."
}

pin() {
	dir="${XDG_CONFIG_HOME:-$HOME/.config}/perkinsos"
	mkdir -p "$dir"
	printf '%s\n' $PINS > "$dir/pinned"
	echo "Pinned: $PINS. The dock reads this when it starts:"
	echo "log out and in again (or restart the desktop) to see them."
}

case "${1:-install}" in
install) install_apps ;;
pin) pin ;;
*) sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 1 ;;
esac
