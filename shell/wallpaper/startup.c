/*
 * The start-up sequence: the cockpit powering up, in the first few seconds.
 *
 *   0.0  everything dark; the panel lighting comes up
 *   0.5  the screens flicker on, one after another
 *   0.9  the needles swing to full and back (a self test)
 *   1.2  the centre display lists what it finds
 *   2.8  the HUD comes on
 *   3.4  done
 *
 * Nothing here remembers anything. Each function is given `age`, the
 * seconds since the first frame, and answers for that moment, as with
 * everything else that moves in the picture. After STARTUP_LENGTH they all
 * answer "as normal".
 */
#include <math.h>
#include <stdio.h>

#include "client.h"
#include "theme.h"
#include "wallpaper.h"

/* 0 before `from`, 1 after `to`, and a smooth climb between. */
static double ramp(double age, double from, double to) {
	double part = (age - from) / (to - from);
	if (part <= 0) return 0;
	if (part >= 1) return 1;
	return part * part * (3 - 2 * part);
}

double startup_darkness(double age) {
	return 0.75 * (1 - ramp(age, 0.15, 1.1));
}

bool startup_lit(double age, int part) {
	static const double on_at[] = {
		[STARTUP_RADAR] = 0.5, [STARTUP_CENTRE] = 0.8,
		[STARTUP_RWR] = 1.1, [STARTUP_HUD] = 2.8,
	};
	double since = age - on_at[part];
	if (since < 0) {
		return false;
	}
	/* It flickers for the first 0.4 seconds: three blinks, each 0.13 long,
	 * lit for the last half of it. */
	return since > 0.4 || fmod(since, 0.133) > 0.066;
}

double startup_dial(double age, double fraction) {
	if (age < 2.1) {
		/* Up to full and back, as half a wave. */
		return age < 0.9 ? 0 : sin((age - 0.9) / 1.2 * M_PI);
	}
	return fraction * ramp(age, 2.1, 2.6);
}

double startup_gauge(double age) {
	return -360 * (1 - ramp(age, 0.9, 2.1)); /* once round, to its place */
}

bool startup_testing(double age) {
	return age < STARTUP_LENGTH - 0.2;
}

void startup_draw_test(cairo_t *cr, const struct page_area *area,
		const struct machine *machine, int programs, double age) {
	char lines[5][64];
	char kernel[40];
	shorten_name(machine->kernel, 36, kernel, sizeof(kernel));
	snprintf(lines[0], sizeof(lines[0]), "PERKINSOS  SELF TEST");
	snprintf(lines[1], sizeof(lines[1]), "KERNEL     %s", kernel);
	snprintf(lines[2], sizeof(lines[2]), "MEMORY     %ld MB",
		machine->memory_total_kb / 1024);
	snprintf(lines[3], sizeof(lines[3]), "PROGRAMS   %d", programs);
	snprintf(lines[4], sizeof(lines[4]), "READY");

	cairo_set_font_size(cr, FONT_SIZE(12));
	cairo_set_source_rgb(cr, HUD_GREEN);
	for (int i = 0; i < 5 && age > 1.2 + 0.35 * i; i++) {
		draw_text(cr, lines[i], area->x + 14, area->y + 26 + 20 * i);
	}
}
