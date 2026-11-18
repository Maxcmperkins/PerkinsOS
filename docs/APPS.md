# Apps on PerkinsOS

Which everyday apps run on the PerkinsOS desktop, and why the others don't.
Tested 17 Nov 2026 in the Ubuntu 26.04 VM (`distro/vm.sh`), installed by
`distro/apps.sh`, each started the way the launcher starts it: the
`Exec=` line of its `.desktop` file, with the compositor's environment.

PerkinsOS has no XWayland yet (docs/ROADMAP.md, M3), so an app opens a
window only if it can talk Wayland itself.

## Works

| App | Version | From | Notes |
| --- | --- | --- | --- |
| Thunar (file manager) | | Ubuntu | With gvfs (Trash, drives, network shares) and thumbnails. The default for opening folders (`xdg-mime`) |
| Firefox | snap | Ubuntu | File open/save dialogs through `xdg-desktop-portal` (M1 slice 1) |
| VS Code | 1.146.0 | Microsoft's `.deb` | Uses Wayland on its own. The **snap does not work**: see below |
| Discord | 1.0.168 | snap | Only with `--ozone-platform=wayland`; `apps.sh` adds it (see below) |
| Blender | 5.0.1 | Ubuntu | Draws with Mesa's software OpenGL in the VM: works, slowly |
| Inkscape | 1.4.3 | Ubuntu | |
| OBS Studio | 32.1.0 | Ubuntu | Opens and runs its setup wizard. Screen capture not tried yet: it needs a screencast portal, which we don't have |
| DB Browser for SQLite | | Ubuntu | |
| git, Python 3.14, Node 22 + npm, sqlite3, Docker 29 | | Ubuntu | Command line, in the terminal. Docker needs `sudo` (you're not in the `docker` group) |

Pinned to the dock by `distro/apps.sh pin`: Thunar, Firefox, VS Code,
Discord, Blender, Inkscape. The rest are in the launcher (Alt+P).

## What went wrong, and the fix

**Electron apps use X11 unless told otherwise.** Both VS Code and Discord
are Electron (Chromium inside). Started plainly, they print
`Missing X server or $DISPLAY` and exit.

- *VS Code snap:* its start-up script, `/snap/code/current/electron-launch`,
  ends with `exec "$@" --ozone-platform=x11`. That comes last, so it beats
  any flag you pass (`Option 'ozone-platform' is defined more than once.
  Using value 'x11'`). Fix: Microsoft's `.deb`, which picks Wayland by
  itself. It also adds Microsoft's apt repository so it updates with the
  system.
- *Discord snap:* picks Wayland only with `--ozone-platform=wayland` on the
  command line; the `ELECTRON_OZONE_PLATFORM_HINT` variable makes no
  difference. Fix: `apps.sh` writes a copy of its `.desktop` file to
  `~/.local/share/applications` with the flag added. The launcher reads
  that folder first and an earlier folder's copy wins
  (`add_installed()` in `shell/common/apps.c`), so the dock and launcher
  run the fixed command. If the snap renames its `.desktop` file, the
  copy needs redoing.

## Rough edges seen

- Discord opened an "Open With… No apps available" box at start-up: it
  asks the system to open a link (a `discord:` address, it seems) and
  nothing in the VM handles it. Harmless; close it.
- The cockpit display cuts app names at 12 characters ("VISUAL STUDI").
- `libEGL warning` / `vaInitialize failed` in app logs: the VM has no
  graphics card driver for 3D or video, so apps fall back to the CPU. On
  the laptop (M2, M4) this should go away.

## Not tried, because they need XWayland (M3)

Steam, IntelliJ IDEA, PyCharm (Java apps are X11 by default), and most
Windows games through Proton. Games also need the graphics card work (M2)
and real hardware (M4): the VM has no GPU.

## Windows-only, so replaced

Microsoft Office (Office on the web, or LibreOffice), Razer Synapse
(OpenRazer + Polychromatic), and the laptop vendor tools (MSI, HP, Killer,
Tobii), which Linux's own drivers replace. Fortnite does not run on Linux
(anti-cheat).
