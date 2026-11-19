# PerkinsOS roadmap: from a demo in a VM to the desktop I use

Draft by Claude; Max edits. Written 16 November 2026.

[PLAN-DAILY-DRIVER.md](PLAN-DAILY-DRIVER.md) says why and sets the five stages. This file breaks them into milestones small enough to finish, in the order to do them. Each milestone ends with a check you can run or see.

## What "production" means here

Version 1.0 is: PerkinsOS is installed on my laptop, I use it every day for a month, and I do not need Windows. It is a desktop for one person on one laptop. It is not a distro for other people to install: that would need an installer, support for other hardware and a security process, and is out of scope.

## Where it is today

- Runs in QEMU two ways: the Buildroot image (`make run`) and on Ubuntu 26.04 (`distro/vm.sh`).
- On Ubuntu: the launcher lists installed apps, and Firefox starts from it and loads a page.
- Everything is drawn on the CPU. It has never run on real hardware.
- A lot of work is not committed, and CI has not run since week 8.

## Sizes

S is an evening or two. M is a few weekends. L is a month or more of weekends. These are guesses; hardware milestones can be much longer or much shorter than they look.

## The milestones

### M0. Tidy up (S)

Nothing new; make what exists safe.

- Commit the work in progress, in pieces that each do one thing, and push.
- Get CI green again on the Buildroot image.
- Fill in the F-35 model's licence in the README, or replace the model.
- Make KVM permanent (step 4 of [SETUP.md](SETUP.md)).
- Bring `CLAUDE.md` and `PLAN.md` up to date with how the project is actually run.

**Done when:** `git status` is clean, GitHub shows the cockpit, and CI passes.

**Status, 17 November 2026:** committed and pushed, CI passes, and both aircraft models' licences are recorded in the README. Still to do, both by Max: make KVM permanent, and bring `CLAUDE.md` up to date.

### M1. An hour in the VM without ssh (M)

Everything a normal sitting needs, in the Ubuntu VM, using only the keyboard and mouse.

- Windows: switch with Alt+Tab; move and resize with the mouse; a title bar with a close button on every window (the `xdg-decoration` protocol); minimise.
- Firefox for real: file open and save dialogs (needs the "portal" service: `xdg-desktop-portal` plus a backend), downloads, fullscreen video, copy and paste to and from the terminal.
- Fonts for other languages and emoji (install Noto).
- Launcher: mouse clicks, key repeat, hide the duplicate terminal entries.
- A way to shut down and restart from the desktop.

**Done when:** I browse, download a file, open it, and shut down, without touching ssh or the serial console.

**Status (17 Nov 2026), slice 1 done:** Alt+Tab; Alt+left-drag moves and Alt+right-drag resizes a window; SHUT DOWN and RESTART in the launcher; Noto fonts; `xdg-desktop-portal` with the GTK backend, started through `perkinsos-session.target`, and a portal file dialog opens on the desktop in the VM. Next: title bars with a close button, minimise, launcher clicks and key repeat, and moving "quit the compositor" off Alt+Escape.

### M2. Draw with the graphics card (L)

The biggest technical change. Today wlroots' pixman renderer draws every frame on the CPU.

- Switch the compositor to wlroots' GLES renderer. Test in the VM with virgl first.
- The wallpaper costs 45% of a CPU core all the time. Choose: redraw it on the graphics card, or pause it when a window covers it and when on battery.
- Screen scaling, so everything is a readable size on a high-resolution screen (output scale, and the `fractional-scale` protocol).
- Keep the pixman path working: CI has no graphics card.

**Done when:** the desktop runs at the screen's refresh rate in the VM with the graphics card, and the Buildroot tests still pass on the CPU.

### M3. A proper session (M)

The services every desktop app assumes are there.

- Old apps that do not speak Wayland: XWayland.
- A lock screen (the `ext-session-lock` protocol) and locking when idle. Start with `swaylock` and `swayidle`; write my own later if I want.
- Notifications. Start with an existing daemon (`mako`); restyle or replace later.
- Sound: PipeWire, with volume keys.
- A login screen (`greetd`), replacing the automatic console login.
- The plumbing apps expect: a D-Bus session, the portal services, a password store (keyring), a polkit agent for "enter your password" prompts.

**Done when:** I lock and unlock the screen, a notification appears, a video plays with sound, and an X11-only app opens.

**Status (18 Nov 2026), on Ubuntu:** notifications (mako, in the HUD's colours), sound (PipeWire; the test VM has a sound card that plays through Windows), the keyring, and password prompts (MATE's polkit agent; GNOME's crashes without X) all work, started by `perkinsos-session.target`. The XWayland server is installed, waiting for the compositor side. Still to do, all needing compositor code: XWayland itself, the lock screen (`ext-session-lock`), and volume keys.

**Login screen (19 Nov 2026), on Ubuntu:** greetd with tuigreet in the HUD's greens (`distro/greetd.toml`, installed by `sudo make -C distro install-login`). Tested in the VM: it boots to the desktop, quitting the desktop shows the login screen, and logging in with a password starts the desktop again. Opening downloads also works now: pictures, PDFs, text and zip files open in light GTK apps (Ristretto, Atril, Mousepad, Xarchiver), and links in Firefox (`distro/apps.sh`).

### M4. Boot the laptop from a USB stick (L)

Stage 2 of the plan. First contact with real hardware: MSI Prestige 14 AI Studio, Intel Arc graphics, Intel BE200 wifi.

- Build a bootable USB of the Ubuntu system with PerkinsOS on it.
- Screen on the Intel graphics. Leave the NVIDIA card off.
- Keyboard and touchpad, with tap-to-click and scroll direction settings (libinput).
- Wifi: NetworkManager, and a wifi picker on the dock.
- Brightness and volume keys; battery level on the dock.
- An external monitor: plugging in and unplugging while running.

**Done when:** the laptop boots from the USB to the cockpit, joins my wifi, and Firefox loads a page.

**Status (19 Nov 2026), the image:** `distro/usb.sh build` makes `output/usb/perkinsos-usb.img`, 24 GB, for a 32 GB stick: Ubuntu 26.04 with the desktop, the full kernel, firmware for the Arc graphics and BE200 wifi, NetworkManager and Firefox. No automatic login, no ssh, and sudo asks for the password (`perkinsos`: change it with `passwd` after the first login; greetd cannot force the change). `distro/usb.sh test` boots it in QEMU through UEFI with nothing else helping: it reaches the login screen, logs in to the cockpit, and NetworkManager is connected. Next: write it to a stick (Rufus or balenaEtcher, "DD image") and boot the laptop. Wifi, the touchpad and the screen are untested until then.

### M5. Install it beside Windows (M)

Stage 3.

- Package PerkinsOS as a `.deb`, so it installs and updates like any other program.
- Full backup of the laptop. Then shrink Windows from inside Windows and install Ubuntu beside it.
- Sleep when the lid closes, and wake again.
- Bluetooth, webcam, microphone.

**Done when:** the boot menu offers Windows and PerkinsOS, both start, and the laptop sleeps and wakes.

### M6. Survive a working day (L)

Stage 4. Fix what stops me, most painful first. Expected:

- A Settings app: screen, touchpad, wifi, sound, wallpaper.
- Screenshots; screen sharing in video calls (`xdg-desktop-portal-wlr`).
- If the compositor crashes, every open app dies with it. At least: restart the desktop automatically and keep a log of why.
- A second screen that behaves: remembered layout, windows that move with it.
- Battery life within reach of Windows.

**Done when:** one full day of my normal use without booting Windows.

### M7. A month, then 1.0 (M)

Stage 5.

- Keep a list of every time I boot Windows and why. Fix or accept each one.
- Automated test for the Ubuntu path in CI, like the Buildroot one.
- Port the compositor to the current wlroots, so it is not tied to one Ubuntu release.
- Write it up.

**Done when:** thirty days without Windows. Tag `v1.0`.

## What I do not have to write

The distro provides these; the work is connecting them, not building them: the kernel and drivers, Mesa, PipeWire, NetworkManager, Bluetooth, the portal services, the login manager, XWayland, the browser, the file manager, the office suite.

What stays mine: the compositor, the cockpit, the dock, the launcher, the settings app, and how it all fits together.

## What could stop it

- **The NVIDIA card.** The plan avoids it. If something I need only works on it, that is a real problem.
- **Sleep and wake.** The most common thing that never fully works on a Linux laptop.
- **The lock screen.** It is a security feature: a bug means an unlocked laptop. Use a well-tested locker before trusting my own.
- **Time.** At 5 to 8 hours a week this is many months. Every milestone leaves something that works, so stopping early still leaves a result.
