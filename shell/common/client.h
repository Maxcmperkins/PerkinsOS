/*
 * Helpers shared by every PerkinsOS client (dock, launcher, apps).
 * Files in shell/common/ are compiled into each program.
 */
#ifndef PERKINSOS_CLIENT_H
#define PERKINSOS_CLIENT_H

#include <stddef.h>
#include <stdint.h>
#include <cairo.h>
#include <wayland-client.h>

/*
 * A block of pixels shared between us and the compositor.
 *
 * We draw into `pixels` (through cairo). The compositor reads the same
 * memory to put it on screen: nothing is copied over the socket, only a
 * file descriptor naming the memory is sent once, when it is created.
 */
struct buffer {
	struct wl_buffer *wl_buffer;   /* the compositor's handle for it */
	void *pixels;                  /* our mapping of the memory */
	size_t size;                   /* bytes */
	int width, height;
	cairo_surface_t *cairo_surface; /* cairo's view of the same pixels */
	cairo_t *cr;                   /* drawing context: use this to draw */
};

/* Create a width x height buffer of 32-bit ARGB pixels. NULL on failure. */
struct buffer *buffer_create(struct wl_shm *shm, int width, int height);
void buffer_destroy(struct buffer *buffer);

/*
 * Handle Wayland events for up to timeout_ms milliseconds (-1 = no limit).
 * Returns -1 if the connection to the compositor is gone, otherwise 0.
 * Returning after a timeout lets a client redraw on a timer (a clock).
 */
int dispatch_with_timeout(struct wl_display *display, int timeout_ms);

/* Start a shell command as a separate program and return immediately. */
void spawn(const char *command);

/* Path of a rounded rectangle; follow with cairo_fill() or cairo_stroke(). */
void rounded_rect(cairo_t *cr, double x, double y, double w, double h,
	double radius);

/* Draw text with its left edge at x, vertically centred on y_centre. */
void draw_text(cairo_t *cr, const char *text, double x, double y_centre);
/* Width in pixels that draw_text() would use for this text. */
double text_width(cairo_t *cr, const char *text);

#endif
