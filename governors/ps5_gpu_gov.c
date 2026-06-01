// SPDX-License-Identifier: GPL-2.0
/*
 * ps5_gpu_gov - userspace PS5 GPU (GFXCLK) governor, NO kernel module.
 *
 * Senses GPU load WITHOUT touching the mailbox: sums the `drm-engine-gfx`
 * nanosecond counter across all amdgpu DRM clients (proc fdinfo), computes
 * load = delta(gfx-ns)/delta(wall-ns), and drives GFXCLK via the SMU MP1
 * mailbox directly over PCI config space (see ../smn.h). Writes only when the
 * target changes.
 *
 * Policy: load >= 0.50 -> 2000 ; >= 0.20 -> 1500 ; >= 0.05 -> 1200 ; else 800.
 * Ramp UP immediate; ramp DOWN needs `down_count` consecutive low samples.
 * On SIGINT/SIGTERM: restore 2000 MHz and exit.
 *
 * Run as root. Do NOT run a kernel mailbox module or ps5boost concurrently.
 *
 * Usage: ps5_gpu_gov [-i ms] [-d downcount] [-v]
 */
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "../smn.h"

#define MSG_REQUEST_GFXCLK 0x0e
#define G_MAX  2000
#define G_MID  1500
#define G_LOW  1200
#define G_IDLE  800

static double up_high = 0.50, up_mid = 0.20, up_low = 0.05;
static int interval_ms = 500, down_count = 6, verbose;
static volatile sig_atomic_t stop;

static void on_signal(int s) { (void)s; stop = 1; }

/* sum drm-engine-gfx (ns) across all amdgpu DRM clients (proc fdinfo) */
static unsigned long long sum_gfx_ns(void)
{
	unsigned long long total = 0;
	DIR *proc = opendir("/proc");
	struct dirent *de;
	char path[300], buf[4096];

	if (!proc)
		return 0;
	while ((de = readdir(proc))) {
		if (de->d_name[0] < '0' || de->d_name[0] > '9')
			continue;
		snprintf(path, sizeof(path), "/proc/%s/fdinfo", de->d_name);
		DIR *fdi = opendir(path);
		struct dirent *fe;
		if (!fdi)
			continue;
		while ((fe = readdir(fdi))) {
			if (fe->d_name[0] == '.')
				continue;
			char fp[320];
			int n = snprintf(fp, sizeof(fp), "%s/%s", path, fe->d_name);
			if (n < 0 || n >= (int)sizeof(fp))
				continue;
			int f = open(fp, O_RDONLY);
			if (f < 0)
				continue;
			ssize_t r = read(f, buf, sizeof(buf) - 1);
			close(f);
			if (r <= 0)
				continue;
			buf[r] = 0;
			char *p = strstr(buf, "drm-engine-gfx:");
			if (p)
				total += strtoull(p + 15, NULL, 10);
		}
		closedir(fdi);
	}
	closedir(proc);
	return total;
}

static int set_gfx(uint32_t mhz)
{
	uint32_t st = 0;
	if (smn_mbox(Q0_CMD, Q0_RSP, Q0_ARG, MSG_REQUEST_GFXCLK, mhz, &st, NULL) || st != 1)
		return -1;
	return 0;
}

static uint32_t level_to_mhz(int lvl)
{
	switch (lvl) {
	case 3: return G_MAX; case 2: return G_MID;
	case 1: return G_LOW; default: return G_IDLE;
	}
}

int main(int argc, char **argv)
{
	int opt, cur_lvl, low_streak = 0;
	unsigned long long pg, g;
	struct timespec pt, t, ts;

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

	set_gfx(G_MAX);
	cur_lvl = 3;
	pg = sum_gfx_ns();
	clock_gettime(CLOCK_MONOTONIC, &pt);
	if (verbose)
		printf("ps5_gpu_gov: started, interval=%dms down_count=%d\n", interval_ms, down_count);

	while (!stop) {
		ts.tv_sec = interval_ms / 1000;
		ts.tv_nsec = (long)(interval_ms % 1000) * 1000000L;
		nanosleep(&ts, NULL);

		g = sum_gfx_ns();
		clock_gettime(CLOCK_MONOTONIC, &t);
		unsigned long long wall = (unsigned long long)(t.tv_sec - pt.tv_sec) * 1000000000ULL
					  + (t.tv_nsec - pt.tv_nsec);
		double load = 0.0;
		if (g >= pg && wall > 0)
			load = (double)(g - pg) / (double)wall;
		pg = g; pt = t;

		int demand;
		if (load >= up_high)      demand = 3;
		else if (load >= up_mid)  demand = 2;
		else if (load >= up_low)  demand = 1;
		else                      demand = 0;

		int target = cur_lvl;
		if (demand > cur_lvl) {
			target = demand;
			low_streak = 0;
		} else if (demand < cur_lvl) {
			if (++low_streak >= down_count) {
				target = cur_lvl - 1;
				if (target < demand)
					target = demand;
				low_streak = 0;
			}
		} else {
			low_streak = 0;
		}

		if (target != cur_lvl) {
			if (set_gfx(level_to_mhz(target)) == 0) {
				if (verbose)
					printf("gfx load %.0f%% : %u -> %u MHz\n",
					       load * 100, level_to_mhz(cur_lvl), level_to_mhz(target));
				cur_lvl = target;
			}
		} else if (verbose) {
			printf("gfx load %.0f%% : hold %u MHz\n", load * 100, level_to_mhz(cur_lvl));
		}
	}

	set_gfx(G_MAX);
	if (verbose)
		printf("ps5_gpu_gov: stopped, restored 2000 MHz\n");
	return 0;
}
