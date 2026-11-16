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
 *   5. loop: redraw when the pointer moves over a button or the minute
 *      changes; start a program when a button is clicked
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

	/* Background, with a one-pixel green line along the top edge. */
	cairo_set_source_rgb(cr, HUD_BLACK);
	cairo_paint(cr);
	cairo_set_source_rgb(cr, HUD_DIM);
	cairo_rectangle(cr, 0, 0, dock->width, 1);
	cairo_fill(cr);

	cairo_select_font_face(cr, UI_FONT, CAIRO_FONT_SLANT_NORMAL,
		CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, 13);
	cairo_set_line_width(cr, HUD_LINE);

	/* Buttons: an outline with the label inside, like the soft keys round
	 * a cockpit display. The one under the pointer is drawn in inverse
	 * video (solid green, dark text). */
	int top = (DOCK_HEIGHT - BUTTON_HEIGHT) / 2;
	for (int i = 0; i < BUTTON_COUNT; i++) {
		/* The half-pixel offset centres a 1.5 px line on the pixel grid,
		 * which keeps straight edges crisp. */
		rounded_rect(cr, button_x(i) + 0.5, top + 0.5,
			BUTTON_WIDTH - 1, BUTTON_HEIGHT - 1, 3);
		if (i == dock->hover) {
			cairo_set_source_rgb(cr, HUD_GREEN);
			cairo_fill(cr);
			cairo_set_source_rgb(cr, HUD_BLACK);
		} else {
			cairo_set_source_rgb(cr, HUD_PANEL);
			cairo_fill_preserve(cr);
			/* The launcher button gets the bright outline. */
			if (i == 0) {
				cairo_set_source_rgb(cr, HUD_GREEN);
			} else {
				cairo_set_source_rgb(cr, HUD_DIM);
			}
			cairo_stroke(cr);
			cairo_set_source_rgb(cr, HUD_GREEN);
		}
		/* Cut a long name short so it stays inside its button. */
		char label[14];
		snprintf(label, sizeof(label), "%.13s", button_label(i));
		draw_text(cr, label,
			button_x(i) + (BUTTON_WIDTH - text_width(cr, label)) / 2,
			DOCK_HEIGHT / 2.0);
	}

	/* Clock, right-aligned. The VM's clock runs on UTC, which aviation
	 * writes with a Z ("Zulu time"). */
	char clock_text[16];
	time_t now = time(NULL);
	strftime(clock_text, sizeof(clock_text), "%H:%MZ", gmtime(&now));
	cairo_set_source_rgb(cr, HUD_GREEN);
	draw_text(cr, clock_text,
		dock->width - text_width(cr, clock_text) - 16, DOCK_HEIGHT / 2.0);

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
	set_hover(data, -1);
}

static void pointer_motion(void *data, struct wl_pointer *pointer,
		uint32_t time, wl_fixed_t x, wl_fixed_t y) {
	set_hover(data, button_at(wl_fixed_to_double(x), wl_fixed_to_double(y)));
}

static void pointer_button(void *data, struct wl_pointer *pointer,
		uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
	struct dock *dock = data;
	/* Act on press of the left button over a button. */
	if (button == BTN_LEFT && state == WL_POINTER_BUTTON_STATE_PRESSED &&
			dock->hover >= 0) {
		spawn(button_command(dock->hover));
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
	struct dock dock = { .hover = -1, .running = true };
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
