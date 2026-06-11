// SPDX-License-Identifier: GPL-2.0
/*
 * ps5_gpu_gov - userspace PS5 GPU (GFXCLK) governor, NO kernel module.
 *
 * Senses GPU load WITHOUT touching the mailbox: sums the `drm-engine-gfx`
 * nanosecond counter across all amdgpu DRM clients (proc fdinfo), computes
 * load = delta(gfx-ns)/delta(wall-ns), and drives GFXCLK via the SMU MP1
 * mailbox directly over PCI config space (see ../smn.h). Writes only when the
 * target changes by a meaningful amount.
 *
 * Policy: compute a moving target inside the requested frequency range, up to
 * 2000 MHz normally or 2230 MHz with boost.
 * Thermal guard: staged cap. Warm disables boost, hot caps at 1500, critical caps at 1200.
 * Ramp UP is rate-limited; burst mode uses a larger step. Ramp DOWN needs
 * `down_count` consecutive low samples.
 * On SIGINT/SIGTERM: leave boost mode, restore 2000 MHz, and exit.
 *
 * Run as root. Do NOT run a kernel mailbox module or ps5boost concurrently.
 *
 * Usage: ps5_gpu_gov [-P auto|quiet|balanced|performance] [-i ms] [-d downcount] [-H high] [-M mid] [-L low]
 *                    [-A adjust_mhz] [-U step_mhz] [-b burst_step_mhz] [-B burst_samples]
 *                    [-n min_mhz] [-x max_mhz] [-T temp] [-R temp] [-S source] [-m method] [-v]
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

#define MSG_REQUEST_GFXCLK 0x0e
#define G_BOOST 2230
#define G_MAX  2000
#define G_MID  1500
#define G_LOW  1200
#define G_IDLE  800
#define STATUS_PATH "/run/ps5-power.gpu"

static double up_high = 0.50, up_mid = 0.20, up_low = 0.05;
static int interval_ms = 500, down_count = 6, throttle_temp = 85, recovery_temp = 75, verbose;
static int adjust_mhz = 50, step_mhz = 150, burst_step_mhz = 500, burst_samples = 3;
static uint32_t range_min_mhz = G_IDLE, range_max_mhz = G_BOOST;
static const char *preset_name = "custom";
static const char *temp_source = "auto";
static const char *load_method = "fdinfo";
static volatile sig_atomic_t stop;

static void on_signal(int s) { (void)s; stop = 1; }

static int apply_preset(const char *preset)
{
	if (!strcmp(preset, "auto") || !strcmp(preset, "balanced")) {
		preset_name = preset;
		interval_ms = 700; down_count = 3;
		up_high = 0.50; up_mid = 0.20; up_low = 0.05;
		adjust_mhz = 50; step_mhz = 150; burst_step_mhz = 500; burst_samples = 3;
		range_min_mhz = G_IDLE; range_max_mhz = G_BOOST;
		throttle_temp = 85; recovery_temp = 75;
		temp_source = "auto"; load_method = "fdinfo";
		return 0;
	}
	if (!strcmp(preset, "quiet")) {
		preset_name = preset;
		interval_ms = 900; down_count = 6;
		up_high = 0.65; up_mid = 0.30; up_low = 0.08;
		adjust_mhz = 80; step_mhz = 120; burst_step_mhz = 360; burst_samples = 4;
		range_min_mhz = G_IDLE; range_max_mhz = G_MAX;
		throttle_temp = 80; recovery_temp = 70;
		temp_source = "auto"; load_method = "fdinfo";
		return 0;
	}
	if (!strcmp(preset, "performance")) {
		preset_name = preset;
		interval_ms = 350; down_count = 5;
		up_high = 0.30; up_mid = 0.12; up_low = 0.03;
		adjust_mhz = 30; step_mhz = 220; burst_step_mhz = 700; burst_samples = 2;
		range_min_mhz = G_LOW; range_max_mhz = G_BOOST;
		throttle_temp = 90; recovery_temp = 80;
		temp_source = "auto"; load_method = "fdinfo";
		return 0;
	}
	return -1;
}

static int path_printf(char *buf, size_t len, const char *fmt, const char *a, const char *b)
{
	int n = snprintf(buf, len, fmt, a, b);

	return n < 0 || n >= (int)len ? -1 : 0;
}

static double parse_load_arg(const char *s)
{
	double v = strtod(s, NULL);

	return v > 1.0 ? v / 100.0 : v;
}

static uint32_t parse_mhz_arg(const char *s)
{
	long v = strtol(s, NULL, 10);

	return v < 0 ? 0 : (uint32_t)v;
}

static int validate_args(const char *prog)
{
	if (interval_ms < 50 || down_count < 1 ||
	    adjust_mhz < 1 || adjust_mhz > 500 ||
	    step_mhz < 1 || step_mhz > 1000 ||
	    burst_step_mhz < step_mhz || burst_step_mhz > 1500 ||
	    burst_samples < 0 || burst_samples > 64 ||
	    range_min_mhz < 400 || range_min_mhz > G_BOOST ||
	    range_max_mhz < 400 || range_max_mhz > G_BOOST ||
	    range_min_mhz > range_max_mhz ||
	    up_high <= 0.0 || up_mid <= 0.0 || up_low <= 0.0 ||
	    up_high > 1.0 || up_mid > 1.0 || up_low > 1.0 ||
	    !(up_high > up_mid && up_mid > up_low) ||
	    throttle_temp < 1 || throttle_temp > 110 ||
	    recovery_temp < 1 || recovery_temp >= throttle_temp ||
	    (strcmp(temp_source, "auto") && strcmp(temp_source, "gpu") && strcmp(temp_source, "k10temp")) ||
	    (strcmp(load_method, "fdinfo") && strcmp(load_method, "debugfs") &&
	     strcmp(load_method, "auto") && strcmp(load_method, "busy"))) {
		fprintf(stderr,
			"usage: %s [-P auto|quiet|balanced|performance] [-i ms>=50] [-d downcount>=1] [-H high] [-M mid] [-L low] [-A adjust_mhz] [-U step_mhz] [-b burst_step_mhz] [-B burst_samples] [-n 400..2230] [-x 400..2230] [-T 1..110] [-R 1..T-1] [-S auto|gpu|k10temp] [-m fdinfo|debugfs|auto|busy] [-v]\n",
			prog);
		return -1;
	}
	return 0;
}

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

static int read_debugfs_load(double *load)
{
	const char *paths[] = {
		"/sys/kernel/debug/dri/0/amdgpu_pm_info",
		"/sys/kernel/debug/dri/1/amdgpu_pm_info",
		NULL
	};
	char buf[8192];
	int i;

	for (i = 0; paths[i]; i++) {
		int fd = open(paths[i], O_RDONLY);
		ssize_t r;
		char *p;

		if (fd < 0)
			continue;
		r = read(fd, buf, sizeof(buf) - 1);
		close(fd);
		if (r <= 0)
			continue;
		buf[r] = 0;
		p = strstr(buf, "GPU Load:");
		if (!p)
			p = strstr(buf, "GPU load:");
		if (!p)
			continue;
		while (*p && (*p < '0' || *p > '9'))
			p++;
		if (!*p)
			continue;
		*load = strtod(p, NULL) / 100.0;
		return 0;
	}

	return -1;
}

static int set_gfx(uint32_t mhz)
{
	uint32_t st = 0;
	if (smn_mbox(Q0_CMD, Q0_RSP, Q0_ARG, MSG_REQUEST_GFXCLK, mhz, &st, NULL) || st != 1)
		return -1;
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

static int read_gpu_temp(void)
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

static uint32_t cap_to_mhz(int cap)
{
	switch (cap) {
	case 1: return G_LOW;
	case 2: return G_MID;
	case 3: return G_MAX;
	default: return G_BOOST;
	}
}

static uint32_t clamp_mhz(uint32_t mhz, uint32_t min, uint32_t max)
{
	if (mhz < min)
		return min;
	if (mhz > max)
		return max;
	return mhz;
}

static uint32_t round10(uint32_t mhz)
{
	return ((mhz + 5) / 10) * 10;
}

static uint32_t interp_mhz(double load, double lo_load, double hi_load, uint32_t lo_mhz, uint32_t hi_mhz)
{
	double span, pos;

	if (load <= lo_load)
		return lo_mhz;
	if (load >= hi_load)
		return hi_mhz;
	span = hi_load - lo_load;
	pos = (load - lo_load) / span;
	return round10((uint32_t)(lo_mhz + (hi_mhz - lo_mhz) * pos));
}

static uint32_t desired_mhz_for_load(double load, uint32_t max_mhz)
{
	uint32_t desired;

	if (load < up_low)
		desired = G_IDLE;
	else if (load < up_mid)
		desired = interp_mhz(load, up_low, up_mid, G_LOW, G_MID);
	else if (load < up_high)
		desired = interp_mhz(load, up_mid, up_high, G_MID, G_MAX);
	else
		desired = max_mhz;

	return clamp_mhz(desired, G_IDLE, max_mhz);
}

static unsigned int mhz_diff(uint32_t a, uint32_t b)
{
	return a > b ? a - b : b - a;
}

static void write_status(double load, int temp, uint32_t current, uint32_t target,
			 uint32_t desired, uint32_t min_mhz, uint32_t max_mhz,
			 int boost, int thermal_cap, int burst)
{
	char tmp[] = STATUS_PATH ".tmp";
	FILE *f = fopen(tmp, "w");

	if (!f)
		return;
	fprintf(f, "preset=%s\n", preset_name);
	fprintf(f, "load=%.3f\n", load);
	fprintf(f, "temperature_c=%d\n", temp);
	fprintf(f, "current_mhz=%u\n", current);
	fprintf(f, "target_mhz=%u\n", target);
	fprintf(f, "desired_mhz=%u\n", desired);
	fprintf(f, "range_min_mhz=%u\n", min_mhz);
	fprintf(f, "range_max_mhz=%u\n", max_mhz);
	fprintf(f, "boost=%d\n", boost);
	fprintf(f, "thermal_cap=%d\n", thermal_cap);
	fprintf(f, "burst=%d\n", burst);
	fclose(f);
	rename(tmp, STATUS_PATH);
}

int main(int argc, char **argv)
{
	int opt, low_streak = 0, burst_streak = 0;
	int boost_on = 0, boost_changed, thermal_cap = 0;
	uint32_t cur_mhz = G_MAX, target_mhz = G_MAX;
	unsigned long long pg, g;
	struct timespec pt, t, ts;

	while ((opt = getopt(argc, argv, "P:i:d:H:M:L:A:U:b:B:n:x:T:R:S:m:v")) != -1) {
		switch (opt) {
		case 'P':
			if (apply_preset(optarg)) {
				fprintf(stderr, "invalid preset '%s' (use auto, quiet, balanced, or performance)\n", optarg);
				return 2;
			}
			break;
		case 'i': interval_ms = atoi(optarg); break;
		case 'd': down_count = atoi(optarg); break;
		case 'H': up_high = parse_load_arg(optarg); break;
		case 'M': up_mid = parse_load_arg(optarg); break;
		case 'L': up_low = parse_load_arg(optarg); break;
		case 'A': adjust_mhz = atoi(optarg); break;
		case 'U': step_mhz = atoi(optarg); break;
		case 'b': burst_step_mhz = atoi(optarg); break;
		case 'B': burst_samples = atoi(optarg); break;
		case 'n': range_min_mhz = parse_mhz_arg(optarg); break;
		case 'x': range_max_mhz = parse_mhz_arg(optarg); break;
		case 'T': throttle_temp = atoi(optarg); break;
		case 'R': recovery_temp = atoi(optarg); break;
		case 'S': temp_source = optarg; break;
		case 'm': load_method = optarg; break;
		case 'v': verbose = 1; break;
		default:
			fprintf(stderr, "usage: %s [-P auto|quiet|balanced|performance] [-i ms] [-d downcount] [-H high] [-M mid] [-L low] [-A adjust_mhz] [-U step_mhz] [-b burst_step_mhz] [-B burst_samples] [-n min_mhz] [-x max_mhz] [-T throttle_c] [-R recovery_c] [-S auto|gpu|k10temp] [-m fdinfo|debugfs|auto|busy] [-v]\n", argv[0]);
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

	smn_boost_vote(SMN_BOOST_GPU, 0);
	set_gfx(G_MAX);
	pg = sum_gfx_ns();
	clock_gettime(CLOCK_MONOTONIC, &pt);
	if (verbose)
		printf("ps5_gpu_gov: started preset=%s interval_ms=%d down_count=%d load_thresholds=%.0f/%.0f/%.0f adjust_mhz=%d step_mhz=%d burst_step_mhz=%d burst_samples=%d range=%u..%u throttle_c=%d recovery_c=%d temp_source=%s load_method=%s\n",
		       preset_name, interval_ms, down_count, up_high * 100, up_mid * 100, up_low * 100,
		       adjust_mhz, step_mhz, burst_step_mhz, burst_samples, range_min_mhz, range_max_mhz,
		       throttle_temp, recovery_temp, temp_source, load_method);

	while (!stop) {
		ts.tv_sec = interval_ms / 1000;
		ts.tv_nsec = (long)(interval_ms % 1000) * 1000000L;
		nanosleep(&ts, NULL);

		clock_gettime(CLOCK_MONOTONIC, &t);
		unsigned long long wall = (unsigned long long)(t.tv_sec - pt.tv_sec) * 1000000000ULL
					  + (t.tv_nsec - pt.tv_nsec);
		double load = 0.0;
		if ((!strcmp(load_method, "debugfs") || !strcmp(load_method, "busy") ||
		     !strcmp(load_method, "auto")) && read_debugfs_load(&load) == 0) {
			pg = sum_gfx_ns();
		} else {
			g = sum_gfx_ns();
			if (g >= pg && wall > 0)
				load = (double)(g - pg) / (double)wall;
			pg = g;
		}
		pt = t;

		if (load >= up_high)
			burst_streak++;
		else
			burst_streak = 0;

		int temp = read_gpu_temp();
		int next_cap = thermal_cap;
		if (temp >= 0) {
			if (temp >= throttle_temp + 5)
				next_cap = 1;
			else if (temp >= throttle_temp)
				next_cap = 2;
			else if (temp >= recovery_temp)
				next_cap = 3;
			else
				next_cap = 0;
		}
		if (next_cap != thermal_cap) {
			thermal_cap = next_cap;
			if (verbose)
				printf("gpu event=thermal temp=%dC cap_level=%d cap_mhz=%u\n",
				       temp, thermal_cap, cap_to_mhz(thermal_cap));
		}

		int high_load = load >= up_high;
		int burst = load >= 0.99 || (burst_samples > 0 && burst_streak >= burst_samples);
		uint32_t requested_max = high_load ? range_max_mhz : clamp_mhz(range_max_mhz, range_min_mhz, G_MAX);
		uint32_t cap_mhz = thermal_cap ? cap_to_mhz(thermal_cap) : requested_max;
		uint32_t min_mhz = thermal_cap && cap_mhz < range_min_mhz ? cap_mhz : range_min_mhz;
		uint32_t max_mhz = clamp_mhz(cap_mhz, min_mhz, requested_max);
		uint32_t desired_mhz = desired_mhz_for_load(load, max_mhz);
		uint32_t step = burst ? (uint32_t)burst_step_mhz : (uint32_t)step_mhz;
		desired_mhz = clamp_mhz(desired_mhz, min_mhz, max_mhz);

		if (thermal_cap && target_mhz > max_mhz) {
			target_mhz = max_mhz;
			low_streak = 0;
		} else if (desired_mhz > target_mhz) {
			target_mhz = clamp_mhz(target_mhz + step, min_mhz, desired_mhz);
			low_streak = 0;
		} else if (desired_mhz < target_mhz) {
			if (++low_streak >= down_count) {
				if (target_mhz > desired_mhz + step)
					target_mhz -= step;
				else
					target_mhz = desired_mhz;
				low_streak = 0;
			}
		} else {
			low_streak = 0;
		}
		target_mhz = clamp_mhz(target_mhz, min_mhz, max_mhz);

		boost_changed = 0;
		int need_boost = !thermal_cap && (high_load || burst || target_mhz > G_MAX);
		if (need_boost && !boost_on) {
			if (smn_boost_vote(SMN_BOOST_GPU, 1) == 0) {
				boost_on = 1;
				boost_changed = 1;
				if (verbose)
					printf("gpu event=boost_on load=%.0f%% temp=%dC target=%u thermal_cap=%d burst=%d\n",
					       load * 100, temp, target_mhz, thermal_cap, burst);
			}
		} else if (!need_boost && boost_on) {
			if (smn_boost_vote(SMN_BOOST_GPU, 0) == 0) {
				boost_on = 0;
				boost_changed = 1;
				if (verbose)
					printf("gpu event=boost_off load=%.0f%% temp=%dC target=%u thermal_cap=%d burst=%d\n",
					       load * 100, temp, target_mhz, thermal_cap, burst);
			}
		}

		int hit_bound = target_mhz == min_mhz || target_mhz == max_mhz;
		int meaningful = mhz_diff(cur_mhz, target_mhz) >= (unsigned int)adjust_mhz;
		if ((cur_mhz != target_mhz && (meaningful || burst || hit_bound)) || boost_changed) {
			if (set_gfx(target_mhz) == 0) {
				if (verbose)
					printf("gpu event=clock load=%.0f%% temp=%dC from=%u to=%u desired=%u boost=%d thermal_cap=%d burst=%d\n",
					       load * 100, temp, cur_mhz, target_mhz, desired_mhz,
					       boost_on, thermal_cap, burst);
				cur_mhz = target_mhz;
			}
		} else if (verbose) {
			printf("gpu event=hold load=%.0f%% temp=%dC current=%u target=%u desired=%u boost=%d thermal_cap=%d burst=%d\n",
			       load * 100, temp, cur_mhz, target_mhz, desired_mhz,
			       boost_on, thermal_cap, burst);
		}
		write_status(load, temp, cur_mhz, target_mhz, desired_mhz, min_mhz,
			     max_mhz, boost_on, thermal_cap, burst);
	}

	set_gfx(G_MAX);
	smn_boost_vote(SMN_BOOST_GPU, 0);
	unlink(STATUS_PATH);
	if (verbose)
		printf("ps5_gpu_gov: stopped boost=0 restored=2000\n");
	return 0;
}
