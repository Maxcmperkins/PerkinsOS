/*
 * Drawing the wallpaper: one frame of an animated scene.
 *
 * What is in the picture, back to front:
 *   sky            a dark gradient (cached, never changes)
 *   low clouds     sprites drifting down the screen, under the aircraft
 *   the formation  Typhoon leading, Tornado and Lightning on its wings,
 *                  seen from above, each with two fading contrails
 *   high clouds    a few faster sprites passing over the aircraft
 *   HUD            heading tape, speed and altitude read-outs
 *   radar          a sweeping scope; each contact is a running program
 *   RWR            radar warning receiver; each "threat" is a program
 *                  that is using the CPU right now
 *
 * Everything that moves is a function of the time `t`, so a frame can be
 * drawn for any moment without remembering the previous one.
 *
 * The flight numbers (heading, speed, altitude) are a simulation. The
 * radar and RWR show real data from the machine.
 */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "client.h"
#include "theme.h"
#include "wallpaper.h"

#define DOCK_HEIGHT 48   /* keep clear of the dock along the bottom */
#define SCOPE_RADIUS 84  /* radar and RWR circles */
#define DEGREES (M_PI / 180.0)

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

/* A repeatable stream of "random" numbers from 0 to 1, so the clouds are
 * in the same places every boot. (A linear congruential generator.) */
static unsigned int random_state = 20261115;
static double random_unit(void) {
	random_state = random_state * 1103515245u + 12345u;
	return ((random_state >> 16) & 0x7fff) / 32768.0;
}

/* ---- where things are on the screen ---- */

struct layout {
	double cx;               /* horizontal centre */
	double formation_y;      /* centre of the formation */
	double radar_x, rwr_x;   /* scope centres */
	double scope_y;
	double tape_y;           /* heading tape */
	double readout_y;        /* speed and altitude boxes */
};

static struct layout layout_for(const struct scene *scene) {
	struct layout l;
	l.cx = scene->width / 2.0;
	l.formation_y = scene->height * 0.43;
	l.radar_x = 44 + SCOPE_RADIUS;
	l.rwr_x = scene->width - 44 - SCOPE_RADIUS;
	l.scope_y = scene->height - DOCK_HEIGHT - SCOPE_RADIUS - 30;
	l.tape_y = 46;
	l.readout_y = scene->height * 0.26;
	return l;
}

/* ---- aircraft outlines ----
 *
 * Each aircraft is the right-hand half of its outline seen from above, nose
 * up, as x,y pairs. x runs 0 (centreline) to 1 (about half a wingspan);
 * y runs -1 (nose) to +1 (tail). The left half is the mirror image, so the
 * drawing code walks the points forwards and then backwards with x negated.
 */
struct aircraft {
	const char *name;
	int point_count;
	const double (*points)[2];
	/* Place in the formation, in aircraft-lengths from its centre. */
	double offset_x, offset_y;
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
#define AIRCRAFT_SIZE 58.0 /* half an aircraft's length, in pixels */

/* A "vic": the leader in front, one aircraft behind on each side. */
static const struct aircraft formation[] = {
	{ "TYPHOON", COUNT(typhoon_points), typhoon_points, 0.0, -0.9 },
	{ "TORNADO", COUNT(tornado_points), tornado_points, -3.0, 1.0 },
	{ "LIGHTNING", COUNT(lightning_points), lightning_points, 3.0, 1.0 },
};

/* Two exhaust trails streaming back from the tail, fading out. */
static void draw_contrails(cairo_t *cr, double x, double y, double t,
		double phase) {
	const int segments = 10;
	const double segment_length = 24;
	for (int side = -1; side <= 1; side += 2) {
		double px = x + side * 0.09 * AIRCRAFT_SIZE;
		double py = y + AIRCRAFT_SIZE;
		for (int i = 0; i < segments; i++) {
			/* Each segment is fainter and wider than the one before, and
			 * wanders a little so the trail isn't a ruled line. */
			double fade = 1.0 - (double)i / segments;
			double nx = x + side * (0.09 * AIRCRAFT_SIZE + i * 0.9) +
				2.5 * sin(t * 1.7 + phase + i * 0.7) * (1 - fade);
			double ny = py + segment_length;
			cairo_set_source_rgba(cr, HUD_GREEN, 0.30 * fade);
			cairo_set_line_width(cr, 1.5 + i * 0.5);
			line(cr, px, py, nx, ny);
			px = nx;
			py = ny;
		}
	}
}

static void draw_aircraft(cairo_t *cr, const struct aircraft *aircraft,
		double x, double y, double angle) {
	/* save/restore brackets a temporary change of coordinate system: move
	 * the origin to the aircraft and turn it, so the outline can be drawn
	 * as if it were at 0,0 pointing straight up. */
	cairo_save(cr);
	cairo_translate(cr, x, y);
	cairo_rotate(cr, angle);
	cairo_new_path(cr);
	for (int i = 0; i < aircraft->point_count; i++) {
		cairo_line_to(cr, aircraft->points[i][0] * AIRCRAFT_SIZE,
			aircraft->points[i][1] * AIRCRAFT_SIZE);
	}
	for (int i = aircraft->point_count - 1; i >= 0; i--) {
		cairo_line_to(cr, -aircraft->points[i][0] * AIRCRAFT_SIZE,
			aircraft->points[i][1] * AIRCRAFT_SIZE);
	}
	cairo_close_path(cr);
	cairo_restore(cr);

	/* Solid dark fill so clouds underneath don't show through, then the
	 * green outline. */
	cairo_set_source_rgb(cr, 0.03, 0.15, 0.07);
	cairo_fill_preserve(cr);
	cairo_set_source_rgb(cr, HUD_GREEN);
	cairo_set_line_width(cr, HUD_LINE);
	cairo_stroke(cr);
}

static void draw_formation(cairo_t *cr, const struct layout *l, double t) {
	cairo_set_font_size(cr, 11);
	for (int i = 0; i < COUNT(formation); i++) {
		const struct aircraft *aircraft = &formation[i];
		/* Each aircraft drifts slowly around its slot, out of step with
		 * the others, the way real formation flying is never perfectly
		 * still. sin() of time gives a smooth back-and-forth. */
		double phase = i * 2.1;
		double x = l->cx + aircraft->offset_x * AIRCRAFT_SIZE +
			7.0 * sin(t * 0.55 + phase);
		double y = l->formation_y + aircraft->offset_y * AIRCRAFT_SIZE +
			5.0 * sin(t * 0.8 + phase * 1.7);
		/* Bank a degree or two in the direction it is drifting. */
		double angle = 1.6 * DEGREES * cos(t * 0.55 + phase);

		draw_contrails(cr, x, y, t, phase);
		draw_aircraft(cr, aircraft, x, y, angle);

		cairo_set_source_rgb(cr, HUD_DIM);
		draw_text(cr, aircraft->name, x + 0.8 * AIRCRAFT_SIZE,
			y - 0.7 * AIRCRAFT_SIZE);
	}
}

/* ---- clouds ---- */

static struct {
	double x, speed, start;
	int width, height;
	bool high; /* drawn above the aircraft */
} cloud_info[CLOUD_COUNT];

/* Draw one cloud into its own small transparent image: a handful of soft
 * blobs. Done once at start-up; each frame only moves the finished image. */
static cairo_surface_t *make_cloud(int width, int height, double strength) {
	cairo_surface_t *surface = cairo_image_surface_create(
		CAIRO_FORMAT_ARGB32, width, height);
	cairo_t *cr = cairo_create(surface);
	for (int i = 0; i < 6; i++) {
		double radius = height * (0.25 + 0.25 * random_unit());
		double x = radius + random_unit() * (width - 2 * radius);
		double y = radius + random_unit() * (height - 2 * radius);
		/* A radial gradient: most opaque in the middle, fading to nothing
		 * at the edge, which is what makes the blob look soft. */
		cairo_pattern_t *blob = cairo_pattern_create_radial(
			x, y, 0, x, y, radius);
		cairo_pattern_add_color_stop_rgba(blob, 0.0, HUD_GREEN, strength);
		cairo_pattern_add_color_stop_rgba(blob, 1.0, HUD_GREEN, 0.0);
		cairo_set_source(cr, blob);
		circle(cr, x, y, radius);
		cairo_fill(cr);
		cairo_pattern_destroy(blob);
	}
	cairo_destroy(cr);
	return surface;
}

static void draw_clouds(struct scene *scene, cairo_t *cr, double t, bool high) {
	for (int i = 0; i < CLOUD_COUNT; i++) {
		if (cloud_info[i].high != high) {
			continue;
		}
		/* The aircraft fly up the screen, so the clouds slide down it.
		 * fmod wraps the position: a cloud that leaves the bottom
		 * re-enters at the top. */
		double span = scene->height + cloud_info[i].height;
		double y = fmod(cloud_info[i].start + cloud_info[i].speed * t, span) -
			cloud_info[i].height;
		cairo_set_source_surface(cr, scene->clouds[i], cloud_info[i].x, y);
		cairo_paint(cr);
	}
}

/* ---- HUD: heading tape and read-outs ---- */

/* The frame and caption of a read-out box (static). */
static void draw_readout_frame(cairo_t *cr, const char *caption,
		double x, double y) {
	cairo_set_font_size(cr, 11);
	cairo_set_source_rgb(cr, HUD_DIM);
	draw_text(cr, caption, x, y - 10);
	cairo_set_source_rgb(cr, HUD_BLACK);
	cairo_rectangle(cr, x, y, 86, 30);
	cairo_fill_preserve(cr);
	cairo_set_source_rgb(cr, HUD_GREEN);
	cairo_set_line_width(cr, HUD_LINE);
	cairo_stroke(cr);
}

/* The number inside a read-out box (changes every frame). */
static void draw_readout_value(cairo_t *cr, const char *value,
		double x, double y) {
	cairo_set_font_size(cr, 16);
	cairo_set_source_rgb(cr, HUD_GREEN);
	draw_text(cr, value, x + (86 - text_width(cr, value)) / 2, y + 15);
}

static void draw_hud(cairo_t *cr, const struct layout *l, double t) {
	/* A gentle weave either side of north, and small changes in speed and
	 * height. Different periods so they don't all peak together. */
	double heading = 360 + 14 * sin(t * 0.07);
	int speed = 450 + (int)lround(4 * sin(t * 0.31));
	int altitude = 24500 + 10 * (int)lround(6 * sin(t * 0.19));

	/* Heading tape: a tick every 2 degrees, 7 pixels per degree, sliding
	 * sideways under a fixed caret as the heading changes. */
	const double pixels_per_degree = 7, half_width = 250;
	cairo_set_source_rgb(cr, HUD_GREEN);
	cairo_set_line_width(cr, HUD_LINE);
	cairo_set_font_size(cr, 12);
	int first = (int)floor(heading - half_width / pixels_per_degree);
	int last = (int)ceil(heading + half_width / pixels_per_degree);
	for (int degree = first; degree <= last; degree++) {
		if (degree % 2 != 0) {
			continue;
		}
		double x = l->cx + (degree - heading) * pixels_per_degree;
		if (fabs(x - l->cx) > half_width) {
			continue;
		}
		bool major = degree % 10 == 0;
		line(cr, x, l->tape_y, x, l->tape_y + (major ? 12 : 6));
		if (major) {
			/* Tens of degrees as two digits, the way a HUD shows them:
			 * 350 is "35", 360 is "00", 10 is "01". */
			char label[8];
			snprintf(label, sizeof(label), "%02d", (degree % 360) / 10);
			text_centred(cr, label, x, l->tape_y - 12);
		}
	}

	char value[16];
	snprintf(value, sizeof(value), "%d", speed);
	draw_readout_value(cr, value, 44, l->readout_y);
	snprintf(value, sizeof(value), "%d", altitude);
	draw_readout_value(cr, value, l->rwr_x + SCOPE_RADIUS - 86, l->readout_y);

	cairo_set_font_size(cr, 12);
	cairo_set_source_rgb(cr, HUD_DIM);
	snprintf(value, sizeof(value), "HDG %03d", (int)lround(heading) % 360);
	draw_text(cr, value, 44, l->readout_y + 50);
}

/* ---- radar ---- */

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

	char count[24];
	snprintf(count, sizeof(count), "%d CONTACTS", list->count);
	cairo_set_font_size(cr, 11);
	cairo_set_source_rgb(cr, HUD_DIM);
	draw_text(cr, count, cx - SCOPE_RADIUS, cy + SCOPE_RADIUS + 14);
}

/* The designated contact: the locked one, or else the one under the
 * pointer. Draws a target box on the radar and its details beside it. */
static void draw_target(struct scene *scene, cairo_t *cr,
		const struct layout *l) {
	int pid = scene->locked_pid != 0 ? scene->locked_pid : scene->hover_pid;
	const struct process *target = NULL;
	for (int i = 0; i < scene->processes.count; i++) {
		if (scene->processes.items[i].pid == pid) {
			target = &scene->processes.items[i];
		}
	}
	if (target == NULL) {
		scene->locked_pid = 0; /* the locked program has exited */
		return;
	}
	bool locked = scene->locked_pid != 0;

	/* Target designator box around the blip. */
	cairo_set_source_rgb(cr, HUD_GREEN);
	cairo_set_line_width(cr, HUD_LINE);
	cairo_rectangle(cr, target->radar_x - 8, target->radar_y - 8, 16, 16);
	cairo_stroke(cr);

	/* Details panel to the right of the scope. */
	double x = l->radar_x + SCOPE_RADIUS + 18;
	double y = l->scope_y - 36;
	char row[48];
	cairo_set_font_size(cr, 12);
	cairo_set_source_rgb(cr, HUD_GREEN);
	draw_text(cr, locked ? "TGT LOCKED" : "TGT", x, y);
	char name[sizeof(target->name)];
	for (size_t i = 0; i < sizeof(name); i++) {
		name[i] = toupper((unsigned char)target->name[i]);
	}
	draw_text(cr, name, x, y + 20);
	cairo_set_source_rgb(cr, HUD_DIM);
	snprintf(row, sizeof(row), "PID %d", target->pid);
	draw_text(cr, row, x, y + 38);
	snprintf(row, sizeof(row), "MEM %.1f MB", target->memory_kb / 1024.0);
	draw_text(cr, row, x, y + 54);
	snprintf(row, sizeof(row), "CPU %.1f%%", target->cpu_percent);
	draw_text(cr, row, x, y + 70);
}

/* ---- RWR ---- */

/* A radar warning receiver shows who is "painting" you with their radar,
 * as symbols round a circle: direction is where the threat is, and the
 * closer to the centre, the more dangerous. Here a threat is a program
 * using the CPU: the busier it is, the closer to the centre. It uses the
 * same bearing as on the radar, so the two scopes line up. */
static void draw_rwr(struct scene *scene, cairo_t *cr, const struct layout *l) {
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
		double range = SCOPE_RADIUS * (0.86 - 0.62 * danger);
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
			SCOPE_RADIUS * (0.86 - 0.62 * danger), &x, &y);
		cairo_move_to(cr, x, y - 13);
		cairo_line_to(cr, x + 15, y);
		cairo_line_to(cr, x, y + 13);
		cairo_line_to(cr, x - 15, y);
		cairo_close_path(cr);
		cairo_set_line_width(cr, HUD_LINE);
		cairo_stroke(cr);
	}

	char status[24];
	if (threats == 0) {
		snprintf(status, sizeof(status), "CLEAR");
	} else {
		snprintf(status, sizeof(status), "%d ACTIVE", threats);
	}
	cairo_set_source_rgb(cr, HUD_DIM);
	draw_text(cr, status, cx - SCOPE_RADIUS, cy + SCOPE_RADIUS + 14);
}

/* ---- the parts that never change ---- */

static void draw_scope_frame(cairo_t *cr, const char *title,
		double cx, double cy, bool rwr) {
	/* Dark disc so clouds don't show through the instrument. */
	circle(cr, cx, cy, SCOPE_RADIUS);
	cairo_set_source_rgb(cr, HUD_BLACK);
	cairo_fill_preserve(cr);
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

	cairo_set_font_size(cr, 12);
	cairo_set_source_rgb(cr, HUD_GREEN);
	draw_text(cr, title, cx - SCOPE_RADIUS, cy - SCOPE_RADIUS - 12);
}

static void draw_background(struct scene *scene, cairo_t *cr) {
	struct layout l = layout_for(scene);

	/* Sky: near-black, a little lighter in a band across the middle. */
	cairo_pattern_t *sky = cairo_pattern_create_linear(0, 0, 0, scene->height);
	cairo_pattern_add_color_stop_rgb(sky, 0.0, HUD_BLACK);
	cairo_pattern_add_color_stop_rgb(sky, 0.45, 0.012, 0.060, 0.030);
	cairo_pattern_add_color_stop_rgb(sky, 1.0, HUD_BLACK);
	cairo_set_source(cr, sky);
	cairo_paint(cr);
	cairo_pattern_destroy(sky);

	cairo_select_font_face(cr, UI_FONT, CAIRO_FONT_SLANT_NORMAL,
		CAIRO_FONT_WEIGHT_NORMAL);
	draw_scope_frame(cr, "RADAR", l.radar_x, l.scope_y, false);
	draw_scope_frame(cr, "RWR", l.rwr_x, l.scope_y, true);
	draw_readout_frame(cr, "KCAS", 44, l.readout_y);
	draw_readout_frame(cr, "ALT FT", l.rwr_x + SCOPE_RADIUS - 86, l.readout_y);

	/* The caret under the heading tape marks the current heading. */
	cairo_set_source_rgb(cr, HUD_GREEN);
	cairo_set_line_width(cr, HUD_LINE);
	cairo_move_to(cr, l.cx, l.tape_y + 16);
	cairo_line_to(cr, l.cx - 7, l.tape_y + 28);
	cairo_line_to(cr, l.cx + 7, l.tape_y + 28);
	cairo_close_path(cr);
	cairo_stroke(cr);
}

/* ---- public functions (see wallpaper.h) ---- */

void scene_init(struct scene *scene, int width, int height) {
	scene_finish(scene);
	scene->width = width;
	scene->height = height;

	/* RGB24: no transparency needed, and copying it each frame is cheap. */
	scene->background = cairo_image_surface_create(CAIRO_FORMAT_RGB24,
		width, height);
	cairo_t *cr = cairo_create(scene->background);
	draw_background(scene, cr);
	cairo_destroy(cr);

	random_state = 20261115; /* same clouds every time */
	for (int i = 0; i < CLOUD_COUNT; i++) {
		/* Two of the clouds are "high": bigger, faster and fainter, and
		 * they pass over the aircraft. Things nearer the camera move
		 * faster, which is what gives a flat picture depth. */
		bool high = i >= CLOUD_COUNT - 2;
		cloud_info[i].high = high;
		cloud_info[i].width = (high ? 420 : 240) + (int)(random_unit() * 160);
		cloud_info[i].height = (high ? 220 : 120) + (int)(random_unit() * 80);
		cloud_info[i].x = random_unit() * width - cloud_info[i].width / 2.0;
		cloud_info[i].speed = (high ? 150 : 45) + random_unit() * 45;
		cloud_info[i].start = random_unit() * (height + cloud_info[i].height);
		scene->clouds[i] = make_cloud(cloud_info[i].width,
			cloud_info[i].height, high ? 0.07 : 0.11);
	}
}

void scene_finish(struct scene *scene) {
	if (scene->background != NULL) {
		cairo_surface_destroy(scene->background);
		scene->background = NULL;
	}
	for (int i = 0; i < CLOUD_COUNT; i++) {
		if (scene->clouds[i] != NULL) {
			cairo_surface_destroy(scene->clouds[i]);
			scene->clouds[i] = NULL;
		}
	}
}

void scene_draw(struct scene *scene, cairo_t *cr, double t) {
	struct layout l = layout_for(scene);
	processes_update(&scene->processes, t);

	/* Start from the cached background. SOURCE copies pixels instead of
	 * blending them, which is the fastest way to fill the whole frame. */
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_surface(cr, scene->background, 0, 0);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

	cairo_select_font_face(cr, UI_FONT, CAIRO_FONT_SLANT_NORMAL,
		CAIRO_FONT_WEIGHT_NORMAL);

	/* The clouds would otherwise drift over the instruments. Clip them to
	 * the sky: everything except the two scope discs. EVEN_ODD makes the
	 * circles holes in the rectangle. */
	cairo_save(cr);
	cairo_rectangle(cr, 0, 0, scene->width, scene->height);
	circle(cr, l.radar_x, l.scope_y, SCOPE_RADIUS + 1);
	circle(cr, l.rwr_x, l.scope_y, SCOPE_RADIUS + 1);
	cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
	cairo_clip(cr);
	draw_clouds(scene, cr, t, false);
	draw_formation(cr, &l, t);
	draw_clouds(scene, cr, t, true);
	cairo_restore(cr);

	draw_hud(cr, &l, t);
	draw_radar(scene, cr, &l, t);
	draw_rwr(scene, cr, &l);
	draw_target(scene, cr, &l);
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
	/* Click a contact to lock it; click it again, or anywhere else, to
	 * break the lock. hover_pid was worked out during the last draw. */
	if (scene->hover_pid != 0 && scene->hover_pid != scene->locked_pid) {
		scene->locked_pid = scene->hover_pid;
	} else {
		scene->locked_pid = 0;
	}
}
