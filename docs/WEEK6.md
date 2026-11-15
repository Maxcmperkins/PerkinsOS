# Week 6: launcher and first app

Draft by Claude; Max edits.

> **Update:** The launcher and app this describes are now in [shell/launcher/main.c](../shell/launcher/main.c) and [shell/sysinfo/main.c](../shell/sysinfo/main.c), written by Claude at Max's request. The launcher is started on demand and exits after one use. Use this page as a reading guide.

Two more clients. The launcher reuses everything from the dock and adds keyboard input. The app is your first ordinary window, so it uses xdg-shell instead of layer-shell.

## Launcher (`shell/launcher/` → `perkinsos-launcher`)

A panel listing apps; type to filter, Enter to launch, Escape to hide.

New ideas compared with the dock:

- **Layer `overlay`**, anchored to nothing (centred), no exclusive zone.
- **Keyboard interactivity**: `zwlr_layer_surface_v1_set_keyboard_interactivity`. This only works if your compositor implemented step 5 of [WEEK4.md](WEEK4.md).
- **Keyboard input**: `wl_seat` → `wl_keyboard`. The compositor sends you a keymap as a file descriptor; you hand it to xkbcommon (`xkb_keymap_new_from_string`), then turn each key event into a character with `xkb_state_key_get_utf8`. Key codes from Wayland are evdev codes; xkbcommon wants them plus 8. Find out why before you add the 8.
- **Show and hide**: destroying and recreating the layer surface is the simplest way. Decide how the dock asks the launcher to appear (a signal? a second program started on demand?) and write the reason down in the commit message.

Smallest first step: a centred box that prints every key you press to stderr.

## First app (`shell/NAME/` → `perkinsos-NAME`)

Pick something small you'd like to demo: a clock, a system monitor reading `/proc`, a notes pad.

New ideas:

- **xdg-shell**: `xdg_wm_base` → `xdg_surface` → `xdg_toplevel`. Same configure/ack handshake as layer-shell, in two levels.
- **Ping/pong**: the compositor pings; if you don't `xdg_wm_base_pong`, you're marked unresponsive.
- **Your compositor decides** position and may suggest a size; a size of 0x0 in configure means "you choose".
- **Close**: handle `xdg_toplevel.close` (your week 3 close keybinding sends it).
- **Redrawing on a timer** (for a clock): use `wl_surface_frame` callbacks or `poll()` on the display fd with a timeout, not a busy loop.

Then add it to the dock and the launcher's list.

## Done when

- Launcher opens, filters as you type, launches the app.
- Your app opens as a bordered window (week 3), can be moved, and closes cleanly.
- That is every box in the plan's "Done means" except the README and CI.
