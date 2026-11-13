# Week 3: make TinyWL yours

Draft by Claude; Max edits. Line numbers are for `tinywl.c` from wlroots 0.19.2, the file `make fork-tinywl` gives you.

## Start

```sh
make fork-tinywl      # copies tinywl.c, Makefile, LICENSE into compositor/
git add compositor && git commit      # your first compositor commit: the untouched fork
make compositor       # rebuild just the compositor and repack the image
make run              # the desktop now runs YOUR binary, not stock TinyWL
```

Commit the untouched fork first, so `git diff` against that commit always shows exactly what you changed. That diff is your interview story.

The edit loop from here on is: edit `compositor/tinywl.c`, `make compositor`, `make run`. If the screen stays black, log in on the serial console (the terminal you ran `make run` in) and read `/var/log/desktop.log`.

## Read before you write

Read `main()` (line 874) top to bottom once. It is the whole architecture in 200 lines: create the display, backend, renderer, scene; hook up listeners; run the event loop. Everything else in the file is a callback that `main()` registered with `wl_signal_add`.

Three ideas carry the whole file:

1. **Listeners.** wlroots tells you things by emitting signals. `wl_signal_add(&thing->events.x, &listener)` says "call my function when x happens". Find any callback's trigger by searching for where its listener is added.
2. **The scene graph.** You don't draw. You add nodes (surfaces, rectangles, trees of them) to `server.scene` and wlroots works out what to repaint. `output_frame` (line 560) is the only place rendering happens and it's four lines.
3. **Toplevel.** Wayland's word for an application window. `struct tinywl_toplevel` (line 81) is your per-window state.

## The four changes

Do them in this order. Each one is small and each gets its own commit.

### 1. Wallpaper (solid colour first)

- Where: `server_new_output` (line 596), which runs once per screen.
- API: `wlr_scene_rect_create(parent, width, height, color)` makes a filled rectangle node. `color` is `float[4]`, RGBA, 0.0–1.0.
- Problem to solve: it must sit *behind* every window. Look up `wlr_scene_node_lower_to_bottom`.
- Before you run it: what do you expect to see where there is no window today, and why is it black now?

### 2. Window borders

- Where: `server_new_xdg_toplevel` (line 790) creates `toplevel->scene_tree`. A border is four thin `wlr_scene_rect`s that move with that tree.
- Problem to solve: the rects need the window's size, which isn't known at creation. Find where TinyWL already reads a window's size (`process_cursor_resize`, line 389) and decide which event should resize your rects (`xdg_toplevel_commit`, line 681, is a candidate; say why).
- Stretch: change the border colour in `focus_toplevel` (line 112) so the focused window stands out.

### 3. Keybindings

- Where: `handle_keybinding` (line 173). The Alt check that gates it is at line 218.
- Add one that launches `foot`, so you can open a second window without the serial console. That means `fork()` then `execl()`; look at how `main()` runs the `-s` startup command near the end.
- Add one that closes the focused window: `wlr_xdg_toplevel_send_close`.

### 4. Click-to-focus

TinyWL already does this. Your job is to be able to explain it: trace a mouse click from `server_cursor_button` (line 515) through `desktop_toplevel_at` (line 347) to `focus_toplevel` (line 112). Then answer: why does `focus_toplevel` call `wlr_scene_node_raise_to_top`, and what would the desktop feel like without it?

## Done when

- `make test-desktop` passes and `output/desktop-test.png` shows your wallpaper colour and a bordered window.
- You can explain every line you added without looking anything up.

## References

- wlroots 0.19 headers are the API docs: `output/host/x86_64-buildroot-linux-gnu/sysroot/usr/include/wlroots-0.19/wlr/types/wlr_scene.h`
- The Wayland Book, chapters 1–4: https://wayland-book.com
