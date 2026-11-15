/*
 * The PerkinsOS look: a fighter-jet head-up display. Green on near-black,
 * outlines instead of filled shapes, monospace capitals.
 *
 * Every client takes its colours and font from here, so the whole desktop
 * changes in one place. The compositor (window borders) and the terminal
 * (buildroot/overlay/etc/xdg/foot/foot.ini) repeat the same greens, because
 * they can't include this file.
 */
#ifndef PERKINSOS_THEME_H
#define PERKINSOS_THEME_H

/* Colours as "red, green, blue" so they drop straight into cairo calls:
 *   cairo_set_source_rgb(cr, HUD_GREEN);  */
#define HUD_GREEN  0.20, 1.00, 0.40   /* lines and text: phosphor green */
#define HUD_DIM    0.09, 0.45, 0.19   /* secondary lines and labels */
#define HUD_FAINT  0.04, 0.17, 0.08   /* fills and very quiet detail */
#define HUD_PANEL  0.015, 0.06, 0.03  /* panel backgrounds */
#define HUD_BLACK  0.005, 0.025, 0.012 /* the darkest background */

/* The font every PerkinsOS client draws with (the dejavu package). */
#define UI_FONT "DejaVu Sans Mono"

/* Thickness of HUD lines, in pixels. */
#define HUD_LINE 1.5

#endif
