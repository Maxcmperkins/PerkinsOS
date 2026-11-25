/*
 * Reading facts about the machine. Everything comes from the kernel: two
 * system calls (uname and statvfs), and the text files it provides under
 * /proc. A figure that cannot be read is left at 0.
 */
#include <stdio.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>

#include "machine.h"

/* The first number in a file. */
static double first_number(const char *path) {
	double value = 0;
	FILE *file = fopen(path, "r");
	if (file != NULL) {
		if (fscanf(file, "%lf", &value) != 1) {
			value = 0;
		}
		fclose(file);
	}
	return value;
}

void machine_read(struct machine *machine) {
	memset(machine, 0, sizeof(*machine));

	struct utsname uts;
	if (uname(&uts) == 0) {
		snprintf(machine->kernel, sizeof(machine->kernel), "%s %s",
			uts.sysname, uts.release);
	}
	machine->uptime = first_number("/proc/uptime");
	machine->load_average = first_number("/proc/loadavg");

	/* /proc/meminfo has one "Name:   12345 kB" line per figure. sscanf
	 * changes nothing when a line does not match. */
	FILE *file = fopen("/proc/meminfo", "r");
	if (file != NULL) {
		char line[128];
		long available = 0;
		while (fgets(line, sizeof(line), file) != NULL) {
			sscanf(line, "MemTotal: %ld", &machine->memory_total_kb);
			sscanf(line, "MemAvailable: %ld", &available);
		}
		fclose(file);
		machine->memory_used_kb = machine->memory_total_kb - available;
	}

	/* Sizes are counted in blocks of f_frsize bytes. */
	struct statvfs disk;
	if (statvfs("/", &disk) == 0) {
		machine->disk_total = (double)disk.f_blocks * disk.f_frsize;
		machine->disk_used =
			(double)(disk.f_blocks - disk.f_bfree) * disk.f_frsize;
	}
}
