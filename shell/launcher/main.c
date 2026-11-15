/*
 * perkinsos-launcher: a search box in the middle of the screen.
 * Type to filter the app list, Up/Down to choose, Enter to start the app,
 * Escape to cancel. It runs once and exits; the dock's "Apps" button and
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
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

#include "apps.h"
#include "client.h"
#include "theme.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#define WIDTH 420
#define PADDING 16
#define ROW_HEIGHT 40
/* search box + one row per app + padding above, below and between */
#define HEIGHT (PADDING * 3 + ROW_HEIGHT * (1 + APP_COUNT))

struct launcher {
	struct wl_display *display;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct zwlr_layer_shell_v1 *layer_shell;
	struct wl_seat *seat;
	struct wl_keyboard *keyboard;

	struct wl_surface *surface;
	struct zwlr_layer_surface_v1 *layer_surface;
	struct buffer *buffer;
	bool configured;

	/* xkbcommon: the keymap says which symbol each key produces; the state
	 * tracks which modifiers (Shift, Caps Lock...) are active right now. */
	struct xkb_context *xkb_context;
	struct xkb_keymap *xkb_keymap;
	struct xkb_state *xkb_state;

	char query[32];          /* what the user has typed */
	int matches[APP_COUNT];  /* indexes into apps[] that match the query */
	int match_count;
	int selected;            /* index into matches[] */
	bool running;
};

/* Rebuild matches[]: every app whose name contains the query. */
static void filter(struct launcher *launcher) {
	launcher->match_count = 0;
	for (int i = 0; i < APP_COUNT; i++) {
		/* strcasestr: find a substring, ignoring upper/lower case. An
		 * empty query is found in every name, so everything matches. */
		if (strcasestr(apps[i].name, launcher->query) != NULL) {
			launcher->matches[launcher->match_count++] = i;
		}
	}
	/* Keep the selection inside the (possibly shorter) list. */
	if (launcher->selected >= launcher->match_count) {
		launcher->selected = launcher->match_count > 0 ?
			launcher->match_count - 1 : 0;
	}
}

static void draw(struct launcher *launcher) {
	if (!launcher->configured) {
		return;
	}
	if (launcher->buffer == NULL) {
		launcher->buffer = buffer_create(launcher->shm, WIDTH, HEIGHT);
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
	rounded_rect(cr, 1, 1, WIDTH - 2, HEIGHT - 2, 4);
	cairo_set_source_rgb(cr, HUD_BLACK);
	cairo_fill_preserve(cr);
	cairo_set_source_rgb(cr, HUD_GREEN);
	cairo_set_line_width(cr, HUD_LINE);
	cairo_stroke(cr);

	cairo_select_font_face(cr, UI_FONT, CAIRO_FONT_SLANT_NORMAL,
		CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, 14);

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

	/* Results. */
	y += ROW_HEIGHT + PADDING;
	for (int i = 0; i < launcher->match_count; i++) {
		if (i == launcher->selected) {
			/* Selected row in inverse video. */
			rounded_rect(cr, x, y, w, ROW_HEIGHT, 3);
			cairo_set_source_rgb(cr, HUD_GREEN);
			cairo_fill(cr);
			cairo_set_source_rgb(cr, HUD_BLACK);
		} else {
			cairo_set_source_rgb(cr, HUD_GREEN);
		}
		draw_text(cr, apps[launcher->matches[i]].name, x + 12,
			y + ROW_HEIGHT / 2.0);
		y += ROW_HEIGHT;
	}
	if (launcher->match_count == 0) {
		cairo_set_source_rgb(cr, HUD_DIM);
		draw_text(cr, "NO MATCH", x + 12, y + ROW_HEIGHT / 2.0);
	}

	cairo_surface_flush(launcher->buffer->cairo_surface);
	wl_surface_attach(launcher->surface, launcher->buffer->wl_buffer, 0, 0);
	wl_surface_damage_buffer(launcher->surface, 0, 0, WIDTH, HEIGHT);
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

static void keyboard_enter(void *data, struct wl_keyboard *keyboard,
		uint32_t serial, struct wl_surface *surface, struct wl_array *keys) {
	/* We now have keyboard focus. */
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard,
		uint32_t serial, struct wl_surface *surface) {
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard,
		uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
	struct launcher *launcher = data;
	if (state != WL_KEYBOARD_KEY_STATE_PRESSED || launcher->xkb_state == NULL) {
		return;
	}
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
			spawn(apps[launcher->matches[launcher->selected]].command);
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
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
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
		launcher->seat = wl_registry_bind(registry, name,
			&wl_seat_interface, 1);
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
	struct launcher launcher = { .running = true };
	launcher.xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
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
	zwlr_layer_surface_v1_set_size(launcher.layer_surface, WIDTH, HEIGHT);
	/* Exclusive: all keyboard input comes to us while we are mapped. */
	zwlr_layer_surface_v1_set_keyboard_interactivity(launcher.layer_surface,
		ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
	wl_surface_commit(launcher.surface);

	while (launcher.running) {
		if (dispatch_with_timeout(launcher.display, -1) < 0) {
			break;
		}
	}

	/* Disconnecting destroys our surface on the compositor's side, which
	 * unmaps it and hands the keyboard back to the previous window. */
	buffer_destroy(launcher.buffer);
	wl_display_disconnect(launcher.display);
	return 0;
}
