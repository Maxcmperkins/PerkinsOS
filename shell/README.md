# shell/

The PerkinsOS dock, launcher and apps. Each folder here becomes one program in the image:

| Folder | Program | What it is | Started by |
| --- | --- | --- | --- |
| `wallpaper/` | `perkinsos-wallpaper` | Animated backdrop: the view from a cockpit, with two jets in formation ahead, a HUD, a radar and RWR showing running programs, and a centre display whose pages list the apps and show the system (layer-shell, background layer) | `perkinsos-session` at boot |
| `dock/` | `perkinsos-dock` | Bar on the bottom edge: launcher button, app buttons, clock (layer-shell) | `perkinsos-session` at boot |
| `launcher/` | `perkinsos-launcher` | Search box: type to filter, Enter to start (layer-shell, takes the keyboard) | Dock button, or Alt+P |
| `sysinfo/` | `perkinsos-sysinfo` | Window showing kernel, uptime, memory, load (xdg-shell) | Dock or launcher |
| `common/` | (none) | Shared code: shm buffers, event loop step, drawing helpers, finding installed apps, the theme | compiled into every program |

All `.c` files in a folder are compiled together with everything in `common/`. There is no Makefile here: the build rule lives in [buildroot/package/perkinsos-shell/perkinsos-shell.mk](../buildroot/package/perkinsos-shell/perkinsos-shell.mk). Rebuild with `make shell` from the top of the repo.

The look (colours, font, line weight) is defined once in [common/theme.h](common/theme.h). The window borders in `compositor/tinywl.c` and the terminal colours in `buildroot/overlay/etc/xdg/foot/foot.ini` repeat the same greens.

The wallpaper is nine files: `wallpaper/main.c` (surface, two buffers, frame callbacks, frame-rate control, mouse), `wallpaper/scene.c` (HUD, cockpit, instruments), `wallpaper/sky.c` (the sky, sea and cloud, written pixel by pixel with a perspective texture), `wallpaper/model.c` (the 3D aircraft: loading the meshes, and a software renderer with lighting and a depth buffer), `wallpaper/surface.c` (light and shadow on the cockpit), `wallpaper/vapour.c` (wing-tip trails), `wallpaper/pages.c` (the centre display's pages), `wallpaper/startup.c` (the cockpit powering up) and `wallpaper/procs.c` (reading programs from `/proc`). It is designed on a canvas 768 units tall and scaled to the screen, so it fills any resolution. To add an aircraft: convert a .glb with [tools/glb2mesh.py](../tools/glb2mesh.py) into `buildroot/overlay/usr/share/perkinsos/models/`, load it with `load_mesh()` in [wallpaper/model.c](wallpaper/model.c), and give it a slot in `formation[]` in [wallpaper/scene.c](wallpaper/scene.c). The cockpit's keys come from `common/apps.h`, the same list the dock uses. The frame rate adapts: `wallpaper/main.c` draws up to `MAX_FRAMES_PER_SECOND` but never spends more than `MAX_CPU_SHARE` of one core.

To add an app of your own: create `shell/NAME/main.c`, and add a line to the `builtin` list in [common/apps.c](common/apps.c) so the dock, cockpit and launcher show it. Apps installed on the system (anything with a `.desktop` file) appear in the launcher by themselves; to give one a button on the dock and in the cockpit, put its `.desktop` file's name on a line in `~/.config/perkinsos/pinned`.

Suggested reading order: `common/client.c`, then `dock/main.c` (start at `main()`), then `launcher/main.c` (keyboard input), then `sysinfo/main.c` (xdg-shell), then `wallpaper/` (animation).

Debugging: run a client by hand from the serial console with `WAYLAND_DEBUG=1 perkinsos-dock` to see every protocol message in both directions.
