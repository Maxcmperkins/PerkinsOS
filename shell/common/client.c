/* See client.h for what each function is for. */
#define _GNU_SOURCE /* memfd_create */
#include <math.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#include "client.h"

struct buffer *buffer_create(struct wl_shm *shm, int width, int height) {
	/* 4 bytes per pixel. "stride" is the number of bytes in one row. */
	int stride = width * 4;
	size_t size = (size_t)stride * height;

	/* memfd_create makes a file that exists only in memory. It has no
	 * name on disk, but it has a file descriptor, and a file descriptor
	 * can be passed to another process over a Unix socket. */
	int fd = memfd_create("perkinsos-buffer", MFD_CLOEXEC);
	if (fd < 0) {
		return NULL;
	}
	if (ftruncate(fd, size) < 0) { /* give the file its size */
		close(fd);
		return NULL;
	}
	/* Map the file into our address space so we can write pixels to it.
	 * MAP_SHARED: writes are visible to everyone else who maps it. */
	void *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (pixels == MAP_FAILED) {
		close(fd);
		return NULL;
	}

	struct buffer *buffer = calloc(1, sizeof(*buffer));
	buffer->pixels = pixels;
	buffer->size = size;
	buffer->width = width;
	buffer->height = height;

	/* Hand the fd to the compositor (a "pool"), then describe one buffer
	 * inside it. After that the pool object and our copy of the fd are no
	 * longer needed: the compositor has its own mapping. */
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	buffer->wl_buffer = wl_shm_pool_create_buffer(pool, 0, width, height,
		stride, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);

	/* Cairo's ARGB32 and Wayland's ARGB8888 are the same layout in memory,
	 * so cairo can draw straight into the shared pixels. */
	buffer->cairo_surface = cairo_image_surface_create_for_data(pixels,
		CAIRO_FORMAT_ARGB32, width, height, stride);
	buffer->cr = cairo_create(buffer->cairo_surface);
	return buffer;
}

void buffer_destroy(struct buffer *buffer) {
	if (buffer == NULL) {
		return;
	}
	cairo_destroy(buffer->cr);
	cairo_surface_destroy(buffer->cairo_surface);
	wl_buffer_destroy(buffer->wl_buffer);
	munmap(buffer->pixels, buffer->size);
	free(buffer);
}

int dispatch_with_timeout(struct wl_display *display, int timeout_ms) {
	/* wl_display_dispatch() would block until an event arrives, with no
	 * way to wake up for a timer. This is the same thing split into its
	 * steps, with poll() in the middle so we can give up after a while.
	 *
	 * prepare_read fails if events are already queued; handle those first. */
	while (wl_display_prepare_read(display) != 0) {
		if (wl_display_dispatch_pending(display) < 0) {
			return -1;
		}
	}
	/* Send our own pending requests before waiting for replies. */
	if (wl_display_flush(display) < 0) {
		wl_display_cancel_read(display);
		return -1;
	}
	struct pollfd pfd = { .fd = wl_display_get_fd(display), .events = POLLIN };
	if (poll(&pfd, 1, timeout_ms) > 0) {
		if (wl_display_read_events(display) < 0) {
			return -1;
		}
	} else {
		wl_display_cancel_read(display); /* timed out: nothing to read */
	}
	/* Run the listener functions for whatever was read. */
	return wl_display_dispatch_pending(display) < 0 ? -1 : 0;
}

void spawn(const char *command) {
	/* fork() returns 0 in the new process. Only that copy runs exec,
	 * which replaces it with the shell running our command. */
	if (fork() == 0) {
		setsid();
		execl("/bin/sh", "/bin/sh", "-c", command, (void *)NULL);
		_exit(1); /* only reached if exec failed */
	}
}

void rounded_rect(cairo_t *cr, double x, double y, double w, double h,
		double radius) {
	/* Four quarter-circle arcs; cairo joins them with straight lines. */
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - radius, y + radius, radius, -M_PI / 2, 0);
	cairo_arc(cr, x + w - radius, y + h - radius, radius, 0, M_PI / 2);
	cairo_arc(cr, x + radius, y + h - radius, radius, M_PI / 2, M_PI);
	cairo_arc(cr, x + radius, y + radius, radius, M_PI, 3 * M_PI / 2);
	cairo_close_path(cr);
}

void draw_text(cairo_t *cr, const char *text, double x, double y_centre) {
	/* Font metrics, not the text's own extents, so that every string sits
	 * on the same baseline whether or not it has tall or hanging letters. */
	cairo_font_extents_t font;
	cairo_font_extents(cr, &font);
	cairo_move_to(cr, x, y_centre + (font.ascent - font.descent) / 2);
	cairo_show_text(cr, text);
}

double text_width(cairo_t *cr, const char *text) {
	cairo_text_extents_t extents;
	cairo_text_extents(cr, text, &extents);
	return extents.x_advance;
}
