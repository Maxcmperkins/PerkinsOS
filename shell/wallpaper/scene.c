/*
 * Drawing the wallpaper: one frame of the view from a fighter cockpit.
 *
 * What is in the picture, back to front:
 *   outside     sky, a horizon that rolls and pitches, and a cloud deck
 *               streaming towards you underneath
 *   formation   three jets ahead of you, seen from behind: a Typhoon in
 *               front, a Tornado to the left, a Lightning to the right
 *   HUD         the glass in front of you: pitch ladder, flight path
 *               marker, heading tape, speed and altitude
 *   cockpit     canopy frame and instrument panel (cached, never changes)
 *   screens     left screen: radar, each contact a running program
 *               right screen: RWR, each "threat" a program using the CPU
 *   keys        the buttons between the screens start apps
 *
 * Everything that moves is a function of the time `t`, so a frame can be
 * drawn for any moment without remembering the previous one.
 *
 * The flight (heading, roll, speed, altitude) is a simulation. The radar
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

#define DOCK_HEIGHT 48   /* keep clear of the dock along the bottom */
#define DEGREES (M_PI / 180.0)

/* Instrument panel sizes, in pixels. */
#define PANEL_HEIGHT 236     /* instrument panel, above the dock */
#define SCREEN_SIZE 176      /* the two square screens */
#define SCOPE_RADIUS 78      /* radar and RWR circles inside them */
#define KEY_WIDTH 150
#define KEY_HEIGHT 46
#define KEY_GAP 16
#define KEYS_PER_ROW 3
/* Key 0 opens the launcher; keys 1.. are apps[0]..., as on the dock. */
#define KEY_COUNT (APP_COUNT + 1)

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

/* A repeatable "random" number from 0 to 1 for a given whole number, so
 * the clouds are in the same places every boot. */
static double random_for(int n) {
	unsigned int state = (unsigned int)n * 2654435761u;
	state ^= state >> 15;
	state *= 2246822519u;
	state ^= state >> 13;
	return (state & 0xffff) / 65536.0;
}

/* ---- where things are on the screen ---- */

struct layout {
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
	l.cx = scene->width / 2.0;
	l.panel_bottom = scene->height - DOCK_HEIGHT;
	l.panel_top = l.panel_bottom - PANEL_HEIGHT;
	l.radar_x = 48 + SCREEN_SIZE / 2.0;
	l.rwr_x = scene->width - 48 - SCREEN_SIZE / 2.0;
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
	double roll;     /* radians; positive = right wing down */
	double pitch;    /* pixels the horizon is below the aim point */
	int speed;       /* knots */
	int altitude;    /* feet */
};

static struct flight flight_at(double t) {
	struct flight f;
	/* Heading swings either side of north. Roll is the rate the heading
	 * is changing (you bank to turn), which is its derivative: the
	 * derivative of sin is cos. Different periods for the rest so nothing
	 * repeats in step. */
	f.heading = 360 + 14 * sin(t * 0.07);
	f.roll = 5.0 * DEGREES * cos(t * 0.07);
	f.pitch = 14 + 7 * sin(t * 0.23);
	f.speed = 450 + (int)lround(4 * sin(t * 0.31));
	f.altitude = 24500 + 10 * (int)lround(6 * sin(t * 0.19));
	return f;
}

/* Switch to "world" coordinates: origin at the point on the horizon dead
 * ahead, x along the horizon, y down. Everything outside the aircraft is
 * drawn in these, so rolling and pitching the aircraft moves it all
 * together. Must be paired with cairo_restore(). */
static void enter_world(cairo_t *cr, const struct layout *l,
		const struct flight *f) {
	cairo_save(cr);
	cairo_translate(cr, l->aim_x, l->aim_y);
	cairo_rotate(cr, -f->roll); /* we roll right, so the world tilts left */
	cairo_translate(cr, 0, f->pitch);
}

/* ---- outside: sky, horizon, cloud deck ---- */

static void draw_outside(struct scene *scene, cairo_t *cr,
		const struct layout *l, const struct flight *f, double t) {
	/* Sky: one flat fill of the whole frame. */
	cairo_set_source_rgb(cr, 0.006, 0.030, 0.015);
	cairo_paint(cr);

	enter_world(cr, l, f);
	const double far = 2000; /* big enough to cover the screen when rolled */

	/* A band of haze just above the horizon. */
	cairo_pattern_t *haze = cairo_pattern_create_linear(0, -90, 0, 0);
	cairo_pattern_add_color_stop_rgba(haze, 0.0, HUD_GREEN, 0.0);
	cairo_pattern_add_color_stop_rgba(haze, 1.0, HUD_GREEN, 0.10);
	cairo_set_source(cr, haze);
	cairo_rectangle(cr, -far, -90, 2 * far, 90);
	cairo_fill(cr);
	cairo_pattern_destroy(haze);

	/* The cloud deck below: brightest at the horizon, darker close to. */
	cairo_pattern_t *deck = cairo_pattern_create_linear(0, 0, 0, 340);
	cairo_pattern_add_color_stop_rgb(deck, 0.0, 0.035, 0.170, 0.080);
	cairo_pattern_add_color_stop_rgb(deck, 1.0, 0.012, 0.060, 0.030);
	cairo_set_source(cr, deck);
	cairo_rectangle(cr, -far, 0, 2 * far, far);
	cairo_fill(cr);
	cairo_pattern_destroy(deck);

	/* Cloud tops rushing towards you. Each puff has a fixed sideways
	 * position and a distance that shrinks with time and wraps round.
	 * Perspective is one division: things at distance z are drawn 1/z as
	 * big and 1/z as far from the horizon. That single rule makes puffs
	 * crawl near the horizon and sweep past underneath you. */
	for (int i = 0; i < 34; i++) {
		double sideways = (random_for(i * 3) - 0.5) * 2.6;
		double progress = fmod(random_for(i * 3 + 1) + t * 0.085, 1.0);
		double z = 1.0 - 0.93 * progress;       /* 1 = far, 0.07 = close */
		double x = sideways * 330 / z;
		double y = 20 / z;
		double rx = (26 + 22 * random_for(i * 3 + 2)) / z;
		double ry = 4.5 / z;
		/* Fade in at the horizon so puffs don't pop into existence. */
		double alpha = 0.16 * fmin(1.0, progress * 5);
		ellipse(cr, x, y, rx, ry);
		cairo_set_source_rgba(cr, HUD_GREEN, alpha);
		cairo_fill(cr);
	}

	/* A few distant cloud banks sitting on the horizon. */
	for (int i = 0; i < 5; i++) {
		double x = (random_for(100 + i) - 0.5) * 1500 -
			(f->heading - 360) * 9; /* they slide as the heading changes */
		ellipse(cr, x, -6 - 10 * random_for(200 + i),
			120 + 90 * random_for(300 + i), 9 + 7 * random_for(400 + i));
		cairo_set_source_rgba(cr, HUD_GREEN, 0.07);
		cairo_fill(cr);
	}

	/* The horizon itself. */
	cairo_set_source_rgb(cr, HUD_DIM);
	cairo_set_line_width(cr, 1);
	line(cr, -far, 0, far, 0);
	cairo_restore(cr);
}

/* ---- the formation, seen from behind ----
 *
 * Each aircraft is drawn around 0,0 with `s` = half its wingspan in pixels.
 * From behind, what tells them apart is the engines and the tail:
 *   Typhoon    two engines side by side, low delta wing, small canards
 *   Tornado    two engines side by side, wings on top of the fuselage, a
 *              very tall fin, tailplanes drooping
 *   Lightning  two engines stacked one above the other
 */

/* One engine nozzle: a ring with the glow of the exhaust inside. */
static void nozzle(cairo_t *cr, double x, double y, double radius,
		double glow) {
	circle(cr, x, y, radius);
	cairo_set_source_rgba(cr, HUD_GREEN, glow);
	cairo_fill_preserve(cr);
	cairo_set_source_rgb(cr, HUD_GREEN);
	cairo_stroke(cr);
}

/* Fill the current path dark (so the horizon doesn't show through the
 * aircraft) and outline it. */
static void body(cairo_t *cr) {
	cairo_set_source_rgb(cr, 0.02, 0.11, 0.05);
	cairo_fill_preserve(cr);
	cairo_set_source_rgb(cr, HUD_GREEN);
	cairo_stroke(cr);
}

/* A wing or tailplane seen edge-on: a thin sliver from root to tip, on
 * both sides. */
static void wings(cairo_t *cr, double root_x, double root_y,
		double tip_x, double tip_y, double thickness) {
	for (int side = -1; side <= 1; side += 2) {
		cairo_move_to(cr, side * root_x, root_y - thickness);
		cairo_line_to(cr, side * tip_x, tip_y);
		cairo_line_to(cr, side * root_x, root_y + thickness);
		cairo_close_path(cr);
		body(cr);
	}
}

/* A fin seen from behind: a narrow upright blade. */
static void fin(cairo_t *cr, double base_y, double top_y, double half_width) {
	cairo_move_to(cr, -half_width, base_y);
	cairo_line_to(cr, 0, top_y);
	cairo_line_to(cr, half_width, base_y);
	cairo_close_path(cr);
	body(cr);
}

static void draw_typhoon(cairo_t *cr, double s, double glow) {
	wings(cr, 0.14 * s, 0.05 * s, s, 0.11 * s, 0.035 * s);
	wings(cr, 0.15 * s, -0.10 * s, 0.34 * s, -0.15 * s, 0.018 * s); /* canards */
	fin(cr, -0.14 * s, -0.56 * s, 0.022 * s);
	/* Fuselage, with the box of the chin intake underneath. */
	rounded_rect(cr, -0.17 * s, -0.15 * s, 0.34 * s, 0.27 * s, 0.07 * s);
	body(cr);
	cairo_rectangle(cr, -0.14 * s, 0.12 * s, 0.28 * s, 0.07 * s);
	body(cr);
	nozzle(cr, -0.08 * s, -0.02 * s, 0.068 * s, glow);
	nozzle(cr, 0.08 * s, -0.02 * s, 0.068 * s, glow);
}

static void draw_tornado(cairo_t *cr, double s, double glow) {
	wings(cr, 0.16 * s, 0.08 * s, 0.50 * s, 0.14 * s, 0.022 * s);  /* tailplanes */
	wings(cr, 0.16 * s, -0.13 * s, s, -0.07 * s, 0.035 * s);       /* wings, up high */
	fin(cr, -0.17 * s, -0.86 * s, 0.03 * s);
	rounded_rect(cr, -0.19 * s, -0.18 * s, 0.38 * s, 0.36 * s, 0.05 * s);
	body(cr);
	nozzle(cr, -0.09 * s, 0.02 * s, 0.076 * s, glow);
	nozzle(cr, 0.09 * s, 0.02 * s, 0.076 * s, glow);
}

static void draw_lightning(cairo_t *cr, double s, double glow) {
	wings(cr, 0.13 * s, 0.26 * s, 0.46 * s, 0.30 * s, 0.020 * s);  /* tailplanes, low */
	wings(cr, 0.14 * s, 0.00 * s, s, 0.09 * s, 0.035 * s);
	fin(cr, -0.30 * s, -0.84 * s, 0.028 * s);
	/* A tall, narrow fuselage: the two engines are one above the other. */
	ellipse(cr, 0, 0, 0.16 * s, 0.33 * s);
	body(cr);
	nozzle(cr, 0, -0.13 * s, 0.105 * s, glow);
	nozzle(cr, 0, 0.13 * s, 0.105 * s, glow);
}

struct wingman {
	const char *name;
	void (*draw)(cairo_t *cr, double s, double glow);
	double offset_x, offset_y; /* from the aim point, in pixels */
	double size;               /* half wingspan: nearer aircraft are bigger */
	bool label_beside;         /* name by the wingtip instead of underneath */
};

/* The leader is dead ahead, so you see it through the HUD glass. It sits
 * between two rungs of the pitch ladder, with its name off to one side,
 * to keep the symbols readable. */
static const struct wingman formation[] = {
	{ "TORNADO", draw_tornado, -262, 6, 74, false },
	{ "LIGHTNING", draw_lightning, 262, 2, 64, false },
	{ "TYPHOON", draw_typhoon, 0, -62, 70, true },
};
#define COUNT(array) ((int)(sizeof(array) / sizeof((array)[0])))

static void draw_formation(cairo_t *cr, const struct layout *l,
		const struct flight *f, double t) {
	cairo_set_line_width(cr, HUD_LINE);
	cairo_set_font_size(cr, 11);
	for (int i = 0; i < COUNT(formation); i++) {
		const struct wingman *wingman = &formation[i];
		/* Each aircraft drifts slowly around its slot, out of step with
		 * the others: formation flying is never perfectly still. */
		double phase = i * 2.1;
		double x = l->aim_x + wingman->offset_x + 9 * sin(t * 0.50 + phase);
		double y = l->aim_y + wingman->offset_y + 6 * sin(t * 0.78 + phase * 1.7);
		/* They bank with us in the turn, each a little late and a little
		 * more or less, as it follows the leader. */
		double bank = -f->roll * 0.6 + 2.0 * DEGREES * cos(t * 0.50 + phase);
		/* The exhaust glow flickers. */
		double glow = 0.42 + 0.14 * sin(t * 9 + phase * 3);

		cairo_save(cr);
		cairo_translate(cr, x, y);
		cairo_rotate(cr, bank);
		wingman->draw(cr, wingman->size, glow);
		cairo_restore(cr);

		cairo_set_source_rgb(cr, HUD_DIM);
		if (wingman->label_beside) {
			draw_text(cr, wingman->name, x + wingman->size * 0.55,
				y - wingman->size * 0.30);
		} else {
			text_centred(cr, wingman->name, x, y + wingman->size * 0.5 + 12);
		}
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

	cairo_set_source_rgb(cr, HUD_GREEN);
	cairo_set_line_width(cr, HUD_LINE);
	cairo_set_font_size(cr, 11);

	/* Pitch ladder: a rung every 5 degrees, in world coordinates so it
	 * stays parallel to the real horizon as the aircraft rolls. Above the
	 * horizon rungs are solid, below they are dashed. */
	enter_world(cr, l, f);
	const double rung_spacing = 46;
	for (int step = -2; step <= 2; step++) {
		double y = -step * rung_spacing;
		if (step == 0) {
			line(cr, -118, 0, -30, 0); /* the horizon bar */
			line(cr, 30, 0, 118, 0);
			continue;
		}
		double tick = step > 0 ? 6 : -6;
		if (step < 0) {
			const double dashes[] = {7, 5};
			cairo_set_dash(cr, dashes, 2, 0);
		}
		line(cr, -82, y, -30, y);
		line(cr, 30, y, 82, y);
		cairo_set_dash(cr, NULL, 0, 0);
		line(cr, -82, y, -82, y + tick);
		line(cr, 82, y, 82, y + tick);
		char label[8];
		snprintf(label, sizeof(label), "%d", step * 5);
		draw_text(cr, label, -88 - text_width(cr, label), y);
		draw_text(cr, label, 88, y);
	}
	cairo_restore(cr);

	/* Flight path marker: fixed on the glass, where the aircraft is
	 * going. A small circle with wings and a fin. */
	circle(cr, l->aim_x, l->aim_y, 7);
	cairo_stroke(cr);
	line(cr, l->aim_x - 22, l->aim_y, l->aim_x - 7, l->aim_y);
	line(cr, l->aim_x + 7, l->aim_y, l->aim_x + 22, l->aim_y);
	line(cr, l->aim_x, l->aim_y - 7, l->aim_x, l->aim_y - 15);

	/* Heading tape along the top of the glass: a tick every 2 degrees,
	 * sliding under a fixed caret. */
	const double pixels_per_degree = 5, half_width = 112;
	double tape_y = l->hud_y + 30;
	int first = (int)floor(f->heading - half_width / pixels_per_degree);
	int last = (int)ceil(f->heading + half_width / pixels_per_degree);
	for (int degree = first; degree <= last; degree++) {
		double x = l->cx + (degree - f->heading) * pixels_per_degree;
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
	double nearest = 11; /* pixels: how close the pointer has to be */
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
	rounded_rect(cr, x + 0.5, y + 0.5, KEY_WIDTH - 1, KEY_HEIGHT - 1, 5);
	if (lit) {
		cairo_set_source_rgb(cr, HUD_GREEN);
		cairo_fill(cr);
		cairo_set_source_rgb(cr, HUD_BLACK);
	} else {
		/* A raised button: lighter than the panel, a bright edge, and a
		 * faint highlight along the top as if lit from above. */
		cairo_set_source_rgb(cr, 0.035, 0.115, 0.058);
		cairo_fill_preserve(cr);
		cairo_set_source_rgb(cr, HUD_DIM);
		cairo_set_line_width(cr, HUD_LINE);
		cairo_stroke(cr);
		cairo_set_source_rgba(cr, HUD_GREEN, 0.35);
		cairo_set_line_width(cr, 1);
		line(cr, x + 7, y + 3.5, x + KEY_WIDTH - 7, y + 3.5);
		cairo_set_source_rgb(cr, HUD_GREEN);
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

	/* Bezel: a frame with a row of small unlabelled buttons above and
	 * below, like the soft keys round a real multi-function display. */
	rounded_rect(cr, cx - half - 9, cy - half - 9, SCREEN_SIZE + 18,
		SCREEN_SIZE + 18, 8);
	cairo_set_source_rgb(cr, 0.030, 0.100, 0.050);
	cairo_fill_preserve(cr);
	cairo_set_source_rgb(cr, HUD_DIM);
	cairo_set_line_width(cr, HUD_LINE);
	cairo_stroke(cr);
	cairo_set_source_rgb(cr, HUD_DIM);
	for (int i = 0; i < 5; i++) {
		double x = cx - half + 14 + i * 33;
		cairo_rectangle(cr, x, cy - half - 7, 16, 4);
		cairo_rectangle(cr, x, cy + half + 3, 16, 4);
	}
	cairo_fill(cr);

	/* The glass. */
	cairo_rectangle(cr, cx - half, cy - half, SCREEN_SIZE, SCREEN_SIZE);
	cairo_set_source_rgb(cr, HUD_BLACK);
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
	int w = scene->width, h = scene->height;

	cairo_select_font_face(cr, UI_FONT, CAIRO_FONT_SLANT_NORMAL,
		CAIRO_FONT_WEIGHT_NORMAL);

	/* Canopy frame: you look out through a big oval of perspex. Everything
	 * outside the oval is frame. EVEN_ODD makes the oval a hole in the
	 * rectangle, so only the corners get filled. */
	double oval_x = l.cx, oval_y = h * 0.73;
	double oval_rx = w * 0.742, oval_ry = h * 0.807;
	cairo_rectangle(cr, 0, 0, w, h);
	ellipse(cr, oval_x, oval_y, oval_rx, oval_ry);
	cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
	cairo_set_source_rgb(cr, 0.012, 0.045, 0.022);
	cairo_fill(cr);
	cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
	ellipse(cr, oval_x, oval_y, oval_rx, oval_ry);
	cairo_set_source_rgb(cr, HUD_DIM);
	cairo_set_line_width(cr, 2);
	cairo_stroke(cr);

	/* HUD: two posts rising from the panel, and the glass they hold. The
	 * glass is only an outline with a hint of tint: you look through it. */
	cairo_set_source_rgb(cr, HUD_DIM);
	cairo_set_line_width(cr, 3);
	line(cr, l.hud_x, l.hud_y + 26, l.hud_x - 10, l.panel_top);
	line(cr, l.hud_x + l.hud_w, l.hud_y + 26, l.hud_x + l.hud_w + 10,
		l.panel_top);
	rounded_rect(cr, l.hud_x, l.hud_y, l.hud_w, l.hud_h, 26);
	cairo_set_source_rgba(cr, HUD_GREEN, 0.035);
	cairo_fill_preserve(cr);
	cairo_set_source_rgb(cr, HUD_DIM);
	cairo_set_line_width(cr, HUD_LINE);
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
	cairo_set_source_rgb(cr, 0.016, 0.058, 0.029);
	cairo_fill_preserve(cr);
	cairo_set_source_rgb(cr, HUD_DIM);
	cairo_set_line_width(cr, 2);
	cairo_stroke(cr);

	draw_screen(cr, "RADAR", l.radar_x, l.scope_y, false);
	draw_screen(cr, "RWR", l.rwr_x, l.scope_y, true);

	/* Keys, with a caption over the group. */
	cairo_set_font_size(cr, 11);
	cairo_set_source_rgb(cr, HUD_DIM);
	text_centred(cr, "SYSTEMS", l.cx, l.keys_y - 12);
	for (int i = 0; i < KEY_COUNT; i++) {
		draw_key(cr, &l, i, false);
	}

	/* The data display: a recessed dark window. */
	rounded_rect(cr, l.data_x + 0.5, l.data_y + 0.5, l.data_w - 1,
		l.data_h - 1, 4);
	cairo_set_source_rgb(cr, HUD_BLACK);
	cairo_fill_preserve(cr);
	cairo_set_source_rgb(cr, HUD_DIM);
	cairo_set_line_width(cr, HUD_LINE);
	cairo_stroke(cr);
}

/* ---- public functions (see wallpaper.h) ---- */

void scene_init(struct scene *scene, int width, int height) {
	scene_finish(scene);
	scene->width = width;
	scene->height = height;
	scene->hover_key = -1;

	/* ARGB32: has an alpha channel, so the window part stays see-through. */
	scene->cockpit = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
		width, height);
	cairo_t *cr = cairo_create(scene->cockpit);
	draw_cockpit(scene, cr);
	cairo_destroy(cr);
}

void scene_finish(struct scene *scene) {
	if (scene->cockpit != NULL) {
		cairo_surface_destroy(scene->cockpit);
		scene->cockpit = NULL;
	}
}

void scene_draw(struct scene *scene, cairo_t *cr, double t) {
	struct layout l = layout_for(scene);
	struct flight f = flight_at(t);
	processes_update(&scene->processes, t);

	cairo_select_font_face(cr, UI_FONT, CAIRO_FONT_SLANT_NORMAL,
		CAIRO_FONT_WEIGHT_NORMAL);

	/* Only the part above the instrument panel shows the outside, so
	 * don't spend time drawing sky that the panel will cover. */
	cairo_save(cr);
	cairo_rectangle(cr, 0, 0, scene->width, l.panel_top + 18);
	cairo_clip(cr);
	draw_outside(scene, cr, &l, &f, t);
	draw_formation(cr, &l, &f, t);
	draw_hud(cr, &l, &f);
	cairo_restore(cr);

	/* Lay the cached cockpit over the top. Where the picture so far was
	 * clipped away (the panel), SOURCE would be needed to overwrite old
	 * pixels; the panel is fully opaque, so plain blending does the same. */
	cairo_set_source_surface(cr, scene->cockpit, 0, 0);
	cairo_paint(cr);

	/* The live parts of the panel. */
	draw_radar(scene, cr, &l, t);
	int threats = draw_rwr(scene, cr, &l);
	draw_data_display(scene, cr, &l, threats);

	scene->hover_key = scene->pointer_inside ?
		key_at(&l, scene->pointer_x, scene->pointer_y) : -1;
	if (scene->hover_key >= 0) {
		draw_key(cr, &l, scene->hover_key, true);
	}
}

void scene_pointer_motion(struct scene *scene, double x, double y) {
	scene->pointer_x = x;
	scene->pointer_y = y;
	scene->pointer_inside = true;
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
