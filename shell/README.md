# shell/

The PerkinsOS dock, launcher and apps. Each folder here becomes one program in the image:

| Folder | Program | What it is | Started by |
| --- | --- | --- | --- |
| `dock/` | `perkinsos-dock` | Bar on the bottom edge: launcher button, app buttons, clock (layer-shell) | `perkinsos-session` at boot |
| `launcher/` | `perkinsos-launcher` | Search box: type to filter, Enter to start (layer-shell, takes the keyboard) | Dock button, or Alt+P |
| `sysinfo/` | `perkinsos-sysinfo` | Window showing kernel, uptime, memory, load (xdg-shell) | Dock or launcher |
| `common/` | (none) | Shared code: shm buffers, event loop step, drawing helpers, the app list | compiled into every program |

All `.c` files in a folder are compiled together with everything in `common/`. There is no Makefile here: the build rule lives in [buildroot/package/perkinsos-shell/perkinsos-shell.mk](../buildroot/package/perkinsos-shell/perkinsos-shell.mk). Rebuild with `make shell` from the top of the repo.

To add an app: create `shell/NAME/main.c`, and add a line to the list in [common/apps.h](common/apps.h) so the dock and launcher show it.

Suggested reading order: `common/client.c`, then `dock/main.c` (start at `main()`), then `launcher/main.c` (keyboard input), then `sysinfo/main.c` (xdg-shell).

Debugging: run a client by hand from the serial console with `WAYLAND_DEBUG=1 perkinsos-dock` to see every protocol message in both directions.
