/*
 * perkinsos-wallpaper: the desktop background, drawn as the view through a
 * fighter jet's head-up display: horizon and pitch ladder, heading tape,
 * speed and altitude boxes, and plan-view outlines of three aircraft.
 *
 * It is a layer-shell client like the dock, on the "background" layer and
 * anchored to all four edges, so the compositor gives it the whole screen
 * and stacks it behind every window. Nothing moves, so it draws once per
 * configure and then just sleeps in the event loop.
 *
 * Everything is drawn with cairo paths: there is no image file, and it is
 * sharp at any screen size.
 */
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "client.h"
#include "theme.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

struct wallpaper {
	struct wl_display *display;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct zwlr_layer_shell_v1 *layer_shell;

	struct wl_surface *surface;
	struct zwlr_layer_surface_v1 *layer_surface;
	struct buffer *buffer;
	int width, height;
	bool running;
};

/* ---- aircraft outlines ----
 *
 * Each aircraft is the right-hand half of its outline seen from above, nose
 * up, as x,y pairs. x runs 0 (centreline) to 1 (about half a wingspan);
 * y runs -1 (nose) to +1 (tail). The left half is the mirror image, so the
 * drawing code walks the points forwards and then backwards with x negated.
 */
struct aircraft {
	const char *name;
	const char *role;
	int point_count;
	const double (*points)[2];
};

/* Eurofighter Typhoon: canards by the cockpit, one big cropped delta wing. */
static const double typhoon_points[][2] = {
	{0.00, -1.00}, {0.05, -0.80}, {0.09, -0.60},
	{0.30, -0.50}, {0.32, -0.43}, {0.10, -0.42}, /* canard */
	{0.13, -0.24},
	{0.72, 0.50}, {0.72, 0.62},                  /* wing tip */
	{0.17, 0.68}, {0.14, 0.94}, {0.00, 0.94},    /* trailing edge, nozzles */
};

/* Panavia Tornado: swing wings (drawn swept forward) and a big tailplane. */
static const double tornado_points[][2] = {
	{0.00, -1.00}, {0.06, -0.78}, {0.13, -0.42},
	{0.24, -0.26},                               /* wing glove */
	{0.96, 0.06}, {0.96, 0.16},                  /* wing tip */
	{0.26, 0.10}, {0.19, 0.42},
	{0.54, 0.74}, {0.54, 0.85},                  /* tailplane tip */
	{0.17, 0.80}, {0.13, 0.96}, {0.00, 0.96},
};

/* English Electric Lightning: blunt nose intake, sharply swept wings with
 * the trailing edge notched out, and a matching swept tailplane. */
static const double lightning_points[][2] = {
	{0.00, -1.00}, {0.09, -0.97}, {0.12, -0.34},
	{0.70, 0.40}, {0.70, 0.54},                  /* wing tip */
	{0.12, 0.20}, {0.12, 0.56},
	{0.44, 0.86}, {0.44, 0.96},                  /* tailplane tip */
	{0.12, 0.82}, {0.10, 1.00}, {0.00, 1.00},
};

#define COUNT(array) ((int)(sizeof(array) / sizeof((array)[0])))
static const struct aircraft fleet[] = {
	{ "TORNADO", "GR4", COUNT(tornado_points), tornado_points },
	{ "TYPHOON", "FGR4", COUNT(typhoon_points), typhoon_points },
	{ "LIGHTNING", "F.6", COUNT(lightning_points), lightning_points },
};

/* Draw one aircraft centred on cx,cy; `size` is half its length in pixels. */
static void draw_aircraft(cairo_t *cr, const struct aircraft *aircraft,
		double cx, double cy, double size) {
	/* Right half nose to tail, then left half tail to nose. */
	cairo_new_path(cr);
	for (int i = 0; i < aircraft->point_count; i++) {
		cairo_line_to(cr, cx + aircraft->points[i][0] * size,
			cy + aircraft->points[i][1] * size);
	}
	for (int i = aircraft->point_count - 1; i >= 0; i--) {
		cairo_line_to(cr, cx - aircraft->points[i][0] * size,
			cy + aircraft->points[i][1] * size);
	}
	cairo_close_path(cr);
	/* fill_preserve keeps the path so the same shape can be outlined. */
	cairo_set_source_rgb(cr, HUD_FAINT);
	cairo_fill_preserve(cr);
	cairo_set_source_rgb(cr, HUD_GREEN);
	cairo_set_line_width(cr, HUD_LINE);
	cairo_stroke(cr);

	/* Name and mark underneath, centred. */
	cairo_set_font_size(cr, 13);
	cairo_set_source_rgb(cr, HUD_GREEN);
	draw_text(cr, aircraft->name,
		cx - text_width(cr, aircraft->name) / 2, cy + size + 18);
	cairo_set_font_size(cr, 11);
	cairo_set_source_rgb(cr, HUD_DIM);
	draw_text(cr, aircraft->role,
		cx - text_width(cr, aircraft->role) / 2, cy + size + 34);
}

/* A straight line from x1,y1 to x2,y2 in the current colour. */
static void line(cairo_t *cr, double x1, double y1, double x2, double y2) {
	cairo_move_to(cr, x1, y1);
	cairo_line_to(cr, x2, y2);
	cairo_stroke(cr);
}

/* A boxed read-out with a small caption above it (speed, altitude). */
static void readout(cairo_t *cr, const char *caption, const char *value,
		double x, double y) {
	const double w = 86, h = 30;
	cairo_set_font_size(cr, 11);
	cairo_set_source_rgb(cr, HUD_DIM);
	draw_text(cr, caption, x, y - 10);
	cairo_set_source_rgb(cr, HUD_GREEN);
	cairo_rectangle(cr, x, y, w, h);
	cairo_stroke(cr);
	cairo_set_font_size(cr, 16);
	draw_text(cr, value, x + (w - text_width(cr, value)) / 2, y + h / 2);
}

static void draw(struct wallpaper *wallpaper) {
	int w = wallpaper->width, h = wallpaper->height;
	if (w == 0 || h == 0) {
		return;
	}
	if (wallpaper->buffer == NULL || wallpaper->buffer->width != w ||
			wallpaper->buffer->height != h) {
		buffer_destroy(wallpaper->buffer);
		wallpaper->buffer = buffer_create(wallpaper->shm, w, h);
		if (wallpaper->buffer == NULL) {
			wallpaper->running = false;
			return;
		}
	}
	cairo_t *cr = wallpaper->buffer->cr;

	/* Sky: near-black, very slightly lighter towards the horizon. */
	double cx = w / 2.0;
	double horizon = h * 0.40;
	cairo_pattern_t *sky = cairo_pattern_create_linear(0, 0, 0, h);
	cairo_pattern_add_color_stop_rgb(sky, 0.0, HUD_BLACK);
	cairo_pattern_add_color_stop_rgb(sky, horizon / h, 0.012, 0.055, 0.028);
	cairo_pattern_add_color_stop_rgb(sky, 1.0, HUD_BLACK);
	cairo_set_source(cr, sky);
	cairo_paint(cr);
	cairo_pattern_destroy(sky);

	cairo_select_font_face(cr, UI_FONT, CAIRO_FONT_SLANT_NORMAL,
		CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_line_width(cr, HUD_LINE);

	/* Horizon: a long line with a gap in the middle for the flight path
	 * marker. */
	cairo_set_source_rgb(cr, HUD_GREEN);
	line(cr, cx - 330, horizon, cx - 44, horizon);
	line(cr, cx + 44, horizon, cx + 330, horizon);

	/* Pitch ladder: a rung every 5 degrees. Above the horizon rungs are
	 * solid, below they are dashed; each has a small tick pointing back
	 * towards the horizon and its angle written at both ends. */
	const double rung_spacing = 58;
	cairo_set_font_size(cr, 12);
	for (int step = -2; step <= 2; step++) {
		if (step == 0) {
			continue;
		}
		double y = horizon - step * rung_spacing;
		double tick = step > 0 ? 8 : -8;
		if (step < 0) {
			const double dashes[] = {9, 6};
			cairo_set_dash(cr, dashes, 2, 0);
		}
		line(cr, cx - 150, y, cx - 50, y);
		line(cr, cx + 50, y, cx + 150, y);
		cairo_set_dash(cr, NULL, 0, 0);
		line(cr, cx - 150, y, cx - 150, y + tick);
		line(cr, cx + 150, y, cx + 150, y + tick);

		char label[8];
		snprintf(label, sizeof(label), "%d", step * 5);
		draw_text(cr, label, cx - 158 - text_width(cr, label), y);
		draw_text(cr, label, cx + 158, y);
	}

	/* Flight path marker: where the aircraft is actually going. A small
	 * circle with wings and a fin. */
	cairo_new_path(cr);
	cairo_arc(cr, cx, horizon, 9, 0, 2 * M_PI);
	cairo_stroke(cr);
	line(cr, cx - 30, horizon, cx - 9, horizon);
	line(cr, cx + 9, horizon, cx + 30, horizon);
	line(cr, cx, horizon - 9, cx, horizon - 20);

	/* Heading tape across the top: a tick every 2 degrees, a longer tick
	 * and a number every 10, and a caret marking our heading (north). */
	const double tape_y = 46, tick_gap = 14;
	cairo_set_font_size(cr, 12);
	for (int i = -18; i <= 18; i++) {
		double x = cx + i * tick_gap;
		bool major = i % 5 == 0;
		line(cr, x, tape_y, x, tape_y + (major ? 12 : 6));
		if (major) {
			/* i ticks of 2 degrees either side of 360; show tens of
			 * degrees as two digits, the way a HUD does (35, 00, 01). */
			int tens = ((360 + i * 2) % 360) / 10;
			char label[8];
			snprintf(label, sizeof(label), "%02d", tens);
			draw_text(cr, label, x - text_width(cr, label) / 2, tape_y - 12);
		}
	}
	cairo_move_to(cr, cx, tape_y + 16);
	cairo_line_to(cr, cx - 7, tape_y + 28);
	cairo_line_to(cr, cx + 7, tape_y + 28);
	cairo_close_path(cr);
	cairo_stroke(cr);

	/* Speed on the left, altitude on the right, level with the horizon. */
	readout(cr, "KCAS", "450", cx - 440, horizon - 15);
	readout(cr, "ALT FT", "24500", cx + 354, horizon - 15);

	/* Small status lines under each box. */
	cairo_set_font_size(cr, 12);
	cairo_set_source_rgb(cr, HUD_DIM);
	draw_text(cr, "M 0.82", cx - 440, horizon + 36);
	draw_text(cr, "G 1.0", cx - 440, horizon + 54);
	draw_text(cr, "NAV", cx + 354, horizon + 36);
	draw_text(cr, "PERKINSOS", cx + 354, horizon + 54);

	/* The fleet along the bottom, above where the dock sits. */
	double aircraft_size = 62;
	double fleet_y = h - 48 - aircraft_size - 58;
	double fleet_gap = w / 4.0;
	int fleet_count = COUNT(fleet);
	for (int i = 0; i < fleet_count; i++) {
		double x = cx + (i - (fleet_count - 1) / 2.0) * fleet_gap;
		draw_aircraft(cr, &fleet[i], x, fleet_y, aircraft_size);
	}

	cairo_surface_flush(wallpaper->buffer->cairo_surface);
	wl_surface_attach(wallpaper->surface, wallpaper->buffer->wl_buffer, 0, 0);
	wl_surface_damage_buffer(wallpaper->surface, 0, 0, w, h);
	wl_surface_commit(wallpaper->surface);
}

/* ---- layer surface events ---- */

static void layer_surface_configure(void *data,
		struct zwlr_layer_surface_v1 *layer_surface,
		uint32_t serial, uint32_t width, uint32_t height) {
	struct wallpaper *wallpaper = data;
	zwlr_layer_surface_v1_ack_configure(layer_surface, serial);
	/* The compositor re-sends configure whenever it rearranges the layers
	 * (for example when the dock appears). Only redraw if our size really
	 * changed; the picture is otherwise identical. */
	if ((int)width == wallpaper->width && (int)height == wallpaper->height) {
		return;
	}
	wallpaper->width = width;
	wallpaper->height = height;
	draw(wallpaper);
}

static void layer_surface_closed(void *data,
		struct zwlr_layer_surface_v1 *layer_surface) {
	struct wallpaper *wallpaper = data;
	wallpaper->running = false;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = layer_surface_configure,
	.closed = layer_surface_closed,
};

/* ---- registry events ---- */

static void registry_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	struct wallpaper *wallpaper = data;
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		wallpaper->compositor = wl_registry_bind(registry, name,
			&wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		wallpaper->shm = wl_registry_bind(registry, name,
			&wl_shm_interface, 1);
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		wallpaper->layer_shell = wl_registry_bind(registry, name,
			&zwlr_layer_shell_v1_interface, 1);
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry,
		uint32_t name) {
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

int main(void) {
	struct wallpaper wallpaper = { .running = true };

	wallpaper.display = wl_display_connect(NULL);
	if (wallpaper.display == NULL) {
		fprintf(stderr, "wallpaper: cannot connect to a Wayland compositor\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(wallpaper.display);
	wl_registry_add_listener(registry, &registry_listener, &wallpaper);
	wl_display_roundtrip(wallpaper.display);
	if (wallpaper.compositor == NULL || wallpaper.shm == NULL ||
			wallpaper.layer_shell == NULL) {
		fprintf(stderr, "wallpaper: compositor lacks layer-shell or a core global\n");
		return 1;
	}

	wallpaper.surface = wl_compositor_create_surface(wallpaper.compositor);
	wallpaper.layer_surface = zwlr_layer_shell_v1_get_layer_surface(
		wallpaper.layer_shell, wallpaper.surface, NULL,
		ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, "wallpaper");
	zwlr_layer_surface_v1_add_listener(wallpaper.layer_surface,
		&layer_surface_listener, &wallpaper);
	/* Anchored to all four edges with size 0x0: "make me as big as the
	 * screen". Exclusive zone -1 means "ignore other surfaces' reserved
	 * space", so we extend underneath the dock instead of stopping at it. */
	zwlr_layer_surface_v1_set_anchor(wallpaper.layer_surface,
		ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	zwlr_layer_surface_v1_set_size(wallpaper.layer_surface, 0, 0);
	zwlr_layer_surface_v1_set_exclusive_zone(wallpaper.layer_surface, -1);
	wl_surface_commit(wallpaper.surface);

	while (wallpaper.running) {
		if (dispatch_with_timeout(wallpaper.display, -1) < 0) {
			break;
		}
	}

	buffer_destroy(wallpaper.buffer);
	wl_display_disconnect(wallpaper.display);
	return 0;
}
