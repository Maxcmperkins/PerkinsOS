# PerkinsOS

A custom Linux-based desktop OS. The kernel is real Linux; everything you see is mine: my own Wayland compositor, dock, launcher and apps, booting straight to the desktop in QEMU.

> Status: week 3 of 8. The image boots straight to a desktop running stock TinyWL; my own compositor, dock and launcher are next. See [docs/PLAN.md](docs/PLAN.md) and [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

<!-- Demo GIF goes here (week 8) -->

## What's mine vs. what isn't

| Part | Source |
| --- | --- |
| Linux kernel, Mesa, libinput, wlroots | Upstream, built via Buildroot |
| Compositor (`compositor/`) | Mine, forked from TinyWL, with layer-shell added |
| Dock, launcher, apps (`shell/`) | Mine |
| Boot-to-desktop init (`buildroot/overlay/`) | Written with AI assistance |
| Build + CI (`Makefile`, `buildroot/`, `ci/`) | Written with AI assistance |

## Architecture

```
QEMU (x86_64, virtio-gpu)
└── Linux kernel
    └── init (boots straight to desktop)
        └── PerkinsOS compositor (wlroots)
            ├── dock         (layer-shell client)
            ├── launcher     (layer-shell client)
            └── apps         (xdg-shell clients)
```

## Build and run

Requires Linux (WSL2 Ubuntu on Windows works). Setup is in [docs/SETUP.md](docs/SETUP.md).

```sh
make image          # build the Buildroot image (first build ~25 min)
make run            # boot it in QEMU
make test           # boot headless, log in over serial, power off
make test-desktop   # boot headless, screenshot the desktop
make compositor     # rebuild only compositor/ (seconds)
make shell          # rebuild only shell/ (seconds)
```

## Repo layout

```
buildroot/    Buildroot external tree: configs, packages, rootfs overlay, init
compositor/   the Wayland compositor
shell/        dock, launcher, apps
ci/           headless boot and desktop tests
docs/         plan, setup, architecture, week-by-week guides
```

## Roadmap

- [x] Week 1: boot a Buildroot image in QEMU
- [x] Week 2: graphics stack + stock TinyWL running
- [x] Week 3: compositor made my own
- [ ] Week 4: layer-shell protocol
- [ ] Week 5: dock
- [ ] Week 6: launcher + first app
- [ ] Week 7: boot to desktop + CI
- [ ] Week 8: polish + write-up

## Later (not now)

Ideas parked here so they don't derail the plan.
