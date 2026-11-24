/*
 * Drawing the wallpaper: one frame of the view from a fighter cockpit.
 *
 * What is in the picture, back to front:
 *   outside     blue sky, sea and cloud streaming past below (sky.c)
 *   formation   two jets ahead of you: a Typhoon on the left, an F-35 on
 *               the right (3D models, model.c)
 *   HUD         green symbols on the glass in front of you: pitch ladder,
 *               flight path marker, heading tape, speed and altitude
 *   cockpit     canopy arch, sills and instrument panel (cached)
 *   screens     left screen: radar, each contact one of the busiest programs
 *               right screen: RWR, each "threat" a program using the CPU
 *   centre      a display with pages (pages.c), and buttons round it that
 *               start apps and change the page
 *   dials       CPU, memory, load and task count (where there is room)
 *   consoles    caution lights and switches, where the dials don't fit
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
#include <time.h>

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
#define MFD_WIDTH 482        /* the glass of the centre display */
/* The app keys are push buttons along the top and bottom of the centre
 * display's frame, each with its name lit on the glass beside it. Real
 * cockpit displays work like this; the buttons are called "option select
 * buttons" (OSBs). Five along the top, five along the bottom. */
#define OSB_PER_ROW 5
#define OSB_WIDTH 30
#define OSB_HEIGHT 12
/* ---- colours for the physical cockpit and the aircraft ---- */

struct colour { double r, g, b; };

static const struct colour metal_light = { 0.50, 0.53, 0.57 };
static const struct colour metal_mid = { 0.27, 0.29, 0.32 };
static const struct colour metal_dark = { 0.13, 0.14, 0.16 };
static const struct colour metal_black = { 0.055, 0.060, 0.070 };
static const struct colour panel_top_grey = { 0.245, 0.26, 0.28 };
static const struct colour panel_low_grey = { 0.175, 0.185, 0.20 };
static const struct colour bezel_top = { 0.155, 0.16, 0.175 };
static const struct colour bezel_low = { 0.085, 0.09, 0.10 };
static const struct colour glass_black = { 0.000, 0.030, 0.014 }; /* a screen, off */

#define CAUTION_AMBER 1.00, 0.68, 0.12
#define PLACARD 0.78, 0.80, 0.78   /* white stencilled lettering */

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
	double data_x, data_y, data_w, data_h; /* the centre display's glass */
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
	/* The frame round the glass is 26 units deep top and bottom, room for
	 * the buttons. Below it is a strip for the knobs and the ejection
	 * handle. */
	l.data_w = MFD_WIDTH;
	l.data_x = l.cx - l.data_w / 2;
	l.data_y = l.panel_top + 46;
	l.data_h = l.panel_bottom - 56 - l.data_y;
	/* The HUD glass stands on the glareshield. hud_x and hud_w are its
	 * bottom edge; the top edge is narrower (see hud_glass_path()). */
	l.hud_w = 252;
	l.hud_h = 260;
	l.hud_x = l.cx - l.hud_w / 2;
	l.hud_y = l.panel_top - 14 - l.hud_h;
	l.aim_x = l.cx;
	l.aim_y = l.hud_y + l.hud_h * 0.52;
	return l;
}

/* Where button `slot` is: the top-left corner of the button in the frame,
 * and the centre of its name on the glass next to it. Slots 0-4 run along
 * the top, left to right; 5-9 along the bottom. */
static void key_position(const struct layout *l, int slot,
		double *x, double *y, double *legend_x, double *legend_y) {
	double pitch = l->data_w / OSB_PER_ROW;
	double centre = l->data_x + pitch * (slot % OSB_PER_ROW + 0.5);
	bool top = slot < OSB_PER_ROW;
	*x = centre - OSB_WIDTH / 2.0;
	*y = top ? l->data_y - 19 : l->data_y + l->data_h + 7;
	*legend_x = centre;
	*legend_y = top ? l->data_y + 12 : l->data_y + l->data_h - 12;
}

static struct page_area page_area(const struct layout *l) {
	return (struct page_area){ l->data_x, l->data_y, l->data_w, l->data_h };
}

/* ---- the flight: a slow, gentle weave ---- */

struct flight {
	double heading;  /* degrees */
	double pitch;    /* design units the horizon is below the aim point */
	int speed;       /* knots */
	int altitude;    /* feet */
	double climb;    /* feet per minute, negative when descending */
};

static struct flight flight_at(double t) {
	struct flight f;
	/* Each quantity drifts on its own period so nothing repeats in step. */
	f.heading = 360 + 14 * sin(t * 0.07);
	f.pitch = 16 + 8 * sin(t * 0.23);
	f.speed = 450 + (int)lround(4 * sin(t * 0.31));
	f.altitude = 24500 + 10 * (int)lround(6 * sin(t * 0.19));
	/* How fast the altitude above is changing: the slope of 60*sin(0.19t)
	 * is 60*0.19*cos(0.19t) feet per second; times 60 for per minute. */
	f.climb = 60 * 0.19 * cos(t * 0.19) * 60;
	return f;
}

/* ---- the formation ----
 *
 * The aircraft ahead of you, drawn as 3D models by model.c. Here we only
 * decide where each one is and how it is turned at time t.
 */

struct wingman {
	int model;
	double x, y, z; /* its slot: metres right, up and ahead of the camera */
	double yaw;     /* how far its nose is turned out, degrees, right positive */
};

/* One each side of you, the Typhoon on the left and the F-35 on the right.
 * Both are below you, so you look down on their wings from behind.
 * (Level with you, a wing is seen edge-on and all but disappears.)
 *
 * Seen from between them, perspective makes both noses seem to turn in.
 * Turning each out cancels most of that, so they look parallel. */
static const struct wingman formation[MODEL_COUNT] = {
	{ MODEL_F35, 22.0, -4.8, 38.0, 18.0 },
	{ MODEL_TYPHOON, -22.0, -4.0, 38.0, -18.0 },
};

/* How long a lens the camera has, in design units. It sets how big the
 * aircraft look for their distance. */
#define FOCAL_LENGTH 430.0

/* Draw the aircraft into the frame's pixels. */
static void draw_formation(struct scene *scene, const struct layout *l,
		const struct flight *f, double t, uint32_t *pixels, int stride_pixels,
		int rows) {
	for (int i = 0; i < MODEL_COUNT; i++) {
		const struct wingman *wingman = &formation[i];
		/* Each aircraft drifts slowly around its slot, out of step with
		 * the others: formation flying is never perfectly still. */
		double phase = i * 2.1;
		double sway = sin(t * 0.50 + phase);
		struct model_pose pose = {
			.x = wingman->x + 0.7 * sway,
			.y = wingman->y + 0.45 * sin(t * 0.78 + phase * 1.7),
			.z = wingman->z + 0.8 * sin(t * 0.31 + phase),
			/* It rolls towards the way it is drifting, and its nose
			 * hunts up and down and side to side by a degree or so. */
			.roll = -4.0 * DEGREES * cos(t * 0.50 + phase),
			.pitch = 1.2 * DEGREES * sin(t * 0.62 + phase),
			.yaw = (wingman->yaw + sin(t * 0.41 + phase * 0.7)) * DEGREES,
			.focal = FOCAL_LENGTH * scene->scale,
			.screen_x = l->aim_x * scene->scale,
			/* Level flight heads for the horizon, as the cloud does. */
			.screen_y = (l->aim_y + f->pitch) * scene->scale,
			.glow = 0.86 + 0.14 * sin(t * 11 + phase * 3), /* flicker */
		};
		model_draw(wingman->model, &pose, pixels, stride_pixels,
			scene->width, scene->height);

		/* Vapour from the wing tips, towards us and so over the aircraft.
		 * The harder the wings work, the thicker it is, and they work
		 * hardest while it is banked. */
		double strength = 0.45 + 0.55 * fabs(cos(t * 0.50 + phase));
		for (int side = 0; side < 2; side++) {
			double tip[3];
			if (model_wing_tip(wingman->model, side, &pose, tip)) {
				vapour_trail(&pose, tip, strength, t, pixels, stride_pixels,
					scene->width, rows);
			}
		}
	}
}

/* ---- HUD symbology, drawn on the glass ---- */

/* The glass leans back towards you, so its top edge is further from your
 * eye than its bottom edge and looks narrower: each side comes in by
 * HUD_TAPER units between bottom and top. */
#define HUD_TAPER 16

/* Left edge of the glass at height y (the right edge is its mirror image). */
static double hud_left_at(const struct layout *l, double y) {
	double up = (l->hud_y + l->hud_h - y) / l->hud_h; /* 0 bottom, 1 top */
	return l->hud_x + HUD_TAPER * up;
}

/* Make the outline of the glass the current path: straight sloping sides,
 * rounded top corners. Used to clip the symbols, and to draw its edge. */
static void hud_glass_path(cairo_t *cr, const struct layout *l) {
	double top = l->hud_y, bottom = l->hud_y + l->hud_h;
	double left = l->hud_x + HUD_TAPER, right = l->hud_x + l->hud_w - HUD_TAPER;
	const double corner = 18;
	cairo_new_path(cr);
	cairo_move_to(cr, l->hud_x, bottom);
	cairo_line_to(cr, hud_left_at(l, top + corner), top + corner);
	/* A curve that leaves along the side and arrives along the top; both
	 * control points at the corner itself give a tight, even bend. */
	cairo_curve_to(cr, left, top, left, top, left + corner, top);
	cairo_line_to(cr, right - corner, top);
	cairo_curve_to(cr, right, top, right, top,
		2 * l->cx - hud_left_at(l, top + corner), top + corner);
	cairo_line_to(cr, l->hud_x + l->hud_w, bottom);
	cairo_close_path(cr);
}

/* In whatever colour and line width are set. */
static void draw_hud_symbols(cairo_t *cr, const struct layout *l,
		const struct flight *f) {
	cairo_set_font_size(cr, FONT_SIZE(11));

	/* Pitch ladder: a rung every 5 degrees, measured from the horizon,
	 * so it moves up and down with the real horizon outside. Above the
	 * horizon rungs are solid, below they are dashed. */
	double horizon = l->aim_y + f->pitch;
	const double rung_spacing = 44;
	for (int step = -2; step <= 2; step++) {
		double y = horizon - step * rung_spacing;
		if (step == 0) {
			line(cr, l->cx - 62, y, l->cx - 26, y); /* the horizon bar */
			line(cr, l->cx + 26, y, l->cx + 62, y);
			continue;
		}
		double tick = step > 0 ? 6 : -6;
		if (step < 0) {
			const double dashes[] = {7, 5};
			cairo_set_dash(cr, dashes, 2, 0);
		}
		line(cr, l->cx - 48, y, l->cx - 26, y);
		line(cr, l->cx + 26, y, l->cx + 48, y);
		cairo_set_dash(cr, NULL, 0, 0);
		line(cr, l->cx - 48, y, l->cx - 48, y + tick);
		line(cr, l->cx + 48, y, l->cx + 48, y + tick);
		char label[8];
		snprintf(label, sizeof(label), "%d", step * 5);
		draw_text(cr, label, l->cx - 54 - text_width(cr, label), y);
		draw_text(cr, label, l->cx + 54, y);
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
	const double units_per_degree = 5, half_width = 92;
	double tape_y = l->hud_y + 26;
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
	cairo_set_font_size(cr, FONT_SIZE(13));
	snprintf(value, sizeof(value), "%d", f->speed);
	cairo_rectangle(cr, l->hud_x + 14, l->aim_y - 11, 44, 22);
	cairo_stroke(cr);
	text_centred(cr, value, l->hud_x + 36, l->aim_y);
	snprintf(value, sizeof(value), "%d", f->altitude);
	cairo_rectangle(cr, l->hud_x + l->hud_w - 64, l->aim_y - 11, 50, 22);
	cairo_stroke(cr);
	text_centred(cr, value, l->hud_x + l->hud_w - 39, l->aim_y);

}

static void draw_hud(cairo_t *cr, const struct layout *l,
		const struct flight *f, double glass_x, double glass_y) {
	/* Symbols only appear on the glass, so clip to it. */
	cairo_save(cr);
	cairo_translate(cr, glass_x, glass_y);
	hud_glass_path(cr, l);
	cairo_translate(cr, -glass_x, -glass_y); /* the path stays where it is */
	cairo_clip(cr);

	/* The glass is very slightly smoked. */
	cairo_set_source_rgba(cr, 0.00, 0.08, 0.04, 0.20);
	cairo_paint(cr);

	/* The symbols twice: first a soft dark copy, wider and a touch lower,
	 * so that they can be read against white cloud. */
	cairo_save(cr);
	cairo_translate(cr, 0.3, 0.6);
	cairo_set_source_rgba(cr, 0.0, 0.12, 0.06, 0.42);
	cairo_set_line_width(cr, HUD_LINE + 1.2);
	draw_hud_symbols(cr, l, f);
	cairo_restore(cr);
	cairo_set_source_rgb(cr, HUD_GREEN);
	cairo_set_line_width(cr, HUD_LINE);
	draw_hud_symbols(cr, l, f);
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

/* How many contacts the radar shows. A desktop distro runs 50 or more
 * programs, most of them idle system services, and all of them at once
 * fill the scope with dots. */
#define RADAR_CONTACTS 16

/* Is process a busier than process b? Anything using the CPU beats
 * anything idle, and the busier the better; between idle programs, the
 * one using more memory wins. Memory hardly changes from one sample to the
 * next, so the idle ones on the scope stay put. */
static bool busier(const struct process *a, const struct process *b) {
	bool a_active = a->cpu_percent >= 0.5, b_active = b->cpu_percent >= 0.5;
	if (a_active != b_active) {
		return a_active;
	}
	if (a_active && a->cpu_percent != b->cpu_percent) {
		return a->cpu_percent > b->cpu_percent;
	}
	return a->memory_kb > b->memory_kb;
}

/* Marks which processes the radar shows: the locked one, then the busiest
 * until there are RADAR_CONTACTS. Returns how many it marked. */
static int radar_pick(const struct scene *scene, bool shown[MAX_PROCESSES]) {
	const struct process_list *list = &scene->processes;
	int picked = 0;
	for (int i = 0; i < list->count; i++) {
		shown[i] = list->items[i].pid == scene->locked_pid;
		picked += shown[i];
	}
	while (picked < RADAR_CONTACTS && picked < list->count) {
		int best = -1;
		for (int i = 0; i < list->count; i++) {
			if (!shown[i] && (best < 0 ||
					busier(&list->items[i], &list->items[best]))) {
				best = i;
			}
		}
		shown[best] = true;
		picked++;
	}
	return picked;
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
	cairo_set_source_rgba(cr, HUD_GREEN, 0.18); /* its glow */
	cairo_set_line_width(cr, 6);
	line(cr, cx, cy, sx, sy);
	cairo_set_source_rgb(cr, HUD_GREEN);
	cairo_set_line_width(cr, HUD_LINE);
	line(cr, cx, cy, sx, sy);

	/* The contacts. Range still comes from the position in the whole
	 * list, not among the ones shown, so a contact appearing or leaving
	 * doesn't shift the others. */
	bool shown[MAX_PROCESSES];
	radar_pick(scene, shown);
	scene->hover_pid = 0;
	double nearest = 11; /* how close the pointer has to be */
	for (int i = 0; i < list->count; i++) {
		struct process *process = &list->items[i];
		if (!shown[i]) {
			continue;
		}
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
		circle(cr, process->radar_x, process->radar_y, size * 2);
		cairo_set_source_rgba(cr, HUD_GREEN, brightness * 0.16);
		cairo_fill(cr);
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

	cairo_set_font_size(cr, FONT_SIZE(11));
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

	if (target != NULL) {
		cairo_set_source_rgb(cr, HUD_GREEN);
		cairo_set_line_width(cr, HUD_LINE);
		cairo_rectangle(cr, target->radar_x - 8, target->radar_y - 8, 16, 16);
		cairo_stroke(cr);
	}
	if (scene->pages.current != PAGE_HOME) {
		return; /* the rest of the glass is another page's */
	}

	/* Below the row of button names along the top of the glass. */
	double x = l->data_x + 14, y = l->data_y + 40;
	double right = l->data_x + l->data_w / 2 + 20;
	char row[48];
	cairo_set_font_size(cr, FONT_SIZE(12));

	/* Left half: the target. */
	if (target != NULL) {
		cairo_set_source_rgb(cr, HUD_GREEN);
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
	bool shown[MAX_PROCESSES];
	int contacts = radar_pick(scene, shown);
	if (contacts < scene->processes.count) {
		/* Shown, out of how many running. */
		snprintf(row, sizeof(row), "CONTACTS %d/%d", contacts,
			scene->processes.count);
	} else {
		snprintf(row, sizeof(row), "CONTACTS %d", contacts);
	}
	draw_text(cr, row, right, y);
	if (threats == 0) {
		snprintf(row, sizeof(row), "RWR CLEAR");
	} else {
		snprintf(row, sizeof(row), "RWR %d ACTIVE", threats);
	}
	draw_text(cr, row, right, y + 18);
}

/* The line in the window under the HUD: how hard the machine is working. */
static void draw_status_line(const struct scene *scene, cairo_t *cr,
		const struct layout *l) {
	char status[32];
	snprintf(status, sizeof(status), "CPU %d%%  MEM %d%%",
		(int)lround(scene->processes.cpu_fraction * 100),
		(int)lround(scene->processes.memory_fraction * 100));
	cairo_set_font_size(cr, FONT_SIZE(9));
	cairo_set_source_rgb(cr, HUD_GREEN);
	text_centred(cr, status, l->cx, l->panel_top - 7);
}

/* ---- cockpit keys ---- */

/* How big a button's name is on the glass: most of the space between
 * buttons, one line of text high. */
static double legend_width(const struct layout *l) {
	return l->data_w / OSB_PER_ROW - 8;
}
#define LEGEND_HEIGHT 18

/* One button. Drawn into the cached cockpit in its resting state, and again
 * on top when the pointer is on it (lit) or it has just been pressed. */
enum key_state { KEY_RESTING, KEY_LIT, KEY_PRESSED };

static void draw_key(cairo_t *cr, const struct layout *l, int slot,
		enum key_state state) {
	double x, y, lx, ly;
	key_position(l, slot, &x, &y, &lx, &ly);

	/* The button: a dark hole in the frame with a small cap in it. */
	rounded_rect(cr, x - 2, y - 2, OSB_WIDTH + 4, OSB_HEIGHT + 4, 4);
	set_colour(cr, metal_black);
	cairo_fill(cr);
	if (state == KEY_PRESSED) {
		/* Sunk: further from you, so smaller, lower in its hole, and in
		 * shadow along the top. Its light spills round the edge. */
		rounded_rect(cr, x - 1, y - 1, OSB_WIDTH + 2, OSB_HEIGHT + 2, 4);
		cairo_set_source_rgba(cr, HUD_GREEN, 0.30);
		cairo_fill(cr);
		rounded_rect(cr, x + 1.8, y + 2.2, OSB_WIDTH - 3.6, OSB_HEIGHT - 3.4, 2);
		set_shading(cr, y, y + OSB_HEIGHT, (struct colour){ HUD_DIM },
			(struct colour){ 0.55, 1.00, 0.68 });
		cairo_fill(cr);
		return;
	}
	rounded_rect(cr, x + 0.5, y + 0.5, OSB_WIDTH - 1, OSB_HEIGHT - 1, 3);
	if (state == KEY_LIT) {
		cairo_set_source_rgb(cr, HUD_GREEN); /* backlit */
		cairo_fill(cr);
	} else {
		/* Lighter at the top where the light catches it. */
		set_shading(cr, y, y + OSB_HEIGHT, metal_light, metal_mid);
		cairo_fill(cr);
	}
}

/* Its name, which depends on the page. Lit, it is shown inverted in a box,
 * which is how a real display marks the option that is selected. */
static void draw_legend(cairo_t *cr, const struct layout *l,
		const struct pages *pages, int slot, bool lit) {
	const char *name = pages_label(pages, slot);
	if (name == NULL) {
		return;
	}
	double x, y, lx, ly, half = legend_width(l) / 2;
	key_position(l, slot, &x, &y, &lx, &ly);
	cairo_set_font_size(cr, FONT_SIZE(11));
	if (lit) {
		cairo_rectangle(cr, lx - half, ly - LEGEND_HEIGHT / 2.0,
			2 * half, LEGEND_HEIGHT);
		cairo_set_source_rgb(cr, HUD_GREEN);
		cairo_fill(cr);
		cairo_set_source_rgb(cr, HUD_BLACK);
	} else {
		cairo_set_source_rgb(cr, HUD_GREEN);
	}
	char legend[16];
	shorten_name(name, 12, legend, sizeof(legend));
	text_centred(cr, legend, lx, ly);
}

/* Which slot is at x,y? -1 for none. A key can be clicked on its button,
 * on its name, or anywhere between the two; a list entry, on its name. */
static int key_at(const struct layout *l, const struct pages *pages,
		double x, double y) {
	double half = legend_width(l) / 2;
	for (int i = 0; i < PAGE_KEYS; i++) {
		if (pages_label(pages, i) == NULL) {
			continue;
		}
		double bx, by, lx, ly;
		key_position(l, i, &bx, &by, &lx, &ly);
		double top = fmin(by, ly - LEGEND_HEIGHT / 2.0);
		double bottom = fmax(by + OSB_HEIGHT, ly + LEGEND_HEIGHT / 2.0);
		if (x >= lx - half && x < lx + half && y >= top && y < bottom) {
			return i;
		}
	}
	struct page_area area = page_area(l);
	return pages_slot_at(pages, &area, x, y);
}

/* How long a button stays down, and how long the glass takes to redraw when
 * the page changes: blank for the first part, then from the top down. */
#define PRESS_SECONDS 0.20
#define BLANK_SECONDS 0.07
#define REDRAW_SECONDS 0.22

/* The centre display: the buttons, their names, and the page. */
static void draw_centre(struct scene *scene, cairo_t *cr,
		const struct layout *l, int threats, double t) {
	struct pages *pages = &scene->pages;
	struct page_area area = page_area(l);
	double age = t - scene->started_at;
	if (startup_testing(age)) {
		startup_draw_test(cr, &area, &pages->machine,
			scene->processes.count, age);
		return;
	}

	scene->hover_key = scene->pointer_inside ?
		key_at(l, pages, scene->pointer_x, scene->pointer_y) : -1;
	if (scene->hover_key >= 0 && scene->hover_key < PAGE_KEYS) {
		draw_key(cr, l, scene->hover_key, KEY_LIT);
	}
	if (scene->pressed_key < PAGE_KEYS && scene->pressed_at > 0 &&
			t - scene->pressed_at < PRESS_SECONDS) {
		draw_key(cr, l, scene->pressed_key, KEY_PRESSED);
	}

	if (pages->current != scene->shown_page ||
			pages->first != scene->shown_first) {
		scene->shown_page = pages->current;
		scene->shown_first = pages->first;
		scene->page_changed_at = t;
	}
	double since = t - scene->page_changed_at;
	bool redrawing = scene->page_changed_at > 0 && since < REDRAW_SECONDS;
	double done = area.h * (since - BLANK_SECONDS) /
		(REDRAW_SECONDS - BLANK_SECONDS); /* how far down it has got */
	if (redrawing && done <= 0) {
		return; /* blank */
	}
	cairo_save(cr);
	if (redrawing) {
		cairo_rectangle(cr, area.x, area.y, area.w, done);
		cairo_clip(cr);
	}
	draw_data_display(scene, cr, l, threats);
	for (int slot = 0; slot < PAGE_KEYS; slot++) {
		draw_legend(cr, l, pages, slot, slot == scene->hover_key);
	}
	pages_draw(pages, cr, &area, scene->hover_key, t);
	if (redrawing) {
		/* The bright line where it is drawing now. */
		cairo_rectangle(cr, area.x, area.y + done - 3, area.w, 3);
		cairo_set_source_rgba(cr, HUD_GREEN, 0.5);
		cairo_fill(cr);
	}
	cairo_restore(cr);
}

/* ---- dials ----
 *
 * Round gauges in the spaces either side of the centre panel, each showing
 * one real figure about the machine. The face (ring, ticks, name) is part
 * of the cached cockpit; only the needle is drawn each frame. They grow
 * with the space they have: full size on a 16:9 screen, smaller on 16:10,
 * and gone on 4:3, where there is no room.
 */
#define DIAL_COUNT 4
#define DIAL_RADIUS 46      /* full size */
#define DIAL_RADIUS_MIN 30  /* any smaller and the numbers can't be read */

struct dial {
	const char *name;
	double full_scale;  /* the value at the end of the scale */
	const char *top;    /* the number printed at the end of the scale */
};

static const struct dial dials[DIAL_COUNT] = {
	{ "CPU %", 100, "100" },
	{ "MEM %", 100, "100" },
	{ "LOAD", 2, "2" },
	{ "TASKS", 80, "80" },
};

/* The dials' radius on this screen: as big as the strip between a side
 * screen and the centre display allows (keeping 20 units clear each side),
 * up to full size. 0 if even the smallest dial would not fit. */
static double dial_radius(const struct layout *l) {
	double left_edge = l->radar_x + SCREEN_SIZE / 2.0 + 11;
	double gap = (l->data_x - 30) - left_edge;
	double radius = fmin(DIAL_RADIUS, (gap - 40) / 2);
	return radius < DIAL_RADIUS_MIN ? 0 : radius;
}

/* Centre of dial `index`, or false if this screen has no room for dials.
 * Dials 0 and 1 are stacked left of the centre panel, 2 and 3 right. */
static bool dial_position(const struct layout *l, int index,
		double *x, double *y) {
	double radius = dial_radius(l);
	if (radius == 0) {
		return false;
	}
	double left_edge = l->radar_x + SCREEN_SIZE / 2.0 + 11;
	double gap = (l->data_x - 30) - left_edge;
	double offset = left_edge + gap / 2;
	*x = index < 2 ? offset : l->width - offset;
	/* One above the other, either side of the scopes' centre line. */
	*y = l->scope_y + (index % 2 == 0 ? -(radius + 8) : radius + 10);
	return true;
}

/* A dial's scale runs clockwise from lower-left (-135 degrees from "up")
 * to lower-right (+135). */
static double dial_bearing(double fraction) {
	if (fraction < 0) fraction = 0;
	if (fraction > 1) fraction = 1;
	return -135 + 270 * fraction;
}

static void draw_dial_face(cairo_t *cr, const struct dial *dial,
		double x, double y, double r) {
	/* Text shrinks with the dial, but less, so it stays readable. */
	double text_scale = 0.4 + 0.6 * r / DIAL_RADIUS;

	/* Bezel ring, then the black face. */
	circle(cr, x, y, r + 5);
	set_shading(cr, y - r - 5, y + r + 5, metal_light, metal_dark);
	cairo_fill(cr);
	circle(cr, x, y, r);
	set_colour(cr, glass_black);
	cairo_fill(cr);

	/* The last part of the scale is coloured: amber, take care; red, too
	 * much. White ticks over it, eleven, every other one longer. */
	cairo_set_line_width(cr, 4);
	cairo_new_sub_path(cr);
	cairo_arc(cr, x, y, r - 4, (dial_bearing(0.7) - 90) * DEGREES,
		(dial_bearing(0.88) - 90) * DEGREES);
	cairo_set_source_rgb(cr, CAUTION_AMBER);
	cairo_stroke(cr);
	cairo_new_sub_path(cr);
	cairo_arc(cr, x, y, r - 4, (dial_bearing(0.88) - 90) * DEGREES,
		(dial_bearing(1.0) - 90) * DEGREES);
	cairo_set_source_rgb(cr, 0.85, 0.13, 0.08);
	cairo_stroke(cr);
	cairo_set_source_rgb(cr, PLACARD);
	cairo_set_line_width(cr, 1.2);
	for (int i = 0; i <= 10; i++) {
		double x1, y1, x2, y2;
		polar(x, y, dial_bearing(i / 10.0), r - (i % 2 == 0 ? 10 : 6), &x1, &y1);
		polar(x, y, dial_bearing(i / 10.0), r - 2, &x2, &y2);
		line(cr, x1, y1, x2, y2);
	}

	/* The end numbers are in the gap below the scale, where the needle
	 * never goes, and the name is below them. */
	cairo_set_font_size(cr, FONT_SIZE(9 * text_scale));
	double tx, ty;
	polar(x, y, 208, r * 0.66, &tx, &ty);
	text_centred(cr, "0", tx, ty);
	polar(x, y, 152, r * 0.66, &tx, &ty);
	text_centred(cr, dial->top, tx, ty);
	text_centred(cr, dial->name, x, y + r * 0.82);
}

/* The engine shakes the needles very slightly, each in its own way. */
static double needle_shake(double t, int which) {
	return 0.35 * sin(t * 41 + which * 2.1) + 0.25 * sin(t * 67 + which * 1.3);
}

static void draw_dial_needles(struct scene *scene, cairo_t *cr,
		const struct layout *l, double t) {
	double age = t - scene->started_at;
	const struct process_list *list = &scene->processes;
	const double values[DIAL_COUNT] = {
		list->cpu_fraction * 100, list->memory_fraction * 100,
		list->load_average, list->count,
	};
	double r = dial_radius(l);
	for (int i = 0; i < DIAL_COUNT; i++) {
		double x, y, tip_x, tip_y;
		if (!dial_position(l, i, &x, &y)) {
			return;
		}
		polar(x, y, dial_bearing(startup_dial(age,
			values[i] / dials[i].full_scale)) + needle_shake(t, i), r - 11, &tip_x, &tip_y);
		cairo_set_source_rgb(cr, 0.92, 0.94, 0.96); /* a white needle */
		cairo_set_line_width(cr, 2);
		line(cr, x, y, tip_x, tip_y);
		circle(cr, x, y, 4);
		set_colour(cr, metal_mid);
		cairo_fill(cr);
	}
}

/* ---- standby gauges ----
 *
 * Four small round instruments in the gaps beside the big dials. A real
 * cockpit keeps small mechanical "standby" instruments in case the HUD
 * fails; these show the same simulated flight as the HUD. Like the dials,
 * the face is cached and only the needle is drawn each frame, and they
 * only appear on a wide screen.
 */
#define GAUGE_COUNT 4
#define GAUGE_RADIUS 16

enum { GAUGE_SPEED, GAUGE_ALTITUDE, GAUGE_HEADING, GAUGE_CLIMB };
static const char *const gauge_names[GAUGE_COUNT] = { "SPD", "ALT", "HDG", "VS" };

/* Centre of gauge `index`, or false if there is no room. They sit level
 * with the gap between the upper and lower dial, where the round dials
 * leave the most space: 0 and 1 left of centre, 2 and 3 right. */
static bool gauge_position(const struct layout *l, int index,
		double *x, double *y) {
	double dial_x, dial_y;
	if (!dial_position(l, 0, &dial_x, &dial_y)) {
		return false;
	}
	/* Just clear of the radar's frame, and of the seam in the panel. */
	double outer = l->radar_x + SCREEN_SIZE / 2.0 + 15 + GAUGE_RADIUS + 5;
	double inner = l->data_x - 34 - GAUGE_RADIUS - 5;
	if (inner - dial_x < 45) {
		return false; /* it would touch the big dials */
	}
	double from_left = (index == 0 || index == 3) ? outer : inner;
	*x = index < 2 ? from_left : l->width - from_left;
	*y = l->scope_y + 1;
	return true;
}

static void draw_gauge_face(cairo_t *cr, const char *name, double x, double y) {
	circle(cr, x, y, GAUGE_RADIUS + 3);
	set_shading(cr, y - GAUGE_RADIUS - 3, y + GAUGE_RADIUS + 3, metal_light, metal_dark);
	cairo_fill(cr);
	circle(cr, x, y, GAUGE_RADIUS);
	set_colour(cr, glass_black);
	cairo_fill(cr);

	/* Twelve ticks like a clock face, longer at the quarters. */
	cairo_set_source_rgb(cr, PLACARD);
	cairo_set_line_width(cr, 1);
	for (int hour = 0; hour < 12; hour++) {
		double x1, y1, x2, y2;
		polar(x, y, hour * 30, GAUGE_RADIUS - (hour % 3 == 0 ? 4.5 : 2.5), &x1, &y1);
		polar(x, y, hour * 30, GAUGE_RADIUS - 1, &x2, &y2);
		line(cr, x1, y1, x2, y2);
	}
	/* The name goes on the panel: the needle covers every part of the face
	 * at some time. */
	cairo_set_font_size(cr, FONT_SIZE(7));
	text_centred(cr, name, x, y + GAUGE_RADIUS + 10);
}

/* Which way each needle points (a bearing: degrees clockwise from "up"). */
static double gauge_bearing(int index, const struct flight *f) {
	switch (index) {
	case GAUGE_SPEED:
		return f->speed / 600.0 * 360;   /* once round is 600 knots */
	case GAUGE_ALTITUDE:
		/* Like the long hand of a real altimeter: once round per 1000
		 * feet, so it shows the last three digits. */
		return (f->altitude % 1000) / 1000.0 * 360;
	case GAUGE_HEADING:
		return f->heading;               /* a compass: up is north */
	default:
		/* Vertical speed: level flight points left, a climb swings the
		 * needle up and a descent down; +-2000 feet/min is half a turn. */
		return 270 + f->climb / 2000.0 * 180;
	}
}

static void draw_gauge_needles(cairo_t *cr, const struct layout *l,
		const struct flight *f, double t, double age) {
	for (int i = 0; i < GAUGE_COUNT; i++) {
		double x, y, tip_x, tip_y;
		if (!gauge_position(l, i, &x, &y)) {
			return;
		}
		polar(x, y, gauge_bearing(i, f) + startup_gauge(age) +
			needle_shake(t, i + 7), GAUGE_RADIUS - 5, &tip_x, &tip_y);
		cairo_set_source_rgb(cr, 0.92, 0.94, 0.96);
		cairo_set_line_width(cr, 1.5);
		line(cr, x, y, tip_x, tip_y);
		circle(cr, x, y, 2);
		set_colour(cr, metal_mid);
		cairo_fill(cr);
	}
}

/* ---- the cockpit: everything that never changes ---- */

/* One square screen with a round scope in it. */
static void draw_screen(cairo_t *cr, const char *title, double cx, double cy,
		bool rwr) {
	double half = SCREEN_SIZE / 2.0;

	/* The well it is set back into. */
	rounded_rect(cr, cx - half - 15, cy - half - 15, SCREEN_SIZE + 30,
		SCREEN_SIZE + 30, 10);
	set_colour(cr, metal_dark);
	cairo_fill(cr);

	/* Bezel: a metal frame with a row of small buttons above and below,
	 * like the soft keys round a real multi-function display. */
	rounded_rect(cr, cx - half - 11, cy - half - 11, SCREEN_SIZE + 22,
		SCREEN_SIZE + 22, 9);
	set_shading(cr, cy - half - 11, cy + half + 11, bezel_top, bezel_low);
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
	cairo_set_font_size(cr, FONT_SIZE(11));
	cairo_set_source_rgb(cr, HUD_GREEN);
	draw_text(cr, title, cx - half + 5, cy - half + 10);
}

/* The hood over the panel covers this band, measured down from the
 * panel's top edge: it sticks up 8 units above the edge and hangs 12
 * below it. */
#define HOOD_TOP (-8)
#define HOOD_BOTTOM 12

/* Make the line of the panel's top edge the current path, moved down by
 * `down` units. The edge is highest in the middle, where the HUD stands,
 * and falls away to each side. The panel, the hood and the hood's shadow
 * all follow this one curve. */
static void glareshield_curve(cairo_t *cr, const struct layout *l, double down) {
	double w = l->width, top = l->panel_top + down;
	cairo_new_path(cr);
	cairo_move_to(cr, 0, top + 16);
	cairo_curve_to(cr, w * 0.25, top + 12, w * 0.30, top - 8, l->cx, top - 8);
	cairo_curve_to(cr, w * 0.70, top - 8, w * 0.75, top + 12, w, top + 16);
}

/* While the cockpit powers up: shade the panel, and black out the screens
 * that are not on yet (what is always on them is in the cached picture). */
static void draw_blackout(cairo_t *cr, const struct layout *l, double age) {
	if (age >= STARTUP_LENGTH) {
		return;
	}
	const double half = SCREEN_SIZE / 2.0;
	cairo_set_source_rgb(cr, glass_black.r, glass_black.g, glass_black.b);
	if (!startup_lit(age, STARTUP_RADAR)) {
		cairo_rectangle(cr, l->radar_x - half, l->scope_y - half,
			SCREEN_SIZE, SCREEN_SIZE);
		cairo_fill(cr);
	}
	if (!startup_lit(age, STARTUP_RWR)) {
		cairo_rectangle(cr, l->rwr_x - half, l->scope_y - half,
			SCREEN_SIZE, SCREEN_SIZE);
		cairo_fill(cr);
	}
	if (startup_testing(age)) { /* the self test has the glass to itself */
		cairo_rectangle(cr, l->data_x, l->data_y, l->data_w, l->data_h);
		cairo_fill(cr);
	}
	glareshield_curve(cr, l, 0);
	/* Past the screen's edges, as head movement shifts all this. */
	cairo_line_to(cr, l->width + 10, l->panel_top + 16);
	cairo_line_to(cr, l->width + 10, DESIGN_HEIGHT + 10);
	cairo_line_to(cr, -10, DESIGN_HEIGHT + 10);
	cairo_line_to(cr, -10, l->panel_top + 16);
	cairo_close_path(cr);
	cairo_set_source_rgba(cr, 0, 0, 0, startup_darkness(age));
	cairo_fill(cr);
}

/* ---- panel details ----
 *
 * Small fittings that make the panel look built rather than drawn: plates
 * screwed onto it, quick-release fasteners, caution lights, switches and
 * the ejection handle. None of it moves, so all of it is drawn once, into
 * the cached cockpit, and costs nothing per frame.
 */

/* A quick-release fastener (a "Dzus"): a domed head with a slot, turned
 * by a quarter turn to lock a panel on. `angle` turns the slot, so a row
 * of them doesn't look stamped out. */
static void draw_fastener(cairo_t *cr, double x, double y, double angle) {
	circle(cr, x, y, 3.2);
	set_shading(cr, y - 3.2, y + 3.2, metal_light, metal_dark);
	cairo_fill_preserve(cr);
	set_colour(cr, metal_black);
	cairo_set_line_width(cr, 0.8);
	cairo_stroke(cr);
	double dx = 2.3 * cos(angle * DEGREES), dy = 2.3 * sin(angle * DEGREES);
	cairo_set_line_width(cr, 1.1);
	line(cr, x - dx, y - dy, x + dx, y + dy);
}

/* A plate screwed onto the panel. It stands slightly proud, so its top
 * and left edges catch the light and its bottom and right edges are in
 * shade: a bevel, drawn as one light and one dark line. A fastener holds
 * each corner. */
static void draw_plate(cairo_t *cr, double x, double y, double w, double h) {
	rounded_rect(cr, x, y, w, h, 4);
	set_shading(cr, y, y + h, (struct colour){ 0.215, 0.228, 0.245 },
		(struct colour){ 0.15, 0.158, 0.17 });
	cairo_fill(cr);
	cairo_set_line_width(cr, 1);
	set_colour(cr, metal_mid);
	cairo_move_to(cr, x + 0.5, y + h - 4);
	cairo_line_to(cr, x + 0.5, y + 0.5);
	cairo_line_to(cr, x + w - 4, y + 0.5);
	cairo_stroke(cr);
	set_colour(cr, metal_black);
	cairo_move_to(cr, x + w - 0.5, y + 4);
	cairo_line_to(cr, x + w - 0.5, y + h - 0.5);
	cairo_line_to(cr, x + 4, y + h - 0.5);
	cairo_stroke(cr);
	for (int i = 0; i < 4; i++) {
		draw_fastener(cr, i % 2 == 0 ? x + 6 : x + w - 6,
			i < 2 ? y + 6 : y + h - 6, 25 + 50 * i);
	}
}

/* One caution light: a small window that lights amber, with what it is
 * about written on it. Unlit, the writing is just visible through the
 * dark glass, as on a real one. */
static void draw_caution_light(cairo_t *cr, const char *name,
		double x, double y, double w, double h, bool lit) {
	rounded_rect(cr, x, y, w, h, 1.5);
	set_colour(cr, metal_black);
	cairo_fill(cr);
	rounded_rect(cr, x + 1.2, y + 1.2, w - 2.4, h - 2.4, 1);
	if (lit) {
		cairo_set_source_rgb(cr, CAUTION_AMBER);
	} else {
		cairo_set_source_rgb(cr, 0.10, 0.075, 0.03);
	}
	cairo_fill(cr);
	if (lit) {
		/* A faint glow spilling onto the plate round it. */
		rounded_rect(cr, x - 1, y - 1, w + 2, h + 2, 2.5);
		cairo_set_source_rgba(cr, CAUTION_AMBER, 0.25);
		cairo_set_line_width(cr, 2.5);
		cairo_stroke(cr);
		cairo_set_source_rgb(cr, 0.16, 0.08, 0.0);
	} else {
		cairo_set_source_rgb(cr, 0.36, 0.27, 0.12);
	}
	cairo_set_font_size(cr, FONT_SIZE(6.5));
	text_centred(cr, name, x + w / 2, y + h / 2);
}

/* A toggle switch seen from the front: its mounting nut, and the lever
 * leaning up or down. A guarded switch (one that must not be flipped by
 * accident) has a red cover hinged above it, lifted open here, and two
 * red rails either side. `on` and `off` name the two positions. */
static void draw_toggle(cairo_t *cr, double x, double y, const char *name,
		const char *on, const char *off, bool up, bool guarded) {
	cairo_set_source_rgb(cr, PLACARD);
	cairo_set_font_size(cr, FONT_SIZE(6.5));
	text_centred(cr, name, x, y - 25);
	cairo_set_font_size(cr, FONT_SIZE(5));
	text_centred(cr, on, x + 17, y - 9);
	text_centred(cr, off, x + 17, y + 9);

	if (guarded) {
		cairo_set_source_rgb(cr, 0.62, 0.08, 0.06);
		rounded_rect(cr, x - 8, y - 13, 3, 24, 1);
		rounded_rect(cr, x + 5, y - 13, 3, 24, 1);
		rounded_rect(cr, x - 8, y - 17, 16, 4, 1.5); /* the lid, edge-on */
		cairo_fill(cr);
		/* Yellow and black stripes on the lid. */
		cairo_set_source_rgb(cr, 0.95, 0.78, 0.10);
		for (int i = 0; i < 3; i++) {
			cairo_rectangle(cr, x - 5.5 + i * 4.5, y - 16.5, 2, 3);
		}
		cairo_fill(cr);
	}

	/* The nut: six-sided, lighter on its upper faces. */
	cairo_new_path(cr);
	for (int i = 0; i < 6; i++) {
		double a = (60 * i + 30) * DEGREES;
		cairo_line_to(cr, x + 5 * cos(a), y + 5 * sin(a));
	}
	cairo_close_path(cr);
	set_shading(cr, y - 5, y + 5, metal_light, metal_dark);
	cairo_fill(cr);
	circle(cr, x, y, 2.4);
	set_colour(cr, metal_black);
	cairo_fill(cr);

	/* The lever: a tapering bat with a rounded tip. */
	double tip = up ? y - 11 : y + 11;
	cairo_move_to(cr, x - 1.7, y);
	cairo_line_to(cr, x - 1.1, tip);
	cairo_line_to(cr, x + 1.1, tip);
	cairo_line_to(cr, x + 1.7, y);
	cairo_close_path(cr);
	set_shading(cr, fmin(y, tip), fmax(y, tip), metal_light, metal_mid);
	cairo_fill(cr);
	circle(cr, x, tip, 2);
	set_colour(cr, metal_light);
	cairo_fill(cr);
}

/* The consoles: on a screen too narrow for even small dials, the
 * strip between each side screen and the centre display gets a plate of
 * caution lights and a plate of switches instead. Returns false if this
 * screen has the dials, or no room even for this. x and w say where the
 * left console goes; the right one is its mirror image. */
static bool console_position(const struct layout *l, double *x, double *w) {
	double dial_x, dial_y;
	if (dial_position(l, 0, &dial_x, &dial_y)) {
		return false;
	}
	double left_edge = l->radar_x + SCREEN_SIZE / 2.0 + 11;
	double gap = (l->data_x - 30) - left_edge;
	if (gap < 90) {
		return false;
	}
	*w = fmin(gap - 8, 120);
	*x = left_edge + (gap - *w) / 2;
	return true;
}

static void draw_consoles(cairo_t *cr, const struct layout *l) {
	double x, w;
	if (!console_position(l, &x, &w)) {
		return;
	}
	/* Two plates per side, level with the side screens' glass. */
	double top = l->scope_y - SCREEN_SIZE / 2.0 - 8;
	double bottom = l->scope_y + SCREEN_SIZE / 2.0 + 8;
	double split = top + 88;

	for (int side = 0; side < 2; side++) {
		double px = side == 0 ? x : l->width - x - w;
		draw_plate(cr, px, top, w, split - top);
		draw_plate(cr, px, split + 6, w, bottom - split - 6);

		cairo_set_source_rgb(cr, PLACARD);
		cairo_set_font_size(cr, FONT_SIZE(6));
		if (side == 0) {
			/* Left: the caution panel, two columns of four. One
			 * light is on, as there usually is one on somewhere. */
			static const char *const names[8] = {
				"ENG", "HYD", "FUEL", "OXY",
				"GEN", "ECS", "FLCS", "BRK",
			};
			text_centred(cr, "CAUTION", px + w / 2, top + 10);
			double cell_w = (w - 20) / 2, cell_h = 13;
			for (int i = 0; i < 8; i++) {
				double cx = px + 8 + (i % 2) * (cell_w + 4);
				double cy = top + 19 + (i / 2) * (cell_h + 3);
				draw_caution_light(cr, names[i], cx, cy,
					cell_w, cell_h, i == 5);
			}
		} else {
			/* Right: the master warning and master caution lights,
			 * the two that light first whatever the trouble, and
			 * below them the three landing gear lights (dark: the
			 * gear is up). */
			text_centred(cr, "MASTER", px + w / 2, top + 10);
			double light_w = (w - 20) / 2;
			draw_caution_light(cr, "WARN", px + 8, top + 18,
				light_w, 24, false);
			draw_caution_light(cr, "CAUTION", px + 12 + light_w,
				top + 18, light_w, 24, true);
			/* The warning light is red, not amber, when unlit too. */
			rounded_rect(cr, px + 9.2, top + 19.2, light_w - 2.4, 21.6, 1);
			cairo_set_source_rgba(cr, 0.5, 0.0, 0.0, 0.35);
			cairo_fill(cr);
			cairo_set_source_rgb(cr, PLACARD);
			text_centred(cr, "GEAR", px + w / 2, top + 43);
			static const char *const wheels[3] = { "L", "N", "R" };
			for (int i = 0; i < 3; i++) {
				double gx = px + w / 2 + (i - 1) * 22, gy = top + 70;
				circle(cr, gx, gy, 6.5);
				set_colour(cr, metal_black);
				cairo_fill(cr);
				circle(cr, gx, gy, 5);
				cairo_set_source_rgb(cr, HUD_FAINT);
				cairo_fill(cr);
				cairo_set_source_rgba(cr, 1, 1, 1, 0.25);
				circle(cr, gx - 1.5, gy - 1.5, 1.4); /* a glint */
				cairo_fill(cr);
				cairo_set_source_rgb(cr, PLACARD);
				cairo_set_font_size(cr, FONT_SIZE(5));
				text_centred(cr, wheels[i], gx, gy + 11.5);
				cairo_set_font_size(cr, FONT_SIZE(6));
			}
		}

		/* The switch plate: two toggles. */
		double sy = split + 6 + (bottom - split - 6) / 2 + 9;
		double s1 = px + w / 2 - w / 4, s2 = px + w / 2 + w / 4;
		if (side == 0) {
			draw_toggle(cr, s1, sy, "MASTER ARM", "ARM", "SAFE", false, true);
			draw_toggle(cr, s2, sy, "RDR ALT", "ON", "OFF", true, false);
		} else {
			draw_toggle(cr, s1, sy, "ANTI-COLL", "ON", "OFF", true, false);
			draw_toggle(cr, s2, sy, "POSITION", "ON", "OFF", true, false);
		}
	}
}

/* The ejection seat's handle: a loop striped yellow and black so it can be
 * found without looking. It is hinged to a plate under the centre display. */
static void draw_ejection_handle(cairo_t *cr, const struct layout *l) {
	double x = l->cx - 34, y = l->panel_bottom - 25, w = 68, h = 22;
	rounded_rect(cr, x - 12, y - 1, w + 24, 11, 3);
	set_shading(cr, y - 1, y + 10, bezel_top, bezel_low);
	cairo_fill(cr);
	draw_fastener(cr, x - 6, y + 4.5, 30);
	draw_fastener(cr, x + w + 6, y + 4.5, 75);
	/* The loop is a ring: the outer rounded rectangle minus the inner
	 * one ("even-odd" fills only the area between them). Clip to it,
	 * then paint it yellow and lay black stripes across. */
	cairo_save(cr);
	cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
	rounded_rect(cr, x, y, w, h, 7);
	rounded_rect(cr, x + 6, y + 6, w - 12, h - 12, 3);
	cairo_clip_preserve(cr);
	cairo_set_source_rgb(cr, 0.95, 0.78, 0.10);
	cairo_fill(cr);
	cairo_set_source_rgb(cr, 0.06, 0.06, 0.05);
	for (double sx = x - h; sx < x + w; sx += 9) {
		cairo_move_to(cr, sx, y + h);
		cairo_line_to(cr, sx + h, y);
		cairo_line_to(cr, sx + h + 4.5, y);
		cairo_line_to(cr, sx + 4.5, y + h);
		cairo_close_path(cr);
	}
	cairo_fill(cr);
	/* Light the top and shade the rest (the loop is round in section):
	 * paint fills the whole clip, here the ring. */
	set_shading(cr, y, y + 6, (struct colour){ 1, 1, 1 }, (struct colour){ 0, 0, 0 });
	cairo_paint_with_alpha(cr, 0.18);
	cairo_restore(cr);
	cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
	rounded_rect(cr, x, y, w, h, 7);
	set_colour(cr, metal_black);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);
}

/* The canopy arch is an ellipse whose width changes: narrow overhead and
 * wide low down, where it is nearest to you. Find the point at `angle`
 * round it; `across` is -1 at its inner edge, 0 in the middle, 1 outside. */
#define SEAL_WIDTH 0.17   /* of the rubber seal, measured the same way */

static void arch_point(const struct layout *l, double angle, double across,
		double *x, double *y) {
	double side = cos(angle);
	double half_width = 21 + 15 * side * side;
	*x = l->cx + (l->width * 0.47 + across * half_width) * cos(angle);
	*y = DESIGN_HEIGHT * 1.02 + (DESIGN_HEIGHT + across * half_width) * sin(angle);
}

/* Make the part of the arch between two values of `across` the path: out
 * along one edge and back along the other. */
static void arch_band(cairo_t *cr, const struct layout *l, double from, double to) {
	const int steps = 180;
	cairo_new_path(cr);
	for (int edge = 0; edge < 2; edge++) {
		for (int i = 0; i <= steps; i++) {
			double part = (double)(edge == 0 ? i : steps - i) / steps;
			double x, y;
			arch_point(l, -M_PI * (1.05 - 1.10 * part), edge == 0 ? from : to,
				&x, &y);
			cairo_line_to(cr, x, y);
		}
	}
	cairo_close_path(cr);
}

#define MIRROR_WIDTH 80
#define MIRROR_HEIGHT 20

/* The top-left corner of the glass of mirror -1, 0 or 1. */
static void mirror_position(const struct layout *l, int which,
		double *x, double *y) {
	*x = l->cx + which * 120 - MIRROR_WIDTH / 2.0;
	*y = DESIGN_HEIGHT * 0.02 + 26 + (which == 0 ? 0 : 5);
}

/* Two knobs under the centre display, and what they and the handle are,
 * in white paint on the panel. */
#define KNOB_OUT 190      /* how far each is from the middle */
#define KNOB_RADIUS 8.5

static void draw_knobs(cairo_t *cr, const struct layout *l) {
	static const char *const names[2] = { "BRT", "CONT" };
	double y = l->panel_bottom - 14;
	cairo_set_font_size(cr, FONT_SIZE(7));
	for (int i = 0; i < 2; i++) {
		double x = l->cx + (i == 0 ? -KNOB_OUT : KNOB_OUT);
		circle(cr, x, y, KNOB_RADIUS);
		set_shading(cr, y - KNOB_RADIUS, y + KNOB_RADIUS, metal_dark, metal_black);
		cairo_fill(cr);
		/* Ridges round the edge for grip, then the white pointer. */
		set_colour(cr, metal_mid);
		cairo_set_line_width(cr, 0.8);
		for (int ridge = 0; ridge < 18; ridge++) {
			double x1, y1, x2, y2;
			polar(x, y, ridge * 20, KNOB_RADIUS - 1.8, &x1, &y1);
			polar(x, y, ridge * 20, KNOB_RADIUS, &x2, &y2);
			line(cr, x1, y1, x2, y2);
		}
		double px, py;
		polar(x, y, i == 0 ? 40 : -25, KNOB_RADIUS - 1.5, &px, &py);
		cairo_set_source_rgb(cr, PLACARD);
		cairo_set_line_width(cr, 1.4);
		line(cr, x, y, px, py);
		text_centred(cr, names[i], x + (i == 0 ? 25 : -27), y);
	}
	text_centred(cr, "EJECTION SEAT", l->cx - 90, y);
	text_centred(cr, "PULL TO EJECT", l->cx + 90, y);
}

/* Paint rubbed off the frame by fingers reaching for the buttons: small
 * specks of bare metal, thickest close to each button. */
static void draw_key_wear(cairo_t *cr, const struct layout *l) {
	for (int slot = 0; slot < PAGE_KEYS; slot++) {
		double x, y, lx, ly;
		key_position(l, slot, &x, &y, &lx, &ly);
		x += OSB_WIDTH / 2.0;
		y += OSB_HEIGHT / 2.0;
		for (int i = 0; i < 26; i++) {
			/* Scattered but always the same, as in draw_scratches(). */
			double a = fmod(i * 137.5 + slot * 53, 360) * DEGREES;
			double out = 1.06 + 0.28 * fmod(i * 0.618 + slot * 0.37, 1.0);
			circle(cr, x + cos(a) * OSB_WIDTH / 2 * out * 1.12,
				y + sin(a) * OSB_HEIGHT / 2 * out * 1.25,
				0.35 + 0.5 * fmod(i * 0.414, 1.0));
			cairo_set_source_rgba(cr, 0.55, 0.57, 0.59, 0.45 * (1.5 - out));
			cairo_fill(cr);
		}
	}
}

static void draw_cockpit(struct scene *scene, cairo_t *cr) {
	struct layout l = layout_for(scene);
	double w = l.width, h = DESIGN_HEIGHT;

	cairo_select_font_face(cr, UI_FONT, CAIRO_FONT_SLANT_NORMAL,
		CAIRO_FONT_WEIGHT_NORMAL);

	/* Canopy arch: the hoop of the windscreen frame, rising from either
	 * side of the panel and meeting overhead. A dark edge, a lighter face,
	 * and a thin highlight on its inner rim; then the black rubber seal
	 * that the glass sits in. */
	arch_band(cr, &l, -1, 1);
	set_colour(cr, metal_black);
	cairo_fill(cr);
	arch_band(cr, &l, -0.84, 0.84);
	set_colour(cr, metal_dark);
	cairo_fill(cr);
	arch_band(cr, &l, -0.24, 0.58);
	set_colour(cr, metal_mid);
	cairo_fill(cr);
	/* White cloud and sky light its inner side. */
	for (int i = 0; i < 4; i++) {
		arch_band(cr, &l, -0.95, -0.75 + 0.22 * i);
		cairo_set_source_rgba(cr, 0.70, 0.84, 1.0, 0.055);
		cairo_fill(cr);
	}
	arch_band(cr, &l, -0.80, -0.73);
	set_colour(cr, metal_light);
	cairo_fill(cr);
	arch_band(cr, &l, -1.0 - SEAL_WIDTH, -0.98);
	cairo_set_source_rgb(cr, 0.035, 0.035, 0.04);
	cairo_fill(cr);

	/* Bolts along the arch, every 6 degrees. */
	for (int i = -13; i <= 13; i++) {
		double bx, by;
		arch_point(&l, (i * 6 - 90) * DEGREES, 0.26, &bx, &by);
		circle(cr, bx, by, 2.2);
		set_colour(cr, metal_light);
		cairo_fill(cr);
		circle(cr, bx + 0.6, by + 0.6, 1.0);
		set_colour(cr, metal_black);
		cairo_fill(cr);
	}

	/* Three rear-view mirrors hanging inside the top of the arch. Only
	 * their frames: what they show is drawn by draw_mirrors(). */
	for (int i = -1; i <= 1; i++) {
		double mx, my;
		mirror_position(&l, i, &mx, &my);
		rounded_rect(cr, mx - 4, my - 3, MIRROR_WIDTH + 8, MIRROR_HEIGHT + 6, 7);
		set_colour(cr, metal_black);
		cairo_fill(cr);
	}

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

	/* HUD: a pane of glass in a thin metal frame, held at its lower corners
	 * by two brackets. The glass itself is not filled in here (you
	 * look through it; draw_hud() tints it), only its edge: thick glass
	 * seen edge-on is pale green. Drawn twice along the top to show the
	 * thickness of the pane. */
	hud_glass_path(cr, &l);
	set_colour(cr, metal_dark);
	cairo_set_line_width(cr, 3.6);
	cairo_stroke(cr);
	hud_glass_path(cr, &l);
	cairo_set_source_rgba(cr, 0.62, 0.90, 0.76, 0.75);
	cairo_set_line_width(cr, 1.0);
	cairo_stroke(cr);
	cairo_set_source_rgba(cr, 0.62, 0.90, 0.76, 0.40);
	line(cr, l.hud_x + HUD_TAPER + 14, l.hud_y + 3.5,
		l.hud_x + l.hud_w - HUD_TAPER - 14, l.hud_y + 3.5);

	/* The brackets: each rises from the glareshield beside the glass to
	 * about 40% of its height, following the slope of the glass, wide at
	 * the foot and narrow at the top. */
	double glass_bottom = l.hud_y + l.hud_h;
	double bracket_top = glass_bottom - l.hud_h * 0.40;
	for (int side = -1; side <= 1; side += 2) {
		/* Work on the left one; `side` mirrors x for the right one. */
		double foot = l.hud_x, top = hud_left_at(&l, bracket_top);
#define MIRROR(x) (l.cx + side * (l.cx - (x)))
		cairo_move_to(cr, MIRROR(foot - 17), l.panel_top + 4);
		cairo_line_to(cr, MIRROR(top - 7), bracket_top);
		cairo_line_to(cr, MIRROR(top + 4), bracket_top);
		cairo_line_to(cr, MIRROR(foot + 9), glass_bottom - 20);
		cairo_line_to(cr, MIRROR(foot + 9), l.panel_top + 4);
		cairo_close_path(cr);
		set_shading(cr, bracket_top, l.panel_top, metal_mid, metal_black);
		cairo_fill(cr);
		/* The outer edge catches the light. */
		set_colour(cr, metal_light);
		cairo_set_line_width(cr, 1.2);
		line(cr, MIRROR(foot - 17), l.panel_top + 4, MIRROR(top - 7), bracket_top);
		/* Two bolts clamp the glass. */
		for (int bolt = 0; bolt < 2; bolt++) {
			double y = bracket_top + 22 + bolt * 44;
			circle(cr, MIRROR(hud_left_at(&l, y) - 1), y, 2.4);
			set_colour(cr, metal_light);
			cairo_fill(cr);
		}
#undef MIRROR
	}

	/* Instrument panel. Its top edge (the glareshield) rises in the
	 * middle, where the HUD sits. */
	glareshield_curve(cr, &l, 0);
	cairo_line_to(cr, w, h);
	cairo_line_to(cr, 0, h);
	cairo_close_path(cr);
	set_shading(cr, l.panel_top - 8, h,
		panel_top_grey, panel_low_grey);
	cairo_fill(cr);

	/* The hood: a padded black roll along the top of the panel that
	 * overhangs the instruments and keeps the sun off them. It is one
	 * thick stroke along the panel's top edge, so it sticks up above the
	 * edge as well as hanging below it. */
	glareshield_curve(cr, &l, (HOOD_TOP + HOOD_BOTTOM) / 2.0);
	set_shading(cr, l.panel_top + HOOD_TOP - 8, l.panel_top + HOOD_BOTTOM + 16,
		(struct colour){ 0.15, 0.155, 0.17 }, (struct colour){ 0.02, 0.02, 0.025 });
	cairo_set_line_width(cr, HOOD_BOTTOM - HOOD_TOP);
	cairo_stroke(cr);
	/* Its top catches the light from the sky. */
	glareshield_curve(cr, &l, HOOD_TOP + 1);
	set_colour(cr, metal_light);
	cairo_set_line_width(cr, 1.5);
	cairo_stroke(cr);
	/* A row of stitching along the padding. */
	glareshield_curve(cr, &l, HOOD_TOP + 7);
	const double stitches[] = {5, 4};
	cairo_set_dash(cr, stitches, 2, 0);
	cairo_set_source_rgba(cr, 1, 1, 1, 0.16);
	cairo_set_line_width(cr, 1);
	cairo_stroke(cr);
	cairo_set_dash(cr, NULL, 0, 0);

	/* The HUD's projector: a lens lying in front of the glass, and the
	 * housing before it, with a window for the status line. */
	ellipse(cr, l.cx, l.panel_top - 21, 70, 9);
	set_colour(cr, metal_dark);
	cairo_fill(cr);
	ellipse(cr, l.cx, l.panel_top - 22, 64, 6.5);
	set_shading(cr, l.panel_top - 29, l.panel_top - 15,
		(struct colour){ 0.30, 0.62, 0.50 }, (struct colour){ 0.06, 0.05, 0.14 });
	cairo_fill(cr);
	rounded_rect(cr, l.cx - 78, l.panel_top - 20, 156, 34, 6);
	set_shading(cr, l.panel_top - 20, l.panel_top + 14, metal_mid, metal_black);
	cairo_fill(cr);
	rounded_rect(cr, l.cx - 52, l.panel_top - 13, 104, 12, 4);
	set_colour(cr, glass_black);
	cairo_fill(cr);

	/* Seams where the panel is made of separate sections, and the dials
	 * that sit in the outer sections on a wide screen. */
	double dial_x, dial_y;
	if (dial_position(&l, 0, &dial_x, &dial_y)) {
		for (int side = -1; side <= 1; side += 2) {
			double seam = l.cx + side * (l.cx - l.data_x + 34);
			set_colour(cr, metal_black);
			cairo_set_line_width(cr, 2);
			line(cr, seam, l.panel_top + 30, seam, l.panel_bottom);
			cairo_set_source_rgba(cr, 1, 1, 1, 0.10);
			cairo_set_line_width(cr, 1);
			line(cr, seam + 1.5, l.panel_top + 30, seam + 1.5, l.panel_bottom);
		}
		for (int i = 0; i < DIAL_COUNT; i++) {
			dial_position(&l, i, &dial_x, &dial_y);
			draw_dial_face(cr, &dials[i], dial_x, dial_y, dial_radius(&l));
		}
		for (int i = 0; i < GAUGE_COUNT; i++) {
			if (gauge_position(&l, i, &dial_x, &dial_y)) {
				draw_gauge_face(cr, gauge_names[i], dial_x, dial_y);
			}
		}
	} else {
		draw_consoles(cr, &l);
	}

	/* The panel's outer sections are held on by a column of fasteners
	 * down each edge, outside the side screens. */
	for (int i = 0; ; i++) {
		double y = l.panel_top + 62 + i * 46;
		if (y > l.panel_bottom - 8) {
			break;
		}
		draw_fastener(cr, 19, y, 20 + 35 * i);
		draw_fastener(cr, w - 19, y, 70 + 35 * i);
	}

	draw_screen(cr, "RADAR", l.radar_x, l.scope_y, false);
	draw_screen(cr, "RWR", l.rwr_x, l.scope_y, true);

	/* The centre display: a wide screen in a metal frame, deep enough at
	 * the top and bottom for a row of push buttons. Same style of frame
	 * as the radar and RWR screens. */
	double frame_top = l.data_y - 26, frame_bottom = l.data_y + l.data_h + 26;
	rounded_rect(cr, l.data_x - 12, frame_top, l.data_w + 24,
		frame_bottom - frame_top, 9);
	set_shading(cr, frame_top, frame_bottom, bezel_top, bezel_low);
	cairo_fill_preserve(cr);
	set_colour(cr, metal_black);
	cairo_set_line_width(cr, 1.5);
	cairo_stroke(cr);
	set_colour(cr, metal_light);
	cairo_set_line_width(cr, 1);
	line(cr, l.data_x - 4, frame_top + 1, l.data_x + l.data_w + 4, frame_top + 1);

	cairo_rectangle(cr, l.data_x, l.data_y, l.data_w, l.data_h);
	set_colour(cr, glass_black);
	cairo_fill(cr);
	/* Faint rules that set the button names off from the data. */
	cairo_set_source_rgb(cr, HUD_FAINT);
	line(cr, l.data_x + 6, l.data_y + 24, l.data_x + l.data_w - 6, l.data_y + 24);
	line(cr, l.data_x + 6, l.data_y + l.data_h - 24,
		l.data_x + l.data_w - 6, l.data_y + l.data_h - 24);

	for (int slot = 0; slot < PAGE_KEYS; slot++) {
		draw_key(cr, &l, slot, KEY_RESTING);
	}

	/* A screw in each corner of the centre panel. */
	set_colour(cr, metal_light);
	for (int i = 0; i < 4; i++) {
		double x = (i % 2 == 0) ? l.data_x - 22 : l.data_x + l.data_w + 22;
		double y = (i < 2) ? frame_top + 6 : frame_bottom - 6;
		circle(cr, x, y, 3);
		cairo_fill(cr);
	}

	draw_ejection_handle(cr, &l);
	draw_knobs(cr, &l);
	draw_key_wear(cr, &l);

	/* The hood's shadow, last, so it falls across everything on the panel.
	 * Fourteen see-through strokes hang from the hood, each deeper than the
	 * last. All cover the part right under it, so that is darkest, and only
	 * the deepest reaches the soft lower edge. */
	for (int i = 1; i <= 14; i++) {
		double depth = 3.0 * i;
		glareshield_curve(cr, &l, HOOD_BOTTOM + depth / 2);
		cairo_set_source_rgba(cr, 0, 0, 0, 0.062);
		cairo_set_line_width(cr, depth);
		cairo_stroke(cr);
	}
}

/* ---- the cockpit's relief ----
 *
 * The cockpit again, as heights instead of colours, for surface.c to light.
 * It has to follow draw_cockpit(): a part that moves there must move here.
 * Heights are 0 to 1: the panel is at PANEL_LEVEL, things standing up from
 * it are higher and things sunk into it lower.
 */
#define PANEL_LEVEL 0.40

/* Set the height to draw with, how grainy the metal is (0 for glass), and
 * how leathery. */
static void set_relief(cairo_t *cr, double height, double grain, double leather) {
	cairo_set_source_rgb(cr, height, grain, leather);
}

/* A domed disc, for the heads of bolts: highest in the middle. */
static void relief_dome(cairo_t *cr, double x, double y, double r,
		double base, double rise) {
	for (int i = 0; i < 6; i++) {
		double out = 1 - i / 6.0; /* 1 at the rim */
		circle(cr, x, y, r * out);
		set_relief(cr, base + rise * sqrt(1 - out * out * 0.97), 0.7, 0);
		cairo_fill(cr);
	}
}

/* A ring that is round in section, like a tyre: from radius r1 to r2. */
static void relief_ring(cairo_t *cr, double x, double y, double r1, double r2,
		double base, double rise) {
	for (int i = 0; i < 8; i++) {
		double out = 1 - i / 8.0;
		circle(cr, x, y, (r1 + r2) / 2);
		cairo_set_line_width(cr, (r2 - r1) * out);
		set_relief(cr, base + rise * sqrt(1 - out * out * 0.97), 0.8, 0);
		cairo_stroke(cr);
	}
}

/* A block whose edges slope over `bevel` units from height h0 up (or down)
 * to h1, with the slope inside the rectangle. */
static void relief_block(cairo_t *cr, double x, double y, double w, double h,
		double corner, double bevel, double h0, double h1, double grain) {
	for (int i = 0; i <= 8; i++) {
		double in = bevel * i / 8;
		rounded_rect(cr, x + in, y + in, w - 2 * in, h - 2 * in,
			fmax(corner - in, 0.5));
		set_relief(cr, h0 + (h1 - h0) * sin(i / 8.0 * M_PI / 2), grain, 0);
		cairo_fill(cr);
	}
}

/* Stroke the current path as a tube of the given width. */
static void relief_tube(cairo_t *cr, double width, double base, double rise,
		double grain, double leather) {
	for (int i = 0; i < 24; i++) {
		double out = 1 - i / 24.0;
		cairo_set_line_width(cr, width * out);
		set_relief(cr, base + rise * sqrt(1 - out * out * 0.99), grain, leather);
		cairo_stroke_preserve(cr);
	}
	cairo_new_path(cr);
}

static void relief_plate(cairo_t *cr, double x, double y, double w, double h) {
	relief_block(cr, x, y, w, h, 4, 2, PANEL_LEVEL, PANEL_LEVEL + 0.06, 1);
	for (int i = 0; i < 4; i++) {
		relief_dome(cr, i % 2 == 0 ? x + 6 : x + w - 6,
			i < 2 ? y + 6 : y + h - 6, 3.4, PANEL_LEVEL + 0.06, 0.05);
	}
}

static void relief_screen(cairo_t *cr, double cx, double cy) {
	double half = SCREEN_SIZE / 2.0;
	relief_block(cr, cx - half - 15, cy - half - 15, SCREEN_SIZE + 30,
		SCREEN_SIZE + 30, 10, 3, PANEL_LEVEL, PANEL_LEVEL - 0.14, 0.9);
	relief_block(cr, cx - half - 11, cy - half - 11, SCREEN_SIZE + 22,
		SCREEN_SIZE + 22, 9, 5, PANEL_LEVEL - 0.14, PANEL_LEVEL + 0.17, 0.9);
	for (int i = 0; i < 5; i++) {
		double x = cx - half + 14 + i * 33;
		relief_block(cr, x, cy - half - 8, 16, 5, 1, 1.5,
			PANEL_LEVEL + 0.15, PANEL_LEVEL + 0.22, 0.6);
		relief_block(cr, x, cy + half + 3, 16, 5, 1, 1.5,
			PANEL_LEVEL + 0.15, PANEL_LEVEL + 0.22, 0.6);
	}
	relief_block(cr, cx - half - 2, cy - half - 2, SCREEN_SIZE + 4,
		SCREEN_SIZE + 4, 3, 3, PANEL_LEVEL + 0.15, PANEL_LEVEL - 0.16, 0);
}

static void draw_relief(cairo_t *cr, const struct layout *l) {
	double w = l->width, h = DESIGN_HEIGHT;

	/* The seal, then the canopy arch: a tube. */
	arch_band(cr, l, -1.0 - SEAL_WIDTH, -0.9);
	set_relief(cr, 0.10, 0, 0.3);
	cairo_fill(cr);
	for (int i = 0; i < 24; i++) {
		double out = 1 - i / 24.0;
		arch_band(cr, l, -out, out);
		set_relief(cr, 0.10 + 0.48 * sqrt(1 - out * out * 0.99), 0.9, 0);
		cairo_fill(cr);
	}
	for (int i = -13; i <= 13; i++) {
		double bx, by;
		arch_point(l, (i * 6 - 90) * DEGREES, 0.26, &bx, &by);
		relief_dome(cr, bx, by, 2.8, 0.55, 0.06);
	}
	for (int i = -1; i <= 1; i++) {
		double mx, my;
		mirror_position(l, i, &mx, &my);
		relief_block(cr, mx - 4, my - 3, MIRROR_WIDTH + 8, MIRROR_HEIGHT + 6,
			7, 5, 0.45, 0.72, 0.6);
	}

	/* The sills: rolled metal, highest along their top edge and falling
	 * away towards the outside. */
	for (int side = -1; side <= 1; side += 2) {
		double edge = side < 0 ? 0 : w;
		for (int i = 0; i <= 96; i++) {
			double in = 40.0 * i / 96; /* how far in from the top edge */
			double rise = sin(fmin(i / 36.0, 1) * M_PI / 2) -
				0.30 * fmax(i - 36, 0) / 60.0;
			cairo_move_to(cr, edge + side * 20, l->panel_top - 148 + in);
			cairo_line_to(cr, edge - side * (64 - in * 0.6), l->panel_top - 74 + in);
			cairo_line_to(cr, edge - side * (128 - in * 0.9), l->panel_top + 18 + in);
			cairo_line_to(cr, edge + side * 20, l->panel_top + 18 + in);
			cairo_close_path(cr);
			set_relief(cr, 0.12 + 0.34 * rise, 1, 0);
			cairo_fill(cr);
		}
	}

	/* The brackets that hold the HUD glass. */
	double glass_bottom = l->hud_y + l->hud_h;
	double bracket_top = glass_bottom - l->hud_h * 0.40;
	for (int side = -1; side <= 1; side += 2) {
		double foot = l->hud_x, top = hud_left_at(l, bracket_top);
#define MIRROR(x) (l->cx + side * (l->cx - (x)))
		for (int i = 0; i <= 6; i++) {
			double in = i * 1.0;
			cairo_move_to(cr, MIRROR(foot - 17 + in), l->panel_top + 4);
			cairo_line_to(cr, MIRROR(top - 7 + in), bracket_top + in);
			cairo_line_to(cr, MIRROR(top + 4 - in), bracket_top + in);
			cairo_line_to(cr, MIRROR(foot + 9 - in), glass_bottom - 20 + in);
			cairo_line_to(cr, MIRROR(foot + 9 - in), l->panel_top + 4);
			cairo_close_path(cr);
			set_relief(cr, 0.30 + 0.16 * sin(i / 6.0 * M_PI / 2), 0.9, 0);
			cairo_fill(cr);
		}
		for (int bolt = 0; bolt < 2; bolt++) {
			double y = bracket_top + 22 + bolt * 44;
			relief_dome(cr, MIRROR(hud_left_at(l, y) - 1), y, 3, 0.44, 0.08);
		}
#undef MIRROR
	}

	/* The panel. */
	glareshield_curve(cr, l, 0);
	cairo_line_to(cr, w + 40, h + 40);
	cairo_line_to(cr, -40, h + 40);
	cairo_close_path(cr);
	set_relief(cr, PANEL_LEVEL, 1, 0);
	cairo_fill(cr);

	/* The hood: a soft leather roll. */
	glareshield_curve(cr, l, (HOOD_TOP + HOOD_BOTTOM) / 2.0);
	relief_tube(cr, HOOD_BOTTOM - HOOD_TOP, PANEL_LEVEL - 0.05, 0.28, 0, 1);
	/* The stitching pulls it in. */
	glareshield_curve(cr, l, HOOD_TOP + 7);
	set_relief(cr, PANEL_LEVEL + 0.17, 0, 0.6);
	cairo_set_line_width(cr, 1.6);
	cairo_stroke(cr);

	ellipse(cr, l->cx, l->panel_top - 21, 70, 9);
	set_relief(cr, PANEL_LEVEL + 0.22, 0.6, 0);
	cairo_fill(cr);
	ellipse(cr, l->cx, l->panel_top - 22, 64, 6.5);
	set_relief(cr, PANEL_LEVEL + 0.16, 0, 0);
	cairo_fill(cr);
	relief_block(cr, l->cx - 78, l->panel_top - 20, 156, 34, 6, 6,
		PANEL_LEVEL, PANEL_LEVEL + 0.34, 0.8);
	relief_block(cr, l->cx - 54, l->panel_top - 15, 108, 16, 5, 3,
		PANEL_LEVEL + 0.34, PANEL_LEVEL + 0.18, 0);

	double x, y;
	if (dial_position(l, 0, &x, &y)) {
		for (int side = -1; side <= 1; side += 2) {
			double groove = l->cx + side * (l->cx - l->data_x + 34);
			set_relief(cr, PANEL_LEVEL - 0.10, 1, 0);
			cairo_set_line_width(cr, 3);
			line(cr, groove, l->panel_top + 30, groove, h + 40);
		}
		for (int i = 0; i < DIAL_COUNT; i++) {
			double r = dial_radius(l);
			dial_position(l, i, &x, &y);
			relief_ring(cr, x, y, r - 1, r + 7, PANEL_LEVEL - 0.01, 0.16);
			circle(cr, x, y, r);
			set_relief(cr, PANEL_LEVEL - 0.02, 0, 0);
			cairo_fill(cr);
		}
		for (int i = 0; i < GAUGE_COUNT; i++) {
			if (gauge_position(l, i, &x, &y)) {
				relief_ring(cr, x, y, GAUGE_RADIUS - 1, GAUGE_RADIUS + 4,
					PANEL_LEVEL - 0.01, 0.13);
				circle(cr, x, y, GAUGE_RADIUS);
				set_relief(cr, PANEL_LEVEL - 0.01, 0, 0);
				cairo_fill(cr);
			}
		}
	} else if (console_position(l, &x, &y)) {
		double plate_w = y;
		double top = l->scope_y - SCREEN_SIZE / 2.0 - 8;
		double bottom = l->scope_y + SCREEN_SIZE / 2.0 + 8;
		double split = top + 88;
		for (int side = 0; side < 2; side++) {
			double px = side == 0 ? x : w - x - plate_w;
			relief_plate(cr, px, top, plate_w, split - top);
			relief_plate(cr, px, split + 6, plate_w, bottom - split - 6);
			double sy = split + 6 + (bottom - split - 6) / 2 + 9;
			for (int i = -1; i <= 1; i += 2) {
				/* A switch: its nut, then the lever leaning out. Only the
				 * first, the guarded one, is down. */
				double sx = px + plate_w / 2 + i * plate_w / 4;
				bool up = !(side == 0 && i < 0);
				relief_dome(cr, sx, sy, 5, PANEL_LEVEL + 0.06, 0.08);
				cairo_move_to(cr, sx, sy);
				cairo_line_to(cr, sx, sy + (up ? -11 : 11));
				relief_tube(cr, 3.4, PANEL_LEVEL + 0.18, 0.06, 0.6, 0);
				relief_dome(cr, sx, sy + (up ? -11 : 11), 2.2,
					PANEL_LEVEL + 0.24, 0.06);
			}
		}
	}

	for (int i = 0; ; i++) {
		double fy = l->panel_top + 62 + i * 46;
		if (fy > l->panel_bottom - 8) {
			break;
		}
		relief_dome(cr, 19, fy, 3.6, PANEL_LEVEL, 0.06);
		relief_dome(cr, w - 19, fy, 3.6, PANEL_LEVEL, 0.06);
	}

	relief_screen(cr, l->radar_x, l->scope_y);
	relief_screen(cr, l->rwr_x, l->scope_y);

	double frame_top = l->data_y - 26, frame_bottom = l->data_y + l->data_h + 26;
	relief_block(cr, l->data_x - 12, frame_top, l->data_w + 24,
		frame_bottom - frame_top, 9, 5, PANEL_LEVEL - 0.02,
		PANEL_LEVEL + 0.17, 0.9);
	relief_block(cr, l->data_x - 2, l->data_y - 2, l->data_w + 4, l->data_h + 4,
		3, 3, PANEL_LEVEL + 0.15, PANEL_LEVEL - 0.16, 0);
	for (int slot = 0; slot < PAGE_KEYS; slot++) {
		double kx, ky, lx, ly;
		key_position(l, slot, &kx, &ky, &lx, &ly);
		/* A hole, and a domed cap standing in it. */
		rounded_rect(cr, kx - 2, ky - 2, OSB_WIDTH + 4, OSB_HEIGHT + 4, 4);
		set_relief(cr, PANEL_LEVEL + 0.06, 0.4, 0);
		cairo_fill(cr);
		relief_block(cr, kx + 0.5, ky + 0.5, OSB_WIDTH - 1, OSB_HEIGHT - 1, 3,
			5, PANEL_LEVEL + 0.14, PANEL_LEVEL + 0.26, 0.5);
	}
	for (int i = 0; i < 4; i++) {
		relief_dome(cr, (i % 2 == 0) ? l->data_x - 22 : l->data_x + l->data_w + 22,
			(i < 2) ? frame_top + 6 : frame_bottom - 6, 3.4, PANEL_LEVEL, 0.06);
	}

	/* The ejection handle: a thick loop. */
	double handle_y = l->panel_bottom - 25;
	relief_block(cr, l->cx - 46, handle_y - 1, 92, 11, 3, 2,
		PANEL_LEVEL, PANEL_LEVEL + 0.08, 0.8);
	for (int side = -1; side <= 1; side += 2) {
		relief_dome(cr, l->cx + side * 40, handle_y + 4.5, 3.4,
			PANEL_LEVEL + 0.08, 0.05);
		double knob_x = l->cx + side * KNOB_OUT, knob_y = l->panel_bottom - 14;
		for (int i = 0; i <= 5; i++) {
			circle(cr, knob_x, knob_y, KNOB_RADIUS - i * 0.4);
			set_relief(cr, PANEL_LEVEL + 0.05 * i, 0.5, 0);
			cairo_fill(cr);
		}
	}
	rounded_rect(cr, l->cx - 31, handle_y + 3, 62, 16, 5);
	relief_tube(cr, 6, PANEL_LEVEL, 0.18, 0.3, 0);
}

/* ---- glass ----
 *
 * The covers of the screens and dials, which reflect a little of the sky.
 * Drawn once; laid over the frame after the needles and text.
 */
static void draw_pane(cairo_t *cr, double x, double y, double w, double h) {
	/* The sky's reflection: brighter at the top left, towards the sun. */
	cairo_pattern_t *sheen = cairo_pattern_create_linear(x, y, x + w * 0.5, y + h);
	cairo_pattern_add_color_stop_rgba(sheen, 0.0, 0.75, 0.88, 1.0, 0.13);
	cairo_pattern_add_color_stop_rgba(sheen, 0.5, 0.75, 0.88, 1.0, 0.03);
	cairo_pattern_add_color_stop_rgba(sheen, 1.0, 0.75, 0.88, 1.0, 0.0);
	cairo_set_source(cr, sheen);
	cairo_paint(cr);
	cairo_pattern_destroy(sheen);
	/* The canopy frame's bright edge, as a slanting streak. */
	cairo_move_to(cr, x + w * 0.16, y);
	cairo_line_to(cr, x + w * 0.16 + h * 0.22, y);
	cairo_line_to(cr, x + w * 0.16 - h * 0.28, y + h);
	cairo_line_to(cr, x + w * 0.16 - h * 0.50, y + h);
	cairo_close_path(cr);
	cairo_set_source_rgba(cr, 0.8, 0.9, 1.0, 0.045);
	cairo_fill(cr);
}

static void draw_round_glass(cairo_t *cr, double x, double y, double r) {
	cairo_save(cr);
	circle(cr, x, y, r);
	cairo_clip(cr);
	draw_pane(cr, x - r, y - r, 2 * r, 2 * r);
	/* A domed cover bends its reflection into a crescent near the rim:
	 * a circle less another, slightly lower and to the right. */
	cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
	circle(cr, x, y, r * 0.93);
	circle(cr, x + r * 0.10, y + r * 0.14, r * 0.90);
	cairo_clip(cr);
	circle(cr, x - r * 0.3, y - r * 0.4, r * 0.75);
	cairo_set_source_rgba(cr, 0.9, 0.96, 1.0, 0.07);
	cairo_fill(cr);
	cairo_restore(cr);
}

static void draw_square_glass(cairo_t *cr, double x, double y, double w, double h) {
	cairo_save(cr);
	cairo_rectangle(cr, x, y, w, h);
	cairo_clip(cr);
	/* The screen is set back, so its frame shades the top and left. */
	for (int i = 0; i < 2; i++) {
		cairo_pattern_t *shade = i == 0 ?
			cairo_pattern_create_linear(0, y, 0, y + 10) :
			cairo_pattern_create_linear(x, 0, x + 7, 0);
		cairo_pattern_add_color_stop_rgba(shade, 0, 0, 0, 0, 0.55);
		cairo_pattern_add_color_stop_rgba(shade, 1, 0, 0, 0, 0);
		cairo_set_source(cr, shade);
		cairo_paint(cr);
		cairo_pattern_destroy(shade);
	}
	draw_pane(cr, x, y, w, h);
	cairo_restore(cr);
}

/* The smudge left by a fingertip: a soft oval, made of see-through ovals
 * each smaller than the last. */
static void draw_smudge(cairo_t *cr, double x, double y) {
	cairo_set_source_rgba(cr, 0.8, 0.9, 1.0, 0.016);
	for (int i = 0; i < 4; i++) {
		ellipse(cr, x, y, 7.5 - 1.4 * i, 10 - 1.8 * i);
		cairo_fill(cr);
	}
}

static void draw_glass(cairo_t *cr, const struct layout *l) {
	double half = SCREEN_SIZE / 2.0, x, y;
	for (int i = -1; i <= 1; i++) {
		/* The mirrors: a glint, and darker towards the rim. */
		mirror_position(l, i, &x, &y);
		cairo_save(cr);
		rounded_rect(cr, x, y, MIRROR_WIDTH, MIRROR_HEIGHT, 5);
		cairo_clip(cr);
		cairo_set_source_rgba(cr, 1, 1, 1, 0.25);
		cairo_set_line_width(cr, 1.2);
		line(cr, x + 8, y + 15, x + 24, y + 3);
		cairo_set_source_rgba(cr, 0, 0, 0.03, 0.16);
		for (int ring = 1; ring <= 4; ring++) {
			rounded_rect(cr, x, y, MIRROR_WIDTH, MIRROR_HEIGHT, 5);
			cairo_set_line_width(cr, ring * 2);
			cairo_stroke(cr);
		}
		cairo_restore(cr);
	}
	draw_square_glass(cr, l->radar_x - half, l->scope_y - half,
		SCREEN_SIZE, SCREEN_SIZE);
	draw_square_glass(cr, l->rwr_x - half, l->scope_y - half,
		SCREEN_SIZE, SCREEN_SIZE);
	draw_square_glass(cr, l->data_x, l->data_y, l->data_w, l->data_h);
	draw_smudge(cr, l->data_x + l->data_w * 0.88, l->data_y + 62);
	draw_smudge(cr, l->data_x + l->data_w * 0.84, l->data_y + 74);
	draw_smudge(cr, l->radar_x + 66, l->scope_y + 70);
	draw_smudge(cr, l->rwr_x - 62, l->scope_y + 72);
	for (int i = 0; i < DIAL_COUNT; i++) {
		if (dial_position(l, i, &x, &y)) {
			draw_round_glass(cr, x, y, dial_radius(l));
		}
	}
	for (int i = 0; i < GAUGE_COUNT; i++) {
		if (gauge_position(l, i, &x, &y)) {
			draw_round_glass(cr, x, y, GAUGE_RADIUS);
		}
	}
}

/* Faint scratches in the canopy, curving as if wiped. Part of the cockpit
 * picture, so they are drawn first, under everything solid. */
static void draw_scratches(cairo_t *cr, const struct layout *l) {
	cairo_set_line_width(cr, 0.7);
	for (int i = 0; i < 16; i++) {
		/* Scattered, but the same every time: multiples of awkward numbers
		 * wrapped into range. */
		double sx = fmod(i * 0.618034 + 0.12, 1.0) * l->width;
		double sy = fmod(i * 0.414214 + 0.07, 1.0) * l->panel_top * 0.85;
		double length = 14 + fmod(i * 37.0, 41);
		double bend = fmod(i * 23.0, 19) - 9;
		cairo_move_to(cr, sx, sy);
		cairo_curve_to(cr, sx + length * 0.3, sy + bend, sx + length * 0.7,
			sy + bend, sx + length, sy + length * 0.2);
		cairo_set_source_rgba(cr, 1, 1, 1, 0.10 + 0.02 * (i % 4));
		cairo_stroke(cr);
	}
}

/* The windscreen's glass: hazy against its frame, and low down a dim
 * reflection of the panel. Drawn with the scratches, under everything. */
static void draw_windscreen(cairo_t *cr, const struct layout *l) {
	for (int i = 1; i <= 8; i++) {
		arch_band(cr, l, -1.0 - SEAL_WIDTH - i * 0.13, -1.0);
		cairo_set_source_rgba(cr, 0.80, 0.90, 1.0, 0.026);
		cairo_fill(cr);
	}

	double bottom = l->panel_top + 18, top = l->panel_top - 72;
	cairo_pattern_t *fade = cairo_pattern_create_linear(0, top, 0, bottom);
	cairo_pattern_add_color_stop_rgba(fade, 0, 0.42, 0.48, 0.56, 0);
	cairo_pattern_add_color_stop_rgba(fade, 1, 0.42, 0.48, 0.56, 0.26);
	cairo_set_source(cr, fade);
	cairo_rectangle(cr, 0, top, l->width, bottom - top);
	cairo_fill(cr);
	cairo_pattern_destroy(fade);

	/* The lit screens in it, upside down and squashed. Soft-edged: several
	 * see-through copies, each smaller than the last. */
	const double xs[3] = { l->radar_x + 60, l->cx, l->rwr_x - 60 };
	const double ws[3] = { 120, 400, 120 };
	for (int i = 0; i < 3; i++) {
		for (int soft = 0; soft < 6; soft++) {
			rounded_rect(cr, xs[i] - ws[i] / 2 + soft * 2,
				l->panel_top - 44 + soft * 1.5, ws[i] - soft * 4,
				30 - soft * 3, 4);
			cairo_set_source_rgba(cr, 0.24, 0.34, 0.36, 0.016);
			cairo_fill(cr);
		}
	}
}

/* Light that depends on where a part is, not on its shape, added to the
 * cockpit after surface.c has lit it. ATOP paints only where something is
 * already drawn. */
static void draw_daylight(cairo_t *cr, const struct layout *l) {
	cairo_set_operator(cr, CAIRO_OPERATOR_ATOP);

	/* White cloud below and sky above light the top of the hood. */
	for (int i = 0; i < 4; i++) {
		glareshield_curve(cr, l, HOOD_TOP + 1 + i);
		cairo_set_source_rgba(cr, 0.78, 0.88, 1.0, 0.11);
		cairo_set_line_width(cr, 2 + 2 * i);
		cairo_stroke(cr);
	}

	/* It gets darker down towards your lap, and out to the sides. */
	cairo_pattern_t *lap = cairo_pattern_create_linear(0, l->panel_top + 60,
		0, DESIGN_HEIGHT);
	cairo_pattern_add_color_stop_rgba(lap, 0, 0, 0, 0.02, 0);
	cairo_pattern_add_color_stop_rgba(lap, 1, 0, 0, 0.02, 0.45);
	cairo_set_source(cr, lap);
	cairo_paint(cr);
	cairo_pattern_destroy(lap);

	/* The screens' green light on their own frames: wide see-through
	 * strokes on top of each other, so brightest at the glass. */
	double half = SCREEN_SIZE / 2.0;
	for (int i = 1; i <= 5; i++) {
		rounded_rect(cr, l->radar_x - half, l->scope_y - half,
			SCREEN_SIZE, SCREEN_SIZE, 1);
		rounded_rect(cr, l->rwr_x - half, l->scope_y - half,
			SCREEN_SIZE, SCREEN_SIZE, 1);
		rounded_rect(cr, l->data_x, l->data_y, l->data_w, l->data_h, 1);
		cairo_set_source_rgba(cr, HUD_GREEN, 0.010);
		cairo_set_line_width(cr, i * 4);
		cairo_stroke(cr);
		/* And a halo round each scope. */
		circle(cr, l->radar_x, l->scope_y, SCOPE_RADIUS);
		circle(cr, l->rwr_x, l->scope_y, SCOPE_RADIUS);
		cairo_set_source_rgba(cr, HUD_GREEN, 0.022);
		cairo_set_line_width(cr, i * 2.5);
		cairo_stroke(cr);
	}
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
}

/* Sunlight through the canopy: two soft slanting bars (the gap is the
 * shadow of a frame behind you), which slide over the panel as you turn. */
#define SUN_PATCH 560   /* the picture's width, in design units */

static void draw_sunlight(cairo_t *cr, double height) {
	/* Its brightness across, along a tilted line. */
	cairo_pattern_t *bars = cairo_pattern_create_linear(60, 0, 440, 120);
	const double stops[][2] = {
		{ 0, 0 }, { 0.22, 0.13 }, { 0.50, 0.13 }, { 0.57, 0.02 },
		{ 0.64, 0.02 }, { 0.71, 0.10 }, { 0.80, 0.10 }, { 1, 0 },
	};
	for (size_t i = 0; i < sizeof(stops) / sizeof(stops[0]); i++) {
		cairo_pattern_add_color_stop_rgba(bars, stops[i][0],
			1.0, 0.95, 0.84, stops[i][1]);
	}
	/* And how it fades in below the hood and out towards the bottom. */
	cairo_pattern_t *fade = cairo_pattern_create_linear(0, 0, 0, height);
	cairo_pattern_add_color_stop_rgba(fade, 0, 0, 0, 0, 0);
	cairo_pattern_add_color_stop_rgba(fade, 0.22, 0, 0, 0, 1);
	cairo_pattern_add_color_stop_rgba(fade, 1, 0, 0, 0, 0.25);
	cairo_set_source(cr, bars);
	cairo_mask(cr, fade);
	cairo_pattern_destroy(bars);
	cairo_pattern_destroy(fade);
}

/* ---- mirrors ----
 *
 * The view behind, painted once as one wide strip. Each mirror shows the
 * part of it next to its neighbour's, and it slides as your head moves.
 */
#define BEHIND_WIDTH (3 * MIRROR_WIDTH + 20)
#define BEHIND_HEIGHT (MIRROR_HEIGHT + 12)

static void draw_behind(cairo_t *cr) {
	double middle = BEHIND_WIDTH / 2.0, horizon = 19;
	cairo_rectangle(cr, 0, 0, BEHIND_WIDTH, horizon);
	set_shading(cr, 0, horizon, (struct colour){ 0.24, 0.45, 0.76 },
		(struct colour){ 0.76, 0.86, 0.95 });
	cairo_fill(cr);
	cairo_rectangle(cr, 0, horizon, BEHIND_WIDTH, BEHIND_HEIGHT - horizon);
	set_shading(cr, horizon, BEHIND_HEIGHT, (struct colour){ 0.90, 0.93, 0.96 },
		(struct colour){ 0.66, 0.72, 0.80 });
	cairo_fill(cr);
	for (int i = 0; i < 26; i++) { /* wisps, and the tops of the cloud below */
		double x = fmod(i * 0.618034, 1.0) * BEHIND_WIDTH;
		double y = i < 10 ? 3 + fmod(i * 4.7, 13) : horizon + fmod(i * 1.9, 9);
		ellipse(cr, x, y, 12 + fmod(i * 7.0, 15), 1.0 + fmod(i * 0.7, 1.2));
		cairo_set_source_rgba(cr, 1, 1, 1, 0.32);
		cairo_fill(cr);
	}
	for (int side = -1; side <= 1; side += 2) {
		/* The wing, and a tail fin leaning out. */
		cairo_move_to(cr, middle + side * 30, 27);
		cairo_line_to(cr, middle + side * 118, 21.5);
		cairo_line_to(cr, middle + side * 122, 23);
		cairo_line_to(cr, middle + side * 92, 32);
		cairo_line_to(cr, middle + side * 30, 32);
		cairo_close_path(cr);
		set_shading(cr, 21, 32, (struct colour){ 0.50, 0.53, 0.57 },
			(struct colour){ 0.30, 0.32, 0.35 });
		cairo_fill(cr);
		cairo_move_to(cr, middle + side * 57, 25);
		cairo_line_to(cr, middle + side * 75, 4);
		cairo_line_to(cr, middle + side * 85, 4);
		cairo_line_to(cr, middle + side * 81, 25);
		cairo_close_path(cr);
		set_shading(cr, 4, 25, (struct colour){ 0.46, 0.49, 0.53 },
			(struct colour){ 0.27, 0.29, 0.32 });
		cairo_fill(cr);
	}
	/* The top of your seat, and your helmet in front of it. */
	rounded_rect(cr, middle - 23, 10, 46, 30, 4);
	set_shading(cr, 10, 32, metal_mid, metal_black);
	cairo_fill(cr);
	ellipse(cr, middle, 30, 17, 18);
	set_shading(cr, 12, 30, (struct colour){ 0.72, 0.74, 0.75 },
		(struct colour){ 0.34, 0.35, 0.37 });
	cairo_fill(cr);
	ellipse(cr, middle, 30, 13, 9); /* the dark visor */
	set_shading(cr, 21, 32, (struct colour){ 0.16, 0.22, 0.30 }, metal_black);
	cairo_fill(cr);
}

/* head_x and head_y are how far the cockpit has been shifted, in units. */
static void draw_mirrors(const struct scene *scene, cairo_t *cr,
		const struct layout *l, double head_x, double head_y) {
	for (int i = -1; i <= 1; i++) {
		double x, y;
		mirror_position(l, i, &x, &y);
		cairo_save(cr);
		rounded_rect(cr, x, y, MIRROR_WIDTH, MIRROR_HEIGHT, 5);
		cairo_clip(cr);
		cairo_set_source_surface(cr, scene->behind,
			x - 10 - (i + 1) * MIRROR_WIDTH + head_x * 2,
			y - 4 + head_y * 1.2);
		cairo_paint(cr);
		cairo_restore(cr);
	}
}

/* ---- public functions (see wallpaper.h) ---- */

/* How far your head moves, in design units. The cockpit moves the other
 * way: lean right, and everything near you slides left. 0 turns it off. */
#define HEAD_MOVE 2.6

static void head_at(double t, const struct flight *f, double *x, double *y) {
	/* Turning pushes you sideways and pulling up pushes you down into the
	 * seat; on top of that, a little restlessness. Slow, and from -1 to 1. */
	*x = 0.75 * cos(t * 0.07) + 0.25 * sin(t * 0.43);
	*y = -0.75 * (f->pitch - 16) / 8 + 0.25 * sin(t * 0.59);
}

/* A cairo that draws on a cached picture in design units, as if it were
 * the screen. */
static cairo_t *cached_cairo(const struct scene *scene, cairo_surface_t *surface) {
	cairo_t *cr = cairo_create(surface);
	cairo_translate(cr, scene->margin, scene->margin);
	cairo_scale(cr, scene->scale, scene->scale);
	return cr;
}

/* Make a cached picture, bigger than the screen by the margin, and a cairo
 * for it. */
static cairo_surface_t *cached_create(const struct scene *scene,
		cairo_format_t format, cairo_t **cr) {
	cairo_surface_t *surface = cairo_image_surface_create(format,
		scene->width + 2 * scene->margin, scene->height + 2 * scene->margin);
	*cr = cached_cairo(scene, surface);
	return surface;
}

/* The cockpit is drawn up to the screen's edges. Fill the margin outside
 * them by repeating the pixels at the edge. */
static void fill_margin(cairo_surface_t *surface, int margin) {
	cairo_surface_flush(surface);
	int width = cairo_image_surface_get_width(surface);
	int height = cairo_image_surface_get_height(surface);
	int stride = cairo_image_surface_get_stride(surface) / 4;
	uint32_t *pixels = (uint32_t *)cairo_image_surface_get_data(surface);
	for (int y = margin; y < height - margin; y++) {
		uint32_t *row = pixels + (size_t)y * stride;
		for (int x = 0; x < margin; x++) {
			row[x] = row[margin];
			row[width - 1 - x] = row[width - 1 - margin];
		}
	}
	for (int y = 0; y < margin; y++) {
		memcpy(pixels + (size_t)y * stride,
			pixels + (size_t)margin * stride, (size_t)width * 4);
		memcpy(pixels + (size_t)(height - 1 - y) * stride,
			pixels + (size_t)(height - 1 - margin) * stride, (size_t)width * 4);
	}
	cairo_surface_mark_dirty(surface);
}

static struct overlay *overlay_of(cairo_surface_t *surface) {
	cairo_surface_flush(surface);
	return overlay_create(
		(const uint32_t *)cairo_image_surface_get_data(surface),
		cairo_image_surface_get_stride(surface) / 4,
		cairo_image_surface_get_width(surface),
		cairo_image_surface_get_height(surface));
}

/* A stopwatch reading, in seconds. */
static double stopwatch(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

void scene_init(struct scene *scene, int width, int height) {
	scene_finish(scene);
	scene->width = width;
	scene->height = height;
	scene->scale = height / DESIGN_HEIGHT;
	scene->hover_key = -1;
	sky_init();
	models_init();
	pages_init(&scene->pages);

	/* ARGB32: has an alpha channel, so the window part stays see-through. */
	struct layout l = layout_for(scene);
	cairo_t *cr;
	scene->margin = (int)ceil(HEAD_MOVE * scene->scale) + 1;
	scene->cockpit = cached_create(scene, CAIRO_FORMAT_ARGB32, &cr);
	draw_windscreen(cr, &l);
	draw_scratches(cr, &l);
	draw_cockpit(scene, cr);
	cairo_destroy(cr);

	/* Light it. The relief is only needed for that. */
	cairo_surface_t *relief = cached_create(scene, CAIRO_FORMAT_RGB96F, &cr);
	draw_relief(cr, &l);
	cairo_destroy(cr);
	surface_light(scene->cockpit, relief, scene->scale);
	cairo_surface_destroy(relief);
	cr = cached_cairo(scene, scene->cockpit);
	draw_daylight(cr, &l);
	cairo_destroy(cr);
	fill_margin(scene->cockpit, scene->margin);
	scene->cockpit_overlay = overlay_of(scene->cockpit);

	scene->glass = cached_create(scene, CAIRO_FORMAT_ARGB32, &cr);
	draw_glass(cr, &l);
	cairo_destroy(cr);
	scene->glass_overlay = overlay_of(scene->glass);

	/* Two small pictures that move on their own. */
	double sun_height = l.panel_bottom - l.panel_top - 24;
	scene->sunlight = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
		(int)(SUN_PATCH * scene->scale), (int)(sun_height * scene->scale));
	cr = cairo_create(scene->sunlight);
	cairo_scale(cr, scene->scale, scene->scale);
	draw_sunlight(cr, sun_height);
	cairo_destroy(cr);
	scene->sunlight_overlay = overlay_of(scene->sunlight);

	scene->behind = cairo_image_surface_create(CAIRO_FORMAT_RGB24,
		(int)ceil(BEHIND_WIDTH * scene->scale),
		(int)ceil(BEHIND_HEIGHT * scene->scale));
	/* Tells cairo this picture has `scale` pixels to the unit. */
	cairo_surface_set_device_scale(scene->behind, scene->scale, scene->scale);
	cr = cairo_create(scene->behind);
	draw_behind(cr);
	cairo_destroy(cr);
}

void scene_finish(struct scene *scene) {
	overlay_destroy(scene->cockpit_overlay);
	scene->cockpit_overlay = NULL;
	if (scene->cockpit != NULL) {
		cairo_surface_destroy(scene->cockpit);
		scene->cockpit = NULL;
	}
	overlay_destroy(scene->glass_overlay);
	scene->glass_overlay = NULL;
	if (scene->glass != NULL) {
		cairo_surface_destroy(scene->glass);
		scene->glass = NULL;
	}
	overlay_destroy(scene->sunlight_overlay);
	scene->sunlight_overlay = NULL;
	if (scene->sunlight != NULL) {
		cairo_surface_destroy(scene->sunlight);
		scene->sunlight = NULL;
	}
	if (scene->behind != NULL) {
		cairo_surface_destroy(scene->behind);
		scene->behind = NULL;
	}
}

void scene_draw(struct scene *scene, cairo_t *cr, double t) {
	struct layout l = layout_for(scene);
	struct flight f = flight_at(t);
	processes_update(&scene->processes, t);
	if (!scene->started) {
		/* Not when the program started: preparing the pictures takes a
		 * while, longer on a slow machine. */
		scene->started = true;
		scene->started_at = t;
		machine_read(&scene->pages.machine); /* for the self test */
	}
	double age = t - scene->started_at;
	scene->now = t;

	double head_x, head_y;
	head_at(t, &f, &head_x, &head_y);
	scene->head_x = (int)lround(-head_x * HEAD_MOVE * scene->scale);
	scene->head_y = (int)lround(-head_y * HEAD_MOVE * scene->scale);
	scene->pointer_x = (scene->pointer_screen_x - scene->head_x) / scene->scale;
	scene->pointer_y = (scene->pointer_screen_y - scene->head_y) / scene->scale;

	/* The frame's raw pixels, for the two steps that bypass cairo.
	 * flush = "cairo, finish anything you have pending before I touch the
	 * memory"; mark_dirty = "I changed the memory behind your back". */
	cairo_surface_t *frame = cairo_get_target(cr);
	uint32_t *pixels = (uint32_t *)cairo_image_surface_get_data(frame);
	int stride_pixels = cairo_image_surface_get_stride(frame) / 4;

	/* 1. Outside. Only the part above the instrument panel shows it. */
	struct sky_view view = {
		.width = scene->width,
		.rows = (int)((l.panel_top + 20) * scene->scale) + scene->margin,
		.horizon = (l.aim_y + f.pitch) * scene->scale,
		.drift = (f.heading - 360) * 6,
		.time = t,
		.scale = scene->scale,
	};
	cairo_surface_flush(frame);
	double started = stopwatch();
	sky_render(pixels, stride_pixels, &view);
	double sky_done = stopwatch();

	/* 2. The aircraft, also straight into the pixels. */
	draw_formation(scene, &l, &f, t, pixels, stride_pixels, view.rows);
	cairo_surface_mark_dirty(frame);
	double models_done = stopwatch();

	/* From here on, draw in design units. */
	cairo_save(cr);
	cairo_scale(cr, scene->scale, scene->scale);
	cairo_select_font_face(cr, UI_FONT, CAIRO_FONT_SLANT_NORMAL,
		CAIRO_FONT_WEIGHT_NORMAL);

	/* 3. HUD symbols. They are focused at infinity, so head movement
	 * shifts the glass they are seen on but not the symbols. */
	if (startup_lit(age, STARTUP_HUD)) {
		draw_hud(cr, &l, &f, scene->head_x / scene->scale,
			scene->head_y / scene->scale);
	}

	/* 4. The cached cockpit over the top. */
	cairo_surface_flush(frame);
	double hud_done = stopwatch();
	int left = scene->head_x - scene->margin, top = scene->head_y - scene->margin;
	overlay_blit(scene->cockpit_overlay, pixels, stride_pixels, left, top,
		scene->width, scene->height);
	/* The sun swings from one side to the other as the heading does. */
	double sun_x = l.cx + (f.heading - 360) / 14 * l.width * 0.36 - SUN_PATCH / 2.0;
	overlay_blit(scene->sunlight_overlay, pixels, stride_pixels,
		scene->head_x + (int)lround(sun_x * scene->scale),
		scene->head_y + (int)lround((l.panel_top + 24) * scene->scale),
		scene->width, scene->height);
	cairo_surface_mark_dirty(frame);
	double cockpit_done = stopwatch();

	/* 5. The live parts of the panel, which move with it. */
	cairo_translate(cr, scene->head_x / scene->scale,
		scene->head_y / scene->scale);
	draw_mirrors(scene, cr, &l, scene->head_x / scene->scale,
		scene->head_y / scene->scale);
	draw_blackout(cr, &l, age);
	draw_dial_needles(scene, cr, &l, t);
	draw_gauge_needles(cr, &l, &f, t, age);
	if (startup_lit(age, STARTUP_RADAR)) {
		draw_radar(scene, cr, &l, t);
	}
	int threats = 0;
	if (startup_lit(age, STARTUP_RWR)) {
		threats = draw_rwr(scene, cr, &l);
	}
	if (startup_lit(age, STARTUP_CENTRE)) {
		draw_status_line(scene, cr, &l);
		draw_centre(scene, cr, &l, threats, t);
	}
	cairo_restore(cr);

	/* 6. The glass over them. */
	cairo_surface_flush(frame);
	overlay_blit(scene->glass_overlay, pixels, stride_pixels, left, top,
		scene->width, scene->height);
	cairo_surface_mark_dirty(frame);

	scene->time_sky += sky_done - started;
	scene->time_models += models_done - sky_done;
	scene->time_cockpit += cockpit_done - hud_done;
	scene->time_vector += (hud_done - models_done) + (stopwatch() - cockpit_done);
}

void scene_pointer_motion(struct scene *scene, double x, double y) {
	/* The compositor reports screen pixels; everything here works in
	 * design units, on a cockpit that head movement has shifted. */
	if (scene->scale > 0) {
		scene->pointer_screen_x = x;
		scene->pointer_screen_y = y;
		scene->pointer_x = (x - scene->head_x) / scene->scale;
		scene->pointer_y = (y - scene->head_y) / scene->scale;
		scene->pointer_inside = true;
	}
}

void scene_pointer_leave(struct scene *scene) {
	scene->pointer_inside = false;
}

void scene_pointer_click(struct scene *scene) {
	/* Work the key out from the pointer position rather than from the
	 * last frame, so a click right after a move isn't missed. */
	if (scene->started && scene->now - scene->started_at < STARTUP_LENGTH) {
		/* Impatient: skip the rest of the start-up sequence. */
		scene->started_at = scene->now - STARTUP_LENGTH;
		return;
	}
	struct layout l = layout_for(scene);
	int key = key_at(&l, &scene->pages, scene->pointer_x, scene->pointer_y);
	if (key >= 0) {
		scene->pressed_key = key;
		scene->pressed_at = scene->now;
		pages_press(&scene->pages, key);
		return;
	}
	/* Click a radar contact to lock it; click it again, or anywhere else,
	 * to break the lock. hover_pid was worked out during the last draw. */
	if (scene->hover_pid != 0 && scene->hover_pid != scene->locked_pid) {
		scene->locked_pid = scene->hover_pid;
		pages_show(&scene->pages, PAGE_HOME); /* where the target is shown */
	} else {
		scene->locked_pid = 0;
	}
}
