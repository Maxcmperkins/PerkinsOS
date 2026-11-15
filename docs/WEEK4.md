# Week 4: wlr-layer-shell

Draft by Claude; Max edits.

> **Update:** The code this describes is now in [compositor/tinywl.c](../compositor/tinywl.c) (search for `layer`), written by Claude at Max's request. Use this page as a reading guide.

## Why

xdg-shell (what TinyWL speaks) only describes ordinary application windows: the compositor decides where they go. A dock needs to say "pin me to the bottom edge, full width, and keep other windows off me". That is what the `wlr-layer-shell-unstable-v1` protocol is for. Your dock (week 5) can't exist until your compositor implements the server side of it.

Read the protocol XML first. It is the specification, and it's readable:
`output/build/wlroots-0.19.2/protocol/wlr-layer-shell-unstable-v1.xml`

Things to be able to explain after reading it: the four **layers** (background, bottom, top, overlay), **anchor**, **exclusive zone**, and the **configure / ack_configure** handshake.

## Step 1: build plumbing (yours, in `compositor/Makefile`)

wlroots' layer-shell header includes a file that wayland-scanner must generate from the XML. Your Makefile already does this for xdg-shell; read that rule and write the same one for layer-shell.

- The build passes you `WLR_PROTOCOLS`, the folder holding the XML (see `buildroot/package/perkinsos-compositor/perkinsos-compositor.mk`).
- The header to generate is `wlr-layer-shell-unstable-v1-protocol.h`, with `wayland-scanner server-header`.
- `tinywl.o` must depend on it, or the compile races the generation.

Check: `#include <wlr/types/wlr_layer_shell_v1.h>` compiles.

## Step 2: advertise the global

In `main()`, next to `wlr_xdg_shell_create` (line 966): `wlr_layer_shell_v1_create(display, 4)` and listen for its `new_surface` event.

Check, from the serial console inside the VM:

```sh
wayland-info | grep layer_shell
```

Stock TinyWL prints nothing. Yours should print `zwlr_layer_shell_v1`. That one line is the proof the protocol is live, before any client exists.

## Step 3: put layer surfaces in the scene

The scene graph draws children in order, so stacking is just tree order. Create one `wlr_scene_tree` per layer, in this order, and move TinyWL's windows into the middle:

```
background  →  bottom  →  (xdg windows)  →  top  →  overlay
```

In your `new_surface` handler:

- If the client didn't choose an output (`layer_surface->output == NULL`), assign one.
- `wlr_scene_layer_surface_v1_create(tree_for_its_layer, layer_surface)`.
- Listen for the surface's `commit` and `destroy`.

## Step 4: arrange

Write one function, `arrange_layers(output)`, and call it when a layer surface commits and when an output changes size. It walks the layer surfaces and calls `wlr_scene_layer_surface_v1_configure(scene_layer_surface, &full_area, &usable_area)` on each.

That helper does the anchor and margin maths and *shrinks `usable_area`* by each exclusive zone. After the loop, `usable_area` is what's left for normal windows. Store it; use it when a window maximizes (`xdg_toplevel_request_maximize`, line 764) so it doesn't cover the dock.

Before you write it: with a 1024x768 screen and a 48-pixel dock anchored bottom with exclusive zone 48, what should `usable_area` be afterwards?

## Step 5: keyboard focus (needed for the launcher, week 6)

A layer surface can ask for keyboard input (`keyboard_interactive`). When the top-most one that wants it maps, give it focus with `wlr_seat_keyboard_notify_enter`; when it unmaps, give focus back to the last window.

## Done when

- `wayland-info` shows the global.
- Your wallpaper from week 3 could now be a `background`-layer client instead of a rect (optional, but a nice demo of why layers exist).
- You can draw the configure handshake on a whiteboard.

## Reference implementation to read, not copy

sway's `sway/desktop/layer_shell.c` does all of this with the same scene helpers.
