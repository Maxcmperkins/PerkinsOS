/*
 * The running programs, read from /proc.
 *
 * Linux exposes every process as a folder /proc/<pid>/. The file "stat"
 * inside is one line of numbers describing it. We read the name, the CPU
 * time used so far and the memory in use. CPU *usage* is not stored
 * anywhere: it is how much the CPU time grew between two samples.
 */
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "wallpaper.h"

/* Bit in a process's flags that marks a kernel thread (PF_KTHREAD). Those
 * are parts of the kernel, not programs, and there are dozens of them. */
#define KERNEL_THREAD_FLAG 0x00200000

/* Fill `out` from /proc/<pid>/stat. False if it is gone or a kernel thread. */
static bool read_process(int pid, struct process *out) {
	char path[64], line[512];
	snprintf(path, sizeof(path), "/proc/%d/stat", pid);
	FILE *file = fopen(path, "r");
	if (file == NULL) {
		return false; /* the process exited between listing and reading */
	}
	size_t length = fread(line, 1, sizeof(line) - 1, file);
	fclose(file);
	line[length] = '\0';

	/* The line looks like: 213 (foot) S 1 213 ... The name is in brackets
	 * and may itself contain spaces or brackets, so find the first "(" and
	 * the LAST ")" rather than splitting on spaces. */
	char *open = strchr(line, '(');
	char *close = strrchr(line, ')');
	if (open == NULL || close == NULL || close < open) {
		return false;
	}
	size_t name_length = close - open - 1;
	if (name_length >= sizeof(out->name)) {
		name_length = sizeof(out->name) - 1;
	}
	memcpy(out->name, open + 1, name_length);
	out->name[name_length] = '\0';

	/* The numbered fields after the name (see `man 5 proc`). We want
	 * 9 flags, 14 utime, 15 stime and 24 rss; %* means "read and skip". */
	unsigned int flags;
	unsigned long user_ticks, system_ticks;
	long resident_pages;
	int found = sscanf(close + 1,
		" %*c %*d %*d %*d %*d %*d %u"   /* 3 state .. 8 tpgid, 9 flags */
		" %*u %*u %*u %*u %lu %lu"      /* 10-13 faults, 14 utime, 15 stime */
		" %*d %*d %*d %*d %*d %*d"      /* 16 cutime .. 21 itrealvalue */
		" %*u %*u %ld",                 /* 22 starttime, 23 vsize, 24 rss */
		&flags, &user_ticks, &system_ticks, &resident_pages);
	if (found != 4 || (flags & KERNEL_THREAD_FLAG)) {
		return false;
	}

	out->pid = pid;
	out->ticks = user_ticks + system_ticks;
	/* rss is counted in memory pages (4 kB each on x86). */
	out->memory_kb = resident_pages * (sysconf(_SC_PAGESIZE) / 1024);
	out->cpu_percent = 0;
	out->radar_x = out->radar_y = 0;
	return true;
}

static int compare_pid(const void *a, const void *b) {
	return ((const struct process *)a)->pid - ((const struct process *)b)->pid;
}

void processes_update(struct process_list *list, double now) {
	double elapsed = now - list->sampled_at;
	if (list->sampled_at != 0 && elapsed < 2.0) {
		return; /* the current sample is fresh enough */
	}

	/* Read the new list into a separate array so the old one is still
	 * there to compare against. */
	struct process fresh[MAX_PROCESSES];
	int count = 0;
	DIR *proc = opendir("/proc");
	if (proc == NULL) {
		return;
	}
	struct dirent *entry;
	while ((entry = readdir(proc)) != NULL && count < MAX_PROCESSES) {
		/* Process folders are the ones whose name is a number. */
		if (!isdigit((unsigned char)entry->d_name[0])) {
			continue;
		}
		if (read_process(atoi(entry->d_name), &fresh[count])) {
			count++;
		}
	}
	closedir(proc);
	qsort(fresh, count, sizeof(fresh[0]), compare_pid);

	/* CPU use = growth in CPU time / time that passed. Ticks are counted
	 * in units of 1/CLK_TCK seconds (normally 1/100). */
	double ticks_per_second = sysconf(_SC_CLK_TCK);
	for (int i = 0; i < count && list->sampled_at != 0; i++) {
		for (int j = 0; j < list->count; j++) {
			if (list->items[j].pid == fresh[i].pid) {
				double used = fresh[i].ticks - list->items[j].ticks;
				fresh[i].cpu_percent =
					100.0 * used / (elapsed * ticks_per_second);
				break;
			}
		}
	}

	memcpy(list->items, fresh, sizeof(fresh[0]) * count);
	list->count = count;
	list->sampled_at = now;

	/* Whole-machine figures for the dials. CPU: every program's share
	 * added up, out of 100% per CPU core. */
	double total = 0;
	for (int i = 0; i < count; i++) {
		total += list->items[i].cpu_percent;
	}
	long cores = sysconf(_SC_NPROCESSORS_ONLN);
	list->cpu_fraction = total / (100.0 * (cores > 0 ? cores : 1));
	if (list->cpu_fraction > 1) {
		list->cpu_fraction = 1;
	}

	/* Memory: /proc/meminfo has one "Name:   12345 kB" line per figure. */
	FILE *file = fopen("/proc/meminfo", "r");
	if (file != NULL) {
		char line[128];
		long memory_total = 0, memory_available = 0;
		while (fgets(line, sizeof(line), file) != NULL) {
			sscanf(line, "MemTotal: %ld", &memory_total);
			sscanf(line, "MemAvailable: %ld", &memory_available);
		}
		fclose(file);
		if (memory_total > 0) {
			list->memory_fraction =
				1.0 - (double)memory_available / memory_total;
		}
	}

	/* Load average: the first number in /proc/loadavg. */
	file = fopen("/proc/loadavg", "r");
	if (file != NULL) {
		if (fscanf(file, "%lf", &list->load_average) != 1) {
			list->load_average = 0;
		}
		fclose(file);
	}
}
