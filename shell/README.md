# shell/

The PerkinsOS dock, launcher and apps. Each folder here becomes one program in the image:

| Folder | Program | What it is | Started by |
| --- | --- | --- | --- |
| `wallpaper/` | `perkinsos-wallpaper` | Animated backdrop: the view from a cockpit, with three jets in formation ahead, a HUD, a radar and RWR showing running programs, and panel keys that start apps (layer-shell, background layer) | `perkinsos-session` at boot |
| `dock/` | `perkinsos-dock` | Bar on the bottom edge: launcher button, app buttons, clock (layer-shell) | `perkinsos-session` at boot |
| `launcher/` | `perkinsos-launcher` | Search box: type to filter, Enter to start (layer-shell, takes the keyboard) | Dock button, or Alt+P |
| `sysinfo/` | `perkinsos-sysinfo` | Window showing kernel, uptime, memory, load (xdg-shell) | Dock or launcher |
| `common/` | (none) | Shared code: shm buffers, event loop step, drawing helpers, the app list, the theme | compiled into every program |

All `.c` files in a folder are compiled together with everything in `common/`. There is no Makefile here: the build rule lives in [buildroot/package/perkinsos-shell/perkinsos-shell.mk](../buildroot/package/perkinsos-shell/perkinsos-shell.mk). Rebuild with `make shell` from the top of the repo.

The look (colours, font, line weight) is defined once in [common/theme.h](common/theme.h). The window borders in `compositor/tinywl.c` and the terminal colours in `buildroot/overlay/etc/xdg/foot/foot.ini` repeat the same greens.

The wallpaper is three files: `wallpaper/main.c` (surface, two buffers, frame callbacks, mouse), `wallpaper/scene.c` (drawing one frame) and `wallpaper/procs.c` (reading programs from `/proc`). To add an aircraft: write a `draw_NAME()` function (its rear view, built from `wings()`, `fin()`, `nozzle()`) and add a line to `formation[]` in [wallpaper/scene.c](wallpaper/scene.c). The cockpit's keys come from `common/apps.h`, the same list the dock uses. The frame rate is `FRAMES_PER_SECOND` in `wallpaper/main.c`; everything is drawn by the CPU, so higher costs more.

To add an app: create `shell/NAME/main.c`, and add a line to the list in [common/apps.h](common/apps.h) so the dock and launcher show it.

Suggested reading order: `common/client.c`, then `dock/main.c` (start at `main()`), then `launcher/main.c` (keyboard input), then `sysinfo/main.c` (xdg-shell), then `wallpaper/` (animation).

Debugging: run a client by hand from the serial console with `WAYLAND_DEBUG=1 perkinsos-dock` to see every protocol message in both directions.
