# PerkinsOS phase 3: use it instead of Windows

Draft by Claude; Max edits. Written 16 November 2026.

The first two phases are in [PLAN.md](PLAN.md): the eight-week build, then the cockpit. This is the plan for what comes after. The step-by-step milestones are in [ROADMAP.md](ROADMAP.md).

## The goal

Run real applications on PerkinsOS and use it on my laptop instead of Windows. It is an experiment to see whether I can. It will take a long time, and it is allowed to fail at any stage: each stage below is worth having on its own.

## Done means

One box per stage. Later stages only start when the one before is ticked.

- [x] **Stage 1, apps:** my compositor and shell run on a normal Linux distro in a VM, and Firefox opens a web page in it
- [ ] **Stage 2, hardware:** the same thing boots on my laptop from a USB stick, with screen, keyboard, touchpad and wifi working
- [ ] **Stage 3, installed:** it is installed beside Windows (dual boot), and I can pick either at start-up
- [ ] **Stage 4, a working day:** I get through one full day of normal use without booting Windows
- [ ] **Stage 5, a month:** thirty days without booting Windows

Windows stays installed until stage 5 is ticked.

## The decision that comes first: what sits underneath

Today PerkinsOS is a Buildroot image. Buildroot builds small fixed systems: no package manager, no security updates, and no Firefox or Chromium package (checked in Buildroot 2026.02.3). A system for daily use needs thousands of packages that somebody else keeps patched.

| Option | Apps | Cost |
| --- | --- | --- |
| **A. Put my desktop on a distro** (recommended) | Everything the distro has; Firefox is one install command | The base system is the distro's, not mine. I own the compositor, shell and session |
| B. Stay on Buildroot, add packages | Only what Buildroot packages: WebKit browsers, no Firefox | Every app is a rebuild of the image; no updates |
| C. Buildroot plus Flatpak | Most desktop apps | Flatpak needs a lot of the system a distro already provides; I would be rebuilding a distro badly |

**Recommendation: A.** The Buildroot image stays as it is: it remains the demo, the CI test and the portfolio piece. The distro version is a second way to run the same `compositor/` and `shell/` code.

**Which distro.** The one hard requirement is wlroots 0.19, the version the compositor is written against. Checked on [Repology](https://repology.org/project/wlroots/versions) on 16 November 2026. The newest wlroots is 0.20.2, so rolling distros have already moved past 0.19.

| Distro | wlroots it ships | Fits? |
| --- | --- | --- |
| **Ubuntu 26.04 LTS** | 0.19.2 | Yes: the exact version Buildroot pins |
| Fedora 43 | 0.19.3 | Yes |
| Fedora 44 | 0.20.2, plus a separate `wlroots0.19` package (0.19.3) | Yes, through the extra package |
| Alpine 3.23 | 0.19.2 | Yes |
| Alpine 3.24 | 0.20.2, plus `wlroots0.19` (0.19.3) | Yes, through the extra package |
| NixOS 25.11 | 0.19.2 | Yes |
| openSUSE Tumbleweed, Void | 0.20.2, plus a 0.19 package | Yes, through the extra package |
| Arch | 0.20.2 only (0.19 is in the user-run AUR) | Not without extra work |
| Debian 13 (stable) | 0.18.2 | No |
| Ubuntu 26.10 | 0.20.2 | No |

0.19.3 is a bug-fix release of 0.19.2, so code written for one builds against the other.

**Recommendation: Ubuntu 26.04 LTS.** It ships exactly 0.19.2, it is supported for years, and it is the distro I already use in WSL, so the commands are familiar. Not yet checked: that its wlroots development package installs cleanly and the compositor builds against it. That is the first job of stage 1.

Sooner or later every distro drops 0.19, so porting the compositor to a newer wlroots will come up. It is not needed to start.

## My laptop

Read from Windows on 16 November 2026. This matters for stage 2.

| Part | What it is | What to expect on Linux |
| --- | --- | --- |
| Model | MSI Prestige 14 AI Studio C1UDXG | |
| Graphics 1 | Intel Arc (built into the CPU) | Good: open-source driver in Mesa. Use this one |
| Graphics 2 | NVIDIA GeForce RTX 3050 | The hard one for Wayland. Plan to leave it switched off at first |
| Wifi | Intel "Killer" Wi-Fi 7 BE200 | Supported by the kernel's `iwlwifi` driver; needs its firmware file in the image |
| Ethernet | Intel I219-V | Supported (`e1000e`) |

Not checked yet: screen resolution, sound, webcam, Bluetooth, fingerprint reader, sleep and wake.

## Stages

### Stage 1: apps (in a VM)

1. Install the chosen distro (see above) in a QEMU VM.
2. Build `compositor/` and `shell/` on it. The build rules live in `buildroot/package/`; they need a plain Makefile or Meson build that works outside Buildroot.
3. Start the compositor from a login on the console, the way the Buildroot image's init script does.
4. Install Firefox and open it from the launcher.
5. Fix what breaks. Likely: fullscreen, pop-up menus placed off screen, copy and paste between apps.

The app list used to be fixed at build time. The launcher now reads installed apps from the system's `.desktop` files.

**Result (16 November 2026): done.** On Ubuntu 26.04.1 in QEMU, `compositor/` and `shell/` built unchanged against Ubuntu's own wlroots 0.19.2, the desktop started from a console login, and Firefox 160 opened `https://example.com`.

How to repeat it: `distro/vm.sh create`, `start`, `install` (see the top of [distro/vm.sh](../distro/vm.sh)). The build for a distro is [distro/Makefile](../distro/Makefile).

What it took:

- Ubuntu has no package with the layer-shell protocol file, so `distro/protocol/` keeps a copy from wlroots 0.19.2.
- The wallpaper's model folder was fixed in the code; the distro build now passes it in (`MODEL_DIR`).
- `distro/perkinsos-start` does the job of the Buildroot image's init script.

What is still rough:

- ~~Firefox was started by typing a command over ssh.~~ Fixed the same day: the launcher now lists every installed app (it reads the system's `.desktop` files, in `shell/common/apps.c`), and an app named in `~/.config/perkinsos/pinned` gets a button on the dock and in the cockpit. Tested in the VM: Alt+P, typing "fire", Enter opened Firefox.
- The launcher has no icons, no mouse support and no key repeat, and it only matches on the app's name.
- Firefox logs a warning that there is no "portal" service. Portals are how a sandboxed app asks the desktop for a file picker, a screen share and so on. File open and save dialogs have not been tested.
- Some scripts on the test page show as boxes: only the DejaVu font is installed.
- All of this ran without KVM, so it was slow. Nothing about speed has been measured.
- Only one page was loaded. Video, sound, downloads, copy and paste, and fullscreen are untested.

### Stage 2: real hardware (from USB)

1. Make a bootable USB of the stage 1 system.
2. Boot the laptop from it. The compositor has only ever run on QEMU's virtual graphics card and virtual input devices.
3. Get the screen, keyboard, touchpad and wifi working, in that order.

Today the compositor draws everything on the CPU (the pixman renderer). On a real high-resolution laptop screen that will probably be too slow, so this is where it moves to the graphics card.

### Stage 3: installed

Dual boot beside Windows. Back up first. Shrink the Windows partition from inside Windows, not from Linux.

### Stage 4 and 5: living in it

Fix whatever stops me using it, most painful first. The list below is a starting guess; real use will reorder it.

## What is missing

| Area | Missing | Stage |
| --- | --- | --- |
| Compositor | Apps that do not speak Wayland (needs XWayland) | 1 |
| Compositor | Drawing with the graphics card | 2 |
| Compositor | Screen scaling, so text is readable on a high-resolution screen | 2 |
| Compositor | Touchpad settings: tap to click, natural scrolling | 2 |
| Compositor | A second screen; plugging one in while running | 4 |
| Compositor | Alt+Tab, moving and resizing windows with the mouse from anywhere | 4 |
| Compositor | Lock screen; turning the screen off when idle | 4 |
| Compositor | Screen sharing for video calls | 4 |
| Shell | ~~Launcher reads installed apps~~ done | 1 |
| Shell | Wifi picker | 2 |
| Shell | Battery and volume on the dock | 3 |
| Shell | Notifications | 4 |
| Shell | Settings app | 4 |
| Shell | Login screen | 3 |
| System | Sound (PipeWire) | 2 |
| System | Sleep and wake when the lid closes | 3 |
| Wallpaper | It uses 45% of one CPU core all the time. Needs a pause or still mode on battery | 3 |

A file manager, a text editor, a PDF viewer and the rest come from the distro. I do not have to write them.

## Risks

| Risk | Early sign | Fallback |
| --- | --- | --- |
| The distro drops wlroots 0.19 in a later release | Compositor stops building after an upgrade | Stay on the LTS release, or port the compositor to the newer version |
| The laptop's graphics do not work | Black screen in stage 2 | Use only the Intel graphics; if still black, test on an older or simpler machine first |
| Sleep, sound or wifi never fully work | A week on one driver | Accept it and note it. Plenty of Linux laptops live with one of these |
| It works but is worse than Windows for something I need | Booting Windows for the same task every day | That is a result. Write down what it was |
| Losing files during dual-boot install | | Full backup before stage 3. No exceptions |
| Drop-off | A month with no commits | Stop at the last ticked stage and write it up |

## What does not change

- The Buildroot image, `make run`, the tests and CI keep working. Phase 3 work must not break them.
- `compositor/` and `shell/` stay one codebase for both.

## Next steps

1. Enable KVM (step 4 of [SETUP.md](SETUP.md)) so the VM is fast enough to use by hand.
2. Use Firefox in the VM for real (video, a download, a file dialog, copy and paste) and fix what breaks.
3. Then stage 2: a bootable USB.
