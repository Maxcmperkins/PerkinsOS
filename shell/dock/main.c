/*
 * perkinsos-dock: a bar along the bottom of the screen with a button for
 * the launcher, a button per app, and a clock.
 *
 * It is a Wayland client that uses the layer-shell protocol: instead of
 * asking for a window, it asks to be pinned to the bottom edge and for
 * other windows to be kept off it (an "exclusive zone").
 *
 * The life of this program:
 *   1. connect to the compositor and collect the globals we need
 *   2. create a surface and give it the layer-shell role
 *   3. commit with no buffer; the compositor replies with a configure
 *      event carrying our real size
 *   4. on configure: draw a buffer that size, attach it, commit
 *   5. loop: redraw when the pointer moves over a button, one is pressed,
 *      or the minute changes; start a program when a button is clicked
 */
#include <linux/input-event-codes.h> /* BTN_LEFT */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "apps.h"
#include "client.h"
#include "theme.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#define DOCK_HEIGHT 48
#define BUTTON_WIDTH 120
#define BUTTON_HEIGHT 34
#define BUTTON_GAP 8

/* Button 0 is the launcher; buttons 1.. are the pinned apps (see
 * common/apps.h), loaded once in main(). */
static struct app_list pinned;
#define BUTTON_COUNT (pinned.count + 1)

struct dock {
	/* Globals: objects the compositor offers to every client. */
	struct wl_display *display;
	struct wl_compositor *compositor;      /* creates surfaces */
	struct wl_shm *shm;                    /* shared-memory buffers */
	struct zwlr_layer_shell_v1 *layer_shell;
	struct wl_seat *seat;                  /* input devices */
	struct wl_pointer *pointer;

	/* Our one surface and its layer-shell role. */
	struct wl_surface *surface;
	struct zwlr_layer_surface_v1 *layer_surface;
	struct buffer *buffer;
	int width, height; /* 0 until the first configure */

	int hover;      /* index of the button under the pointer, or -1 */
	int pressed;    /* the button held down by the mouse, or -1 */
	bool running;
};

static const char *button_label(int index) {
	return index == 0 ? "APPS" : pinned.items[index - 1].name;
}

static const char *button_command(int index) {
	return index == 0 ? LAUNCHER_COMMAND : pinned.items[index - 1].command;
}

/* Left edge of button `index`, in surface coordinates. */
static int button_x(int index) {
	return BUTTON_GAP + index * (BUTTON_WIDTH + BUTTON_GAP);
}

/* Which button contains this x coordinate? -1 for none. */
static int button_at(double x, double y) {
	int top = (DOCK_HEIGHT - BUTTON_HEIGHT) / 2;
	if (y < top || y >= top + BUTTON_HEIGHT) {
		return -1;
	}
	for (int i = 0; i < BUTTON_COUNT; i++) {
		if (x >= button_x(i) && x < button_x(i) + BUTTON_WIDTH) {
			return i;
		}
	}
	return -1;
}

/* ---- drawing ----
 *
 * The dock is the bottom edge of the cockpit's instrument panel: the same
 * dark painted metal as shell/wallpaper draws above it, with push buttons
 * and a clock let into it. Light comes from above, so top edges are bright
 * and bottom edges dark; a thing let in has those the other way round.
 */

/* Paint a shape with a blend of two greys, from y0 down to y1. */
static void set_greys(cairo_t *cr, double y0, double y1, double top, double bottom) {
	cairo_pattern_t *pattern = cairo_pattern_create_linear(0, y0, 0, y1);
	/* A little bluer than grey, like the rest of the metal. */
	cairo_pattern_add_color_stop_rgb(pattern, 0, top, top * 1.05, top * 1.13);
	cairo_pattern_add_color_stop_rgb(pattern, 1, bottom, bottom * 1.05, bottom * 1.13);
	cairo_set_source(cr, pattern);
	cairo_pattern_destroy(pattern);
}

/* A recess: a rounded hole with a lit lower lip, dark inside. */
static void draw_recess(cairo_t *cr, double x, double y, double w, double h) {
	rounded_rect(cr, x, y + 1, w, h, 5);
	set_greys(cr, y, y + h + 1, 0.05, 0.30);
	cairo_fill(cr);
	rounded_rect(cr, x, y, w, h, 5);
	cairo_set_source_rgb(cr, 0.018, 0.02, 0.023);
	cairo_fill(cr);
}

/* The metal, its top edge and a fastener at each end. */
static void draw_panel(struct buffer *buffer) {
	cairo_t *cr = buffer->cr;
	cairo_set_line_width(cr, 1);
	set_greys(cr, 0, DOCK_HEIGHT, 0.135, 0.070);
	cairo_paint(cr);

	/* Grain: nudge each pixel up or down a little, at random but the same
	 * every time (a small hash of its position). */
	cairo_surface_flush(buffer->cairo_surface);
	for (int y = 0; y < buffer->height; y++) {
		uint32_t *row = (uint32_t *)buffer->pixels + (size_t)y * buffer->width;
		for (int x = 0; x < buffer->width; x++) {
			uint32_t n = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
			n = (n ^ (n >> 13)) * 1274126177u;
			/* 0 to 4 on each of red, green and blue. None of them is
			 * near 255 here, so the sum can't overflow. */
			row[x] += (n >> 16) % 5 * 0x00010101u;
		}
	}
	cairo_surface_mark_dirty(buffer->cairo_surface);

	/* A seam where the panel above ends: a dark groove, then a lit edge. */
	cairo_rectangle(cr, 0, 0, buffer->width, 2);
	cairo_set_source_rgb(cr, 0.01, 0.01, 0.012);
	cairo_fill(cr);
	cairo_rectangle(cr, 0, 2, buffer->width, 1);
	cairo_set_source_rgb(cr, 0.30, 0.32, 0.35);
	cairo_fill(cr);
}

static void draw_button(cairo_t *cr, int index, bool hover, bool pressed) {
	double x = button_x(index), y = (DOCK_HEIGHT - BUTTON_HEIGHT) / 2 + 1;
	draw_recess(cr, x, y, BUTTON_WIDTH, BUTTON_HEIGHT);

	/* The cap. Pressed, it is further in: smaller, lower, and darker. */
	double in = pressed ? 4 : 2.5, down = pressed ? 1 : 0;
	double cx = x + in, cy = y + in + down;
	double cw = BUTTON_WIDTH - 2 * in, ch = BUTTON_HEIGHT - 2 * in;
	rounded_rect(cr, cx, cy, cw, ch, 4);
	if (pressed) {
		set_greys(cr, cy, cy + ch, 0.06, 0.10);
	} else {
		set_greys(cr, cy, cy + ch, hover ? 0.25 : 0.20, hover ? 0.13 : 0.10);
	}
	cairo_fill(cr);
	if (!pressed) {
		/* Light along the top edge. */
		cairo_set_source_rgba(cr, 0.8, 0.88, 1.0, 0.28);
		cairo_rectangle(cr, cx + 4, cy + 0.5, cw - 8, 1);
		cairo_fill(cr);
	}

	/* The legend, on a dark window in the cap, lit from behind. It is
	 * brighter under the pointer, and turns inside out when pressed. */
	rounded_rect(cr, cx + 4, cy + 5, cw - 8, ch - 10, 2);
	if (pressed) {
		cairo_set_source_rgb(cr, HUD_GREEN);
	} else if (hover) {
		cairo_set_source_rgb(cr, 0.02, 0.22, 0.09);
	} else {
		cairo_set_source_rgb(cr, HUD_BLACK);
	}
	cairo_fill_preserve(cr);
	/* The launcher's button, and the one under the pointer, are outlined. */
	if (hover || index == 0) {
		cairo_set_source_rgba(cr, HUD_GREEN, hover ? 0.9 : 0.45);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
	}
	cairo_new_path(cr);

	char label[16];
	shorten_name(button_label(index), 14, label, sizeof(label));
	cairo_set_font_size(cr, FONT_SIZE(11.5));
	if (pressed) {
		cairo_set_source_rgb(cr, HUD_BLACK);
	} else {
		cairo_set_source_rgb(cr, HUD_GREEN);
	}
	draw_text(cr, label, cx + (cw - text_width(cr, label)) / 2, cy + ch / 2);
}

/* The clock: a small screen let into the panel. The VM's clock runs on
 * UTC, which aviation writes with a Z ("Zulu time"). */
#define CLOCK_WIDTH 96

static void draw_clock(cairo_t *cr, int width) {
	double x = width - CLOCK_WIDTH - 22, y = (DOCK_HEIGHT - BUTTON_HEIGHT) / 2 + 1;
	draw_recess(cr, x - 3, y - 1, CLOCK_WIDTH + 6, BUTTON_HEIGHT + 2);
	rounded_rect(cr, x, y + 2, CLOCK_WIDTH, BUTTON_HEIGHT - 4, 3);
	cairo_set_source_rgb(cr, HUD_BLACK);
	cairo_fill(cr);

	char text[16];
	time_t now = time(NULL);
	strftime(text, sizeof(text), "%H:%MZ", gmtime(&now));
	cairo_set_font_size(cr, FONT_SIZE(17));
	double tx = x + (CLOCK_WIDTH - text_width(cr, text)) / 2;
	cairo_set_source_rgb(cr, HUD_GREEN);
	draw_text(cr, text, tx, DOCK_HEIGHT / 2.0 + 1);

	/* The glass over it reflects the sky, in its upper half. */
	rounded_rect(cr, x, y + 2, CLOCK_WIDTH, (BUTTON_HEIGHT - 4) / 2.0, 3);
	cairo_set_source_rgba(cr, 0.75, 0.88, 1.0, 0.07);
	cairo_fill(cr);

	/* A fastener beside it. */
	cairo_new_sub_path(cr);
	cairo_arc(cr, width - 9, DOCK_HEIGHT / 2.0 + 1, 3.4, 0, 6.2832);
	set_greys(cr, DOCK_HEIGHT / 2.0 - 3, DOCK_HEIGHT / 2.0 + 5, 0.42, 0.12);
	cairo_fill(cr);
	cairo_move_to(cr, width - 11, DOCK_HEIGHT / 2.0 + 2.5);
	cairo_line_to(cr, width - 7, DOCK_HEIGHT / 2.0 - 0.5);
	cairo_set_source_rgb(cr, 0.02, 0.02, 0.025);
	cairo_set_line_width(cr, 1.2);
	cairo_stroke(cr);
}

static void draw(struct dock *dock) {
	if (dock->width == 0) {
		return; /* not configured yet: we don't know our size */
	}
	/* (Re)create the buffer if this is the first draw or the size changed. */
	if (dock->buffer == NULL || dock->buffer->width != dock->width ||
			dock->buffer->height != dock->height) {
		buffer_destroy(dock->buffer);
		dock->buffer = buffer_create(dock->shm, dock->width, dock->height);
		if (dock->buffer == NULL) {
			fprintf(stderr, "dock: could not create a buffer\n");
			dock->running = false;
			return;
		}
	}
	cairo_t *cr = dock->buffer->cr;

	draw_panel(dock->buffer);
	cairo_select_font_face(cr, UI_FONT, CAIRO_FONT_SLANT_NORMAL,
		CAIRO_FONT_WEIGHT_NORMAL);
	for (int i = 0; i < BUTTON_COUNT; i++) {
		draw_button(cr, i, i == dock->hover, i == dock->pressed);
	}
	draw_clock(cr, dock->width);

	/* Make sure cairo has written everything to the shared memory. */
	cairo_surface_flush(dock->buffer->cairo_surface);

	/* attach: "this buffer is the surface's new content".
	 * damage: "this part of it changed" (all of it).
	 * commit: "apply everything I just said, atomically". */
	wl_surface_attach(dock->surface, dock->buffer->wl_buffer, 0, 0);
	wl_surface_damage_buffer(dock->surface, 0, 0, dock->width, dock->height);
	wl_surface_commit(dock->surface);
}

/* ---- layer surface events ---- */

static void layer_surface_configure(void *data,
		struct zwlr_layer_surface_v1 *layer_surface,
		uint32_t serial, uint32_t width, uint32_t height) {
	/* The compositor has decided our size. We must acknowledge with the
	 * same serial before attaching a buffer, so it knows which configure
	 * the buffer answers. */
	struct dock *dock = data;
	dock->width = width;
	dock->height = height;
	zwlr_layer_surface_v1_ack_configure(layer_surface, serial);
	draw(dock);
}

static void layer_surface_closed(void *data,
		struct zwlr_layer_surface_v1 *layer_surface) {
	/* The compositor took the surface away (e.g. the screen went away). */
	struct dock *dock = data;
	dock->running = false;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = layer_surface_configure,
	.closed = layer_surface_closed,
};

/* ---- pointer events ---- */

static void set_hover(struct dock *dock, int hover) {
	if (hover != dock->hover) {
		dock->hover = hover;
		draw(dock); /* repaint so the highlight follows the pointer */
	}
}

static void pointer_enter(void *data, struct wl_pointer *pointer,
		uint32_t serial, struct wl_surface *surface,
		wl_fixed_t x, wl_fixed_t y) {
	/* Coordinates arrive as fixed-point numbers; convert to doubles. */
	set_hover(data, button_at(wl_fixed_to_double(x), wl_fixed_to_double(y)));
}

static void pointer_leave(void *data, struct wl_pointer *pointer,
		uint32_t serial, struct wl_surface *surface) {
	/* Forget a held button too: if what it opened took the pointer, we may
	 * never hear that it was let go. */
	struct dock *dock = data;
	dock->pressed = -1;
	dock->hover = -1;
	draw(dock);
}

static void pointer_motion(void *data, struct wl_pointer *pointer,
		uint32_t time, wl_fixed_t x, wl_fixed_t y) {
	set_hover(data, button_at(wl_fixed_to_double(x), wl_fixed_to_double(y)));
}

static void pointer_button(void *data, struct wl_pointer *pointer,
		uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
	struct dock *dock = data;
	if (button != BTN_LEFT) {
		return;
	}
	/* Act on press of the left button over a button, which stays down
	 * until the mouse button is let go. */
	bool down = state == WL_POINTER_BUTTON_STATE_PRESSED;
	dock->pressed = down ? dock->hover : -1;
	draw(dock);
	if (dock->pressed >= 0) {
		spawn(button_command(dock->pressed));
	}
}

static void pointer_axis(void *data, struct wl_pointer *pointer,
		uint32_t time, uint32_t axis, wl_fixed_t value) {
	/* Scroll wheel: nothing to scroll. */
}

static const struct wl_pointer_listener pointer_listener = {
	.enter = pointer_enter,
	.leave = pointer_leave,
	.motion = pointer_motion,
	.button = pointer_button,
	.axis = pointer_axis,
};

/* ---- seat events ---- */

static void seat_capabilities(void *data, struct wl_seat *seat,
		uint32_t capabilities) {
	/* The seat tells us which kinds of input device exist. Ask for the
	 * pointer object once there is a pointer. */
	struct dock *dock = data;
	if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && dock->pointer == NULL) {
		dock->pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(dock->pointer, &pointer_listener, dock);
	}
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
};

/* ---- registry events ---- */

static void registry_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	/* Called once per global the compositor offers. "Binding" one creates
	 * our own object for it. The number is the protocol version we speak. */
	struct dock *dock = data;
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		dock->compositor = wl_registry_bind(registry, name,
			&wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		dock->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		dock->layer_shell = wl_registry_bind(registry, name,
			&zwlr_layer_shell_v1_interface, 1);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		dock->seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
		wl_seat_add_listener(dock->seat, &seat_listener, dock);
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry,
		uint32_t name) {
	/* A global went away. Nothing we use is ever removed. */
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

int main(void) {
	struct dock dock = { .hover = -1, .pressed = -1, .running = true };
	apps_load_pinned(&pinned);

	/* NULL: connect to the compositor named by $WAYLAND_DISPLAY. */
	dock.display = wl_display_connect(NULL);
	if (dock.display == NULL) {
		fprintf(stderr, "dock: cannot connect to a Wayland compositor\n");
		return 1;
	}

	/* Ask for the list of globals. The answers arrive as events, and a
	 * roundtrip blocks until the compositor has processed everything we
	 * sent so far, so after it registry_global has run for every global. */
	struct wl_registry *registry = wl_display_get_registry(dock.display);
	wl_registry_add_listener(registry, &registry_listener, &dock);
	wl_display_roundtrip(dock.display);

	if (dock.compositor == NULL || dock.shm == NULL) {
		fprintf(stderr, "dock: compositor is missing a core global\n");
		return 1;
	}
	if (dock.layer_shell == NULL) {
		fprintf(stderr, "dock: compositor does not support layer-shell\n");
		return 1;
	}

	/* A surface is just a rectangle of pixels with no meaning yet. Giving
	 * it a role (here: layer surface) says how it should be shown. */
	dock.surface = wl_compositor_create_surface(dock.compositor);
	dock.layer_surface = zwlr_layer_shell_v1_get_layer_surface(
		dock.layer_shell, dock.surface,
		NULL,                           /* output: let the compositor pick */
		ZWLR_LAYER_SHELL_V1_LAYER_TOP,  /* above normal windows */
		"dock");                        /* namespace: a label for our role */
	zwlr_layer_surface_v1_add_listener(dock.layer_surface,
		&layer_surface_listener, &dock);

	/* Stick to the bottom edge and stretch between left and right. Width 0
	 * means "whatever the anchors give me". */
	zwlr_layer_surface_v1_set_anchor(dock.layer_surface,
		ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	zwlr_layer_surface_v1_set_size(dock.layer_surface, 0, DOCK_HEIGHT);
	/* Reserve our height: maximized windows stop above the dock. */
	zwlr_layer_surface_v1_set_exclusive_zone(dock.layer_surface, DOCK_HEIGHT);

	/* First commit, deliberately without a buffer. The compositor answers
	 * with configure, and layer_surface_configure() draws the first frame. */
	wl_surface_commit(dock.surface);

	while (dock.running) {
		/* Sleep until the next minute starts, or until an event arrives. */
		time_t now = time(NULL);
		int ms_to_next_minute = (60 - (int)(now % 60)) * 1000;
		if (dispatch_with_timeout(dock.display, ms_to_next_minute) < 0) {
			break; /* the compositor went away */
		}
		/* Redraw if the minute rolled over, so the clock stays right. */
		if (time(NULL) / 60 != now / 60) {
			draw(&dock);
		}
	}

	buffer_destroy(dock.buffer);
	wl_display_disconnect(dock.display);
	return 0;
}
