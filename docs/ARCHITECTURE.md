# Architecture

Draft by Claude; Max edits. Describes the repo as of week 2 plus the build and boot plumbing for later weeks. Sections marked *planned* describe code that doesn't exist yet.

## From `make run` to pixels

```
make run
└── qemu-system-x86_64            virtual PC: virtio disk, net, GPU, keyboard, tablet
    └── bzImage                   Linux 6.18.7, loaded directly by QEMU (no bootloader)
        └── /sbin/init            BusyBox init, PID 1; runs /etc/init.d/S* in order
            ├── S10udev           eudev creates /dev/dri/card0, /dev/input/event*
            ├── ...
            └── S99desktop        starts the compositor (ours, or stock TinyWL)
                └── compositor    wlroots: owns the screen and the input devices
                    └── perkinsos-session    starts the clients
                        ├── perkinsos-dock       (planned, week 5)
                        ├── perkinsos-launcher   (planned, week 6)
                        └── foot                 fallback until the dock exists
```

A serial console (`ttyS0`) runs alongside: the terminal you typed `make run` in gets kernel messages and a root login. That is the debugging back door, and what the CI tests drive.

## How a frame reaches the screen

1. A client (foot, the dock) draws into a shared-memory buffer and sends `wl_surface.attach` + `commit` over the Wayland socket in `$XDG_RUNTIME_DIR/wayland-0`.
2. The compositor's scene graph notes that part of the screen changed.
3. On the next output frame, wlroots composites the damaged area with its **pixman** software renderer into a DRM "dumb buffer".
4. The kernel's virtio-gpu driver hands that buffer to QEMU, which shows it in its window.

Software rendering is a deliberate choice (`WLR_RENDERER=pixman` in `buildroot/overlay/etc/profile.d/perkinsos.sh`): it needs no 3D support from the host, so it works the same on WSL2 and on a CI runner. Mesa is in the image because Buildroot's wlroots package requires EGL and GBM to build.

## How input reaches a window

QEMU's virtio keyboard and tablet → kernel evdev → libinput (inside wlroots) → compositor callbacks (`server_cursor_button`, `keyboard_handle_key`) → the compositor picks the focused surface and forwards the event to that client over the socket.

## Where things live

| Path | What | Written by |
| --- | --- | --- |
| `Makefile` | Fetches pinned Buildroot, wraps build/run/test | Claude |
| `buildroot/configs/perkinsos_defconfig` | What goes in the image | Claude |
| `buildroot/package/tinywl/` | Builds stock TinyWL from the wlroots tarball | Claude |
| `buildroot/package/perkinsos-compositor/` | Builds `compositor/` | Claude |
| `buildroot/package/perkinsos-shell/` | Builds each `shell/NAME/` into `perkinsos-NAME` | Claude |
| `buildroot/overlay/` | Files copied into the image: boot-to-desktop init, session script, environment | Claude |
| `ci/` | Headless boot and desktop tests | Claude |
| `compositor/` | The Wayland compositor (*planned*, week 3–4) | Max |
| `shell/` | Dock, launcher, apps (*planned*, week 5–6) | Max |

## Pinned versions

One number pins everything: `BUILDROOT_VERSION` in the `Makefile` (2026.02.3, an LTS release). It fixes wlroots at 0.19.2, and the TinyWL package reuses the wlroots package's version and tarball, so the library and the code forked from it cannot drift apart. The kernel version (6.18.7) is pinned in the defconfig.

## Tests

- `make test`: boots headless, waits for `perkinsos login:` on the serial console, logs in, powers off.
- `make test-desktop`: boots headless, checks a compositor and a client are running, takes a screenshot through the QEMU monitor (`output/desktop-test.png`) and fails if the screen is a single colour.

Both drive the VM by writing to its serial console through a FIFO; see `ci/lib.sh`.
