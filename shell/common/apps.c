/*
 * Finding applications (see apps.h for the two lists).
 *
 * A .desktop file looks like this:
 *
 *   [Desktop Entry]
 *   Type=Application
 *   Name=Firefox
 *   Exec=firefox %u
 *
 * The format is the freedesktop.org "Desktop Entry Specification". We read
 * only what we need from the [Desktop Entry] section: Name, Exec, and the
 * keys that say an entry should not be shown.
 *
 * The files live in an "applications" folder inside each of the system's
 * data folders, which the XDG Base Directory Specification names:
 * $XDG_DATA_HOME (your own, default ~/.local/share), then each folder in
 * $XDG_DATA_DIRS (shared, default /usr/local/share and /usr/share). If
 * two folders have a file with the same name, the first one found wins,
 * so your own copy overrides the system's.
 */
#include <ctype.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apps.h"

/* Apps that are always present, first on the dock. */
static const struct app builtin[] = {
	{ "TERMINAL", "foot" },
	{ "SYSTEM INFO", "perkinsos-sysinfo" },
};
#define BUILTIN_COUNT ((int)(sizeof(builtin) / sizeof(builtin[0])))

/* Things the launcher offers that are not apps. They come last in its
 * list and are never pinned. Each tries systemd's command first (a
 * normal distro, where a logged-in user is allowed to do this) and falls
 * back to the plain one (the Buildroot image, where we are root). */
static const struct app actions[] = {
	{ "SHUT DOWN", "systemctl poweroff 2> /dev/null || poweroff" },
	{ "RESTART", "systemctl reboot 2> /dev/null || reboot" },
};
#define ACTION_COUNT ((int)(sizeof(actions) / sizeof(actions[0])))

/* ---- small string helpers ---- */

/* Copy as much of `from` as fits, always ending with a 0 byte. */
static void copy_text(char *to, size_t size, const char *from) {
	size_t length = strlen(from);
	if (length >= size) {
		length = size - 1;
	}
	memcpy(to, from, length);
	to[length] = '\0';
}

/* A new string: a followed by b. The caller frees it. */
static char *join(const char *a, const char *b) {
	char *result = malloc(strlen(a) + strlen(b) + 1);
	strcpy(result, a);
	strcat(result, b);
	return result;
}

static bool ends_with(const char *text, const char *ending) {
	size_t length = strlen(text), ending_length = strlen(ending);
	return length >= ending_length &&
		strcmp(text + length - ending_length, ending) == 0;
}

/* Remove spaces, tabs and the line ending from both ends, in place. */
static char *trim(char *text) {
	while (*text == ' ' || *text == '\t') {
		text++;
	}
	size_t length = strlen(text);
	while (length > 0 && strchr(" \t\r\n", text[length - 1]) != NULL) {
		text[--length] = '\0';
	}
	return text;
}

/* ---- the list ---- */

static void add_app(struct app_list *list, const struct app *app) {
	list->items = realloc(list->items, (list->count + 1) * sizeof(*app));
	list->items[list->count++] = *app;
}

static bool has_command(const struct app_list *list, const char *command) {
	for (int i = 0; i < list->count; i++) {
		if (strcmp(list->items[i].command, command) == 0) {
			return true;
		}
	}
	return false;
}

void apps_free(struct app_list *list) {
	free(list->items);
	list->items = NULL;
	list->count = 0;
}

/* ---- reading one .desktop file ---- */

/*
 * Turn an Exec= value into a shell command. The value may contain "field
 * codes": %u, %f and so on, where a file manager would put the file being
 * opened. We open nothing, so they are removed. %% means a real percent.
 */
static void exec_to_command(const char *exec, char *command, size_t size) {
	size_t out = 0;
	for (const char *in = exec; *in != '\0' && out < size - 1; in++) {
		if (*in != '%') {
			command[out++] = *in;
		} else if (in[1] == '%') {
			command[out++] = '%';
			in++;
		} else if (in[1] != '\0') {
			in++; /* skip the letter after the % */
		}
	}
	command[out] = '\0';
	trim(command);
}

/* Read `path` into `app`. False if it is not an app we should show. */
static bool read_desktop_file(const char *path, struct app *app) {
	FILE *file = fopen(path, "r");
	if (file == NULL) {
		return false;
	}
	char line[1024], name[sizeof(app->name)] = "", exec[sizeof(app->command)] = "";
	bool in_entry = false, is_application = false, hidden = false;
	bool needs_terminal = false;

	while (fgets(line, sizeof(line), file) != NULL) {
		char *text = trim(line);
		if (text[0] == '[') {
			/* A section heading. Only [Desktop Entry] interests us; the
			 * others describe extra actions ("New Private Window"). */
			in_entry = strcmp(text, "[Desktop Entry]") == 0;
			continue;
		}
		char *equals = strchr(text, '=');
		if (!in_entry || text[0] == '#' || equals == NULL) {
			continue;
		}
		*equals = '\0';
		const char *key = trim(text), *value = trim(equals + 1);
		/* Translated names are separate keys, such as "Name[fr]", so
		 * comparing the whole key picks the untranslated one. */
		if (strcmp(key, "Name") == 0) {
			copy_text(name, sizeof(name), value);
		} else if (strcmp(key, "Exec") == 0) {
			copy_text(exec, sizeof(exec), value);
		} else if (strcmp(key, "Type") == 0) {
			is_application = strcmp(value, "Application") == 0;
		} else if (strcmp(key, "NoDisplay") == 0 || strcmp(key, "Hidden") == 0) {
			hidden = hidden || strcmp(value, "true") == 0;
		} else if (strcmp(key, "OnlyShowIn") == 0) {
			/* Made for one particular desktop, e.g. GNOME's settings. */
			hidden = hidden || strstr(value, "PerkinsOS") == NULL;
		} else if (strcmp(key, "Terminal") == 0) {
			needs_terminal = strcmp(value, "true") == 0;
		}
	}
	fclose(file);
	if (!is_application || hidden || name[0] == '\0' || exec[0] == '\0') {
		return false;
	}

	/* Names in capitals, like everything else on the HUD. */
	for (size_t i = 0; name[i] != '\0'; i++) {
		name[i] = (char)toupper((unsigned char)name[i]);
	}
	copy_text(app->name, sizeof(app->name), name);
	/* A text-mode program needs a terminal window to run in. */
	const char *prefix = needs_terminal ? "foot " : "";
	copy_text(app->command, sizeof(app->command), prefix);
	exec_to_command(exec, app->command + strlen(prefix),
		sizeof(app->command) - strlen(prefix));
	return true;
}

/* ---- where .desktop files are ---- */

#define MAX_DIRS 16

/* Fill dirs[] with every "applications" folder, most important first.
 * Returns how many. The caller frees each string. */
static int application_dirs(char *dirs[MAX_DIRS]) {
	int count = 0;
	const char *home = getenv("HOME");
	const char *data_home = getenv("XDG_DATA_HOME");
	const char *data_dirs = getenv("XDG_DATA_DIRS");

	if (data_home != NULL && data_home[0] != '\0') {
		dirs[count++] = join(data_home, "/applications");
	} else if (home != NULL) {
		dirs[count++] = join(home, "/.local/share/applications");
	}
	if (data_dirs == NULL || data_dirs[0] == '\0') {
		data_dirs = "/usr/local/share:/usr/share";
	}
	char *copy = join(data_dirs, ""); /* strtok changes what it splits */
	for (char *dir = strtok(copy, ":"); dir != NULL && count < MAX_DIRS - 1;
			dir = strtok(NULL, ":")) {
		dirs[count++] = join(dir, "/applications");
	}
	free(copy);
	/* Ubuntu's "snap" packages (Firefox is one) put theirs here. Usually
	 * this is in XDG_DATA_DIRS already; a second visit adds nothing. */
	dirs[count++] = join("/var/lib/snapd/desktop", "/applications");
	return count;
}

static int compare_names(const void *a, const void *b) {
	return strcmp(((const struct app *)a)->name, ((const struct app *)b)->name);
}

/* The foot package installs three entries: foot itself, and two for its
 * server mode ("Foot Client", "Foot Server"). TERMINAL already starts foot,
 * and the client does nothing unless the server is running. */
static bool is_duplicate_terminal(const char *file_name) {
	return strcmp(file_name, "foot.desktop") == 0 ||
		strcmp(file_name, "footclient.desktop") == 0 ||
		strcmp(file_name, "foot-server.desktop") == 0;
}

/* Add every installed app that is not on the list already, in A-Z order. */
static void add_installed(struct app_list *list) {
	char *dirs[MAX_DIRS];
	int dir_count = application_dirs(dirs);
	char **seen = NULL; /* file names read so far, in any folder */
	int seen_count = 0;
	int first_new = list->count;

	for (int d = 0; d < dir_count; d++) {
		DIR *dir = opendir(dirs[d]);
		struct dirent *entry;
		while (dir != NULL && (entry = readdir(dir)) != NULL) {
			if (!ends_with(entry->d_name, ".desktop")) {
				continue;
			}
			if (is_duplicate_terminal(entry->d_name)) {
				continue;
			}
			bool already = false;
			for (int i = 0; i < seen_count && !already; i++) {
				already = strcmp(seen[i], entry->d_name) == 0;
			}
			if (already) {
				continue; /* an earlier folder's copy wins */
			}
			seen = realloc(seen, (seen_count + 1) * sizeof(*seen));
			seen[seen_count++] = join(entry->d_name, "");

			char *folder = join(dirs[d], "/");
			char *path = join(folder, entry->d_name);
			struct app app;
			if (read_desktop_file(path, &app) &&
					!has_command(list, app.command)) {
				add_app(list, &app);
			}
			free(path);
			free(folder);
		}
		if (dir != NULL) {
			closedir(dir);
		}
		free(dirs[d]);
	}
	for (int i = 0; i < seen_count; i++) {
		free(seen[i]);
	}
	free(seen);
	qsort(list->items + first_new, list->count - first_new,
		sizeof(struct app), compare_names);
}

/* ---- the pinned list ---- */

/* The pins file: yours if you have one, else the system's, else none. */
static FILE *open_pins_file(void) {
	const char *config_home = getenv("XDG_CONFIG_HOME");
	const char *home = getenv("HOME");
	char *path = NULL;
	if (config_home != NULL && config_home[0] != '\0') {
		path = join(config_home, "/perkinsos/pinned");
	} else if (home != NULL) {
		path = join(home, "/.config/perkinsos/pinned");
	}
	FILE *file = path != NULL ? fopen(path, "r") : NULL;
	free(path);
	return file != NULL ? file : fopen("/etc/perkinsos/pinned", "r");
}

void apps_load_pinned(struct app_list *list) {
	for (int i = 0; i < BUILTIN_COUNT; i++) {
		add_app(list, &builtin[i]);
	}
	/* Each line of the pins file is the name of a .desktop file, with or
	 * without the ".desktop", e.g. "firefox_firefox". Lines starting with
	 * # are comments. */
	FILE *pins = open_pins_file();
	if (pins == NULL) {
		return;
	}
	char *dirs[MAX_DIRS];
	int dir_count = application_dirs(dirs);
	char line[256];
	while (fgets(line, sizeof(line), pins) != NULL) {
		char *id = trim(line);
		if (id[0] == '\0' || id[0] == '#') {
			continue;
		}
		char *file_name = ends_with(id, ".desktop") ? join(id, "") :
			join(id, ".desktop");
		/* Look in each folder in turn; stop at the first that has it. */
		bool found = false;
		for (int d = 0; d < dir_count && !found; d++) {
			char *folder = join(dirs[d], "/");
			char *path = join(folder, file_name);
			struct app app;
			found = read_desktop_file(path, &app);
			if (found && !has_command(list, app.command)) {
				add_app(list, &app);
			}
			free(path);
			free(folder);
		}
		free(file_name);
	}
	fclose(pins);
	for (int d = 0; d < dir_count; d++) {
		free(dirs[d]);
	}
}

void apps_load_all(struct app_list *list) {
	apps_load_pinned(list);
	add_installed(list);
	for (int i = 0; i < ACTION_COUNT; i++) {
		add_app(list, &actions[i]);
	}
}
