# Week 5: the dock

Draft by Claude; Max edits.

The dock is your first Wayland **client**: a separate program that connects to your compositor over a socket, exactly like foot does. It needs week 4 (layer-shell) working first.

## Start

```sh
mkdir shell/dock
$EDITOR shell/dock/main.c
make shell && make run
```

Any `.c` file in `shell/dock/` is compiled into `/usr/bin/perkinsos-dock`, and `perkinsos-session` starts it at boot. The build generates the protocol headers for you and links wayland-client, xkbcommon and cairo; the rule is in `buildroot/package/perkinsos-shell/perkinsos-shell.mk` and is worth reading once.

Headers you'll include:

```c
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"                  /* layer-shell refers to it */
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include <cairo.h>
```

## Build it in four runnable stages

Commit after each. Each one teaches a single idea.

### 1. Connect and list globals

`wl_display_connect(NULL)`, get the registry, add a listener, `wl_display_roundtrip`. Print every global's interface name. Bind `wl_compositor`, `wl_shm` and `zwlr_layer_shell_v1` when you see them.

Question first: why is a *roundtrip* needed before the globals have arrived?

### 2. A coloured bar on the bottom edge

- `wl_compositor_create_surface`
- `zwlr_layer_shell_v1_get_layer_surface(layer_shell, surface, NULL, ZWLR_LAYER_SHELL_V1_LAYER_TOP, "dock")`
- set size (0 wide = "as wide as the anchors make me", 48 high), anchor bottom+left+right, exclusive zone 48
- `wl_surface_commit` with **no buffer attached**. The compositor answers with a `configure` event carrying the real width.
- In the configure handler: `ack_configure`, then make a buffer that size, fill it, attach, commit.

The buffer is shared memory: `memfd_create`, `ftruncate`, `mmap`, then `wl_shm_create_pool` and `wl_shm_pool_create_buffer` with `WL_SHM_FORMAT_ARGB8888`. You write pixels into the mmap; the compositor reads the same memory.

If nothing appears, the usual cause is attaching a buffer before the first configure, which is a protocol error. Run with `WAYLAND_DEBUG=1 perkinsos-dock` from the serial console to see every message both ways.

### 3. Draw with cairo

Wrap the mmap in a cairo surface, `cairo_image_surface_create_for_data(data, CAIRO_FORMAT_ARGB32, width, height, stride)`, and draw rounded rectangles and text labels for a few apps. Cairo's ARGB32 and Wayland's ARGB8888 are the same byte layout, which is why this works with no copying.

### 4. Clicks launch apps

Bind `wl_seat`, get its `wl_pointer`, listen for `motion` (remember x) and `button`. On click, work out which icon x falls in and `fork()` + `execlp()` the app. Start with `foot`.

Question first: after `fork()`, why must the child call `exec` and not just carry on?

## Done when

- The dock is on screen at boot, windows don't cover it when maximized, and clicking an icon opens an app.
- `make test-desktop` passes; the screenshot shows the dock.

## References

- The Wayland Book, chapters 5–9 (surfaces, shm buffers, seats): https://wayland-book.com
- The protocol XML is the spec for every request and event you call.
