/*
 * perkinsos-wallpaper is split over five files:
 *   main.c   talking to the compositor: surface, buffers, frame timing, mouse
 *   scene.c  drawing one frame: HUD, cockpit, instruments
 *   sky.c    the view outside, written pixel by pixel
 *   model.c  the aircraft: 3D models and the code that draws them
 *   procs.c  reading the list of running programs from /proc
 * This header is what they share.
 */
#ifndef PERKINSOS_WALLPAPER_H
#define PERKINSOS_WALLPAPER_H

#include <stdbool.h>
#include <stdint.h>
#include <cairo.h>

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
	double pan;        /* sideways shift in pixels, from the heading */
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
void overlay_blit(const struct overlay *overlay, uint32_t *pixels,
	int stride_pixels);

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

	struct process_list processes;

	/* The mouse, in design units. hover_pid is the radar contact under
	 * the pointer and locked_pid the one that was clicked; 0 means none.
	 * hover_key is the cockpit key under the pointer, or -1. */
	double pointer_x, pointer_y;
	bool pointer_inside;
	int hover_pid;
	int locked_pid;
	int hover_key;

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
