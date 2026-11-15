/*
 * Drawing the wallpaper: one frame of the view from a fighter cockpit.
 *
 * What is in the picture, back to front:
 *   outside     blue sky, sea and cloud streaming past below (sky.c)
 *   formation   three jets ahead of you, seen from behind: a Typhoon in
 *               front, a Tornado to the left, a Lightning to the right
 *   HUD         green symbols on the glass in front of you: pitch ladder,
 *               flight path marker, heading tape, speed and altitude
 *   cockpit     canopy arch, sills and instrument panel (cached)
 *   screens     left screen: radar, each contact a running program
 *               right screen: RWR, each "threat" a program using the CPU
 *   keys        the buttons between the screens start apps
 *
 * Everything is designed on a canvas 768 units tall (DESIGN_HEIGHT) and
 * scaled to the real screen, so the numbers in this file are design units,
 * not pixels. On a 1024x768 screen a unit is one pixel; on 1920x1080 it
 * is about 1.4, and because the drawing is vector shapes it stays sharp.
 *
 * Everything that moves is a function of the time `t`, so a frame can be
 * drawn for any moment without remembering the previous one.
 *
 * The flight (heading, pitch, speed, altitude) is a simulation. The radar
 * and RWR show real data from the machine.
 */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "apps.h"
#include "client.h"
#include "theme.h"
#include "wallpaper.h"

#define DESIGN_HEIGHT 768.0
#define DOCK_HEIGHT 48   /* screen pixels: the dock doesn't scale */
#define DEGREES (M_PI / 180.0)

/* Instrument panel sizes, in design units. */
#define PANEL_HEIGHT 236     /* instrument panel, above the dock */
#define SCREEN_SIZE 176      /* the two square screens */
#define SCOPE_RADIUS 78      /* radar and RWR circles inside them */
#define KEY_WIDTH 150
#define KEY_HEIGHT 46
#define KEY_GAP 16
#define KEYS_PER_ROW 3
/* Key 0 opens the launcher; keys 1.. are apps[0]..., as on the dock. */
#define KEY_COUNT (APP_COUNT + 1)

/* ---- colours for the physical cockpit and the aircraft ---- */

struct colour { double r, g, b; };

static const struct colour metal_light = { 0.50, 0.53, 0.57 };
static const struct colour metal_mid = { 0.27, 0.29, 0.32 };
static const struct colour metal_dark = { 0.13, 0.14, 0.16 };
static const struct colour metal_black = { 0.055, 0.060, 0.070 };
static const struct colour glass_black = { 0.000, 0.030, 0.014 }; /* a screen, off */

static void set_colour(cairo_t *cr, struct colour c) {
	cairo_set_source_rgb(cr, c.r, c.g, c.b);
}

/* Use a top-to-bottom blend of two colours as the paint. Shading like
 * this is most of what makes a flat shape look solid. */
static void set_shading(cairo_t *cr, double top_y, double bottom_y,
		struct colour top, struct colour bottom) {
	cairo_pattern_t *pattern = cairo_pattern_create_linear(0, top_y, 0, bottom_y);
	cairo_pattern_add_color_stop_rgb(pattern, 0, top.r, top.g, top.b);
	cairo_pattern_add_color_stop_rgb(pattern, 1, bottom.r, bottom.g, bottom.b);
	cairo_set_source(cr, pattern);
	cairo_pattern_destroy(pattern); /* cairo keeps its own reference */
}

/* ---- small helpers ---- */

static void line(cairo_t *cr, double x1, double y1, double x2, double y2) {
	cairo_move_to(cr, x1, y1);
	cairo_line_to(cr, x2, y2);
	cairo_stroke(cr);
}

/* Add a circle to the current path. new_sub_path starts a separate shape
 * without discarding what is already in the path (and stops cairo joining
 * the circle to the previous shape with a line). */
static void circle(cairo_t *cr, double x, double y, double radius) {
	cairo_new_sub_path(cr);
	cairo_arc(cr, x, y, radius, 0, 2 * M_PI);
}

/* Add an ellipse to the current path: a circle drawn while the coordinate
 * system is temporarily stretched. */
static void ellipse(cairo_t *cr, double x, double y, double rx, double ry) {
	cairo_save(cr);
	cairo_translate(cr, x, y);
	cairo_scale(cr, rx, ry);
	circle(cr, 0, 0, 1);
	cairo_restore(cr);
}

static void text_centred(cairo_t *cr, const char *text, double x, double y) {
	draw_text(cr, text, x - text_width(cr, text) / 2, y);
}

/* A bearing is an angle measured clockwise from "up" (north), as on a
 * compass. Convert a bearing and distance from a centre into x,y. */
static void polar(double cx, double cy, double bearing_degrees, double range,
		double *x, double *y) {
	*x = cx + range * sin(bearing_degrees * DEGREES);
	*y = cy - range * cos(bearing_degrees * DEGREES);
}

/* ---- where things are, in design units ---- */

struct layout {
	double width;              /* canvas width: depends on the screen's shape */
	double cx;                 /* horizontal centre */
	double panel_top;          /* top edge of the instrument panel (sides) */
	double panel_bottom;
	double radar_x, rwr_x;     /* centres of the two scopes */
	double scope_y;
	double keys_x, keys_y;     /* top-left of the first key */
	double data_x, data_y, data_w, data_h; /* the data display */
	/* The HUD glass, and the point on it you are flying towards. */
	double hud_x, hud_y, hud_w, hud_h;
	double aim_x, aim_y;
};

static struct layout layout_for(const struct scene *scene) {
	struct layout l;
	l.width = scene->width / scene->scale;
	l.cx = l.width / 2.0;
	l.panel_bottom = DESIGN_HEIGHT - DOCK_HEIGHT / scene->scale;
	l.panel_top = l.panel_bottom - PANEL_HEIGHT;
	l.radar_x = 48 + SCREEN_SIZE / 2.0;
	l.rwr_x = l.width - 48 - SCREEN_SIZE / 2.0;
	l.scope_y = l.panel_bottom - 20 - SCREEN_SIZE / 2.0;
	double keys_width = KEYS_PER_ROW * KEY_WIDTH + (KEYS_PER_ROW - 1) * KEY_GAP;
	l.keys_x = l.cx - keys_width / 2;
	l.keys_y = l.panel_top + 46;
	int key_rows = (KEY_COUNT + KEYS_PER_ROW - 1) / KEYS_PER_ROW;
	l.data_x = l.keys_x;
	l.data_y = l.keys_y + key_rows * (KEY_HEIGHT + KEY_GAP) + 4;
	l.data_w = keys_width;
	l.data_h = l.panel_bottom - 16 - l.data_y;
	l.hud_w = 280;
	l.hud_h = 320;
	l.hud_x = l.cx - l.hud_w / 2;
	l.hud_y = l.panel_top - l.hud_h + 10;
	l.aim_x = l.cx;
	l.aim_y = l.hud_y + l.hud_h * 0.48;
	return l;
}

/* Top-left corner of cockpit key `index`. */
static void key_position(const struct layout *l, int index,
		double *x, double *y) {
	*x = l->keys_x + (index % KEYS_PER_ROW) * (KEY_WIDTH + KEY_GAP);
	*y = l->keys_y + (index / KEYS_PER_ROW) * (KEY_HEIGHT + KEY_GAP);
}

static const char *key_label(int index) {
	return index == 0 ? "APPS" : apps[index - 1].name;
}

static const char *key_command(int index) {
	return index == 0 ? LAUNCHER_COMMAND : apps[index - 1].command;
}

/* ---- the flight: a slow, gentle weave ---- */

struct flight {
	double heading;  /* degrees */
	double pitch;    /* design units the horizon is below the aim point */
	int speed;       /* knots */
	int altitude;    /* feet */
};

static struct flight flight_at(double t) {
	struct flight f;
	/* Each quantity drifts on its own period so nothing repeats in step. */
	f.heading = 360 + 14 * sin(t * 0.07);
	f.pitch = 16 + 8 * sin(t * 0.23);
	f.speed = 450 + (int)lround(4 * sin(t * 0.31));
	f.altitude = 24500 + 10 * (int)lround(6 * sin(t * 0.19));
	return f;
}

/* ---- the formation, seen from behind ----
 *
 * Each aircraft is drawn around 0,0 with `s` = half its wingspan. You are
 * a little above them, so you see a sliver of the top of each wing.
 * From behind, what tells them apart is the engines and the tail:
 *   Typhoon    two engines side by side, low delta wing, small canards
 *   Tornado    two engines side by side, wings on top of the fuselage, a
 *              very tall fin, tailplanes drooping
 *   Lightning  two engines stacked one above the other, bare metal finish
 */

/* The paint scheme of the aircraft currently being drawn. */
static struct colour paint_top, paint_side, paint_under, paint_line;

static void use_grey_paint(void) {
	paint_top = (struct colour){ 0.69, 0.72, 0.76 };
	paint_side = (struct colour){ 0.49, 0.52, 0.57 };
	paint_under = (struct colour){ 0.28, 0.30, 0.34 };
	paint_line = (struct colour){ 0.13, 0.14, 0.16 };
}

static void use_metal_paint(void) {
	paint_top = (struct colour){ 0.86, 0.88, 0.90 };
	paint_side = (struct colour){ 0.62, 0.65, 0.69 };
	paint_under = (struct colour){ 0.36, 0.38, 0.42 };
	paint_line = (struct colour){ 0.17, 0.18, 0.20 };
}

/* Fill the current path shaded from `top` at top_y to `bottom` at
 * bottom_y, then outline it thinly. */
static void solid(cairo_t *cr, double top_y, double bottom_y,
		struct colour top, struct colour bottom) {
	set_shading(cr, top_y, bottom_y, top, bottom);
	cairo_fill_preserve(cr);
	set_colour(cr, paint_line);
	cairo_stroke(cr);
}

/* A wing or tailplane on both sides: a tapered slab from root to tip.
 * `thick` is half its thickness at the root; the tip is a third of that. */
static void wings(cairo_t *cr, double root_x, double root_y,
		double tip_x, double tip_y, double thick) {
	double tip_thick = thick / 3;
	for (int side = -1; side <= 1; side += 2) {
		cairo_move_to(cr, side * root_x, root_y - thick);
		cairo_line_to(cr, side * tip_x, tip_y - tip_thick);
		cairo_line_to(cr, side * tip_x, tip_y + tip_thick);
		cairo_line_to(cr, side * root_x, root_y + thick);
		cairo_close_path(cr);
		solid(cr, root_y - thick, root_y + thick, paint_top, paint_under);
	}
}

/* A fin seen from behind: a narrow upright blade, lit from the left. */
static void fin(cairo_t *cr, double base_y, double top_y, double half_width) {
	cairo_move_to(cr, -half_width, base_y);
	cairo_line_to(cr, -half_width * 0.35, top_y);
	cairo_line_to(cr, half_width * 0.35, top_y);
	cairo_line_to(cr, half_width, base_y);
	cairo_close_path(cr);
	solid(cr, top_y, base_y, paint_top, paint_side);
}

/* The back of the canopy, showing over the top of the fuselage. */
static void canopy(cairo_t *cr, double y, double rx, double ry) {
	ellipse(cr, 0, y, rx, ry);
	solid(cr, y - ry, y + ry, (struct colour){ 0.55, 0.68, 0.80 },
		(struct colour){ 0.12, 0.17, 0.24 });
}

/* A jet pipe: a dark metal ring with a black hole. The glow of the
 * exhaust is added every frame, on top (see draw_formation). */
static void nozzle(cairo_t *cr, double x, double y, double radius) {
	circle(cr, x, y, radius);
	solid(cr, y - radius, y + radius, (struct colour){ 0.42, 0.42, 0.44 },
		(struct colour){ 0.15, 0.15, 0.17 });
	circle(cr, x, y, radius * 0.72);
	cairo_set_source_rgb(cr, 0.03, 0.03, 0.04);
	cairo_fill(cr);
}

/* A missile or fuel tank hanging under the wing, on both sides. */
static void stores(cairo_t *cr, double x, double y, double radius) {
	for (int side = -1; side <= 1; side += 2) {
		circle(cr, side * x, y, radius);
		solid(cr, y - radius, y + radius, paint_side, paint_under);
	}
}

struct wingman {
	const char *name;
	void (*draw)(cairo_t *cr, double s);
	double offset_x, offset_y; /* from the aim point */
	double size;               /* half wingspan: nearer aircraft are bigger */
	/* Engine nozzles: x, y, radius, as fractions of size. */
	double nozzles[2][3];
};

static void draw_typhoon(cairo_t *cr, double s) {
	use_grey_paint();
	wings(cr, 0.14 * s, 0.045 * s, 1.00 * s, 0.105 * s, 0.050 * s);
	stores(cr, 0.46 * s, 0.115 * s, 0.022 * s);
	stores(cr, 0.72 * s, 0.120 * s, 0.018 * s);
	stores(cr, 1.00 * s, 0.105 * s, 0.016 * s);                      /* wingtip pods */
	wings(cr, 0.15 * s, -0.115 * s, 0.36 * s, -0.150 * s, 0.022 * s); /* canards */
	fin(cr, -0.14 * s, -0.60 * s, 0.034 * s);
	/* Fuselage, with the box of the chin intake underneath. */
	cairo_rectangle(cr, -0.14 * s, 0.10 * s, 0.28 * s, 0.085 * s);
	solid(cr, 0.10 * s, 0.185 * s, paint_under, paint_line);
	rounded_rect(cr, -0.175 * s, -0.16 * s, 0.35 * s, 0.29 * s, 0.08 * s);
	solid(cr, -0.16 * s, 0.13 * s, paint_top, paint_under);
	canopy(cr, -0.165 * s, 0.062 * s, 0.045 * s);
}

static void draw_tornado(cairo_t *cr, double s) {
	use_grey_paint();
	wings(cr, 0.17 * s, 0.085 * s, 0.52 * s, 0.150 * s, 0.030 * s);   /* tailplanes */
	wings(cr, 0.17 * s, -0.130 * s, 1.00 * s, -0.070 * s, 0.050 * s); /* wings, up high */
	stores(cr, 0.40 * s, -0.045 * s, 0.040 * s);                      /* fuel tanks */
	stores(cr, 0.68 * s, -0.050 * s, 0.020 * s);
	fin(cr, -0.17 * s, -0.90 * s, 0.048 * s);
	rounded_rect(cr, -0.195 * s, -0.19 * s, 0.39 * s, 0.38 * s, 0.06 * s);
	solid(cr, -0.19 * s, 0.19 * s, paint_top, paint_under);
	canopy(cr, -0.195 * s, 0.070 * s, 0.045 * s);
}

static void draw_lightning(cairo_t *cr, double s) {
	use_metal_paint();
	wings(cr, 0.13 * s, 0.270 * s, 0.47 * s, 0.305 * s, 0.028 * s);  /* tailplanes, low */
	wings(cr, 0.14 * s, 0.010 * s, 1.00 * s, 0.090 * s, 0.050 * s);
	fin(cr, -0.30 * s, -0.86 * s, 0.045 * s);
	/* A tall, narrow fuselage: the two engines are one above the other. */
	ellipse(cr, 0, 0, 0.165 * s, 0.335 * s);
	solid(cr, -0.335 * s, 0.335 * s, paint_top, paint_under);
	canopy(cr, -0.335 * s, 0.055 * s, 0.040 * s);
}

/* The leader is dead ahead, so you see it through the HUD glass. */
static const struct wingman formation[AIRCRAFT_COUNT] = {
	{ "TORNADO", draw_tornado, -262, 6, 78,
		{ { -0.092, 0.025, 0.080 }, { 0.092, 0.025, 0.080 } } },
	{ "LIGHTNING", draw_lightning, 262, 2, 66,
		{ { 0, -0.135, 0.105 }, { 0, 0.135, 0.105 } } },
	{ "TYPHOON", draw_typhoon, 0, -62, 74,
		{ { -0.082, -0.015, 0.072 }, { 0.082, -0.015, 0.072 } } },
};

/* An aircraft's picture is this many half-wingspans wide and tall, with
 * the aircraft's 0,0 at this point inside it. */
#define SPRITE_WIDTH 2.3
#define SPRITE_HEIGHT 1.8
#define SPRITE_ORIGIN_X 1.15
#define SPRITE_ORIGIN_Y 1.20

/* Draw one aircraft into its own image, once, at full screen resolution.
 * Each frame then only has to place the finished picture. */
static cairo_surface_t *render_aircraft(const struct scene *scene,
		const struct wingman *wingman) {
	double s = wingman->size;
	cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
		(int)ceil(SPRITE_WIDTH * s * scene->scale),
		(int)ceil(SPRITE_HEIGHT * s * scene->scale));
	cairo_t *cr = cairo_create(surface);
	cairo_scale(cr, scene->scale, scene->scale);
	cairo_translate(cr, SPRITE_ORIGIN_X * s, SPRITE_ORIGIN_Y * s);
	cairo_set_line_width(cr, 0.8);
	cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
	wingman->draw(cr, s);
	for (int i = 0; i < 2; i++) {
		nozzle(cr, wingman->nozzles[i][0] * s, wingman->nozzles[i][1] * s,
			wingman->nozzles[i][2] * s);
	}
	cairo_destroy(cr);
	return surface;
}

static void draw_formation(struct scene *scene, cairo_t *cr,
		const struct layout *l, double t) {
	for (int i = 0; i < AIRCRAFT_COUNT; i++) {
		const struct wingman *wingman = &formation[i];
		double s = wingman->size;
		/* Each aircraft drifts slowly around its slot, out of step with
		 * the others: formation flying is never perfectly still. */
		double phase = i * 2.1;
		double x = l->aim_x + wingman->offset_x + 9 * sin(t * 0.50 + phase);
		double y = l->aim_y + wingman->offset_y + 6 * sin(t * 0.78 + phase * 1.7);
		/* It banks a little towards the way it is drifting. */
		double bank = 2.5 * DEGREES * cos(t * 0.50 + phase);

		/* Move the origin to the aircraft and tilt it; from here on
		 * 0,0 is the aircraft's centre. */
		cairo_save(cr);
		cairo_translate(cr, x, y);
		cairo_rotate(cr, bank);

		/* Place the cached picture. It was drawn in screen pixels, so
		 * undo the design scale while painting it. */
		cairo_save(cr);
		cairo_scale(cr, 1 / scene->scale, 1 / scene->scale);
		cairo_set_source_surface(cr, scene->aircraft[i],
			-SPRITE_ORIGIN_X * s * scene->scale,
			-SPRITE_ORIGIN_Y * s * scene->scale);
		cairo_paint(cr);
		cairo_restore(cr);

		/* Exhaust glow: hot in the middle, fading out past the rim of
		 * the nozzle, and flickering. This is the only part of the
		 * aircraft that is drawn fresh each frame. */
		double glow = 0.72 + 0.20 * sin(t * 9 + phase * 3);
		for (int n = 0; n < 2; n++) {
			double nx = wingman->nozzles[n][0] * s;
			double ny = wingman->nozzles[n][1] * s;
			double radius = wingman->nozzles[n][2] * s * 1.5;
			cairo_pattern_t *flame = cairo_pattern_create_radial(
				nx, ny, 0, nx, ny, radius);
			cairo_pattern_add_color_stop_rgba(flame, 0.0, 1.0, 0.93, 0.70, glow);
			cairo_pattern_add_color_stop_rgba(flame, 0.45, 1.0, 0.55, 0.15, glow * 0.75);
			cairo_pattern_add_color_stop_rgba(flame, 1.0, 1.0, 0.30, 0.05, 0.0);
			cairo_set_source(cr, flame);
			circle(cr, nx, ny, radius);
			cairo_fill(cr);
			cairo_pattern_destroy(flame);
		}
		cairo_restore(cr);
	}
}

/* ---- HUD symbology, drawn on the glass ---- */

static void draw_hud(cairo_t *cr, const struct layout *l,
		const struct flight *f) {
	/* Symbols only appear on the glass, so clip to it. */
	cairo_save(cr);
	cairo_rectangle(cr, l->hud_x + 4, l->hud_y + 4, l->hud_w - 8,
		l->panel_top - l->hud_y - 8);
	cairo_clip(cr);

	/* The glass is slightly smoked, which is what lets bright green
	 * symbols stand out against a bright sky. */
	cairo_set_source_rgba(cr, 0.00, 0.10, 0.05, 0.42);
	cairo_paint(cr);

	cairo_set_source_rgb(cr, HUD_GREEN);
	cairo_set_line_width(cr, HUD_LINE);
	cairo_set_font_size(cr, 11);

	/* Pitch ladder: a rung every 5 degrees, measured from the horizon,
	 * so it moves up and down with the real horizon outside. Above the
	 * horizon rungs are solid, below they are dashed. */
	double horizon = l->aim_y + f->pitch;
	const double rung_spacing = 46;
	for (int step = -2; step <= 2; step++) {
		double y = horizon - step * rung_spacing;
		if (step == 0) {
			line(cr, l->cx - 118, y, l->cx - 30, y); /* the horizon bar */
			line(cr, l->cx + 30, y, l->cx + 118, y);
			continue;
		}
		double tick = step > 0 ? 6 : -6;
		if (step < 0) {
			const double dashes[] = {7, 5};
			cairo_set_dash(cr, dashes, 2, 0);
		}
		line(cr, l->cx - 82, y, l->cx - 30, y);
		line(cr, l->cx + 30, y, l->cx + 82, y);
		cairo_set_dash(cr, NULL, 0, 0);
		line(cr, l->cx - 82, y, l->cx - 82, y + tick);
		line(cr, l->cx + 82, y, l->cx + 82, y + tick);
		char label[8];
		snprintf(label, sizeof(label), "%d", step * 5);
		draw_text(cr, label, l->cx - 88 - text_width(cr, label), y);
		draw_text(cr, label, l->cx + 88, y);
	}

	/* Flight path marker: fixed on the glass, where the aircraft is
	 * going. A small circle with wings and a fin. */
	circle(cr, l->aim_x, l->aim_y, 7);
	cairo_stroke(cr);
	line(cr, l->aim_x - 22, l->aim_y, l->aim_x - 7, l->aim_y);
	line(cr, l->aim_x + 7, l->aim_y, l->aim_x + 22, l->aim_y);
	line(cr, l->aim_x, l->aim_y - 7, l->aim_x, l->aim_y - 15);

	/* Heading tape along the top of the glass: a tick every 2 degrees,
	 * sliding under a fixed caret. */
	const double units_per_degree = 5, half_width = 112;
	double tape_y = l->hud_y + 30;
	int first = (int)floor(f->heading - half_width / units_per_degree);
	int last = (int)ceil(f->heading + half_width / units_per_degree);
	for (int degree = first; degree <= last; degree++) {
		double x = l->cx + (degree - f->heading) * units_per_degree;
		if (degree % 2 != 0 || fabs(x - l->cx) > half_width) {
			continue;
		}
		bool major = degree % 10 == 0;
		line(cr, x, tape_y, x, tape_y + (major ? 9 : 4));
		if (major) {
			/* Tens of degrees as two digits, the way a HUD shows them:
			 * 350 is "35", 360 is "00", 10 is "01". */
			char label[8];
			snprintf(label, sizeof(label), "%02d", (degree % 360) / 10);
			text_centred(cr, label, x, tape_y - 9);
		}
	}
	cairo_move_to(cr, l->cx, tape_y + 12);
	cairo_line_to(cr, l->cx - 5, tape_y + 21);
	cairo_line_to(cr, l->cx + 5, tape_y + 21);
	cairo_close_path(cr);
	cairo_stroke(cr);

	/* Speed on the left of the glass, altitude on the right. */
	char value[16];
	cairo_set_font_size(cr, 13);
	snprintf(value, sizeof(value), "%d", f->speed);
	cairo_rectangle(cr, l->hud_x + 12, l->aim_y - 11, 48, 22);
	cairo_stroke(cr);
	text_centred(cr, value, l->hud_x + 36, l->aim_y);
	snprintf(value, sizeof(value), "%d", f->altitude);
	cairo_rectangle(cr, l->hud_x + l->hud_w - 66, l->aim_y - 11, 54, 22);
	cairo_stroke(cr);
	text_centred(cr, value, l->hud_x + l->hud_w - 39, l->aim_y);

	/* The aircraft you are following, as a HUD would tag it. */
	cairo_set_font_size(cr, 10);
	draw_text(cr, "LEAD  TYPHOON", l->hud_x + 14, l->hud_y + 62);

	cairo_restore(cr);
}

/* ---- radar (left screen) ---- */

/* Where a process sits on the radar. The bearing comes from its PID,
 * multiplied by the "golden angle" (137.5 degrees), which spreads
 * consecutive numbers evenly round a circle instead of bunching them. The
 * range comes from its position in the list: the oldest programs sit near
 * the centre and each newer one further out, so a program you have just
 * started appears at the edge of the scope like a contact coming into range. */
static double process_bearing(const struct process *process) {
	return fmod(process->pid * 137.508, 360.0);
}

static void draw_radar(struct scene *scene, cairo_t *cr,
		const struct layout *l, double t) {
	double cx = l->radar_x, cy = l->scope_y;
	struct process_list *list = &scene->processes;

	/* The sweep: one turn every four seconds. */
	double sweep = fmod(t * 90.0, 360.0);

	/* An afterglow behind the sweep line: thin wedges, each fainter than
	 * the last. Cairo measures angles clockwise from "right", bearings are
	 * clockwise from "up", hence the -90. */
	for (int i = 0; i < 14; i++) {
		double from = (sweep - (i + 1) * 4 - 90) * DEGREES;
		double to = (sweep - i * 4 - 90) * DEGREES;
		cairo_move_to(cr, cx, cy);
		cairo_arc(cr, cx, cy, SCOPE_RADIUS - 1, from, to);
		cairo_close_path(cr);
		cairo_set_source_rgba(cr, HUD_GREEN, 0.20 * (1.0 - i / 14.0));
		cairo_fill(cr);
	}
	double sx, sy;
	polar(cx, cy, sweep, SCOPE_RADIUS - 1, &sx, &sy);
	cairo_set_source_rgb(cr, HUD_GREEN);
	cairo_set_line_width(cr, HUD_LINE);
	line(cr, cx, cy, sx, sy);

	/* The contacts. */
	scene->hover_pid = 0;
	double nearest = 11; /* how close the pointer has to be */
	for (int i = 0; i < list->count; i++) {
		struct process *process = &list->items[i];
		double bearing = process_bearing(process);
		double range = SCOPE_RADIUS * (0.16 + 0.74 *
			(list->count > 1 ? (double)i / (list->count - 1) : 0));
		polar(cx, cy, bearing, range, &process->radar_x, &process->radar_y);

		/* A real radar only sees a target at the moment the beam crosses
		 * it; the blip then fades until the next pass. `behind` is how far
		 * the beam has moved on since it last crossed this contact. */
		double behind = fmod(sweep - bearing + 360.0, 360.0);
		double brightness = 1.0 - 0.75 * behind / 360.0;
		/* Bigger programs give a bigger return (log scale). */
		double size = 1.8 + 0.45 * log2(1.0 + process->memory_kb / 1024.0);
		circle(cr, process->radar_x, process->radar_y, size);
		cairo_set_source_rgba(cr, HUD_GREEN, brightness);
		cairo_fill(cr);

		/* Is the mouse pointer on this contact? Keep the closest one. */
		if (scene->pointer_inside) {
			double distance = hypot(scene->pointer_x - process->radar_x,
				scene->pointer_y - process->radar_y);
			if (distance < nearest) {
				nearest = distance;
				scene->hover_pid = process->pid;
			}
		}
	}
}

/* ---- RWR (right screen) ---- */

/* A radar warning receiver shows who is "painting" you with their radar,
 * as symbols round a circle: direction is where the threat is, and the
 * closer to the centre, the more dangerous. Here a threat is a program
 * using the CPU: the busier it is, the closer to the centre. It uses the
 * same bearing as on the radar, so the two scopes line up. */
static int draw_rwr(struct scene *scene, cairo_t *cr, const struct layout *l) {
	double cx = l->rwr_x, cy = l->scope_y;
	const struct process *worst = NULL;
	int threats = 0;

	cairo_set_font_size(cr, 11);
	for (int i = 0; i < scene->processes.count; i++) {
		const struct process *process = &scene->processes.items[i];
		if (process->cpu_percent < 0.5) {
			continue; /* idle: not a threat */
		}
		threats++;
		if (worst == NULL || process->cpu_percent > worst->cpu_percent) {
			worst = process;
		}
		/* Square root so that small loads are still spread out: 2% and
		 * 8% are far apart, 60% and 90% are both "very close". */
		double danger = fmin(1.0, sqrt(process->cpu_percent / 60.0));
		double range = SCOPE_RADIUS * (0.84 - 0.60 * danger);
		double x, y;
		polar(cx, cy, process_bearing(process), range, &x, &y);

		/* The symbol: the first two letters of the program's name. Our
		 * own programs all start "perkinsos-", so skip that part or they
		 * would all be "PE": the dock is DO, the compositor CO. */
		const char *name = process->name;
		if (strncmp(name, "perkinsos-", 10) == 0) {
			name += 10;
		}
		char symbol[3] = {
			toupper((unsigned char)name[0]),
			toupper((unsigned char)name[1]), '\0',
		};
		cairo_set_source_rgb(cr, HUD_GREEN);
		text_centred(cr, symbol, x, y);
	}

	/* A diamond round the highest threat, as a real RWR does. */
	if (worst != NULL) {
		double danger = fmin(1.0, sqrt(worst->cpu_percent / 60.0));
		double x, y;
		polar(cx, cy, process_bearing(worst),
			SCOPE_RADIUS * (0.84 - 0.60 * danger), &x, &y);
		cairo_move_to(cr, x, y - 13);
		cairo_line_to(cr, x + 15, y);
		cairo_line_to(cr, x, y + 13);
		cairo_line_to(cr, x - 15, y);
		cairo_close_path(cr);
		cairo_set_line_width(cr, HUD_LINE);
		cairo_stroke(cr);
	}
	return threats;
}

/* ---- the data display between the screens ---- */

/* Shows the designated radar contact: the locked one, or else the one
 * under the pointer. Also draws the target box on the radar. */
static void draw_data_display(struct scene *scene, cairo_t *cr,
		const struct layout *l, int threats) {
	int pid = scene->locked_pid != 0 ? scene->locked_pid : scene->hover_pid;
	const struct process *target = NULL;
	for (int i = 0; i < scene->processes.count; i++) {
		if (scene->processes.items[i].pid == pid) {
			target = &scene->processes.items[i];
		}
	}
	if (target == NULL) {
		scene->locked_pid = 0; /* the locked program has exited */
	}

	double x = l->data_x + 14, y = l->data_y + 18;
	double right = l->data_x + l->data_w / 2 + 20;
	char row[48];
	cairo_set_font_size(cr, 12);

	/* Left half: the target. */
	if (target != NULL) {
		cairo_set_source_rgb(cr, HUD_GREEN);
		cairo_set_line_width(cr, HUD_LINE);
		cairo_rectangle(cr, target->radar_x - 8, target->radar_y - 8, 16, 16);
		cairo_stroke(cr);

		draw_text(cr, scene->locked_pid != 0 ? "TGT LOCKED" : "TGT", x, y);
		char name[sizeof(target->name)];
		for (size_t i = 0; i < sizeof(name); i++) {
			name[i] = toupper((unsigned char)target->name[i]);
		}
		draw_text(cr, name, x, y + 18);
		cairo_set_source_rgb(cr, HUD_DIM);
		snprintf(row, sizeof(row), "PID %d", target->pid);
		draw_text(cr, row, x, y + 36);
		snprintf(row, sizeof(row), "MEM %.1f MB  CPU %.1f%%",
			target->memory_kb / 1024.0, target->cpu_percent);
		draw_text(cr, row, x, y + 52);
	} else {
		cairo_set_source_rgb(cr, HUD_DIM);
		draw_text(cr, "NO TARGET", x, y);
		draw_text(cr, "CLICK A RADAR", x, y + 18);
		draw_text(cr, "CONTACT TO LOCK", x, y + 34);
	}

	/* Right half: totals from both scopes. */
	cairo_set_source_rgb(cr, HUD_DIM);
	snprintf(row, sizeof(row), "CONTACTS %d", scene->processes.count);
	draw_text(cr, row, right, y);
	if (threats == 0) {
		snprintf(row, sizeof(row), "RWR CLEAR");
	} else {
		snprintf(row, sizeof(row), "RWR %d ACTIVE", threats);
	}
	draw_text(cr, row, right, y + 18);
}

/* ---- cockpit keys ---- */

/* One key. Drawn into the cached cockpit in its resting state, and again
 * on top, lit, for the key under the pointer. */
static void draw_key(cairo_t *cr, const struct layout *l, int index,
		bool lit) {
	double x, y;
	key_position(l, index, &x, &y);
	/* A dark surround, as if the key sits in a hole in the panel. */
	rounded_rect(cr, x - 2, y - 2, KEY_WIDTH + 4, KEY_HEIGHT + 4, 7);
	set_colour(cr, metal_black);
	cairo_fill(cr);

	rounded_rect(cr, x + 0.5, y + 0.5, KEY_WIDTH - 1, KEY_HEIGHT - 1, 5);
	if (lit) {
		/* Pressed-in and backlit. */
		cairo_set_source_rgb(cr, HUD_GREEN);
		cairo_fill(cr);
		cairo_set_source_rgb(cr, HUD_BLACK);
	} else {
		/* A raised plastic key cap: lighter at the top where the light
		 * catches it, with a bright line along the top edge. */
		set_shading(cr, y, y + KEY_HEIGHT, metal_mid, metal_dark);
		cairo_fill(cr);
		set_colour(cr, metal_light);
		cairo_set_line_width(cr, 1);
		line(cr, x + 6, y + 1.5, x + KEY_WIDTH - 6, y + 1.5);
		cairo_set_source_rgb(cr, HUD_GREEN); /* the legend is backlit */
	}
	cairo_set_font_size(cr, 13);
	text_centred(cr, key_label(index), x + KEY_WIDTH / 2.0,
		y + KEY_HEIGHT / 2.0);
}

/* Which key is at x,y? -1 for none. */
static int key_at(const struct layout *l, double x, double y) {
	for (int i = 0; i < KEY_COUNT; i++) {
		double kx, ky;
		key_position(l, i, &kx, &ky);
		if (x >= kx && x < kx + KEY_WIDTH && y >= ky && y < ky + KEY_HEIGHT) {
			return i;
		}
	}
	return -1;
}

/* ---- the cockpit: everything that never changes ---- */

/* One square screen with a round scope in it. */
static void draw_screen(cairo_t *cr, const char *title, double cx, double cy,
		bool rwr) {
	double half = SCREEN_SIZE / 2.0;

	/* Bezel: a metal frame with a row of small buttons above and below,
	 * like the soft keys round a real multi-function display. */
	rounded_rect(cr, cx - half - 11, cy - half - 11, SCREEN_SIZE + 22,
		SCREEN_SIZE + 22, 9);
	set_shading(cr, cy - half - 11, cy + half + 11, metal_mid, metal_dark);
	cairo_fill_preserve(cr);
	set_colour(cr, metal_black);
	cairo_set_line_width(cr, 1.5);
	cairo_stroke(cr);
	set_colour(cr, metal_light);
	cairo_set_line_width(cr, 1);
	line(cr, cx - half - 4, cy - half - 10, cx + half + 4, cy - half - 10);
	for (int i = 0; i < 5; i++) {
		double x = cx - half + 14 + i * 33;
		cairo_rectangle(cr, x, cy - half - 8, 16, 5);
		cairo_rectangle(cr, x, cy + half + 3, 16, 5);
	}
	set_colour(cr, metal_light);
	cairo_fill(cr);

	/* The glass. */
	cairo_rectangle(cr, cx - half, cy - half, SCREEN_SIZE, SCREEN_SIZE);
	set_colour(cr, glass_black);
	cairo_fill(cr);

	circle(cr, cx, cy, SCOPE_RADIUS);
	cairo_set_source_rgb(cr, HUD_GREEN);
	cairo_set_line_width(cr, HUD_LINE);
	cairo_stroke(cr);

	cairo_set_source_rgb(cr, HUD_FAINT);
	cairo_set_line_width(cr, 1);
	if (rwr) {
		/* Inner ring: inside it is "lethal range". Ticks at each hour. */
		circle(cr, cx, cy, SCOPE_RADIUS * 0.45);
		cairo_stroke(cr);
		cairo_set_source_rgb(cr, HUD_DIM);
		for (int hour = 0; hour < 12; hour++) {
			double x1, y1, x2, y2;
			polar(cx, cy, hour * 30, SCOPE_RADIUS - 7, &x1, &y1);
			polar(cx, cy, hour * 30, SCOPE_RADIUS, &x2, &y2);
			line(cr, x1, y1, x2, y2);
		}
		/* Own aircraft in the middle. */
		line(cr, cx - 7, cy, cx + 7, cy);
		line(cr, cx, cy - 9, cx, cy + 6);
	} else {
		/* Range rings and a cross. */
		circle(cr, cx, cy, SCOPE_RADIUS / 3.0);
		cairo_stroke(cr);
		circle(cr, cx, cy, SCOPE_RADIUS * 2 / 3.0);
		cairo_stroke(cr);
		line(cr, cx - SCOPE_RADIUS, cy, cx + SCOPE_RADIUS, cy);
		line(cr, cx, cy - SCOPE_RADIUS, cx, cy + SCOPE_RADIUS);
	}

	/* The screen's name in its top-left corner. */
	cairo_set_font_size(cr, 11);
	cairo_set_source_rgb(cr, HUD_GREEN);
	draw_text(cr, title, cx - half + 5, cy - half + 10);
}

static void draw_cockpit(struct scene *scene, cairo_t *cr) {
	struct layout l = layout_for(scene);
	double w = l.width, h = DESIGN_HEIGHT;

	cairo_select_font_face(cr, UI_FONT, CAIRO_FONT_SLANT_NORMAL,
		CAIRO_FONT_WEIGHT_NORMAL);

	/* Canopy arch: the hoop of the windscreen frame, rising from either
	 * side of the panel and meeting overhead. It is a wide stroke along
	 * an ellipse, drawn three times: a dark edge, a lighter face, and a
	 * thin highlight where the light catches its inner rim. */
	double arch_x = l.cx, arch_y = h * 1.02;
	double arch_rx = w * 0.47, arch_ry = h * 1.0;
	ellipse(cr, arch_x, arch_y, arch_rx, arch_ry);
	set_colour(cr, metal_black);
	cairo_set_line_width(cr, 46);
	cairo_stroke(cr);
	ellipse(cr, arch_x, arch_y, arch_rx, arch_ry);
	set_colour(cr, metal_dark);
	cairo_set_line_width(cr, 38);
	cairo_stroke(cr);
	ellipse(cr, arch_x, arch_y, arch_rx + 4, arch_ry + 4);
	set_colour(cr, metal_mid);
	cairo_set_line_width(cr, 18);
	cairo_stroke(cr);
	ellipse(cr, arch_x, arch_y, arch_rx - 17, arch_ry - 17);
	set_colour(cr, metal_light);
	cairo_set_line_width(cr, 1.5);
	cairo_stroke(cr);

	/* Cockpit sills: the sides of the cockpit rising at the left and
	 * right edges. */
	for (int side = -1; side <= 1; side += 2) {
		double edge = side < 0 ? 0 : w;
		cairo_move_to(cr, edge, l.panel_top - 130);
		cairo_line_to(cr, edge - side * 64, l.panel_top - 74);
		cairo_line_to(cr, edge - side * 128, l.panel_top + 18);
		cairo_line_to(cr, edge, l.panel_top + 18);
		cairo_close_path(cr);
		set_shading(cr, l.panel_top - 130, l.panel_top + 18,
			metal_mid, metal_black);
		cairo_fill(cr);
		set_colour(cr, metal_light);
		cairo_set_line_width(cr, 1.5);
		cairo_move_to(cr, edge, l.panel_top - 130);
		cairo_line_to(cr, edge - side * 64, l.panel_top - 74);
		cairo_line_to(cr, edge - side * 128, l.panel_top + 18);
		cairo_stroke(cr);
	}

	/* HUD: two posts and the frame of the glass they hold. The glass
	 * itself is not drawn here: you look through it. */
	set_colour(cr, metal_black);
	cairo_set_line_width(cr, 5);
	line(cr, l.hud_x, l.hud_y + 26, l.hud_x - 10, l.panel_top);
	line(cr, l.hud_x + l.hud_w, l.hud_y + 26, l.hud_x + l.hud_w + 10,
		l.panel_top);
	rounded_rect(cr, l.hud_x, l.hud_y, l.hud_w, l.hud_h, 26);
	set_colour(cr, metal_black);
	cairo_set_line_width(cr, 5);
	cairo_stroke(cr);
	rounded_rect(cr, l.hud_x, l.hud_y, l.hud_w, l.hud_h, 26);
	set_colour(cr, metal_mid);
	cairo_set_line_width(cr, 1.5);
	cairo_stroke(cr);

	/* Instrument panel. Its top edge (the glareshield) rises in the
	 * middle, where the HUD sits. */
	cairo_move_to(cr, 0, l.panel_top + 16);
	cairo_curve_to(cr, w * 0.25, l.panel_top + 12, w * 0.30, l.panel_top - 8,
		l.cx, l.panel_top - 8);
	cairo_curve_to(cr, w * 0.70, l.panel_top - 8, w * 0.75, l.panel_top + 12,
		w, l.panel_top + 16);
	cairo_line_to(cr, w, h);
	cairo_line_to(cr, 0, h);
	cairo_close_path(cr);
	set_shading(cr, l.panel_top - 8, h,
		(struct colour){ 0.19, 0.20, 0.22 }, (struct colour){ 0.085, 0.09, 0.10 });
	cairo_fill(cr);
	/* The padded lip of the glareshield catches the light. */
	cairo_move_to(cr, 0, l.panel_top + 16);
	cairo_curve_to(cr, w * 0.25, l.panel_top + 12, w * 0.30, l.panel_top - 8,
		l.cx, l.panel_top - 8);
	cairo_curve_to(cr, w * 0.70, l.panel_top - 8, w * 0.75, l.panel_top + 12,
		w, l.panel_top + 16);
	set_colour(cr, metal_light);
	cairo_set_line_width(cr, 2);
	cairo_stroke(cr);

	/* The HUD projector housing, on top of the glareshield. */
	rounded_rect(cr, l.cx - 78, l.panel_top - 20, 156, 34, 6);
	set_shading(cr, l.panel_top - 20, l.panel_top + 14, metal_mid, metal_black);
	cairo_fill(cr);
	rounded_rect(cr, l.cx - 52, l.panel_top - 13, 104, 12, 4);
	cairo_set_source_rgb(cr, 0.02, 0.16, 0.08); /* the projector lens */
	cairo_fill(cr);

	draw_screen(cr, "RADAR", l.radar_x, l.scope_y, false);
	draw_screen(cr, "RWR", l.rwr_x, l.scope_y, true);

	/* Keys, with an engraved caption over the group. */
	cairo_set_font_size(cr, 11);
	set_colour(cr, metal_light);
	text_centred(cr, "SYSTEMS", l.cx, l.keys_y - 12);
	for (int i = 0; i < KEY_COUNT; i++) {
		draw_key(cr, &l, i, false);
	}

	/* The data display: a small screen with a metal surround. */
	rounded_rect(cr, l.data_x - 5, l.data_y - 5, l.data_w + 10,
		l.data_h + 10, 7);
	set_shading(cr, l.data_y - 5, l.data_y + l.data_h + 5, metal_mid, metal_dark);
	cairo_fill(cr);
	rounded_rect(cr, l.data_x + 0.5, l.data_y + 0.5, l.data_w - 1,
		l.data_h - 1, 3);
	set_colour(cr, glass_black);
	cairo_fill(cr);

	/* A screw in each corner of the centre panel. */
	set_colour(cr, metal_light);
	for (int i = 0; i < 4; i++) {
		double x = (i % 2 == 0) ? l.keys_x - 22 : l.keys_x + l.data_w + 22;
		double y = (i < 2) ? l.keys_y - 16 : l.data_y + l.data_h + 2;
		circle(cr, x, y, 3);
		cairo_fill(cr);
	}
}

/* ---- public functions (see wallpaper.h) ---- */

void scene_init(struct scene *scene, int width, int height) {
	scene_finish(scene);
	scene->width = width;
	scene->height = height;
	scene->scale = height / DESIGN_HEIGHT;
	scene->hover_key = -1;
	sky_init();

	/* ARGB32: has an alpha channel, so the window part stays see-through. */
	scene->cockpit = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
		width, height);
	cairo_t *cr = cairo_create(scene->cockpit);
	cairo_scale(cr, scene->scale, scene->scale);
	draw_cockpit(scene, cr);
	cairo_destroy(cr);
	cairo_surface_flush(scene->cockpit);
	scene->cockpit_overlay = overlay_create(
		(const uint32_t *)cairo_image_surface_get_data(scene->cockpit),
		cairo_image_surface_get_stride(scene->cockpit) / 4, width, height);

	for (int i = 0; i < AIRCRAFT_COUNT; i++) {
		scene->aircraft[i] = render_aircraft(scene, &formation[i]);
	}
}

void scene_finish(struct scene *scene) {
	overlay_destroy(scene->cockpit_overlay);
	scene->cockpit_overlay = NULL;
	if (scene->cockpit != NULL) {
		cairo_surface_destroy(scene->cockpit);
		scene->cockpit = NULL;
	}
	for (int i = 0; i < AIRCRAFT_COUNT; i++) {
		if (scene->aircraft[i] != NULL) {
			cairo_surface_destroy(scene->aircraft[i]);
			scene->aircraft[i] = NULL;
		}
	}
}

void scene_draw(struct scene *scene, cairo_t *cr, double t) {
	struct layout l = layout_for(scene);
	struct flight f = flight_at(t);
	processes_update(&scene->processes, t);

	/* The frame's raw pixels, for the two steps that bypass cairo.
	 * flush = "cairo, finish anything you have pending before I touch the
	 * memory"; mark_dirty = "I changed the memory behind your back". */
	cairo_surface_t *frame = cairo_get_target(cr);
	uint32_t *pixels = (uint32_t *)cairo_image_surface_get_data(frame);
	int stride_pixels = cairo_image_surface_get_stride(frame) / 4;

	/* 1. Outside. Only the part above the instrument panel shows it. */
	struct sky_view view = {
		.width = scene->width,
		.rows = (int)((l.panel_top + 20) * scene->scale),
		.horizon = (l.aim_y + f.pitch) * scene->scale,
		.pan = (f.heading - 360) * 16 * scene->scale,
		.time = t,
		.scale = scene->scale,
	};
	cairo_surface_flush(frame);
	sky_render(pixels, stride_pixels, &view);
	cairo_surface_mark_dirty(frame);

	/* From here on, draw in design units. */
	cairo_save(cr);
	cairo_scale(cr, scene->scale, scene->scale);
	cairo_select_font_face(cr, UI_FONT, CAIRO_FONT_SLANT_NORMAL,
		CAIRO_FONT_WEIGHT_NORMAL);

	/* 2. Aircraft and HUD symbols. */
	draw_formation(scene, cr, &l, t);
	draw_hud(cr, &l, &f);

	/* 3. The cached cockpit over the top. */
	cairo_surface_flush(frame);
	overlay_blit(scene->cockpit_overlay, pixels, stride_pixels);
	cairo_surface_mark_dirty(frame);

	/* 4. The live parts of the panel. */
	draw_radar(scene, cr, &l, t);
	int threats = draw_rwr(scene, cr, &l);
	draw_data_display(scene, cr, &l, threats);

	scene->hover_key = scene->pointer_inside ?
		key_at(&l, scene->pointer_x, scene->pointer_y) : -1;
	if (scene->hover_key >= 0) {
		draw_key(cr, &l, scene->hover_key, true);
	}
	cairo_restore(cr);
}

void scene_pointer_motion(struct scene *scene, double x, double y) {
	/* The compositor reports screen pixels; everything here works in
	 * design units. */
	if (scene->scale > 0) {
		scene->pointer_x = x / scene->scale;
		scene->pointer_y = y / scene->scale;
		scene->pointer_inside = true;
	}
}

void scene_pointer_leave(struct scene *scene) {
	scene->pointer_inside = false;
}

void scene_pointer_click(struct scene *scene) {
	/* Work the key out from the pointer position rather than from the
	 * last frame, so a click right after a move isn't missed. */
	struct layout l = layout_for(scene);
	int key = key_at(&l, scene->pointer_x, scene->pointer_y);
	if (key >= 0) {
		spawn(key_command(key));
		return;
	}
	/* Click a radar contact to lock it; click it again, or anywhere else,
	 * to break the lock. hover_pid was worked out during the last draw. */
	if (scene->hover_pid != 0 && scene->hover_pid != scene->locked_pid) {
		scene->locked_pid = scene->hover_pid;
	} else {
		scene->locked_pid = 0;
	}
}
