/*
 * perkinsos-wallpaper is split over three files:
 *   main.c   talking to the compositor: surface, buffers, frame timing, mouse
 *   scene.c  drawing one frame of the picture
 *   procs.c  reading the list of running programs from /proc
 * This header is what they share.
 */
#ifndef PERKINSOS_WALLPAPER_H
#define PERKINSOS_WALLPAPER_H

#include <stdbool.h>
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
};

/* Re-read /proc if the last sample is older than two seconds. `now` is a
 * steadily increasing time in seconds. */
void processes_update(struct process_list *list, double now);

/* ---- scene.c: the picture ---- */

#define CLOUD_COUNT 9

struct scene {
	int width, height;

	/* Things that never change are drawn once into `background`; each
	 * frame starts by copying it. Clouds are pre-drawn sprites that are
	 * only moved. Both exist to keep the per-frame work small, because
	 * all drawing here is done by the CPU. */
	cairo_surface_t *background;
	cairo_surface_t *clouds[CLOUD_COUNT];

	struct process_list processes;

	/* The mouse. hover_pid is the radar contact under the pointer and
	 * locked_pid the one that was clicked; 0 means none. */
	double pointer_x, pointer_y;
	bool pointer_inside;
	int hover_pid;
	int locked_pid;
};

/* Prepare the cached drawings for a width x height screen. */
void scene_init(struct scene *scene, int width, int height);
void scene_finish(struct scene *scene);

/* Draw the frame for time `t` (seconds since the wallpaper started). */
void scene_draw(struct scene *scene, cairo_t *cr, double t);

/* The pointer moved to x,y / left the surface / the left button was pressed. */
void scene_pointer_motion(struct scene *scene, double x, double y);
void scene_pointer_leave(struct scene *scene);
void scene_pointer_click(struct scene *scene);

#endif
