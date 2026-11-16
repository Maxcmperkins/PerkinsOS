/*
 * The applications the desktop can start. There are two lists:
 *
 *   pinned   the few apps that get a button on the dock and in the
 *            cockpit: the built-in ones, then any you name in the file
 *            ~/.config/perkinsos/pinned (or /etc/perkinsos/pinned)
 *   all      the pinned ones, then every app installed on the system.
 *            The launcher searches this list.
 *
 * "Installed" means the app has a .desktop file: a small text file that
 * every Linux desktop app ships to describe itself (its name, and the
 * command that starts it). The Buildroot image has none, so there both
 * lists are just the built-in apps. See apps.c.
 */
#ifndef PERKINSOS_APPS_H
#define PERKINSOS_APPS_H

struct app {
	char name[41];      /* shown to the user; capitals, HUD style */
	char command[512];  /* run with /bin/sh -c */
};

struct app_list {
	struct app *items;
	int count;
};

/* Fill `list` (which must start out zeroed). Free it with apps_free(). */
void apps_load_pinned(struct app_list *list);
void apps_load_all(struct app_list *list);
void apps_free(struct app_list *list);

/* Opens the launcher, unless one is already open. */
#define LAUNCHER_COMMAND \
	"pidof perkinsos-launcher > /dev/null || perkinsos-launcher"

#endif
