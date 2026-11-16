/*
 * Drawing the wallpaper: one frame of the view from a fighter cockpit.
 *
 * What is in the picture, back to front:
 *   outside     blue sky, sea and cloud streaming past below (sky.c)
 *   formation   two jets ahead of you: a Typhoon in front, an F-35 to
 *               the right (3D models, model.c)
 *   HUD         green symbols on the glass in front of you: pitch ladder,
 *               flight path marker, heading tape, speed and altitude
 *   cockpit     canopy arch, sills and instrument panel (cached)
 *   screens     left screen: radar, each contact a running program
 *               right screen: RWR, each "threat" a program using the CPU
 *   keys        the buttons between the screens start apps
 *   dials       CPU, memory, load and task count (on a wide screen)
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
/* Key 0 opens the launcher; keys 1.. are apps[0]..., as on the dock. */
#define KEY_COUNT (APP_COUNT + 1)
_Static_assert(KEY_COUNT <= 2 * OSB_PER_ROW,
	"more apps than buttons round the centre display");

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
	 * the buttons. */
	l.data_w = MFD_WIDTH;
	l.data_x = l.cx - l.data_w / 2;
	l.data_y = l.panel_top + 46;
	l.data_h = l.panel_bottom - 40 - l.data_y;
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
};

/* The leader is dead ahead and a little above, seen through the HUD. The
 * other is closer, out to the right and below you, so you look down
 * on its wings from behind. (Level with you, a wing is seen edge-on and
 * all but disappears.) */
static const struct wingman formation[MODEL_COUNT] = {
	{ MODEL_F35, 16.5, -5.6, 33.0 },
	{ MODEL_TYPHOON, 0.0, 3.2, 44.0 },
};

/* How long a lens the camera has, in design units. It sets how big the
 * aircraft look for their distance. */
#define FOCAL_LENGTH 430.0

/* Draw the aircraft into the frame's pixels. */
static void draw_formation(struct scene *scene, const struct layout *l,
		double t, uint32_t *pixels, int stride_pixels) {
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
			.yaw = 1.0 * DEGREES * sin(t * 0.41 + phase * 0.7),
			.focal = FOCAL_LENGTH * scene->scale,
			.screen_x = l->aim_x * scene->scale,
			.screen_y = l->aim_y * scene->scale,
			.glow = 0.86 + 0.14 * sin(t * 11 + phase * 3), /* flicker */
		};
		model_draw(wingman->model, &pose, pixels, stride_pixels,
			scene->width, scene->height);
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

static void draw_hud(cairo_t *cr, const struct layout *l,
		const struct flight *f) {
	/* Symbols only appear on the glass, so clip to it. */
	cairo_save(cr);
	hud_glass_path(cr, l);
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
	cairo_set_font_size(cr, 13);
	snprintf(value, sizeof(value), "%d", f->speed);
	cairo_rectangle(cr, l->hud_x + 14, l->aim_y - 11, 44, 22);
	cairo_stroke(cr);
	text_centred(cr, value, l->hud_x + 36, l->aim_y);
	snprintf(value, sizeof(value), "%d", f->altitude);
	cairo_rectangle(cr, l->hud_x + l->hud_w - 64, l->aim_y - 11, 50, 22);
	cairo_stroke(cr);
	text_centred(cr, value, l->hud_x + l->hud_w - 39, l->aim_y);

	/* The aircraft you are following, as a HUD would tag it: its name
	 * just below it, in the gap down the middle of the pitch ladder. */
	cairo_set_font_size(cr, 10);
	text_centred(cr, "TYPHOON", l->cx, l->aim_y + 32);

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

	/* Below the row of button names along the top of the glass. */
	double x = l->data_x + 14, y = l->data_y + 40;
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

/* How big a button's name is on the glass: most of the space between
 * buttons, one line of text high. */
static double legend_width(const struct layout *l) {
	return l->data_w / OSB_PER_ROW - 8;
}
#define LEGEND_HEIGHT 18

/* One button and its name. Drawn into the cached cockpit in its resting
 * state (slots with no app get a button but no name, as on a real panel),
 * and again on top, lit, for the key under the pointer. */
static void draw_key(cairo_t *cr, const struct layout *l, int slot, bool lit) {
	double x, y, lx, ly;
	key_position(l, slot, &x, &y, &lx, &ly);

	/* The button: a dark hole in the frame with a small cap in it. */
	rounded_rect(cr, x - 2, y - 2, OSB_WIDTH + 4, OSB_HEIGHT + 4, 4);
	set_colour(cr, metal_black);
	cairo_fill(cr);
	rounded_rect(cr, x + 0.5, y + 0.5, OSB_WIDTH - 1, OSB_HEIGHT - 1, 3);
	if (lit) {
		cairo_set_source_rgb(cr, HUD_GREEN); /* pressed in and backlit */
		cairo_fill(cr);
	} else {
		/* Lighter at the top where the light catches it. */
		set_shading(cr, y, y + OSB_HEIGHT, metal_light, metal_mid);
		cairo_fill(cr);
	}

	if (slot >= KEY_COUNT) {
		return;
	}
	/* The name. Lit, it is shown inverted in a box, which is how a real
	 * display marks the option that is selected. */
	double half = legend_width(l) / 2;
	cairo_set_font_size(cr, 11);
	if (lit) {
		cairo_rectangle(cr, lx - half, ly - LEGEND_HEIGHT / 2.0,
			2 * half, LEGEND_HEIGHT);
		cairo_set_source_rgb(cr, HUD_GREEN);
		cairo_fill(cr);
		cairo_set_source_rgb(cr, HUD_BLACK);
	} else {
		cairo_set_source_rgb(cr, HUD_GREEN);
	}
	text_centred(cr, key_label(slot), lx, ly);
}

/* Which key is at x,y? -1 for none. A key can be clicked on its button,
 * on its name, or anywhere between the two. */
static int key_at(const struct layout *l, double x, double y) {
	double half = legend_width(l) / 2;
	for (int i = 0; i < KEY_COUNT; i++) {
		double bx, by, lx, ly;
		key_position(l, i, &bx, &by, &lx, &ly);
		double top = fmin(by, ly - LEGEND_HEIGHT / 2.0);
		double bottom = fmax(by + OSB_HEIGHT, ly + LEGEND_HEIGHT / 2.0);
		if (x >= lx - half && x < lx + half && y >= top && y < bottom) {
			return i;
		}
	}
	return -1;
}

/* ---- dials ----
 *
 * Round gauges in the spaces either side of the centre panel, each showing
 * one real figure about the machine. The face (ring, ticks, name) is part
 * of the cached cockpit; only the needle is drawn each frame. They only
 * appear on a wide screen: on a 4:3 screen there is no room.
 */
#define DIAL_COUNT 4
#define DIAL_RADIUS 46

struct dial {
	const char *name;
	double full_scale;  /* the value at the end of the scale */
	const char *top;    /* the number printed at the end of the scale */
};

static const struct dial dials[DIAL_COUNT] = {
	{ "CPU %", 100, "100" },
	{ "MEM %", 100, "100" },
	{ "LOAD", 2, "2" },
	{ "TASKS", 40, "40" },
};

/* Centre of dial `index`, or false if this screen has no room for dials.
 * Dials 0 and 1 are stacked left of the centre panel, 2 and 3 right. */
static bool dial_position(const struct layout *l, int index,
		double *x, double *y) {
	double left_edge = l->radar_x + SCREEN_SIZE / 2.0 + 11;
	double gap = (l->data_x - 30) - left_edge;
	if (gap < 2 * DIAL_RADIUS + 40) {
		return false;
	}
	double offset = left_edge + gap / 2;
	*x = index < 2 ? offset : l->width - offset;
	*y = l->scope_y + (index % 2 == 0 ? -54 : 56);
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
		double x, double y) {
	/* Bezel ring, then the black face. */
	circle(cr, x, y, DIAL_RADIUS + 5);
	set_shading(cr, y - DIAL_RADIUS - 5, y + DIAL_RADIUS + 5, metal_light, metal_dark);
	cairo_fill(cr);
	circle(cr, x, y, DIAL_RADIUS);
	set_colour(cr, glass_black);
	cairo_fill(cr);

	/* Ticks: eleven of them, every other one longer. The last fifth of
	 * the scale is marked with a thicker arc, the "caution" range. */
	cairo_set_source_rgb(cr, HUD_GREEN);
	cairo_set_line_width(cr, 1.2);
	for (int i = 0; i <= 10; i++) {
		double x1, y1, x2, y2;
		polar(x, y, dial_bearing(i / 10.0), DIAL_RADIUS - (i % 2 == 0 ? 9 : 5), &x1, &y1);
		polar(x, y, dial_bearing(i / 10.0), DIAL_RADIUS - 2, &x2, &y2);
		line(cr, x1, y1, x2, y2);
	}
	cairo_new_sub_path(cr);
	cairo_arc(cr, x, y, DIAL_RADIUS - 3, (dial_bearing(0.8) - 90) * DEGREES,
		(dial_bearing(1.0) - 90) * DEGREES);
	cairo_set_line_width(cr, 3);
	cairo_stroke(cr);

	cairo_set_font_size(cr, 9);
	cairo_set_source_rgb(cr, HUD_DIM);
	double tx, ty;
	polar(x, y, dial_bearing(0), DIAL_RADIUS - 17, &tx, &ty);
	text_centred(cr, "0", tx, ty);
	polar(x, y, dial_bearing(1), DIAL_RADIUS - 18, &tx, &ty);
	text_centred(cr, dial->top, tx, ty);
	cairo_set_source_rgb(cr, HUD_GREEN);
	text_centred(cr, dial->name, x, y + DIAL_RADIUS * 0.52);
}

static void draw_dial_needles(struct scene *scene, cairo_t *cr,
		const struct layout *l) {
	const struct process_list *list = &scene->processes;
	const double values[DIAL_COUNT] = {
		list->cpu_fraction * 100, list->memory_fraction * 100,
		list->load_average, list->count,
	};
	for (int i = 0; i < DIAL_COUNT; i++) {
		double x, y, tip_x, tip_y;
		if (!dial_position(l, i, &x, &y)) {
			return;
		}
		polar(x, y, dial_bearing(values[i] / dials[i].full_scale),
			DIAL_RADIUS - 11, &tip_x, &tip_y);
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
	double outer = l->radar_x + SCREEN_SIZE / 2.0 + 11 + GAUGE_RADIUS + 5;
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
	cairo_set_source_rgb(cr, HUD_GREEN);
	cairo_set_line_width(cr, 1);
	for (int hour = 0; hour < 12; hour++) {
		double x1, y1, x2, y2;
		polar(x, y, hour * 30, GAUGE_RADIUS - (hour % 3 == 0 ? 4.5 : 2.5), &x1, &y1);
		polar(x, y, hour * 30, GAUGE_RADIUS - 1, &x2, &y2);
		line(cr, x1, y1, x2, y2);
	}
	cairo_set_font_size(cr, 6.5);
	cairo_set_source_rgb(cr, HUD_DIM);
	text_centred(cr, name, x, y + GAUGE_RADIUS * 0.48);
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
		const struct flight *f) {
	for (int i = 0; i < GAUGE_COUNT; i++) {
		double x, y, tip_x, tip_y;
		if (!gauge_position(l, i, &x, &y)) {
			return;
		}
		polar(x, y, gauge_bearing(i, f), GAUGE_RADIUS - 5, &tip_x, &tip_y);
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

	/* Bolts along the arch, every 6 degrees. */
	for (int i = -13; i <= 13; i++) {
		double angle = (i * 6 - 90) * DEGREES; /* -90 = the top of the arch */
		double bx = arch_x + (arch_rx + 6) * cos(angle);
		double by = arch_y + (arch_ry + 6) * sin(angle);
		circle(cr, bx, by, 2.2);
		set_colour(cr, metal_light);
		cairo_fill(cr);
		circle(cr, bx + 0.6, by + 0.6, 1.0);
		set_colour(cr, metal_black);
		cairo_fill(cr);
	}

	/* Three rear-view mirrors hanging inside the top of the arch. Each
	 * shows a sliver of sky, darker towards the bottom. */
	for (int i = -1; i <= 1; i++) {
		double mx = l.cx + i * 120;
		double my = arch_y - arch_ry + 26 + (i == 0 ? 0 : 5);
		rounded_rect(cr, mx - 44, my - 3, 88, 26, 7);
		set_colour(cr, metal_black);
		cairo_fill(cr);
		rounded_rect(cr, mx - 40, my, 80, 20, 5);
		set_shading(cr, my, my + 20, (struct colour){ 0.62, 0.76, 0.92 },
			(struct colour){ 0.20, 0.33, 0.52 });
		cairo_fill(cr);
		/* A glint across the glass. */
		cairo_set_source_rgba(cr, 1, 1, 1, 0.35);
		cairo_set_line_width(cr, 1.5);
		line(cr, mx - 30, my + 15, mx - 12, my + 4);
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

	/* HUD: a bare pane of glass with no frame, held at its lower corners
	 * by two metal brackets. The glass itself is not filled in here (you
	 * look through it; draw_hud() tints it), only its edge: thick glass
	 * seen edge-on is pale green. Drawn twice along the top to show the
	 * thickness of the pane. */
	hud_glass_path(cr, &l);
	cairo_set_source_rgba(cr, 0.62, 0.90, 0.76, 0.75);
	cairo_set_line_width(cr, 1.5);
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
		cairo_move_to(cr, MIRROR(foot - 24), l.panel_top + 4);
		cairo_line_to(cr, MIRROR(top - 9), bracket_top);
		cairo_line_to(cr, MIRROR(top + 5), bracket_top);
		cairo_line_to(cr, MIRROR(foot + 9), glass_bottom - 20);
		cairo_line_to(cr, MIRROR(foot + 9), l.panel_top + 4);
		cairo_close_path(cr);
		set_shading(cr, bracket_top, l.panel_top, metal_mid, metal_black);
		cairo_fill(cr);
		/* The outer edge catches the light. */
		set_colour(cr, metal_light);
		cairo_set_line_width(cr, 1.2);
		line(cr, MIRROR(foot - 24), l.panel_top + 4, MIRROR(top - 9), bracket_top);
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
		(struct colour){ 0.19, 0.20, 0.22 }, (struct colour){ 0.085, 0.09, 0.10 });
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

	/* The HUD projector housing, on top of the hood. */
	rounded_rect(cr, l.cx - 78, l.panel_top - 20, 156, 34, 6);
	set_shading(cr, l.panel_top - 20, l.panel_top + 14, metal_mid, metal_black);
	cairo_fill(cr);
	rounded_rect(cr, l.cx - 52, l.panel_top - 13, 104, 12, 4);
	cairo_set_source_rgb(cr, 0.02, 0.16, 0.08); /* the projector lens */
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
			draw_dial_face(cr, &dials[i], dial_x, dial_y);
		}
		for (int i = 0; i < GAUGE_COUNT; i++) {
			if (gauge_position(&l, i, &dial_x, &dial_y)) {
				draw_gauge_face(cr, gauge_names[i], dial_x, dial_y);
			}
		}
	}

	draw_screen(cr, "RADAR", l.radar_x, l.scope_y, false);
	draw_screen(cr, "RWR", l.rwr_x, l.scope_y, true);

	/* The centre display: a wide screen in a metal frame, deep enough at
	 * the top and bottom for a row of push buttons. Same style of frame
	 * as the radar and RWR screens. */
	double frame_top = l.data_y - 26, frame_bottom = l.data_y + l.data_h + 26;
	rounded_rect(cr, l.data_x - 12, frame_top, l.data_w + 24,
		frame_bottom - frame_top, 9);
	set_shading(cr, frame_top, frame_bottom, metal_mid, metal_dark);
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

	for (int slot = 0; slot < 2 * OSB_PER_ROW; slot++) {
		draw_key(cr, &l, slot, false);
	}

	/* A screw in each corner of the centre panel. */
	set_colour(cr, metal_light);
	for (int i = 0; i < 4; i++) {
		double x = (i % 2 == 0) ? l.data_x - 22 : l.data_x + l.data_w + 22;
		double y = (i < 2) ? frame_top + 6 : frame_bottom - 6;
		circle(cr, x, y, 3);
		cairo_fill(cr);
	}

	/* The hood's shadow, last, so it falls across everything on the panel:
	 * darkest right under the hood, gone 40 units below it. A blurred
	 * edge is faked with ten thin see-through black bands, each fainter
	 * than the one above. */
	for (int band = 0; band < 10; band++) {
		double fade = 1.0 - band / 10.0;
		glareshield_curve(cr, &l, HOOD_BOTTOM + 2 + band * 4);
		cairo_set_source_rgba(cr, 0, 0, 0, 0.55 * fade * fade);
		cairo_set_line_width(cr, 4.5);
		cairo_stroke(cr);
	}
}

/* ---- public functions (see wallpaper.h) ---- */

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
}

void scene_finish(struct scene *scene) {
	overlay_destroy(scene->cockpit_overlay);
	scene->cockpit_overlay = NULL;
	if (scene->cockpit != NULL) {
		cairo_surface_destroy(scene->cockpit);
		scene->cockpit = NULL;
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
	double started = stopwatch();
	sky_render(pixels, stride_pixels, &view);
	double sky_done = stopwatch();

	/* 2. The aircraft, also straight into the pixels. */
	draw_formation(scene, &l, t, pixels, stride_pixels);
	cairo_surface_mark_dirty(frame);
	double models_done = stopwatch();

	/* From here on, draw in design units. */
	cairo_save(cr);
	cairo_scale(cr, scene->scale, scene->scale);
	cairo_select_font_face(cr, UI_FONT, CAIRO_FONT_SLANT_NORMAL,
		CAIRO_FONT_WEIGHT_NORMAL);

	/* 3. HUD symbols. */
	draw_hud(cr, &l, &f);

	/* 4. The cached cockpit over the top. */
	cairo_surface_flush(frame);
	double hud_done = stopwatch();
	overlay_blit(scene->cockpit_overlay, pixels, stride_pixels);
	cairo_surface_mark_dirty(frame);
	double cockpit_done = stopwatch();

	/* 5. The live parts of the panel. */
	draw_radar(scene, cr, &l, t);
	int threats = draw_rwr(scene, cr, &l);
	draw_data_display(scene, cr, &l, threats);
	draw_dial_needles(scene, cr, &l);
	draw_gauge_needles(cr, &l, &f);

	scene->hover_key = scene->pointer_inside ?
		key_at(&l, scene->pointer_x, scene->pointer_y) : -1;
	if (scene->hover_key >= 0) {
		draw_key(cr, &l, scene->hover_key, true);
	}
	cairo_restore(cr);

	scene->time_sky += sky_done - started;
	scene->time_models += models_done - sky_done;
	scene->time_cockpit += cockpit_done - hud_done;
	scene->time_vector += (hud_done - models_done) + (stopwatch() - cockpit_done);
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
