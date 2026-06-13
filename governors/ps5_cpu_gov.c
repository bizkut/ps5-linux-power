// SPDX-License-Identifier: GPL-2.0
/*
 * ps5_cpu_gov - userspace PS5 CPU frequency governor, NO kernel module.
 *
 * Reads system CPU load from /proc/stat (no mailbox access for sensing) and
 * drives all cores to a P-state by talking to the SMU MP1 mailbox directly via
 * PCI config space (see ../smn.h). Writes the mailbox ONLY when the target
 * P-state changes, so at steady idle there is zero mailbox traffic.
 *
 * Policy: load >= 0.40 -> boost + P0 (3200+) ; >= 0.20 -> P2 (2327) ;
 *         >= 0.08 -> P5 (1600) ; else -> P7 (800). Shared thermal policy
 *         clamps the fastest allowed P-state when the APU is hot.
 * Ramp UP immediate; ramp DOWN needs `down_count` consecutive low samples.
 * On SIGINT/SIGTERM: leave boost mode, restore P0, and exit.
 *
 * Run as root. Do NOT run a kernel mailbox module or ps5boost concurrently.
 *
 * Usage: ps5_cpu_gov [-i ms] [-d downcount] [-H high] [-M mid] [-L low] [-T hot] [-R recovery] [-C critical] [-S source] [-v]
 */
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
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
#define STATUS_PATH "/run/ps5-power.cpu"

static double up_high = 0.40, up_mid = 0.20, up_low = 0.08;
static int interval_ms = 500, down_count = 6, verbose, log_every = 10;
static int hot_temp = 85, recovery_temp = 75, critical_temp = 90;
static const char *temp_source = "auto";
static volatile sig_atomic_t stop;

static void on_signal(int s) { (void)s; stop = 1; }

static double parse_load_arg(const char *s)
{
	double v = strtod(s, NULL);

	return v > 1.0 ? v / 100.0 : v;
}

static int path_printf(char *buf, size_t len, const char *fmt, const char *a, const char *b)
{
	int n = snprintf(buf, len, fmt, a, b);

	return n < 0 || n >= (int)len ? -1 : 0;
}

static int validate_args(const char *prog)
{
	if (interval_ms < 50 || down_count < 1 ||
	    up_high <= 0.0 || up_mid <= 0.0 || up_low <= 0.0 ||
	    up_high > 1.0 || up_mid > 1.0 || up_low > 1.0 ||
	    log_every < 1 ||
	    !(up_high > up_mid && up_mid > up_low) ||
	    hot_temp < 1 || hot_temp > 110 ||
	    recovery_temp < 1 || recovery_temp >= hot_temp ||
	    critical_temp < hot_temp || critical_temp > 110 ||
	    (strcmp(temp_source, "auto") && strcmp(temp_source, "gpu") && strcmp(temp_source, "k10temp"))) {
		fprintf(stderr,
			"usage: %s [-i ms>=50] [-d downcount>=1] [-H high] [-M mid] [-L low] [-T hot_c] [-R recovery_c] [-C critical_c] [-S auto|gpu|k10temp] [-v] [-q hold_samples>=1]\n",
			prog);
		return -1;
	}
	return 0;
}

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

static int read_temp_file(const char *path)
{
	char buf[32];
	int fd = open(path, O_RDONLY);
	ssize_t r;

	if (fd < 0)
		return -1;
	r = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (r <= 0)
		return -1;
	buf[r] = 0;
	return atoi(buf) / 1000;
}

static int read_id_file(const char *path, const char *expected)
{
	char buf[32];
	int fd = open(path, O_RDONLY);
	ssize_t r;

	if (fd < 0)
		return 0;
	r = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (r <= 0)
		return 0;
	buf[r] = 0;
	return !strncmp(buf, expected, strlen(expected));
}

static int read_hwmon_name(const char *hwmon, char *buf, size_t len)
{
	char path[256];
	int fd;
	ssize_t r;

	if (path_printf(path, sizeof(path), "%s/%s", hwmon, "name"))
		return -1;
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	r = read(fd, buf, len - 1);
	close(fd);
	if (r <= 0)
		return -1;
	buf[r] = 0;
	buf[strcspn(buf, "\n")] = 0;
	return 0;
}

static int read_named_hwmon_temp(const char *name)
{
	DIR *root = opendir("/sys/class/hwmon");
	struct dirent *de;
	char hwmon[256], hwmon_name[64], temp_path[320];
	int temp = -1;

	if (!root)
		return -1;

	while ((de = readdir(root))) {
		if (strncmp(de->d_name, "hwmon", 5))
			continue;
		if (path_printf(hwmon, sizeof(hwmon), "%s/%s", "/sys/class/hwmon", de->d_name))
			continue;
		if (read_hwmon_name(hwmon, hwmon_name, sizeof(hwmon_name)) || strcmp(hwmon_name, name))
			continue;
		if (path_printf(temp_path, sizeof(temp_path), "%s/%s", hwmon, "temp1_input"))
			continue;
		temp = read_temp_file(temp_path);
		if (temp >= 0)
			break;
	}

	closedir(root);
	return temp;
}

static int is_amd_card(const char *card)
{
	char path[256];

	if (path_printf(path, sizeof(path), "/sys/class/drm/%s/%s", card, "device/vendor"))
		return 0;
	return read_id_file(path, "0x1002");
}

static int is_ps5_gpu_card(const char *card)
{
	char path[256];

	if (!is_amd_card(card))
		return 0;
	if (path_printf(path, sizeof(path), "/sys/class/drm/%s/%s", card, "device/device"))
		return 0;
	return read_id_file(path, "0x13fb");
}

static int read_card_temp(const char *card)
{
	char hwmon_path[256], temp_path[320];
	DIR *hwmon;
	struct dirent *h;
	int temp = -1;

	if (path_printf(hwmon_path, sizeof(hwmon_path), "/sys/class/drm/%s/%s", card, "device/hwmon"))
		return -1;
	hwmon = opendir(hwmon_path);
	if (!hwmon)
		return -1;

	while ((h = readdir(hwmon))) {
		if (strncmp(h->d_name, "hwmon", 5))
			continue;
		if (path_printf(temp_path, sizeof(temp_path), "%s/%s/temp1_input", hwmon_path, h->d_name))
			continue;
		temp = read_temp_file(temp_path);
		if (temp >= 0)
			break;
	}
	closedir(hwmon);
	return temp;
}

static int read_drm_gpu_temp(void)
{
	DIR *drm = opendir("/sys/class/drm");
	struct dirent *card;
	char fallback_card[NAME_MAX + 1] = "";
	int amd_cards = 0, temp = -1;

	if (!drm)
		return -1;

	while ((card = readdir(drm))) {
		if (strncmp(card->d_name, "card", 4) || strchr(card->d_name, '-'))
			continue;
		if (is_ps5_gpu_card(card->d_name)) {
			temp = read_card_temp(card->d_name);
			if (temp >= 0)
				break;
		}
		if (is_amd_card(card->d_name)) {
			amd_cards++;
			if (strlen(card->d_name) < sizeof(fallback_card))
				strcpy(fallback_card, card->d_name);
		}
	}

	closedir(drm);
	if (temp < 0 && amd_cards == 1 && fallback_card[0])
		temp = read_card_temp(fallback_card);
	return temp;
}

static int read_apu_temp(void)
{
	int temp = -1;

	if (!strcmp(temp_source, "gpu"))
		return read_drm_gpu_temp();
	if (!strcmp(temp_source, "k10temp"))
		return read_named_hwmon_temp("k10temp");

	temp = read_drm_gpu_temp();
	if (temp < 0)
		temp = read_named_hwmon_temp("k10temp");
	return temp;
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

static uint32_t thermal_min_pstate_for_temp(int temp, int *thermal_cap)
{
	if (temp < 0)
		return *thermal_cap ? (*thermal_cap >= 2 ? P_MID : 1) : P_MAX;
	if (temp >= critical_temp) {
		*thermal_cap = 2;
		return P_MID;
	}
	if (temp >= hot_temp) {
		*thermal_cap = 1;
		return 1;
	}
	if (temp < recovery_temp) {
		*thermal_cap = 0;
		return P_MAX;
	}
	return *thermal_cap ? (*thermal_cap >= 2 ? P_MID : 1) : P_MAX;
}

static void write_status(double load, uint32_t current, uint32_t target,
			 uint32_t demand, uint32_t raw_demand, int temp,
			 int thermal_cap, uint32_t max_pstate, int boost, int low_streak)
{
	char tmp[] = STATUS_PATH ".tmp";
	FILE *f = fopen(tmp, "w");

	if (!f)
		return;
	fprintf(f, "load=%.3f\n", load);
	fprintf(f, "current_pstate=%u\n", current);
	fprintf(f, "current_mhz=%s\n", mhz(current));
	fprintf(f, "target_pstate=%u\n", target);
	fprintf(f, "target_mhz=%s\n", mhz(target));
	fprintf(f, "demand_pstate=%u\n", demand);
	fprintf(f, "demand_mhz=%s\n", mhz(demand));
	fprintf(f, "raw_demand_pstate=%u\n", raw_demand);
	fprintf(f, "raw_demand_mhz=%s\n", mhz(raw_demand));
	fprintf(f, "temperature_c=%d\n", temp);
	fprintf(f, "thermal_cap=%d\n", thermal_cap);
	fprintf(f, "max_pstate=%u\n", max_pstate);
	fprintf(f, "max_mhz=%s\n", mhz(max_pstate));
	fprintf(f, "boost=%d\n", boost);
	fprintf(f, "low_streak=%d\n", low_streak);
	fclose(f);
	rename(tmp, STATUS_PATH);
}

int main(int argc, char **argv)
{
	int opt, low_streak = 0;
	int boost_on = 0, thermal_cap = 0;
	int hold_logs = 0;
	unsigned long long pb = 0, pt = 0, b, t;
	uint32_t current = P_MAX, target;
	struct timespec ts;

	while ((opt = getopt(argc, argv, "i:d:H:M:L:T:R:C:S:vq:")) != -1) {
		switch (opt) {
		case 'i': interval_ms = atoi(optarg); break;
		case 'd': down_count = atoi(optarg); break;
		case 'H': up_high = parse_load_arg(optarg); break;
		case 'M': up_mid = parse_load_arg(optarg); break;
		case 'L': up_low = parse_load_arg(optarg); break;
		case 'T': hot_temp = atoi(optarg); break;
		case 'R': recovery_temp = atoi(optarg); break;
		case 'C': critical_temp = atoi(optarg); break;
		case 'S': temp_source = optarg; break;
		case 'v': verbose = 1; break;
		case 'q': log_every = atoi(optarg); break;
		default:
			fprintf(stderr, "usage: %s [-i ms] [-d downcount] [-H high] [-M mid] [-L low] [-T hot_c] [-R recovery_c] [-C critical_c] [-S auto|gpu|k10temp] [-v] [-q hold_samples]\n", argv[0]);
			return 2;
		}
	}
	if (validate_args(argv[0]))
		return 2;

	if (access(MP1_DEV, R_OK | W_OK)) {
		perror("open " MP1_DEV " (required for boost)");
		return 1;
	}
	if (smn_open()) return 1;
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	smn_boost_vote(SMN_BOOST_CPU, 0);
	set_pstate(P_MAX);
	current = P_MAX;
	read_cpu(&pb, &pt);
	if (verbose)
		printf("ps5_cpu_gov: started, interval=%dms down_count=%d log_every=%d load=%.0f/%.0f/%.0f%% thermal=%d/%d/%dC temp_source=%s\n",
		       interval_ms, down_count, log_every, up_high * 100, up_mid * 100, up_low * 100,
		       recovery_temp, hot_temp, critical_temp, temp_source);

	while (!stop) {
		ts.tv_sec = interval_ms / 1000;
		ts.tv_nsec = (long)(interval_ms % 1000) * 1000000L;
		nanosleep(&ts, NULL);

		if (read_cpu(&b, &t))
			continue;
		double load = (t > pt) ? (double)(b - pb) / (double)(t - pt) : 0.0;
		pb = b; pt = t;

		uint32_t demand, raw_demand, max_pstate;
		if (load >= up_high)      demand = P_MAX;
		else if (load >= up_mid)  demand = P_MID;
		else if (load >= up_low)  demand = P_LOW;
		else                      demand = P_IDLE;
		raw_demand = demand;

		int temp = read_apu_temp();
		int prev_cap = thermal_cap;
		max_pstate = thermal_min_pstate_for_temp(temp, &thermal_cap);
		if (demand < max_pstate)
			demand = max_pstate;
		if (verbose && thermal_cap != prev_cap)
			printf("cpu event=thermal temp=%dC cap_level=%d max=P%u(%s)\n",
			       temp, thermal_cap, max_pstate, mhz(max_pstate));

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

		if (demand == P_MAX && !thermal_cap && !boost_on) {
			if (smn_boost_vote(SMN_BOOST_CPU, 1) == 0) {
				boost_on = 1;
				if (verbose)
					printf("cpu event=boost_on load=%.0f%%\n", load * 100);
			}
		}

		if (target != current) {
			if (set_pstate(target) == 0) {
				if (verbose)
					printf("cpu event=clock load=%.0f%% from=P%u(%s) to=P%u(%s) boost=%d\n",
					       load * 100, current, mhz(current), target, mhz(target), boost_on);
				current = target;
			}
		} else if (verbose && ++hold_logs >= log_every) {
			hold_logs = 0;
			printf("cpu event=hold load=%.0f%% target=P%u(%s) boost=%d\n",
			       load * 100, current, mhz(current), boost_on);
		}

		if ((demand != P_MAX || thermal_cap) && boost_on) {
			if (smn_boost_vote(SMN_BOOST_CPU, 0) == 0) {
				boost_on = 0;
				if (verbose)
					printf("cpu event=boost_off load=%.0f%%\n", load * 100);
			}
		}

		write_status(load, current, target, demand, raw_demand, temp,
			     thermal_cap, max_pstate, boost_on, low_streak);
	}

	set_pstate(P_MAX);
	smn_boost_vote(SMN_BOOST_CPU, 0);
	unlink(STATUS_PATH);
	if (verbose)
		printf("ps5_cpu_gov: stopped boost=0 restored=P0(3200)\n");
	return 0;
}
