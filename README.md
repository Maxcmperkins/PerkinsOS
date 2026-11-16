# PerkinsOS

A custom Linux-based desktop OS. The kernel is real Linux; the desktop on top of it is this repo: a Wayland compositor, dock, launcher and an app, booting straight to the desktop in QEMU. The desktop is the view from a fighter cockpit: a Typhoon and an F-35 fly in formation ahead of you over sea and cloud, seen through the HUD, and the instrument panel has a radar and a radar warning receiver that show the machine's running programs, plus keys that start apps. The aircraft are 3D models, lit and drawn by a small software renderer. They are downloaded models converted to a simple mesh format (see Credits).

[![CI](https://github.com/Maxcmperkins/PerkinsOS/actions/workflows/ci.yml/badge.svg)](https://github.com/Maxcmperkins/PerkinsOS/actions/workflows/ci.yml)

![The PerkinsOS desktop: the view from a fighter cockpit](docs/screenshot.png)

![Locking a radar contact, starting an app from the cockpit, the launcher, a terminal](docs/demo.gif)

Both are produced by `make demo` from headless boots: every frame is a screenshot of the real VM, driven by injected key presses and mouse clicks. The still is at 1920x1080; the GIF is half-size and limited to 252 colours.

## What it is

| Part | Source |
| --- | --- |
| Linux kernel, Mesa, libinput, wlroots, foot | Upstream, built from source by Buildroot |
| Compositor (`compositor/`) | Forked from TinyWL (wlroots 0.19.2); wallpaper, borders, keybindings, layer-shell and maximize added |
| Wallpaper, dock, launcher, System Info app (`shell/`) | Written for this project: plain C on wayland-client, cairo and xkbcommon |
| Boot-to-desktop init (`buildroot/overlay/`) | Written for this project |
| Build, tests, CI (`Makefile`, `buildroot/`, `ci/`) | Written for this project |

The code written for this project was written with AI assistance (Claude). Every change to the TinyWL fork is marked `PerkinsOS:` in [compositor/tinywl.c](compositor/tinywl.c), and `git diff 068d7ce -- compositor/` shows exactly what was added to stock TinyWL.

## Architecture

```
QEMU (x86_64, virtio-gpu)
└── Linux kernel
    └── init (boots straight to desktop)
        └── PerkinsOS compositor (wlroots)
            ├── wallpaper    (layer-shell client, background layer)
            ├── dock         (layer-shell client, bottom edge)
            ├── launcher     (layer-shell client, overlay, takes the keyboard)
            └── apps         (xdg-shell clients)
```

More in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Using it

| Key | Action |
| --- | --- |
| Alt+P, or the dock's Apps button | Open the launcher: type to filter, Enter to start, Escape to cancel |
| Alt+Enter | Open a terminal |
| Alt+Q | Close the focused window |
| Alt+F1 | Switch to the next window |
| Alt+Escape | Quit the compositor |

Click a window to focus it. Drag a title bar to move a window.

The cockpit is live:

- **Keys** on the instrument panel start apps, like the dock's buttons. They light up under the pointer.

- **Radar** (left screen): every contact is a running program. Older programs sit near the centre; one you have just started appears at the edge. Bigger dots use more memory. Hover a contact to see what it is in the data display between the screens; click to lock it and keep its name, PID, memory and CPU there. Click again to break the lock.
- **RWR** (right screen): every symbol is a program using the CPU right now, labelled with the first two letters of its name. The busier it is, the closer to the centre; the busiest gets a diamond.

- **Dials** (on a wide screen): CPU, memory, load average and number of running programs.

The flight (heading, speed, altitude) is a simulation; the radar, RWR and dials are real data from `/proc`.

### Frame rate

Nothing here uses a graphics card: every pixel is computed by the CPU, inside a virtual machine. Without KVM (hardware virtualisation) QEMU emulates that CPU in software and the desktop manages about 13 frames a second at 1920x1080. With KVM it should reach its cap of 30. See step 4 of [docs/SETUP.md](docs/SETUP.md). The wallpaper measures itself and writes its frame rate, and where the time goes, to `/var/log/desktop.log` every ten seconds. `make run SCREEN=1280x720` is also much lighter.

## Build and run

Requires Linux (WSL2 Ubuntu on Windows works). Setup is in [docs/SETUP.md](docs/SETUP.md).

```sh
make image          # build the Buildroot image (first build ~25 min)
make run            # boot it in QEMU at 1920x1080 (SCREEN=1280x720 for smaller)
make test           # boot headless, log in over serial, power off
make test-desktop   # boot headless and exercise the desktop
make demo           # record docs/demo.gif and docs/screenshot.png
make compositor     # rebuild only compositor/ (seconds)
make shell          # rebuild only shell/ (seconds)
```

## Repo layout

```
buildroot/    Buildroot external tree: configs, packages, rootfs overlay, init
distro/       build and test VM for running the desktop on Ubuntu instead
compositor/   the Wayland compositor
shell/        dock, launcher, apps, and code they share
ci/           headless tests; screenshot, mouse and GIF tools
docs/         plan, setup, architecture, week-by-week notes, write-up
```

## Roadmap

- [x] Week 1: boot a Buildroot image in QEMU
- [x] Week 2: graphics stack + stock TinyWL running
- [x] Week 3: compositor made my own
- [x] Week 4: layer-shell protocol
- [x] Week 5: dock
- [x] Week 6: launcher + first app
- [x] Week 7: boot to desktop + CI
- [x] Week 8: polish + write-up

## Credits

The Typhoon and F-35 meshes in `buildroot/overlay/usr/share/perkinsos/models/` are converted from Sketchfab models with [tools/glb2mesh.py](tools/glb2mesh.py):

- Eurofighter Typhoon (UK) by 42manako, [CC BY-NC 4.0](http://creativecommons.org/licenses/by-nc/4.0/): https://sketchfab.com/3d-models/eurofighter-typhoon-uk-716c3c241f4b42059ec976de5cfb3e34
- F-35 Lightning II: TODO author, licence and link

## Later (not now)

Ideas parked here so they don't derail the plan. The long-term goal, running real apps and using this instead of Windows, has its own plan, [docs/PLAN-DAILY-DRIVER.md](docs/PLAN-DAILY-DRIVER.md), and a roadmap of milestones, [docs/ROADMAP.md](docs/ROADMAP.md).

- Key repeat in the launcher, and clicking a result to start it
- Icons on the dock; System Info as cockpit gauges
- Server-side title bars, so every window gets the same one
- Alt+drag to move a window from anywhere
- Multiple screens: layer surfaces and maximize currently assume the first one
