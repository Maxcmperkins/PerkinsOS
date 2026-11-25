/*
 * perkinsos-wallpaper is split over nine files:
 *   main.c   talking to the compositor: surface, buffers, frame timing, mouse
 *   scene.c  drawing one frame: HUD, cockpit, instruments
 *   sky.c    the view outside, written pixel by pixel
 *   model.c  the aircraft: 3D models and the code that draws them
 *   surface.c  light and shadow on the cockpit
 *   vapour.c   the trails from the wing tips
 *   pages.c  the pages of the centre display
 *   startup.c  the cockpit powering up
 *   procs.c  reading the list of running programs from /proc
 * This header is what they share.
 */
#ifndef PERKINSOS_WALLPAPER_H
#define PERKINSOS_WALLPAPER_H

#include <stdbool.h>
#include <stdint.h>
#include <cairo.h>

#include "apps.h"
#include "machine.h"

/* ---- procs.c: the running programs ---- */

#define MAX_PROCESSES 64

struct process {
	int pid;
	char name[16];             /* the kernel's short name for the program */
	long memory_kb;            /* memory it is really using right now */
	double cpu_percent;        /* share of one CPU over the last sample */
	unsigned long long ticks;  /* total CPU time so far, in clock ticks */
	/* Where the radar last drew this process, for mouse hit-testing. */
	double radar_x, radar_y;
};

struct process_list {
	struct process items[MAX_PROCESSES];
	int count;
	double sampled_at; /* time of the last sample, in seconds */

	/* Whole-machine figures, sampled at the same time, for the dials. */
	double cpu_fraction;     /* 0..1: how busy the CPUs are */
	double memory_fraction;  /* 0..1: how much memory is in use */
	double load_average;     /* programs wanting the CPU, 1-minute average */
};

/* Re-read /proc if the last sample is older than two seconds. `now` is a
 * steadily increasing time in seconds. */
void processes_update(struct process_list *list, double now);

/* ---- sky.c: the view outside, and fast image overlay ---- */

struct sky_view {
	int width, rows;   /* size of the area to fill, in screen pixels */
	double horizon;    /* row the horizon is on */
	double drift;      /* how far we have slid sideways, in metres */
	double time;       /* seconds; drives the motion */
	double scale;      /* screen pixels per design unit (see scene.c) */
};

/* Build the cloud texture. Call once before sky_render(). */
void sky_init(void);

/* Fill the top `rows` rows of a frame with sky, sea and cloud. `pixels`
 * is the frame's memory, 32 bits per pixel, `stride_pixels` per row. */
void sky_render(uint32_t *pixels, int stride_pixels, const struct sky_view *view);

/* A picture with transparent parts, prepared so it can be laid over a
 * frame quickly: the non-transparent stretches of each row are found once,
 * and only those are touched each frame. */
struct overlay {
	const uint32_t *image;
	int stride_pixels;
	void *spans;
	int count;
};

struct overlay *overlay_create(const uint32_t *image, int stride_pixels,
	int width, int height);
void overlay_destroy(struct overlay *overlay);
/* Lay it over a width x height frame with its top-left corner at x,y
 * (either may be negative). What falls outside the frame is left off. */
void overlay_blit(const struct overlay *overlay, uint32_t *pixels,
	int stride_pixels, int x, int y, int width, int height);

/* ---- surface.c: light and shadow on the cockpit ---- */

/* How many design units tall a height of 1.0 in the relief is. */
#define RELIEF_UNITS 40.0f

void surface_light(cairo_surface_t *image, cairo_surface_t *relief,
	double scale);

/* ---- model.c: the aircraft ---- */

enum { MODEL_F35, MODEL_TYPHOON, MODEL_COUNT };

/* Where an aircraft is and which way it is turned. Distances are metres
 * from the camera: x right, y up, z ahead. Angles are radians. */
struct model_pose {
	double x, y, z;
	double roll, pitch, yaw;
	/* The camera: `focal` is how many screen pixels one metre covers at
	 * one metre away (a bigger number is a longer lens), and screen_x,
	 * screen_y is the pixel the camera points at. */
	double focal;
	double screen_x, screen_y;
	double glow;   /* brightness of the exhaust, around 1.0 */
};

/* Load the aircraft. Call once before model_draw(). An
 * aircraft whose file is missing is left out of the picture. */
void models_init(void);

/* Draw one aircraft straight into a frame's pixels. */
void model_draw(int which, const struct model_pose *pose, uint32_t *frame,
	int stride_pixels, int frame_width, int frame_height);

/* Where one of its wing tips is, in metres from the camera: x right, y up,
 * z ahead. `side` is 0 for the left one, 1 for the right. Returns false if
 * the aircraft isn't loaded. */
bool model_wing_tip(int which, int side, const struct model_pose *pose,
	double tip[3]);

/* ---- vapour.c: the trails from the wing tips ---- */

/* Draw one trail, on the top `rows` rows of the frame. `strength` is 0 to 1. */
void vapour_trail(const struct model_pose *pose, const double tip[3],
	double strength, double time, uint32_t *frame, int stride_pixels,
	int width, int rows);

/* ---- pages.c: the centre display ---- */

enum { PAGE_HOME, PAGE_APPS, PAGE_SYSTEM };

/* Buttons round the display. Their slots are 0 to PAGE_KEYS - 1; higher
 * slots are entries in the list on the APPS page. */
#define PAGE_KEYS 10

struct pages {
	int current;               /* which page is showing */
	int first;                 /* APPS: the first app on show */
	struct app_list pinned;    /* read once */
	struct app_list all;       /* read each time APPS is opened */
	struct machine machine;    /* SYSTEM: the figures, */
	double machine_read_at;    /* and when they were read; 0 means never */
};

/* Where the display's glass is, in design units. */
struct page_area {
	double x, y, w, h;
};

void pages_init(struct pages *pages);
void pages_show(struct pages *pages, int page);

/* The name of a slot, or NULL if it does nothing on this page. */
const char *pages_label(const struct pages *pages, int slot);
void pages_press(struct pages *pages, int slot);
/* The list entry at a point, or -1. */
int pages_slot_at(const struct pages *pages, const struct page_area *area,
	double x, double y);
/* Draw what is between the rows of names. PAGE_HOME is scene.c's. */
void pages_draw(struct pages *pages, cairo_t *cr,
	const struct page_area *area, int hover, double t);

/* ---- startup.c: the cockpit powering up ---- */

/* `age` is the seconds since the first frame. */
#define STARTUP_LENGTH 3.4

enum { STARTUP_RADAR, STARTUP_CENTRE, STARTUP_RWR, STARTUP_HUD };

double startup_darkness(double age);          /* of the panel, 0 to 1 */
bool startup_lit(double age, int part);       /* is this screen on? */
double startup_dial(double age, double fraction); /* what a dial shows */
double startup_gauge(double age);             /* degrees to turn its needle */
bool startup_testing(double age);             /* is the self test showing? */
void startup_draw_test(cairo_t *cr, const struct page_area *area,
	const struct machine *machine, int programs, double age);

/* ---- scene.c: the picture ---- */

struct scene {
	int width, height;   /* the screen, in pixels */
	/* The cockpit is designed on a canvas 768 units tall and as wide as
	 * the screen's shape requires. `scale` is screen pixels per unit, so
	 * the same drawing fills a 1024x768 screen or a 1920x1080 one. */
	double scale;

	/* The cockpit (canopy frame, instrument panel, screen bezels, keys)
	 * never changes, so it is drawn once and reused every frame. It is
	 * transparent where you look out. All drawing here is done by the
	 * CPU, so the less that is redrawn the better. */
	cairo_surface_t *cockpit;
	struct overlay *cockpit_overlay;
	/* The same for the glass over the screens and dials, which goes on
	 * after the needles: they are behind it. */
	cairo_surface_t *glass;
	struct overlay *glass_overlay;

	/* A patch of sunlight that slides over the panel, and the view behind
	 * you, which the mirrors show. */
	cairo_surface_t *sunlight;
	struct overlay *sunlight_overlay;
	cairo_surface_t *behind;

	/* Your head is never quite still, so the cockpit moves a little
	 * against the view outside. How far it is from its usual place now, in
	 * whole screen pixels, so that everything stays sharp. */
	int head_x, head_y;
	/* The cached pictures are bigger than the screen by this much on every
	 * side, so that there is always something to show at the edges. */
	int margin;

	struct process_list processes;
	struct pages pages;

	/* The mouse in screen pixels, and where on the cockpit that is in
	 * design units, allowing for the head movement. hover_pid is the radar
	 * contact under the pointer and locked_pid the one that was clicked; 0
	 * means none.
	 * hover_key is the slot (see struct pages) under the pointer, or -1. */
	double pointer_screen_x, pointer_screen_y;
	double pointer_x, pointer_y;
	bool pointer_inside;
	int hover_pid;
	int locked_pid;
	int hover_key;

	/* For the start-up sequence: when the first frame was. */
	bool started;
	double started_at;
	double now;             /* the time of the last frame */

	/* The key last pressed, and when. Also the page that was showing in
	 * the last frame, and when it last changed, for the redrawing effect. */
	int pressed_key;
	double pressed_at;
	int shown_page, shown_first;
	double page_changed_at;

	/* Seconds spent in each stage of drawing, added up since the last
	 * report, so the log can say where the time goes. */
	double time_sky, time_models, time_cockpit, time_vector;
};

/* Prepare the cached drawings for a width x height screen. */
void scene_init(struct scene *scene, int width, int height);
void scene_finish(struct scene *scene);

/* Draw the frame for time `t` (seconds since the wallpaper started). */
void scene_draw(struct scene *scene, cairo_t *cr, double t);

/* The pointer moved to x,y (screen pixels) / left the surface / the left
 * button was pressed. */
void scene_pointer_motion(struct scene *scene, double x, double y);
void scene_pointer_leave(struct scene *scene);
void scene_pointer_click(struct scene *scene);

#endif
