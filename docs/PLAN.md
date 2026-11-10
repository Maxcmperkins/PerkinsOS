# PerkinsOS plan

Living version: https://claude.ai/artifact/21SGbetMZKTBkYRAKnST1U

## Done means

- [ ] `make run` builds the image and boots it in QEMU
- [ ] My compositor draws windows, wallpaper and cursor; handles focus, move, close
- [ ] My own dock/taskbar and app launcher
- [ ] At least one app of my own
- [ ] README with architecture diagram, demo GIF, and CI that boots the image

Budget: ~8 weeks at 5–8 hrs/week, mostly days off.

## Decisions

- Compositor: C + wlroots, forked from TinyWL (alternative: Rust + Smithay/smallvil)
- Dock/launcher/apps: separate layer-shell clients
- Image: Buildroot · Run: QEMU + virtio-gpu · Target: x86_64 · Dev: WSL2 Ubuntu

## Weeks

1. Dev setup + Buildroot image boots to a prompt in QEMU
2. Graphics stack (Mesa, libinput, wlroots) + stock TinyWL running
3. Make TinyWL mine: wallpaper, borders, keybindings, click-to-focus
4. Implement wlr-layer-shell
5. My own dock
6. Launcher + one app
7. Boot straight to desktop + CI boot test
8. Demo GIF, architecture diagram, README, blog post

## Scope cuts if things slip

- Graphics fights back → develop compositor nested in a desktop on WSL2, package in week 7
- Two weeks with no commits → ship weeks 1–5 + write-up
