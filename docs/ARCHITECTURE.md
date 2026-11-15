# Architecture

Draft by Claude; Max edits.

## From `make run` to pixels

```
make run
└── qemu-system-x86_64            virtual PC: virtio disk, net, GPU, keyboard, tablet
    └── bzImage                   Linux 6.18.7, loaded directly by QEMU (no bootloader)
        └── /sbin/init            BusyBox init, PID 1; runs /etc/init.d/S* in order
            ├── S10udev           eudev creates /dev/dri/card0, /dev/input/event*
            ├── ...
            └── S99desktop        starts the compositor
                └── perkinsos-compositor   wlroots: owns the screen and input devices
                    └── perkinsos-session  run once the compositor is ready
                        ├── perkinsos-wallpaper   always on screen, behind everything
                        └── perkinsos-dock        always on screen
                            ├── perkinsos-launcher   on demand (Apps button, Alt+P)
                            ├── perkinsos-sysinfo    on demand
                            └── foot                 on demand (Alt+Enter)
```

A serial console (`ttyS0`) runs alongside: the terminal you typed `make run` in gets kernel messages and a root login. That is the debugging back door, and what the tests drive.

## The compositor

`compositor/tinywl.c` is stock TinyWL plus changes marked `PerkinsOS:`. It never draws anything itself. It maintains a **scene graph** and wlroots repaints whatever changed. The scene is a stack of trees, drawn back to front:

```
wallpaper   one filled rectangle per screen (seen only until the client below draws)
background  ┐ layer-shell layers below windows: perkinsos-wallpaper is here
bottom      ┘
windows     xdg-shell toplevels, each with four border rectangles
top         ┐ layer-shell layers above windows: the dock is here
overlay     ┘ the launcher is here
```

Stacking is nothing more than which tree a surface is put in.

Two protocols describe what clients can ask for:

- **xdg-shell**: "I am an application window." The compositor chooses position; the client may ask to be moved, resized, maximized or closed.
- **wlr-layer-shell**: "Pin me to this edge, on this layer, and reserve this much space." `arrange_layers()` answers every such request for a screen and computes `usable_area`, the part of the screen left for windows. Maximize fills `usable_area`, which is why a maximized window stops at the dock.

Both use the same handshake: the client commits with no buffer, the compositor replies with a **configure** event carrying a size, the client acknowledges and attaches a buffer of that size.

## The clients

`shell/dock`, `shell/launcher` and `shell/sysinfo` are separate programs that talk to the compositor over the Unix socket `$XDG_RUNTIME_DIR/wayland-0`, exactly as foot does. They share `shell/common/`:

- `buffer_create()`: a shared-memory pixel buffer (`memfd_create` + `mmap`), wrapped in a cairo surface so drawing goes straight into memory the compositor can read.
- `dispatch_with_timeout()`: one step of the Wayland event loop with a timeout, so a client can also wake on a timer (the dock's clock, System Info's refresh).
- `apps.h`: the list of apps shown by both the dock and the launcher.
- `theme.h`: the HUD colours, font and line weight used by every client.

The wallpaper is the one client that animates. It never draws on its own timer: after each frame it asks the compositor for a **frame callback** and draws the next frame when that arrives, capped at 12 frames a second. The compositor only sends frame callbacks to visible surfaces, so a wallpaper hidden behind a maximized window stops drawing. It alternates between two buffers so it never draws into the one the compositor is showing. Its radar and RWR read the process list from `/proc` every two seconds.

Most of the wallpaper is cairo vector drawing, scaled from a 768-unit-tall design canvas to the real screen. Two parts bypass cairo and write the frame's pixels directly, because they touch most of the screen every frame: the outside view (`sky.c`), a tiling cloud texture projected in perspective onto a plane below and a plane above, one table lookup per pixel and no floating point in the inner loop; and laying the cached cockpit image over the frame, which visits only the non-transparent runs of each row. At 1920x1080 under software emulation the wallpaper uses about 16% of the VM's two CPUs and the compositor about 8%.

## How a frame reaches the screen

1. A client draws into its shared-memory buffer and sends `wl_surface.attach` + `commit`.
2. The compositor's scene graph notes that part of the screen changed.
3. On the next output frame, wlroots composites the damaged area with its **pixman** software renderer into a DRM "dumb buffer".
4. The kernel's virtio-gpu driver hands that buffer to QEMU, which shows it in its window.

Software rendering is a deliberate choice (`WLR_RENDERER=pixman` in `buildroot/overlay/etc/profile.d/perkinsos.sh`): it needs no 3D support from the host, so it works the same on WSL2 and on a CI runner. Mesa is in the image because Buildroot's wlroots package requires EGL and GBM to build.

## How input reaches a window

QEMU's virtio keyboard and tablet → kernel evdev → libinput (inside wlroots) → compositor callbacks (`server_cursor_button`, `keyboard_handle_key`). The compositor first checks its own keybindings (Alt+...), then forwards the event to the surface that has focus. Keyboard focus normally belongs to the focused window; a layer surface that asks for exclusive keyboard input (the launcher) takes it while it is open and it returns to the window afterwards.

## Where things live

| Path | What |
| --- | --- |
| `Makefile` | Fetches pinned Buildroot, wraps build/run/test/demo |
| `buildroot/configs/perkinsos_defconfig` | What goes in the image |
| `buildroot/package/tinywl/` | Builds stock TinyWL from the wlroots tarball (kept as a reference; the image prefers our compositor) |
| `buildroot/package/perkinsos-compositor/` | Builds `compositor/` |
| `buildroot/package/perkinsos-shell/` | Builds each `shell/NAME/` into `perkinsos-NAME` |
| `buildroot/overlay/` | Files copied into the image: boot-to-desktop init, session script, environment |
| `compositor/` | The Wayland compositor |
| `shell/` | Dock, launcher, System Info, shared client code |
| `ci/` | Headless tests, screenshot and GIF tools |

## Pinned versions

One number pins everything: `BUILDROOT_VERSION` in the `Makefile` (2026.02.3, an LTS release). It fixes wlroots at 0.19.2, and the compositor was forked from the TinyWL in that same wlroots tarball. The kernel version (6.18.7) is pinned in the defconfig.

## Tests

All tests boot the real image in QEMU with no window and drive it three ways: typing into the serial console through a FIFO, pressing keys on the virtual keyboard through QEMU's monitor socket, and moving and clicking the mouse through QEMU's QMP socket (`ci/lib.sh`, `ci/mouse.py`).

- `make test`: reaches `perkinsos login:`, logs in, powers off.
- `make test-desktop`: the desktop came up at boot; the screen isn't blank; Alt+Enter opens a terminal and Alt+Q closes it; layer-shell is advertised; Alt+P opens the launcher, typing filters it and Enter starts System Info; clicking a dock button starts its program; the wallpaper animates and clicking a radar contact locks it; a maximized window fills the screen and stops at the dock. Screenshots are left in `output/*-test.png`.
- `make demo`: records `docs/demo.gif` the same way.

GitHub Actions (`.github/workflows/ci.yml`) builds the image from scratch and runs both tests on every push.
