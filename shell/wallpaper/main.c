/*
 * perkinsos-wallpaper: the animated desktop background. Three jets in
 * formation over drifting cloud, a heading tape, and a radar and RWR that
 * show the machine's running programs. What is drawn lives in scene.c;
 * this file is the conversation with the compositor.
 *
 * It is a layer-shell client like the dock, on the "background" layer and
 * anchored to all four edges, so the compositor gives it the whole screen
 * and stacks it behind every window.
 *
 * Compared with the dock, three things are new because this one animates:
 *
 *   Frame callbacks. We never draw on a timer of our own. After each frame
 *   we ask the compositor "tell me when it's a good time to draw the next
 *   one" (wl_surface.frame). It answers once per screen refresh, and only
 *   while the surface is visible, so a wallpaper hidden behind a
 *   maximized window stops drawing and costs nothing.
 *
 *   A frame-rate cap. Every pixel here is drawn by the CPU, so drawing at
 *   the screen's full refresh rate would waste it. We skip callbacks until
 *   1/FRAMES_PER_SECOND has passed.
 *
 *   Two buffers. The compositor may still be reading the buffer we last
 *   gave it, so we draw the next frame into the other one and swap.
 */
#include <linux/input-event-codes.h> /* BTN_LEFT */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "client.h"
#include "theme.h"
#include "wallpaper.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#define FRAMES_PER_SECOND 12

struct wallpaper {
	struct wl_display *display;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct zwlr_layer_shell_v1 *layer_shell;
	struct wl_seat *seat;
	struct wl_pointer *pointer;

	struct wl_surface *surface;
	struct zwlr_layer_surface_v1 *layer_surface;
	struct buffer *buffers[2];
	int next_buffer;          /* which of the two to draw into next */
	int width, height;

	struct scene scene;
	double started_at;        /* clock reading when we started */
	double last_frame_at;     /* when the last frame was drawn */
	bool frame_requested;     /* waiting for the compositor's go-ahead */
	bool running;
};

/* Seconds on a clock that only ever goes forwards (unlike the wall clock,
 * which jumps when the time is set). */
static double now_seconds(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void draw(struct wallpaper *wallpaper);

/* ---- frame callback ---- */

static void frame_done(void *data, struct wl_callback *callback,
		uint32_t time) {
	/* The compositor has shown our last frame and is ready for another.
	 * A callback object fires once, so destroy it; draw() asks for a new
	 * one each time. */
	struct wallpaper *wallpaper = data;
	wl_callback_destroy(callback);
	wallpaper->frame_requested = false;
}

static const struct wl_callback_listener frame_listener = {
	.done = frame_done,
};

static void draw(struct wallpaper *wallpaper) {
	int w = wallpaper->width, h = wallpaper->height;
	if (w == 0 || h == 0) {
		return; /* not configured yet */
	}
	/* Pick the buffer the compositor is not showing, and make sure it is
	 * the right size. */
	struct buffer **slot = &wallpaper->buffers[wallpaper->next_buffer];
	if (*slot == NULL || (*slot)->width != w || (*slot)->height != h) {
		buffer_destroy(*slot);
		*slot = buffer_create(wallpaper->shm, w, h);
		if (*slot == NULL) {
			wallpaper->running = false;
			return;
		}
	}
	struct buffer *buffer = *slot;
	wallpaper->next_buffer = 1 - wallpaper->next_buffer;

	double now = now_seconds();
	scene_draw(&wallpaper->scene, buffer->cr, now - wallpaper->started_at);
	wallpaper->last_frame_at = now;
	cairo_surface_flush(buffer->cairo_surface);

	/* Ask to be told when to draw again. The request is part of the
	 * commit below, so it has to come before it. */
	struct wl_callback *callback = wl_surface_frame(wallpaper->surface);
	wl_callback_add_listener(callback, &frame_listener, wallpaper);
	wallpaper->frame_requested = true;

	wl_surface_attach(wallpaper->surface, buffer->wl_buffer, 0, 0);
	wl_surface_damage_buffer(wallpaper->surface, 0, 0, w, h);
	wl_surface_commit(wallpaper->surface);
}

/* ---- layer surface events ---- */

static void layer_surface_configure(void *data,
		struct zwlr_layer_surface_v1 *layer_surface,
		uint32_t serial, uint32_t width, uint32_t height) {
	struct wallpaper *wallpaper = data;
	zwlr_layer_surface_v1_ack_configure(layer_surface, serial);
	/* The compositor re-sends configure whenever it rearranges the layers
	 * (for example when the dock appears). Only a change of size matters. */
	if ((int)width == wallpaper->width && (int)height == wallpaper->height) {
		return;
	}
	wallpaper->width = width;
	wallpaper->height = height;
	scene_init(&wallpaper->scene, width, height);
	draw(wallpaper);
}

static void layer_surface_closed(void *data,
		struct zwlr_layer_surface_v1 *layer_surface) {
	struct wallpaper *wallpaper = data;
	wallpaper->running = false;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = layer_surface_configure,
	.closed = layer_surface_closed,
};

/* ---- pointer events ----
 * These only update the scene's idea of where the mouse is. The picture is
 * redrawn many times a second anyway, so the next frame shows the change. */

static void pointer_enter(void *data, struct wl_pointer *pointer,
		uint32_t serial, struct wl_surface *surface,
		wl_fixed_t x, wl_fixed_t y) {
	struct wallpaper *wallpaper = data;
	scene_pointer_motion(&wallpaper->scene,
		wl_fixed_to_double(x), wl_fixed_to_double(y));
}

static void pointer_leave(void *data, struct wl_pointer *pointer,
		uint32_t serial, struct wl_surface *surface) {
	/* The pointer moved onto a window or the dock. */
	struct wallpaper *wallpaper = data;
	scene_pointer_leave(&wallpaper->scene);
}

static void pointer_motion(void *data, struct wl_pointer *pointer,
		uint32_t time, wl_fixed_t x, wl_fixed_t y) {
	struct wallpaper *wallpaper = data;
	scene_pointer_motion(&wallpaper->scene,
		wl_fixed_to_double(x), wl_fixed_to_double(y));
}

static void pointer_button(void *data, struct wl_pointer *pointer,
		uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
	struct wallpaper *wallpaper = data;
	if (button == BTN_LEFT && state == WL_POINTER_BUTTON_STATE_PRESSED) {
		scene_pointer_click(&wallpaper->scene);
	}
}

static void pointer_axis(void *data, struct wl_pointer *pointer,
		uint32_t time, uint32_t axis, wl_fixed_t value) {
}

static const struct wl_pointer_listener pointer_listener = {
	.enter = pointer_enter,
	.leave = pointer_leave,
	.motion = pointer_motion,
	.button = pointer_button,
	.axis = pointer_axis,
};

/* ---- seat and registry events ---- */

static void seat_capabilities(void *data, struct wl_seat *seat,
		uint32_t capabilities) {
	struct wallpaper *wallpaper = data;
	if ((capabilities & WL_SEAT_CAPABILITY_POINTER) &&
			wallpaper->pointer == NULL) {
		wallpaper->pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(wallpaper->pointer, &pointer_listener,
			wallpaper);
	}
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
};

static void registry_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	struct wallpaper *wallpaper = data;
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		wallpaper->compositor = wl_registry_bind(registry, name,
			&wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		wallpaper->shm = wl_registry_bind(registry, name,
			&wl_shm_interface, 1);
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		wallpaper->layer_shell = wl_registry_bind(registry, name,
			&zwlr_layer_shell_v1_interface, 1);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		wallpaper->seat = wl_registry_bind(registry, name,
			&wl_seat_interface, 1);
		wl_seat_add_listener(wallpaper->seat, &seat_listener, wallpaper);
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry,
		uint32_t name) {
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

int main(void) {
	struct wallpaper wallpaper = { .running = true };
	wallpaper.started_at = now_seconds();

	wallpaper.display = wl_display_connect(NULL);
	if (wallpaper.display == NULL) {
		fprintf(stderr, "wallpaper: cannot connect to a Wayland compositor\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(wallpaper.display);
	wl_registry_add_listener(registry, &registry_listener, &wallpaper);
	wl_display_roundtrip(wallpaper.display);
	if (wallpaper.compositor == NULL || wallpaper.shm == NULL ||
			wallpaper.layer_shell == NULL) {
		fprintf(stderr, "wallpaper: compositor lacks layer-shell or a core global\n");
		return 1;
	}

	wallpaper.surface = wl_compositor_create_surface(wallpaper.compositor);
	wallpaper.layer_surface = zwlr_layer_shell_v1_get_layer_surface(
		wallpaper.layer_shell, wallpaper.surface, NULL,
		ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, "wallpaper");
	zwlr_layer_surface_v1_add_listener(wallpaper.layer_surface,
		&layer_surface_listener, &wallpaper);
	/* Anchored to all four edges with size 0x0: "make me as big as the
	 * screen". Exclusive zone -1 means "ignore other surfaces' reserved
	 * space", so we extend underneath the dock instead of stopping at it. */
	zwlr_layer_surface_v1_set_anchor(wallpaper.layer_surface,
		ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	zwlr_layer_surface_v1_set_size(wallpaper.layer_surface, 0, 0);
	zwlr_layer_surface_v1_set_exclusive_zone(wallpaper.layer_surface, -1);
	wl_surface_commit(wallpaper.surface);

	const double frame_interval = 1.0 / FRAMES_PER_SECOND;
	while (wallpaper.running) {
		/* How long until the next frame is due? While we are waiting for
		 * the compositor's go-ahead there is no deadline: sleep until an
		 * event arrives (-1). */
		int timeout_ms = -1;
		if (!wallpaper.frame_requested) {
			double due = wallpaper.last_frame_at + frame_interval;
			double wait = due - now_seconds();
			timeout_ms = wait > 0 ? (int)(wait * 1000) + 1 : 0;
		}
		if (dispatch_with_timeout(wallpaper.display, timeout_ms) < 0) {
			break;
		}
		/* Draw when both are true: the compositor is ready for a frame,
		 * and enough time has passed since the last one. */
		if (!wallpaper.frame_requested &&
				now_seconds() - wallpaper.last_frame_at >= frame_interval) {
			draw(&wallpaper);
		}
	}

	scene_finish(&wallpaper.scene);
	buffer_destroy(wallpaper.buffers[0]);
	buffer_destroy(wallpaper.buffers[1]);
	wl_display_disconnect(wallpaper.display);
	return 0;
}
