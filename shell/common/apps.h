/*
 * The list of applications. The dock shows them as buttons and the launcher
 * searches them. Add an app here and it appears in both.
 */
#ifndef PERKINSOS_APPS_H
#define PERKINSOS_APPS_H

struct app {
	const char *name;    /* shown to the user */
	const char *command; /* run with /bin/sh -c */
};

static const struct app apps[] = {
	{ "Terminal", "foot" },
	{ "System Info", "perkinsos-sysinfo" },
};
#define APP_COUNT ((int)(sizeof(apps) / sizeof(apps[0])))

/* Opens the launcher, unless one is already open. */
#define LAUNCHER_COMMAND \
	"pidof perkinsos-launcher > /dev/null || perkinsos-launcher"

/* The font every PerkinsOS client draws with (the dejavu package). */
#define UI_FONT "DejaVu Sans"

#endif
