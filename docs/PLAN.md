# PerkinsOS plan

Draft by Claude; Max edits. Last updated 16 November 2026.

An earlier version of this plan lives at https://claude.ai/artifact/21SGbetMZKTBkYRAKnST1U. That page has not been updated; this file is the current one.

The plan for what comes next, using PerkinsOS instead of Windows, is in [PLAN-DAILY-DRIVER.md](PLAN-DAILY-DRIVER.md).

## Where things stand

The original eight-week plan is finished, and the project has moved into a second phase that the plan never covered: turning the desktop into the view from a fighter cockpit.

- **Works today:** `make run` boots a Buildroot Linux image in QEMU straight to the PerkinsOS desktop. Both automated tests pass.
- **On GitHub:** everything up to the end of week 8 (commit `74cd93b`), and CI passed on it.
- **Only on Max's machine:** the cockpit phase and everything after it. GitHub still shows the old blue desktop.
- **Frame rate:** about 13 frames a second at 1920x1080 without KVM. Max switched KVM on by hand on 16 November (`sudo chmod 666 /dev/kvm`); that resets when WSL restarts, and the frame rate with it has not been measured yet.

## Done means (original goals)

- [x] `make run` builds the image and boots it in QEMU
- [x] My compositor draws windows, wallpaper and cursor; handles focus, move, close
- [x] My own dock/taskbar and app launcher
- [x] At least one app of my own
- [x] README with architecture diagram, demo GIF, and CI that boots the image

One caveat on "my": the plan assumed Max would write the compositor and shell code himself. On 13 November he asked Claude to write all of it. The README says so.

## What happened

### Phase 1: the eight weeks (9 to 15 November 2026)

Planned at 5 to 8 hours a week over eight weeks. Done in about a week of sessions, because Claude wrote the code.

| Week | Planned | What was built |
| --- | --- | --- |
| 1 | Buildroot image boots to a prompt | Buildroot 2026.02.3, Linux 6.18.7, `make image` / `make run`, a headless boot test |
| 2 | Graphics stack + stock TinyWL | Mesa, wlroots 0.19.2, stock TinyWL packaged from the wlroots tarball, foot terminal |
| 3 | Make TinyWL mine | Fork in `compositor/`: wallpaper, focus-coloured borders, Alt+Enter / Alt+Q / Alt+P, window cascade |
| 4 | wlr-layer-shell | Layer trees, `arrange_layers()`, keyboard focus for layer surfaces, maximize that stops at the dock |
| 5 | Dock | `shell/dock`: app buttons and a clock, drawn with cairo into shared memory |
| 6 | Launcher + one app | `shell/launcher` (type to filter) and `shell/sysinfo` |
| 7 | Boot to desktop + CI | Init script starts the compositor at boot; GitHub Actions builds the image and runs the tests |
| 8 | Demo, README, write-up | `make demo` records a GIF; README, ARCHITECTURE.md, an outline for the write-up |

### Phase 2: the cockpit (15 November 2026 onwards)

Not in the original plan. Each step came from Max looking at the result and asking for more.

1. **HUD-green theme** across the dock, launcher, System Info, borders and terminal.
2. **Animated wallpaper** with three jets, plus a radar and an RWR that show real data: radar contacts are running programs, RWR threats are programs using the CPU.
3. **First-person cockpit view**: jets ahead of you, a HUD, an instrument panel, and panel keys that start apps.
4. **Realistic look at 1920x1080**: blue sky over sea and cloud, shaded jets, grey cockpit with green displays. The cockpit scales to any resolution.
5. **3D aircraft**: the Typhoon, Tornado and Lightning became 3D models drawn by a software renderer written for this project. Four live dials and more cockpit detail were added. The frame rate now adapts to the machine instead of being capped at 12.
6. **Downloaded aircraft** (16 November): the Typhoon and an F-35 are now models from Sketchfab, converted by `tools/glb2mesh.py` into a simple mesh file that the wallpaper loads. The Tornado, the Lightning and all the code that built aircraft from parts were removed. Two jets fly, not three.
7. **A more realistic cockpit**: app buttons round the edge of a centre display, a smaller HUD glass that leans back, a padded hood over the panel, and four small standby gauges on wide screens.
8. **Installed apps**: the launcher lists every app that has a `.desktop` file, and a pins file adds apps to the dock and the cockpit.

### Phase 3: use it instead of Windows (16 November 2026 onwards)

See [PLAN-DAILY-DRIVER.md](PLAN-DAILY-DRIVER.md) and [ROADMAP.md](ROADMAP.md). Stage 1 is done: the desktop runs on Ubuntu 26.04 in a VM and Firefox starts from the launcher.

## Decisions made

| Decision | Choice | Why |
| --- | --- | --- |
| Compositor | C + wlroots, forked from TinyWL | Original plan |
| Dock, launcher, apps | Separate layer-shell / xdg-shell clients | Original plan |
| Image, target, dev host | Buildroot, x86_64 in QEMU, WSL2 Ubuntu | Original plan |
| Toolchain | Prebuilt Bootlin glibc toolchain | Saves about 20 minutes per clean build |
| Versions | Buildroot 2026.02.3 pins wlroots 0.19.2; kernel 6.18.7 | One number pins everything; the fork matches the library |
| Rendering | Software (wlroots pixman renderer), no GPU | Works the same on WSL2 and on CI with no 3D support from the host |
| Who writes the code | Claude, at Max's request | Changed on 13 November; `CLAUDE.md` still says Max writes `compositor/` and `shell/` |
| Theme | Fighter cockpit, green displays | Max's choice |
| Instruments | Show real machine data, not decoration | Makes the radar, RWR and dials useful |
| Art | Drawn by code, except the two aircraft, which are downloaded models | Max wanted real aircraft. Their licences have to allow a public repo (see below) |
| Aircraft | Downloaded models, converted to mesh files during development; software renderer in whole-number maths | Floating point is slow without KVM |
| Resolution | 1920x1080 by default; tests at 1024x768 | HD was asked for; tests click fixed pixel positions |

## Decisions to make

In rough order of how much they block.

### 1. Enable KVM

Without it the CPU itself is emulated and the desktop reaches about 13 frames a second at 1080p. This needs Max's password, so only he can do it: `sudo chmod 666 /dev/kvm` to try it, and step 4 of [SETUP.md](SETUP.md) to make it permanent. It has not been tested on this laptop. Almost every other decision here depends on how fast things are afterwards.

### 2. Real 3D models: done, with one thing open

Decided on 16 November: glTF models from Sketchfab, converted during development by `tools/glb2mesh.py` (not in the VM and not during the build), kept under 8,000 triangles each, with one colour per vertex taken from the model's texture.

Licences, both recorded in the README and beside the files: the Typhoon is CC BY-NC 4.0 and the F-35 is CC BY-NC-SA 4.0. Both allow sharing an adapted copy with credit, for non-commercial use only. If this project is ever used commercially, both models have to be replaced.

### 3. Software rendering or the graphics card

Everything is drawn by the CPU. The image already contains Mesa's virgl driver, which could pass 3D drawing to the host's graphics card. That would make detailed, textured models practical at full frame rate. It is a large change: the wallpaper would be rewritten on OpenGL ES, it may not work under WSL2, and CI has no graphics card. Decide after KVM: if KVM alone gives 30 frames a second with good enough models, stay with software.

### 4. Push the cockpit work

The cockpit work and everything since is local only. Pushing also runs CI on it for the first time. One thing to check first: the demo GIF and screenshots add about 1 MB per re-recording to the repo's history.

### 5. Update `CLAUDE.md`

It says Max writes `compositor/` and `shell/` and that scope is fixed by this plan. Neither is true any more. Either restore the rule for future work or rewrite it to match how the project is actually run.

### 6. Smaller ones

- **The dock** repeats the cockpit's keys. Keep it (it stays visible when a window covers the panel), shrink it to a clock, or restyle it as part of the cockpit?
- **Windows, terminal and dock don't scale** with resolution, so they are small at 1080p. Add a scale setting?
- **Default resolution**: stay at 1920x1080, or drop to 1280x720 until KVM works?
- **The write-up** ([WRITEUP.md](WRITEUP.md)) is an outline. It needs Max's own words, and it predates the cockpit.

## Next steps

See [ROADMAP.md](ROADMAP.md), starting at milestone M0.

## Later (parked)

Also listed in the README: key repeat and mouse support in the launcher, server-side title bars, Alt+drag to move windows, multiple screens, icons on the dock.

## Known limits

- Mouse interaction is tested at 1024x768 only.
- The flight (heading, speed, altitude) is simulated; only the radar, RWR and dials are real data.
- CI has not run on anything after week 8.
