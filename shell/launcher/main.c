/*
 * perkinsos-launcher: a search box in the middle of the screen.
 * Type to filter the list of installed apps (common/apps.c), Up/Down to choose, Enter to start the app,
 * Escape to cancel. Or point at a result and click it. It runs once and exits; the dock's "Apps" button and
 * the compositor's Alt+P start a new one each time.
 *
 * Like the dock it is a layer-shell client, with two differences:
 *   - it sits on the overlay layer, anchored to no edge, so the compositor
 *     centres it and draws it above everything
 *   - it asks for exclusive keyboard interactivity, so the compositor
 *     sends it every key press while it is open
 *
 * New compared with the dock: turning key presses into characters, which
 * is the job of the xkbcommon library.
 */
#define _GNU_SOURCE /* strcasestr */
#include <linux/input-event-codes.h> /* BTN_LEFT */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

#include "apps.h"
#include "client.h"
#include "theme.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#define WIDTH 420
#define PADDING 16
#define ROW_HEIGHT 40
/* At most this many results are on show at once; the list scrolls. */
#define MAX_ROWS 8

struct launcher {
	struct wl_display *display;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct zwlr_layer_shell_v1 *layer_shell;
	struct wl_seat *seat;
	struct wl_keyboard *keyboard;
	struct wl_pointer *pointer;

	struct wl_surface *surface;
	struct zwlr_layer_surface_v1 *layer_surface;
	struct buffer *buffer;
	bool configured;

	/* xkbcommon: the keymap says which symbol each key produces; the state
	 * tracks which modifiers (Shift, Caps Lock...) are active right now. */
	struct xkb_context *xkb_context;
	struct xkb_keymap *xkb_keymap;
	struct xkb_state *xkb_state;

	/* Key repeat. The compositor only reports a key going down and coming
	 * up; repeating it while it is held is each client's own job, at the
	 * speed the compositor asks for (keyboard_repeat_info()). */
	int32_t repeat_rate;     /* repeats per second; 0 means never */
	int32_t repeat_delay;    /* ms a key is held before the first repeat */
	uint32_t repeat_key;     /* the key being held, or 0 */
	int64_t repeat_at;       /* when it repeats next, in now_ms() time */

	double pointer_x, pointer_y;
	int pressed_app;         /* index into apps the button went down on, or -1 */

	char query[32];          /* what the user has typed */
	struct app_list apps;    /* every app: pinned, then installed */
	int *matches;            /* indexes into apps that match the query */
	int match_count;
	int selected;            /* index into matches[] */
	int first;               /* index into matches[] of the top row on show */
	int rows;                /* result rows that fit: at most MAX_ROWS */
	int height;              /* search box + rows + padding round them */
	bool running;
};

/* Rebuild matches[]: every app whose name contains the query. */
static void filter(struct launcher *launcher) {
	launcher->match_count = 0;
	for (int i = 0; i < launcher->apps.count; i++) {
		/* strcasestr: find a substring, ignoring upper/lower case. An
		 * empty query is found in every name, so everything matches. */
		if (strcasestr(launcher->apps.items[i].name, launcher->query) != NULL) {
			launcher->matches[launcher->match_count++] = i;
		}
	}
	/* Keep the selection inside the (possibly shorter) list. */
	if (launcher->selected >= launcher->match_count) {
		launcher->selected = launcher->match_count > 0 ?
			launcher->match_count - 1 : 0;
	}
	/* Scroll just far enough that the selected row is on show. */
	if (launcher->selected < launcher->first) {
		launcher->first = launcher->selected;
	}
	if (launcher->selected >= launcher->first + launcher->rows) {
		launcher->first = launcher->selected - launcher->rows + 1;
	}
}

static void draw(struct launcher *launcher) {
	if (!launcher->configured) {
		return;
	}
	if (launcher->buffer == NULL) {
		launcher->buffer = buffer_create(launcher->shm, WIDTH,
			launcher->height);
		if (launcher->buffer == NULL) {
			launcher->running = false;
			return;
		}
	}
	cairo_t *cr = launcher->buffer->cr;

	/* Clear to fully transparent first, so the rounded corners show the
	 * desktop behind. OPERATOR_SOURCE replaces pixels instead of blending. */
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 0, 0, 0, 0);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

	/* Panel with a green outline. */
	rounded_rect(cr, 1, 1, WIDTH - 2, launcher->height - 2, 4);
	cairo_set_source_rgb(cr, HUD_BLACK);
	cairo_fill_preserve(cr);
	cairo_set_source_rgb(cr, HUD_GREEN);
	cairo_set_line_width(cr, HUD_LINE);
	cairo_stroke(cr);

	cairo_select_font_face(cr, UI_FONT, CAIRO_FONT_SLANT_NORMAL,
		CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, FONT_SIZE(14));

	/* Search box: the query followed by a cursor bar. */
	int x = PADDING, y = PADDING, w = WIDTH - 2 * PADDING;
	rounded_rect(cr, x + 0.5, y + 0.5, w - 1, ROW_HEIGHT - 1, 3);
	cairo_set_source_rgb(cr, HUD_PANEL);
	cairo_fill_preserve(cr);
	cairo_set_source_rgb(cr, HUD_DIM);
	cairo_stroke(cr);
	if (launcher->query[0] == '\0') {
		cairo_set_source_rgb(cr, HUD_DIM);
		draw_text(cr, "> SEARCH APPS", x + 12, y + ROW_HEIGHT / 2.0);
	} else {
		/* A "> " prompt, then what was typed, then a block cursor. */
		cairo_set_source_rgb(cr, HUD_GREEN);
		draw_text(cr, "> ", x + 12, y + ROW_HEIGHT / 2.0);
		double query_x = x + 12 + text_width(cr, "> ");
		draw_text(cr, launcher->query, query_x, y + ROW_HEIGHT / 2.0);
		double cursor_x = query_x + text_width(cr, launcher->query) + 2;
		cairo_rectangle(cr, cursor_x, y + 11, 8, ROW_HEIGHT - 22);
		cairo_fill(cr);
	}

	/* When there are more matches than rows, say how many there are. */
	if (launcher->match_count > launcher->rows) {
		char count[16];
		snprintf(count, sizeof(count), "%d", launcher->match_count);
		cairo_set_source_rgb(cr, HUD_DIM);
		draw_text(cr, count, x + w - 12 - text_width(cr, count),
			y + ROW_HEIGHT / 2.0);
	}

	/* Results: the rows from `first` on, as many as fit. */
	y += ROW_HEIGHT + PADDING;
	for (int i = launcher->first; i < launcher->match_count &&
			i < launcher->first + launcher->rows; i++) {
		if (i == launcher->selected) {
			/* Selected row in inverse video. */
			rounded_rect(cr, x, y, w, ROW_HEIGHT, 3);
			cairo_set_source_rgb(cr, HUD_GREEN);
			cairo_fill(cr);
			cairo_set_source_rgb(cr, HUD_BLACK);
		} else {
			cairo_set_source_rgb(cr, HUD_GREEN);
		}
		draw_text(cr, launcher->apps.items[launcher->matches[i]].name, x + 12,
			y + ROW_HEIGHT / 2.0);
		y += ROW_HEIGHT;
	}
	if (launcher->match_count == 0) {
		cairo_set_source_rgb(cr, HUD_DIM);
		draw_text(cr, "NO MATCH", x + 12, y + ROW_HEIGHT / 2.0);
	}

	cairo_surface_flush(launcher->buffer->cairo_surface);
	wl_surface_attach(launcher->surface, launcher->buffer->wl_buffer, 0, 0);
	wl_surface_damage_buffer(launcher->surface, 0, 0, WIDTH, launcher->height);
	wl_surface_commit(launcher->surface);
}

/* ---- layer surface events ---- */

static void layer_surface_configure(void *data,
		struct zwlr_layer_surface_v1 *layer_surface,
		uint32_t serial, uint32_t width, uint32_t height) {
	/* We asked for a fixed size, so the size in the event is ours. */
	struct launcher *launcher = data;
	zwlr_layer_surface_v1_ack_configure(layer_surface, serial);
	launcher->configured = true;
	draw(launcher);
}

static void layer_surface_closed(void *data,
		struct zwlr_layer_surface_v1 *layer_surface) {
	struct launcher *launcher = data;
	launcher->running = false;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = layer_surface_configure,
	.closed = layer_surface_closed,
};

/* ---- keyboard events ---- */

static void keyboard_keymap(void *data, struct wl_keyboard *keyboard,
		uint32_t format, int32_t fd, uint32_t size) {
	/* The compositor sends its keymap as a file descriptor to a text
	 * description. Map it, let xkbcommon parse it, and unmap it again. */
	struct launcher *launcher = data;
	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
		close(fd);
		return;
	}
	char *text = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (text != MAP_FAILED) {
		launcher->xkb_keymap = xkb_keymap_new_from_string(
			launcher->xkb_context, text, XKB_KEYMAP_FORMAT_TEXT_V1,
			XKB_KEYMAP_COMPILE_NO_FLAGS);
		munmap(text, size);
	}
	close(fd);
	if (launcher->xkb_keymap != NULL) {
		launcher->xkb_state = xkb_state_new(launcher->xkb_keymap);
	}
}

/* Milliseconds on a clock that only goes forwards. */
static int64_t now_ms(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard,
		uint32_t serial, struct wl_surface *surface, struct wl_array *keys) {
	/* We now have keyboard focus. */
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard,
		uint32_t serial, struct wl_surface *surface) {
	/* We will not hear the key come up, so stop repeating it now. */
	struct launcher *launcher = data;
	launcher->repeat_key = 0;
}

/* Start the chosen app and close. */
static void start_app(struct launcher *launcher, int app) {
	spawn(launcher->apps.items[app].command);
	launcher->running = false;
}

/* Act on a key: once when it goes down, then again for each repeat. */
static void press_key(struct launcher *launcher, uint32_t key) {
	/* Wayland sends Linux evdev key codes. XKB key codes are the same
	 * numbers plus 8, for historical reasons (X11 reserved codes 0 to 7). */
	xkb_keycode_t keycode = key + 8;
	/* A keysym is the meaning of a key in the current layout and modifier
	 * state: the same physical key gives XKB_KEY_a or XKB_KEY_A. */
	xkb_keysym_t sym = xkb_state_key_get_one_sym(launcher->xkb_state, keycode);
	size_t length = strlen(launcher->query);

	switch (sym) {
	case XKB_KEY_Escape:
		launcher->running = false;
		return;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		if (launcher->match_count > 0) {
			start_app(launcher, launcher->matches[launcher->selected]);
		}
		launcher->running = false;
		return;
	case XKB_KEY_Up:
		if (launcher->selected > 0) {
			launcher->selected--;
		}
		break;
	case XKB_KEY_Down:
		if (launcher->selected < launcher->match_count - 1) {
			launcher->selected++;
		}
		break;
	case XKB_KEY_BackSpace:
		if (length > 0) {
			launcher->query[length - 1] = '\0';
		}
		break;
	default: {
		/* Anything else: ask xkbcommon for the text this key types. We
		 * accept one printable ASCII character at a time. */
		char text[8];
		int n = xkb_state_key_get_utf8(launcher->xkb_state, keycode,
			text, sizeof(text));
		if (n == 1 && text[0] >= 32 && text[0] < 127 &&
				length < sizeof(launcher->query) - 1) {
			launcher->query[length] = text[0];
			launcher->query[length + 1] = '\0';
		}
		break;
	}
	}
	filter(launcher);
	draw(launcher);
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard,
		uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
	struct launcher *launcher = data;
	if (launcher->xkb_state == NULL) {
		return;
	}
	if (state != WL_KEYBOARD_KEY_STATE_PRESSED) {
		/* Only the key being repeated stops the repeat: typing fast, the
		 * next key often goes down before the last one is up. */
		if (key == launcher->repeat_key) {
			launcher->repeat_key = 0;
		}
		return;
	}
	press_key(launcher, key);
	/* The keymap knows which keys repeat: letters do, Shift does not. */
	if (launcher->repeat_rate > 0 &&
			xkb_keymap_key_repeats(launcher->xkb_keymap, key + 8)) {
		launcher->repeat_key = key;
		launcher->repeat_at = now_ms() + launcher->repeat_delay;
	}
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *keyboard,
		int32_t rate, int32_t delay) {
	struct launcher *launcher = data;
	launcher->repeat_rate = rate;
	launcher->repeat_delay = delay;
	if (rate <= 0) {
		launcher->repeat_key = 0;
	}
}

/* Repeat the held key if it is due. Returns how many milliseconds until it
 * is next due, or -1 if no key is repeating. */
static int repeat_held_key(struct launcher *launcher) {
	if (launcher->repeat_key == 0) {
		return -1;
	}
	int64_t now = now_ms();
	if (now >= launcher->repeat_at) {
		press_key(launcher, launcher->repeat_key);
		/* Count from now, not from when it was due: after a slow frame,
		 * missed repeats are skipped instead of arriving in a burst. */
		launcher->repeat_at = now + 1000 / launcher->repeat_rate;
	}
	return (int)(launcher->repeat_at - now);
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard,
		uint32_t serial, uint32_t depressed, uint32_t latched,
		uint32_t locked, uint32_t group) {
	/* The compositor tracks Shift, Ctrl, Caps Lock... for us and reports
	 * changes. Feed them into our xkb state so keysyms come out right. */
	struct launcher *launcher = data;
	if (launcher->xkb_state != NULL) {
		xkb_state_update_mask(launcher->xkb_state, depressed, latched,
			locked, 0, 0, group);
	}
}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = keyboard_keymap,
	.enter = keyboard_enter,
	.leave = keyboard_leave,
	.key = keyboard_key,
	.modifiers = keyboard_modifiers,
	.repeat_info = keyboard_repeat_info,
};

/* ---- pointer events ---- */

/* The index into matches[] of the result under the pointer, or -1. The
 * sizes are the ones draw() uses. */
static int match_at(struct launcher *launcher) {
	double y = launcher->pointer_y - (2 * PADDING + ROW_HEIGHT);
	if (launcher->pointer_x < PADDING ||
			launcher->pointer_x >= WIDTH - PADDING || y < 0) {
		return -1;
	}
	int row = (int)(y / ROW_HEIGHT);
	int match = launcher->first + row;
	return row < launcher->rows && match < launcher->match_count ? match : -1;
}

static void pointer_enter(void *data, struct wl_pointer *pointer,
		uint32_t serial, struct wl_surface *surface,
		wl_fixed_t x, wl_fixed_t y) {
	/* Remember where it is, but do not select: the launcher may have
	 * opened under a pointer nobody is touching. */
	struct launcher *launcher = data;
	launcher->pointer_x = wl_fixed_to_double(x);
	launcher->pointer_y = wl_fixed_to_double(y);
}

static void pointer_leave(void *data, struct wl_pointer *pointer,
		uint32_t serial, struct wl_surface *surface) {
	struct launcher *launcher = data;
	launcher->pointer_x = launcher->pointer_y = -1;
	launcher->pressed_app = -1;
}

static void pointer_motion(void *data, struct wl_pointer *pointer,
		uint32_t time, wl_fixed_t x, wl_fixed_t y) {
	struct launcher *launcher = data;
	launcher->pointer_x = wl_fixed_to_double(x);
	launcher->pointer_y = wl_fixed_to_double(y);
	int match = match_at(launcher);
	if (match >= 0 && match != launcher->selected) {
		launcher->selected = match;
		draw(launcher);
	}
}

static void pointer_button(void *data, struct wl_pointer *pointer,
		uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
	struct launcher *launcher = data;
	if (button != BTN_LEFT) {
		return;
	}
	int match = match_at(launcher);
	int app = match >= 0 ? launcher->matches[match] : -1;
	if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
		launcher->pressed_app = app;
	} else {
		/* Start it when the button comes up on the same result. Closing
		 * on the press instead would send the release to whatever is
		 * behind us. */
		if (app >= 0 && app == launcher->pressed_app) {
			start_app(launcher, app);
		}
		launcher->pressed_app = -1;
	}
}

static void pointer_axis(void *data, struct wl_pointer *pointer,
		uint32_t time, uint32_t axis, wl_fixed_t value) {
	/* Scroll wheel: not used. */
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
	struct launcher *launcher = data;
	if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) &&
			launcher->keyboard == NULL) {
		launcher->keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(launcher->keyboard, &keyboard_listener,
			launcher);
	}
	if ((capabilities & WL_SEAT_CAPABILITY_POINTER) &&
			launcher->pointer == NULL) {
		launcher->pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(launcher->pointer, &pointer_listener,
			launcher);
	}
}

static void seat_name(void *data, struct wl_seat *seat, const char *name) {
	/* Sent from version 2 on. Not used, but an event without a handler
	 * would end the program. */
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

static void registry_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	struct launcher *launcher = data;
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		launcher->compositor = wl_registry_bind(registry, name,
			&wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		launcher->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		launcher->layer_shell = wl_registry_bind(registry, name,
			&zwlr_layer_shell_v1_interface, 1);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		/* Version 4 is the first that sends repeat_info. The keyboard
		 * and pointer we get from the seat have its version too. */
		launcher->seat = wl_registry_bind(registry, name,
			&wl_seat_interface, version < 4 ? version : 4);
		wl_seat_add_listener(launcher->seat, &seat_listener, launcher);
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
	struct launcher launcher = { .running = true, .pressed_app = -1 };
	launcher.xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	apps_load_all(&launcher.apps);
	launcher.matches = malloc(launcher.apps.count * sizeof(int));
	launcher.rows = launcher.apps.count < MAX_ROWS ?
		launcher.apps.count : MAX_ROWS;
	launcher.height = PADDING * 3 + ROW_HEIGHT * (1 + launcher.rows);
	filter(&launcher); /* empty query: every app is listed */

	launcher.display = wl_display_connect(NULL);
	if (launcher.display == NULL) {
		fprintf(stderr, "launcher: cannot connect to a Wayland compositor\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(launcher.display);
	wl_registry_add_listener(registry, &registry_listener, &launcher);
	wl_display_roundtrip(launcher.display);
	if (launcher.compositor == NULL || launcher.shm == NULL ||
			launcher.layer_shell == NULL) {
		fprintf(stderr, "launcher: compositor lacks layer-shell or a core global\n");
		return 1;
	}

	launcher.surface = wl_compositor_create_surface(launcher.compositor);
	launcher.layer_surface = zwlr_layer_shell_v1_get_layer_surface(
		launcher.layer_shell, launcher.surface, NULL,
		ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "launcher");
	zwlr_layer_surface_v1_add_listener(launcher.layer_surface,
		&layer_surface_listener, &launcher);
	/* A fixed size and no anchor: the compositor centres us. */
	zwlr_layer_surface_v1_set_size(launcher.layer_surface, WIDTH,
		launcher.height);
	/* Exclusive: all keyboard input comes to us while we are mapped. */
	zwlr_layer_surface_v1_set_keyboard_interactivity(launcher.layer_surface,
		ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
	wl_surface_commit(launcher.surface);

	while (launcher.running) {
		/* Sleep until an event, or until a held key is due to repeat. */
		if (dispatch_with_timeout(launcher.display,
				repeat_held_key(&launcher)) < 0) {
			break;
		}
	}

	/* Disconnecting destroys our surface on the compositor's side, which
	 * unmaps it and hands the keyboard back to the previous window. */
	buffer_destroy(launcher.buffer);
	wl_display_disconnect(launcher.display);
	return 0;
}
