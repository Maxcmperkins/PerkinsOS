/*
 * The pages of the cockpit's centre display.
 *
 * A real cockpit display shows one page at a time, and the buttons round
 * its frame do whatever the page says they do: each button's name is lit on
 * the glass beside it. Ours has three:
 *
 *   HOME     the pinned apps, and the radar's target (drawn by scene.c)
 *   APPS     every installed app, twelve at a time, to click
 *   SYSTEM   kernel, uptime, memory, disk, load
 *
 * A page is two things: what its ten buttons do, worked out by key_for(),
 * and what is on the glass between the two rows of names.
 *
 * The mouse works everything. There is no typing here, because a wallpaper
 * never gets the keyboard; to search by name, use the launcher (Alt+P).
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "client.h"
#include "theme.h"
#include "wallpaper.h"

#define SYSINFO_COMMAND "perkinsos-sysinfo"

/* The list on the APPS page. */
#define LIST_COLUMNS 3
#define LIST_ROWS 4
#define LIST_SIZE (LIST_COLUMNS * LIST_ROWS)

/* ---- what the buttons do ---- */

enum action { DO_NOTHING, DO_START, DO_PAGE, DO_SCROLL };

struct key {
	enum action action;
	const char *label;
	const char *command;  /* DO_START: the program */
	int value;            /* DO_PAGE: which; DO_SCROLL: how many apps */
};

/* A key for an app. System Info has a page of its own here. */
static struct key app_key(const struct app *app) {
	if (strcmp(app->command, SYSINFO_COMMAND) == 0) {
		return (struct key){ DO_PAGE, app->name, NULL, PAGE_SYSTEM };
	}
	return (struct key){ DO_START, app->name, app->command, 0 };
}

static struct key key_for(const struct pages *pages, int slot) {
	const struct key nothing = { .action = DO_NOTHING };
	if (slot >= PAGE_KEYS) {
		/* Not a button: an entry in the APPS list. */
		int index = pages->first + slot - PAGE_KEYS;
		if (pages->current != PAGE_APPS || slot >= PAGE_KEYS + LIST_SIZE ||
				index >= pages->all.count) {
			return nothing;
		}
		return app_key(&pages->all.items[index]);
	}

	if (pages->current == PAGE_HOME) {
		if (slot == 0) {
			return (struct key){ DO_PAGE, "APPS", NULL, PAGE_APPS };
		}
		if (slot > pages->pinned.count) {
			return nothing; /* more buttons than apps */
		}
		return app_key(&pages->pinned.items[slot - 1]);
	}

	/* The other two pages have the same first two buttons. */
	if (slot == 0) {
		return (struct key){ DO_PAGE, "HOME", NULL, PAGE_HOME };
	}
	if (slot == 1) {
		return pages->current == PAGE_APPS ?
			(struct key){ DO_PAGE, "SYSTEM", NULL, PAGE_SYSTEM } :
			(struct key){ DO_PAGE, "APPS", NULL, PAGE_APPS };
	}
	if (pages->current == PAGE_APPS) {
		if (slot == 3 && pages->first > 0) {
			return (struct key){ DO_SCROLL, "PREVIOUS", NULL, -LIST_SIZE };
		}
		if (slot == 4 && pages->first + LIST_SIZE < pages->all.count) {
			return (struct key){ DO_SCROLL, "NEXT", NULL, LIST_SIZE };
		}
	} else if (slot == 5) {
		return (struct key){ DO_START, "WINDOW", SYSINFO_COMMAND, 0 };
	}
	return nothing;
}

const char *pages_label(const struct pages *pages, int slot) {
	return key_for(pages, slot).label;
}

void pages_show(struct pages *pages, int page) {
	if (page == PAGE_APPS) {
		/* Look again each time: something may have been installed. */
		apps_free(&pages->all);
		apps_load_installed(&pages->all);
		pages->first = 0;
	}
	pages->machine_read_at = 0; /* old figures: read them afresh */
	pages->current = page;
}

void pages_press(struct pages *pages, int slot) {
	struct key key = key_for(pages, slot);
	switch (key.action) {
	case DO_START:
		spawn(key.command);
		pages_show(pages, PAGE_HOME); /* that was what you came for */
		break;
	case DO_PAGE:
		pages_show(pages, key.value);
		break;
	case DO_SCROLL:
		pages->first += key.value;
		break;
	case DO_NOTHING:
		break;
	}
}

void pages_init(struct pages *pages) {
	if (pages->pinned.count == 0) {
		apps_load_pinned(&pages->pinned);
	}
}

/* ---- where things are ---- */

/* The glass between the two rows of button names. */
#define EDGE 8          /* kept clear at the sides */
#define NAMES_HEIGHT 24 /* each row of names */

/* The box of entry `i` of the APPS list. */
static void list_box(const struct page_area *area, int i,
		double *x, double *y, double *w, double *h) {
	*w = (area->w - 2 * EDGE) / LIST_COLUMNS;
	*h = (area->h - 2 * NAMES_HEIGHT - 4) / LIST_ROWS;
	/* Down each column in turn, as in a printed list. */
	*x = area->x + EDGE + i / LIST_ROWS * *w;
	*y = area->y + NAMES_HEIGHT + 2 + i % LIST_ROWS * *h;
}

int pages_slot_at(const struct pages *pages, const struct page_area *area,
		double px, double py) {
	for (int i = 0; i < LIST_SIZE; i++) {
		double x, y, w, h;
		list_box(area, i, &x, &y, &w, &h);
		if (px >= x && px < x + w && py >= y && py < y + h &&
				pages_label(pages, PAGE_KEYS + i) != NULL) {
			return PAGE_KEYS + i;
		}
	}
	return -1;
}

/* ---- drawing ---- */

static void draw_apps(const struct pages *pages, cairo_t *cr,
		const struct page_area *area, int hover) {
	cairo_set_font_size(cr, FONT_SIZE(11));
	for (int i = 0; i < LIST_SIZE; i++) {
		const char *name = pages_label(pages, PAGE_KEYS + i);
		if (name == NULL) {
			break;
		}
		double x, y, w, h;
		list_box(area, i, &x, &y, &w, &h);
		if (hover == PAGE_KEYS + i) {
			cairo_rectangle(cr, x + 1, y + 1, w - 2, h - 2);
			cairo_set_source_rgb(cr, HUD_GREEN);
			cairo_fill(cr);
			cairo_set_source_rgb(cr, HUD_BLACK);
		} else {
			cairo_set_source_rgb(cr, HUD_GREEN);
		}
		char text[32];
		shorten_name(name, 22, text, sizeof(text));
		draw_text(cr, text, x + 7, y + h / 2);
	}

	/* Which screenful this is, among the names along the bottom. */
	int screens = (pages->all.count + LIST_SIZE - 1) / LIST_SIZE;
	char note[48];
	snprintf(note, sizeof(note), "%d APPS   PAGE %d/%d", pages->all.count,
		pages->first / LIST_SIZE + 1, screens);
	cairo_set_source_rgb(cr, HUD_DIM);
	draw_text(cr, note, area->x + area->w - EDGE - 6 - text_width(cr, note),
		area->y + area->h - NAMES_HEIGHT / 2.0);
}

/* A row of the SYSTEM page. `part` is how full its bar is, 0 to 1, or
 * negative for no bar. */
static void draw_figure(cairo_t *cr, const struct page_area *area, int row,
		const char *label, const char *value, double part) {
	double height = (area->h - 2 * NAMES_HEIGHT - 4) / 5;
	double y = area->y + NAMES_HEIGHT + 2 + (row + 0.5) * height;
	cairo_set_source_rgb(cr, HUD_DIM);
	draw_text(cr, label, area->x + 14, y);
	cairo_set_source_rgb(cr, HUD_GREEN);
	draw_text(cr, value, area->x + 84, y);
	if (part >= 0) {
		double x = area->x + area->w * 0.60, w = area->w * 0.40 - 14;
		cairo_rectangle(cr, x, y - 4, w, 8);
		cairo_set_source_rgb(cr, HUD_FAINT);
		cairo_fill(cr);
		cairo_rectangle(cr, x, y - 4, w * fmin(part, 1), 8);
		cairo_set_source_rgb(cr, HUD_GREEN);
		cairo_fill(cr);
	}
}

static void draw_system(struct pages *pages, cairo_t *cr,
		const struct page_area *area, double t) {
	if (pages->machine_read_at == 0 || t - pages->machine_read_at >= 1) {
		machine_read(&pages->machine);
		pages->machine_read_at = t;
	}
	const struct machine *m = &pages->machine;
	const double gigabyte = 1024.0 * 1024 * 1024;
	char value[64];
	cairo_set_font_size(cr, FONT_SIZE(11));

	shorten_name(m->kernel, 60, value, sizeof(value));
	draw_figure(cr, area, 0, "KERNEL", value, -1);

	int seconds = (int)m->uptime;
	snprintf(value, sizeof(value), "%d h %02d min %02d s", seconds / 3600,
		seconds / 60 % 60, seconds % 60);
	draw_figure(cr, area, 1, "UPTIME", value, -1);

	snprintf(value, sizeof(value), "%ld of %ld MB", m->memory_used_kb / 1024,
		m->memory_total_kb / 1024);
	draw_figure(cr, area, 2, "MEMORY", value, m->memory_total_kb > 0 ?
		(double)m->memory_used_kb / m->memory_total_kb : 0);

	snprintf(value, sizeof(value), "%.1f of %.1f GB", m->disk_used / gigabyte,
		m->disk_total / gigabyte);
	draw_figure(cr, area, 3, "DISK", value,
		m->disk_total > 0 ? m->disk_used / m->disk_total : 0);

	/* A load of 2 fills the bar, as on the LOAD dial. */
	snprintf(value, sizeof(value), "%.2f (1 min)", m->load_average);
	draw_figure(cr, area, 4, "LOAD", value, m->load_average / 2);
}

void pages_draw(struct pages *pages, cairo_t *cr,
		const struct page_area *area, int hover, double t) {
	if (pages->current == PAGE_APPS) {
		draw_apps(pages, cr, area, hover);
	} else if (pages->current == PAGE_SYSTEM) {
		draw_system(pages, cr, area, t);
	}
}
