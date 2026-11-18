/*
 * perkinsos-sysinfo: the first PerkinsOS app. A small window showing the
 * kernel version, uptime, memory use and load, refreshed every second.
 *
 * Unlike the dock and launcher this is an ordinary application window, so
 * it uses the xdg-shell protocol: wl_surface -> xdg_surface -> xdg_toplevel.
 * The compositor decides where the window goes; we only draw its contents.
 *
 * We draw our own title bar: drag it to move the window, click the X to
 * close. The compositor adds the border around the outside.
 */
#include <linux/input-event-codes.h> /* BTN_LEFT */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>

#include "apps.h"
#include "client.h"
#include "theme.h"
#include "xdg-shell-client-protocol.h"

#define DEFAULT_WIDTH 400
#define DEFAULT_HEIGHT 230
#define TITLE_HEIGHT 34

struct sysinfo {
	struct wl_display *display;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct xdg_wm_base *wm_base; /* the xdg-shell global */
	struct wl_seat *seat;
	struct wl_pointer *pointer;

	struct wl_surface *surface;
	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *toplevel;
	struct buffer *buffer;
	int width, height;
	bool configured;

	double pointer_x, pointer_y;
	bool running;
};

/* Read "Key:   12345 kB" style values from /proc/meminfo, in kilobytes. */
static long meminfo_kb(const char *key) {
	FILE *file = fopen("/proc/meminfo", "r");
	if (file == NULL) {
		return 0;
	}
	char line[128];
	long value = 0;
	size_t key_length = strlen(key);
	while (fgets(line, sizeof(line), file) != NULL) {
		if (strncmp(line, key, key_length) == 0 && line[key_length] == ':') {
			sscanf(line + key_length + 1, "%ld", &value);
			break;
		}
	}
	fclose(file);
	return value;
}

/* One "Label    value" row; returns the y for the next row. */
static double draw_row(cairo_t *cr, const char *label, const char *value,
		double y) {
	cairo_set_source_rgb(cr, HUD_DIM);
	draw_text(cr, label, 20, y);
	cairo_set_source_rgb(cr, HUD_GREEN);
	draw_text(cr, value, 130, y);
	return y + 30;
}

static void draw(struct sysinfo *app) {
	if (!app->configured) {
		return;
	}
	if (app->buffer == NULL || app->buffer->width != app->width ||
			app->buffer->height != app->height) {
		buffer_destroy(app->buffer);
		app->buffer = buffer_create(app->shm, app->width, app->height);
		if (app->buffer == NULL) {
			app->running = false;
			return;
		}
	}
	cairo_t *cr = app->buffer->cr;

	/* Body, then the title bar across the top. */
	cairo_set_source_rgb(cr, HUD_BLACK);
	cairo_paint(cr);
	cairo_set_source_rgb(cr, HUD_PANEL);
	cairo_rectangle(cr, 0, 0, app->width, TITLE_HEIGHT);
	cairo_fill(cr);
	/* A green rule under the title bar. */
	cairo_set_source_rgb(cr, HUD_DIM);
	cairo_rectangle(cr, 0, TITLE_HEIGHT - 1, app->width, 1);
	cairo_fill(cr);

	cairo_select_font_face(cr, UI_FONT, CAIRO_FONT_SLANT_NORMAL,
		CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, FONT_SIZE(13));
	cairo_set_source_rgb(cr, HUD_GREEN);
	draw_text(cr, "SYSTEM INFO", 12, TITLE_HEIGHT / 2.0);

	/* Close button: an X drawn as two lines in the top-right corner. */
	double cx = app->width - TITLE_HEIGHT / 2.0, cy = TITLE_HEIGHT / 2.0;
	cairo_set_line_width(cr, HUD_LINE);
	cairo_move_to(cr, cx - 5, cy - 5);
	cairo_line_to(cr, cx + 5, cy + 5);
	cairo_move_to(cr, cx + 5, cy - 5);
	cairo_line_to(cr, cx - 5, cy + 5);
	cairo_stroke(cr);

	cairo_select_font_face(cr, UI_FONT, CAIRO_FONT_SLANT_NORMAL,
		CAIRO_FONT_WEIGHT_NORMAL);

	/* Gather the numbers. Everything comes from the kernel: uname() and
	 * the text files it exposes under /proc. */
	char value[160]; /* fits two 65-byte utsname fields */
	double y = TITLE_HEIGHT + 34;

	y = draw_row(cr, "SYSTEM", "PerkinsOS", y);

	struct utsname uts;
	uname(&uts);
	snprintf(value, sizeof(value), "%s %s", uts.sysname, uts.release);
	y = draw_row(cr, "KERNEL", value, y);

	double uptime = 0;
	FILE *file = fopen("/proc/uptime", "r");
	if (file != NULL) {
		if (fscanf(file, "%lf", &uptime) != 1) {
			uptime = 0;
		}
		fclose(file);
	}
	int seconds = (int)uptime;
	snprintf(value, sizeof(value), "%d min %02d s", seconds / 60, seconds % 60);
	y = draw_row(cr, "UPTIME", value, y);

	long total = meminfo_kb("MemTotal");
	long available = meminfo_kb("MemAvailable");
	snprintf(value, sizeof(value), "%ld MB used of %ld MB",
		(total - available) / 1024, total / 1024);
	y = draw_row(cr, "MEMORY", value, y);

	double load = 0;
	file = fopen("/proc/loadavg", "r");
	if (file != NULL) {
		if (fscanf(file, "%lf", &load) != 1) {
			load = 0;
		}
		fclose(file);
	}
	snprintf(value, sizeof(value), "%.2f (1 min average)", load);
	draw_row(cr, "LOAD", value, y);

	cairo_surface_flush(app->buffer->cairo_surface);
	wl_surface_attach(app->surface, app->buffer->wl_buffer, 0, 0);
	wl_surface_damage_buffer(app->surface, 0, 0, app->width, app->height);
	wl_surface_commit(app->surface);
}

/* ---- xdg-shell events ---- */

static void wm_base_ping(void *data, struct xdg_wm_base *wm_base,
		uint32_t serial) {
	/* The compositor checks we are alive. A client that doesn't answer is
	 * considered frozen. */
	xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
	.ping = wm_base_ping,
};

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel,
		int32_t width, int32_t height, struct wl_array *states) {
	/* The compositor suggests a size. 0 means "you choose". This event
	 * only records it; the xdg_surface configure that follows applies it. */
	struct sysinfo *app = data;
	app->width = width > 0 ? width : DEFAULT_WIDTH;
	app->height = height > 0 ? height : DEFAULT_HEIGHT;
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel) {
	/* The compositor asks us to close (Alt+Q in PerkinsOS). */
	struct sysinfo *app = data;
	app->running = false;
}

static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = toplevel_configure,
	.close = toplevel_close,
};

static void xdg_surface_configure(void *data, struct xdg_surface *xdg_surface,
		uint32_t serial) {
	/* End of a batch of configure events. Acknowledge, then draw at the
	 * size toplevel_configure() recorded. */
	struct sysinfo *app = data;
	xdg_surface_ack_configure(xdg_surface, serial);
	app->configured = true;
	draw(app);
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure,
};

/* ---- pointer events ---- */

static void pointer_enter(void *data, struct wl_pointer *pointer,
		uint32_t serial, struct wl_surface *surface,
		wl_fixed_t x, wl_fixed_t y) {
	struct sysinfo *app = data;
	app->pointer_x = wl_fixed_to_double(x);
	app->pointer_y = wl_fixed_to_double(y);
}

static void pointer_leave(void *data, struct wl_pointer *pointer,
		uint32_t serial, struct wl_surface *surface) {
}

static void pointer_motion(void *data, struct wl_pointer *pointer,
		uint32_t time, wl_fixed_t x, wl_fixed_t y) {
	struct sysinfo *app = data;
	app->pointer_x = wl_fixed_to_double(x);
	app->pointer_y = wl_fixed_to_double(y);
}

static void pointer_button(void *data, struct wl_pointer *pointer,
		uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
	struct sysinfo *app = data;
	if (button != BTN_LEFT || state != WL_POINTER_BUTTON_STATE_PRESSED ||
			app->pointer_y >= TITLE_HEIGHT) {
		return;
	}
	if (app->pointer_x >= app->width - TITLE_HEIGHT) {
		app->running = false; /* the X */
	} else {
		/* A client can't move its own window: it doesn't even know where
		 * it is. It asks the compositor to start an interactive move,
		 * passing the serial of the click to prove the user asked. */
		xdg_toplevel_move(app->toplevel, app->seat, serial);
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
	struct sysinfo *app = data;
	if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && app->pointer == NULL) {
		app->pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(app->pointer, &pointer_listener, app);
	}
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
};

static void registry_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	struct sysinfo *app = data;
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		app->compositor = wl_registry_bind(registry, name,
			&wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		app->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
		app->wm_base = wl_registry_bind(registry, name,
			&xdg_wm_base_interface, 1);
		xdg_wm_base_add_listener(app->wm_base, &wm_base_listener, app);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		app->seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
		wl_seat_add_listener(app->seat, &seat_listener, app);
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
	struct sysinfo app = {
		.width = DEFAULT_WIDTH, .height = DEFAULT_HEIGHT, .running = true,
	};

	app.display = wl_display_connect(NULL);
	if (app.display == NULL) {
		fprintf(stderr, "sysinfo: cannot connect to a Wayland compositor\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(app.display);
	wl_registry_add_listener(registry, &registry_listener, &app);
	wl_display_roundtrip(app.display);
	if (app.compositor == NULL || app.shm == NULL || app.wm_base == NULL) {
		fprintf(stderr, "sysinfo: compositor is missing a required global\n");
		return 1;
	}

	/* Surface -> xdg_surface (adds the configure handshake and window
	 * geometry) -> xdg_toplevel (makes it a top-level application window). */
	app.surface = wl_compositor_create_surface(app.compositor);
	app.xdg_surface = xdg_wm_base_get_xdg_surface(app.wm_base, app.surface);
	xdg_surface_add_listener(app.xdg_surface, &xdg_surface_listener, &app);
	app.toplevel = xdg_surface_get_toplevel(app.xdg_surface);
	xdg_toplevel_add_listener(app.toplevel, &toplevel_listener, &app);
	xdg_toplevel_set_title(app.toplevel, "System Info");
	xdg_toplevel_set_app_id(app.toplevel, "perkinsos-sysinfo");

	/* First commit without a buffer; the configure events start things. */
	wl_surface_commit(app.surface);

	time_t last_draw = time(NULL);
	while (app.running) {
		/* Wake at least once a second to refresh the numbers. */
		if (dispatch_with_timeout(app.display, 1000) < 0) {
			break;
		}
		/* Events such as pointer motion also wake us; only redraw when
		 * the second has actually changed. */
		time_t now = time(NULL);
		if (now != last_draw) {
			last_draw = now;
			draw(&app);
		}
	}

	buffer_destroy(app.buffer);
	wl_display_disconnect(app.display);
	return 0;
}
