/*
 * Facts about the machine as a whole, for the System Info window and the
 * cockpit's system page.
 */
#ifndef PERKINSOS_MACHINE_H
#define PERKINSOS_MACHINE_H

struct machine {
	char kernel[140];           /* e.g. "Linux 6.12.9" */
	double uptime;              /* seconds since the machine started */
	long memory_total_kb;
	long memory_used_kb;
	double load_average;        /* programs wanting the CPU, 1-minute average */
	double disk_total, disk_used; /* bytes, of the disk that holds "/" */
};

void machine_read(struct machine *machine);

#endif
