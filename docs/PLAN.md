# PerkinsOS plan

Draft by Claude; Max edits. Last updated 16 November 2026.

An earlier version of this plan lives at https://claude.ai/artifact/21SGbetMZKTBkYRAKnST1U. That page has not been updated; this file is the current one.

## Where things stand

The original eight-week plan is finished, and the project has moved into a second phase that the plan never covered: turning the desktop into the view from a fighter cockpit.

- **Works today:** `make run` boots a Buildroot Linux image in QEMU straight to the PerkinsOS desktop. Both automated tests pass.
- **On GitHub:** everything up to the end of week 8 (commit `74cd93b`), and CI passed on it.
- **Only on Max's machine:** the cockpit phase, 13 commits. GitHub still shows the old blue desktop.
- **Main problem:** frame rate. About 13 frames a second at 1920x1080, because KVM is not enabled.

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
| Art | Everything drawn by code, no image or model files | No licensing questions in a public repo. **Max now wants to change this for the aircraft (see below).** |
| Aircraft | 3D meshes built in code, software renderer in whole-number maths | Floating point is slow without KVM |
| Resolution | 1920x1080 by default; tests at 1024x768 | HD was asked for; tests click fixed pixel positions |

## Decisions to make

In rough order of how much they block.

### 1. Enable KVM

Without it the CPU itself is emulated and the desktop reaches about 13 frames a second at 1080p. This needs Max's password, so only he can do it: `sudo chmod 666 /dev/kvm` to try it, and step 4 of [SETUP.md](SETUP.md) to make it permanent. It has not been tested on this laptop. Almost every other decision here depends on how fast things are afterwards.

### 2. Real 3D models

Max wants to use real aircraft models in place of the ones built in code. Choices to make:

- **Which models, and their licence.** The repo is public, so each model must allow redistribution. Options: models Max makes himself; CC0 or CC-BY models (CC-BY needs a credit in the README); or keeping model files out of the repo and loading them from a folder that is not committed. Most free models found by searching are "personal use only" and cannot be committed. Many paid ones forbid redistribution too.
- **File format.** OBJ is the simplest to read (plain text: points, then triangles) and every 3D tool exports it. glTF is the modern standard and carries materials, but needs a much bigger loader. Recommendation: start with OBJ.
- **How detailed.** The current models are a few thousand triangles each and the three together take about 14 ms a frame at 1080p without KVM. A typical downloaded aircraft is 50,000 to 500,000 triangles. Without a GPU that is too slow, so models need reducing (in Blender: Decimate) to roughly 5,000 to 20,000 triangles each. The budget goes up if KVM works.
- **Colour.** The renderer has no texture mapping: it colours each corner of a triangle and blends. Options: one paint colour per part (works now); per-material colours from the model file (small addition); or real textures (a larger addition to the renderer, and slower).
- **Converted when?** Parse the model file in the VM at start-up (simple, slower boot), or convert it during the build into a compact form (faster boot, one more build step). Recommendation: convert during the build.

### 3. Software rendering or the graphics card

Everything is drawn by the CPU. The image already contains Mesa's virgl driver, which could pass 3D drawing to the host's graphics card. That would make detailed, textured models practical at full frame rate. It is a large change: the wallpaper would be rewritten on OpenGL ES, it may not work under WSL2, and CI has no graphics card. Decide after KVM: if KVM alone gives 30 frames a second with good enough models, stay with software.

### 4. Push the cockpit work

13 commits are local only. Pushing also runs CI on them for the first time. One thing to check first: the demo GIF and screenshots add about 1 MB per re-recording to the repo's history.

### 5. Update `CLAUDE.md`

It says Max writes `compositor/` and `shell/` and that scope is fixed by this plan. Neither is true any more. Either restore the rule for future work or rewrite it to match how the project is actually run.

### 6. Smaller ones

- **The dock** repeats the cockpit's keys. Keep it (it stays visible when a window covers the panel), shrink it to a clock, or restyle it as part of the cockpit?
- **Windows, terminal and dock don't scale** with resolution, so they are small at 1080p. Add a scale setting?
- **Default resolution**: stay at 1920x1080, or drop to 1280x720 until KVM works?
- **The write-up** ([WRITEUP.md](WRITEUP.md)) is an outline. It needs Max's own words, and it predates the cockpit.

## Next steps

1. Max tries KVM and reports the frame rate from `/var/log/desktop.log`.
2. Max picks one aircraft model and checks its licence. Claude adds an OBJ loader and a build step, and reports the frame rate with it.
3. Decide between software and GPU rendering from those two numbers.
4. Push, once Max is happy with how it looks.

## Later (parked)

Also listed in the README: key repeat and mouse support in the launcher, server-side title bars, Alt+drag to move windows, multiple screens, icons on the dock.

## Known limits

- Mouse interaction is tested at 1024x768 only.
- The flight (heading, speed, altitude) is simulated; only the radar, RWR and dials are real data.
- CI has not run on anything after week 8.
