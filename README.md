# PerkinsOS

A custom Linux-based desktop OS. The kernel is real Linux; the desktop on top of it is this repo: a Wayland compositor, dock, launcher and an app, booting straight to the desktop in QEMU.

[![CI](https://github.com/Maxcmperkins/PerkinsOS/actions/workflows/ci.yml/badge.svg)](https://github.com/Maxcmperkins/PerkinsOS/actions/workflows/ci.yml)

![PerkinsOS booting to the desktop, opening the launcher, an app and a terminal](docs/demo.gif)

The GIF is recorded by `make demo` from a headless boot: every frame is a screenshot of the real VM, driven by injected key presses.

## What it is

| Part | Source |
| --- | --- |
| Linux kernel, Mesa, libinput, wlroots, foot | Upstream, built from source by Buildroot |
| Compositor (`compositor/`) | Forked from TinyWL (wlroots 0.19.2); wallpaper, borders, keybindings, layer-shell and maximize added |
| Dock, launcher, System Info app (`shell/`) | Written for this project: plain C on wayland-client, cairo and xkbcommon |
| Boot-to-desktop init (`buildroot/overlay/`) | Written for this project |
| Build, tests, CI (`Makefile`, `buildroot/`, `ci/`) | Written for this project |

The code written for this project was written with AI assistance (Claude). Every change to the TinyWL fork is marked `PerkinsOS:` in [compositor/tinywl.c](compositor/tinywl.c), and `git diff 068d7ce -- compositor/` shows exactly what was added to stock TinyWL.

## Architecture

```
QEMU (x86_64, virtio-gpu)
└── Linux kernel
    └── init (boots straight to desktop)
        └── PerkinsOS compositor (wlroots)
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

## Build and run

Requires Linux (WSL2 Ubuntu on Windows works). Setup is in [docs/SETUP.md](docs/SETUP.md).

```sh
make image          # build the Buildroot image (first build ~25 min)
make run            # boot it in QEMU
make test           # boot headless, log in over serial, power off
make test-desktop   # boot headless and exercise the desktop
make demo           # record docs/demo.gif
make compositor     # rebuild only compositor/ (seconds)
make shell          # rebuild only shell/ (seconds)
```

## Repo layout

```
buildroot/    Buildroot external tree: configs, packages, rootfs overlay, init
compositor/   the Wayland compositor
shell/        dock, launcher, apps, and code they share
ci/           headless tests, screenshot and GIF tools
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

## Later (not now)

Ideas parked here so they don't derail the plan.

- Key repeat in the launcher, and clicking a result to start it
- A wallpaper image instead of a solid colour (a background-layer client)
- Server-side title bars, so every window gets the same one
- Alt+drag to move a window from anywhere
- Multiple screens: layer surfaces and maximize currently assume the first one
