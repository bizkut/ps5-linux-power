// SPDX-License-Identifier: GPL-2.0
/*
 * ps5_cpu_gov - userspace PS5 CPU frequency governor, NO kernel module.
 *
 * Reads system CPU load from /proc/stat (no mailbox access for sensing) and
 * drives all cores to a P-state by talking to the SMU MP1 mailbox directly via
 * PCI config space (see ../smn.h). Writes the mailbox ONLY when the target
 * P-state changes, so at steady idle there is zero mailbox traffic.
 *
 * Policy: load >= 0.40 -> P0 (3200) ; >= 0.20 -> P2 (2327) ;
 *         >= 0.08 -> P5 (1600) ; else -> P7 (800).
 * Ramp UP immediate; ramp DOWN needs `down_count` consecutive low samples.
 * On SIGINT/SIGTERM: restore P0 and exit.
 *
 * Run as root. Do NOT run a kernel mailbox module or ps5boost concurrently.
 *
 * Usage: ps5_cpu_gov [-i ms] [-d downcount] [-v]
 */
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "../smn.h"

#define MSG_REQUEST_CORE_PSTATE 0x0b
#define MASK_ALL 0xff
#define P_MAX  0   /* 3200 MHz */
#define P_MID  2   /* 2327 MHz */
#define P_LOW  5   /* 1600 MHz */
#define P_IDLE 7   /*  800 MHz */

static double up_high = 0.40, up_mid = 0.20, up_low = 0.08;
static int interval_ms = 500, down_count = 6, verbose;
static volatile sig_atomic_t stop;

static void on_signal(int s) { (void)s; stop = 1; }

static int read_cpu(unsigned long long *busy, unsigned long long *total)
{
	FILE *f = fopen("/proc/stat", "r");
	unsigned long long u, n, s, idle, iow, irq, sirq, steal;

	if (!f)
		return -1;
	if (fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
		   &u, &n, &s, &idle, &iow, &irq, &sirq, &steal) < 8) {
		fclose(f);
		return -1;
	}
	fclose(f);
	*total = u + n + s + idle + iow + irq + sirq + steal;
	*busy  = *total - idle - iow;
	return 0;
}

static int set_pstate(uint32_t pstate)
{
	uint32_t arg = (MASK_ALL & 0xff) | ((pstate & 0x0f) << 16), st = 0;

	if (smn_mbox(Q0_CMD, Q0_RSP, Q0_ARG, MSG_REQUEST_CORE_PSTATE, arg, &st, NULL) || st != 1)
		return -1;
	return 0;
}

static const char *mhz(uint32_t p)
{
	static const char *tbl[8] = { "3200", "2560", "2327", "1969", "1829", "1600", "1280", "800" };
	return (p < 8) ? tbl[p] : "?";
}

int main(int argc, char **argv)
{
	int opt, low_streak = 0;
	unsigned long long pb = 0, pt = 0, b, t;
	uint32_t current = P_MAX, target;
	struct timespec ts;

	while ((opt = getopt(argc, argv, "i:d:v")) != -1) {
		switch (opt) {
		case 'i': interval_ms = atoi(optarg); break;
		case 'd': down_count = atoi(optarg); break;
		case 'v': verbose = 1; break;
		default:
			fprintf(stderr, "usage: %s [-i ms] [-d downcount] [-v]\n", argv[0]);
			return 2;
		}
	}

	if (smn_open()) return 1;
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	set_pstate(P_MAX);
	current = P_MAX;
	read_cpu(&pb, &pt);
	if (verbose)
		printf("ps5_cpu_gov: started, interval=%dms down_count=%d\n", interval_ms, down_count);

	while (!stop) {
		ts.tv_sec = interval_ms / 1000;
		ts.tv_nsec = (long)(interval_ms % 1000) * 1000000L;
		nanosleep(&ts, NULL);

		if (read_cpu(&b, &t))
			continue;
		double load = (t > pt) ? (double)(b - pb) / (double)(t - pt) : 0.0;
		pb = b; pt = t;

		uint32_t demand;
		if (load >= up_high)      demand = P_MAX;
		else if (load >= up_mid)  demand = P_MID;
		else if (load >= up_low)  demand = P_LOW;
		else                      demand = P_IDLE;

		if (demand < current) {            /* wants more MHz -> jump up */
			target = demand;
			low_streak = 0;
		} else if (demand > current) {     /* wants less -> step down with hysteresis */
			if (++low_streak >= down_count) {
				target = current + 1;
				if (target > demand)
					target = demand;
				low_streak = 0;
			} else {
				target = current;
			}
		} else {
			target = current;
			low_streak = 0;
		}

		if (target != current) {
			if (set_pstate(target) == 0) {
				if (verbose)
					printf("load %.0f%% : P%u(%s) -> P%u(%s)\n",
					       load * 100, current, mhz(current), target, mhz(target));
				current = target;
			}
		} else if (verbose) {
			printf("load %.0f%% : hold P%u(%s)\n", load * 100, current, mhz(current));
		}
	}

	set_pstate(P_MAX);
	if (verbose)
		printf("ps5_cpu_gov: stopped, restored P0 (3200)\n");
	return 0;
}
