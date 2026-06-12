/* SPDX-License-Identifier: GPL-2.0 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define PS5_FAN_DEV "/dev/ps5-fan"
#define ICC_DEV "/dev/icc"
#define FAN_LOCK "/run/lock/ps5-power.lock"
#define FAN_STATE "/run/ps5-power.fan"

#ifndef _IOC_NRBITS
#define _IOC_NRBITS 8
#endif
#ifndef _IOC_TYPEBITS
#define _IOC_TYPEBITS 8
#endif
#ifndef _IOC_SIZEBITS
#define _IOC_SIZEBITS 14
#endif
#ifndef _IOC_DIRBITS
#define _IOC_DIRBITS 2
#endif
#ifndef _IOC_NRSHIFT
#define _IOC_NRSHIFT 0
#endif
#ifndef _IOC_TYPESHIFT
#define _IOC_TYPESHIFT (_IOC_NRSHIFT + _IOC_NRBITS)
#endif
#ifndef _IOC_SIZESHIFT
#define _IOC_SIZESHIFT (_IOC_TYPESHIFT + _IOC_TYPEBITS)
#endif
#ifndef _IOC_DIRSHIFT
#define _IOC_DIRSHIFT (_IOC_SIZESHIFT + _IOC_SIZEBITS)
#endif
#ifndef _IOC_NONE
#define _IOC_NONE 0U
#endif
#ifndef _IOC_WRITE
#define _IOC_WRITE 1U
#endif
#ifndef _IOC
#define _IOC(dir, type, nr, size) \
	(((dir) << _IOC_DIRSHIFT) | ((type) << _IOC_TYPESHIFT) | \
	 ((nr) << _IOC_NRSHIFT) | ((size) << _IOC_SIZESHIFT))
#endif
#ifndef _IOC_TYPECHECK
#define _IOC_TYPECHECK(t) (sizeof(t))
#endif
#ifndef _IOW
#define _IOW(type, nr, size) \
	_IOC(_IOC_WRITE, (type), (nr), _IOC_TYPECHECK(size))
#endif

#define ICC_IOC_MAGIC 'I'
#define ICC_FAN_CHANGE_SERVO_PATTERN _IOW(ICC_IOC_MAGIC, 1, unsigned char)
#define PS5_FAN_IOC_MAGIC 'F'

struct ps5_fan_pattern {
	unsigned char pattern;
};

#define PS5_FAN_CHANGE_SERVO_PATTERN _IOW(PS5_FAN_IOC_MAGIC, 1, struct ps5_fan_pattern)

#define FAN_PATTERN_DEFAULT 0
#define FAN_PATTERN_COOL 1
#define MAX_CURVE_POINTS 8

struct ps5_fan_target_temp {
	unsigned char zone;
	unsigned char temperature_c;
};

#define PS5_FAN_TARGET_TEMP_SET _IOW(PS5_FAN_IOC_MAGIC, 6, struct ps5_fan_target_temp)

struct curve_point {
	int up_c;
	int target_c;
	unsigned char pattern;
};

static volatile sig_atomic_t stop;
static int verbose;
static int restore_on_exit = 1;
static int log_every = 10;
static int hysteresis_c = 4;
static int curve_len;
static struct curve_point curve[MAX_CURVE_POINTS];

static void on_signal(int s)
{
	(void)s;
	stop = 1;
}

static void ensure_runtime_dirs(void)
{
	if (mkdir("/run", 0755) && errno != EEXIST)
		return;
	mkdir("/run/lock", 0755);
}

static int write_state(unsigned char pattern, int temp_c, int on_temp, int off_temp,
		       unsigned char default_pattern, unsigned char cool_pattern,
		       int curve_stage, int target_temp_c,
		       const char *sensor, const char *reason)
{
	char tmp[] = FAN_STATE ".tmp";
	FILE *f = fopen(tmp, "w");

	if (!f)
		return -1;
	fprintf(f, "pattern=%u\n", pattern);
	fprintf(f, "fan=%d\n", pattern == cool_pattern);
	fprintf(f, "temperature_c=%d\n", temp_c);
	fprintf(f, "on_temp_c=%d\n", on_temp);
	fprintf(f, "off_temp_c=%d\n", off_temp);
	fprintf(f, "default_pattern=%u\n", default_pattern);
	fprintf(f, "cool_pattern=%u\n", cool_pattern);
	fprintf(f, "curve_stage=%d\n", curve_stage);
	fprintf(f, "target_temp_c=%d\n", target_temp_c);
	fprintf(f, "sensor=%s\n", sensor ? sensor : "unknown");
	fprintf(f, "reason=%s\n", reason ? reason : "unknown");
	fclose(f);
	return rename(tmp, FAN_STATE);
}

static int set_fan_pattern_locked(int fd, const char *dev, unsigned char pattern)
{
	int ret;

	if (!strcmp(dev, PS5_FAN_DEV)) {
		struct ps5_fan_pattern req = { .pattern = pattern };

		ret = ioctl(fd, PS5_FAN_CHANGE_SERVO_PATTERN, &req);
	} else {
		ret = ioctl(fd, ICC_FAN_CHANGE_SERVO_PATTERN, &pattern);
	}
	if (ret < 0)
		perror(dev);
	return ret < 0 ? -1 : 0;
}

static int set_fan_pattern(unsigned char pattern)
{
	int fd, lock_fd, ret = -1;

	ensure_runtime_dirs();
	lock_fd = open(FAN_LOCK, O_CREAT | O_RDWR | O_CLOEXEC, 0644);
	if (lock_fd < 0) {
		perror("open " FAN_LOCK);
		return -1;
	}
	if (flock(lock_fd, LOCK_EX) < 0) {
		perror("flock " FAN_LOCK);
		close(lock_fd);
		return -1;
	}

	fd = open(PS5_FAN_DEV, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fd = open(ICC_DEV, O_RDWR | O_CLOEXEC);
		if (fd < 0) {
			perror("open " PS5_FAN_DEV " or " ICC_DEV);
			goto out;
		}
		ret = set_fan_pattern_locked(fd, ICC_DEV, pattern);
	} else {
		ret = set_fan_pattern_locked(fd, PS5_FAN_DEV, pattern);
	}
	close(fd);
out:
	flock(lock_fd, LOCK_UN);
	close(lock_fd);
	return ret;
}

static int set_target_temp(int target_c)
{
	struct ps5_fan_target_temp req = {
		.zone = 0,
		.temperature_c = (unsigned char)target_c,
	};
	int fd, ret;

	if (target_c < 1 || target_c > 110)
		return -1;
	fd = open(PS5_FAN_DEV, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return -1;
	ret = ioctl(fd, PS5_FAN_TARGET_TEMP_SET, &req);
	if (ret < 0 && verbose)
		perror(PS5_FAN_DEV " target-temp");
	close(fd);
	return ret < 0 ? -1 : 0;
}

static int parse_u8_field(char **cursor, int *out, char delim)
{
	char *end;
	long v;

	errno = 0;
	v = strtol(*cursor, &end, 10);
	if (errno || v < 0 || v > 255 || end == *cursor || *end != delim)
		return -1;
	*out = (int)v;
	*cursor = end + 1;
	return 0;
}

static int parse_curve(const char *spec)
{
	char *copy, *tok, *save = NULL;
	int last_up = -1;

	if (!spec || !*spec)
		return -1;
	copy = strdup(spec);
	if (!copy)
		return -1;
	curve_len = 0;
	for (tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
		char *p = tok;
		int up_c, target_c, pattern;

		if (curve_len >= MAX_CURVE_POINTS ||
		    parse_u8_field(&p, &up_c, ':') ||
		    parse_u8_field(&p, &target_c, ':'))
			goto bad;
		errno = 0;
		pattern = (int)strtol(p, &p, 10);
		if (errno || *p || pattern < 0 || pattern > 255 ||
		    target_c < 1 || target_c > 110 || up_c <= last_up)
			goto bad;
		curve[curve_len].up_c = up_c;
		curve[curve_len].target_c = target_c;
		curve[curve_len].pattern = (unsigned char)pattern;
		last_up = up_c;
		curve_len++;
	}
	free(copy);
	return curve_len > 0 ? 0 : -1;
bad:
	free(copy);
	curve_len = 0;
	return -1;
}

static int curve_stage_for_temp(int temp_c, int current_stage)
{
	int next = current_stage;

	if (next < 0)
		next = 0;
	while (next + 1 < curve_len && temp_c >= curve[next + 1].up_c)
		next++;
	while (next > 0 && temp_c <= curve[next].up_c - hysteresis_c)
		next--;
	return next;
}

static int read_int_file(const char *path, int *out_millideg)
{
	FILE *f = fopen(path, "r");
	long long v;
	if (!f)
		return -1;
	if (fscanf(f, "%lld", &v) != 1) {
		fclose(f);
		return -1;
	}
	fclose(f);
	if (v < 0)
		return -1;
	*out_millideg = (int)v;
	return 0;
}

static int str_has_suffix(const char *s, const char *suffix)
{
	size_t sl = strlen(s), sufl = strlen(suffix);

	return sl >= sufl && !strcmp(s + sl - sufl, suffix);
}

static int read_sysfs_string(const char *path, char *buf, size_t len)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return -1;
	if (!fgets(buf, (int)len, f)) {
		fclose(f);
		return -1;
	}
	fclose(f);
	buf[strcspn(buf, "\n")] = 0;
	return 0;
}

static int find_named_hwmon(const char *name, char *temp_path, size_t temp_len)
{
	DIR *root = opendir("/sys/class/hwmon");
	struct dirent *de;
	char hwmon_path[256];
	char hwmon_name[64];
	int found = 0;

	if (!root)
		return -1;
	temp_path[0] = 0;
	while ((de = readdir(root))) {
		if (strncmp(de->d_name, "hwmon", 5))
			continue;
		if (snprintf(hwmon_path, sizeof(hwmon_path), "%s/%s", "/sys/class/hwmon", de->d_name) < 0)
			continue;

		char name_path[256];
		if (snprintf(name_path, sizeof(name_path), "%s/name", hwmon_path) < 0)
			continue;
		if (read_sysfs_string(name_path, hwmon_name, sizeof(hwmon_name)))
			continue;
		if (strcmp(hwmon_name, name))
			continue;

		if (snprintf(temp_path, temp_len, "%s/temp1_input", hwmon_path) > 0) {
			found = 1;
			break;
		}
	}
	closedir(root);
	if (!found)
		return -1;
	return access(temp_path, R_OK) ? -1 : 0;
}

static int find_hottest_temp(char *source, size_t source_len, int *out_c)
{
	DIR *root = opendir("/sys/class/hwmon");
	struct dirent *de;
	int best_mdeg = -1;

	if (!root)
		return -1;
	while ((de = readdir(root))) {
		char hwmon_path[256], name_path[320], hwmon_name[64] = "unknown";
		DIR *hwmon;
		struct dirent *te;

		if (strncmp(de->d_name, "hwmon", 5))
			continue;
		if (snprintf(hwmon_path, sizeof(hwmon_path), "/sys/class/hwmon/%s", de->d_name) < 0)
			continue;
		if (snprintf(name_path, sizeof(name_path), "%s/name", hwmon_path) > 0)
			read_sysfs_string(name_path, hwmon_name, sizeof(hwmon_name));
		hwmon = opendir(hwmon_path);
		if (!hwmon)
			continue;
		while ((te = readdir(hwmon))) {
			char temp_path[320];
			int temp_mdeg;

			if (strncmp(te->d_name, "temp", 4) || !str_has_suffix(te->d_name, "_input"))
				continue;
			if (snprintf(temp_path, sizeof(temp_path), "%s/%s", hwmon_path, te->d_name) < 0)
				continue;
			if (read_int_file(temp_path, &temp_mdeg))
				continue;
			if (temp_mdeg > best_mdeg) {
				best_mdeg = temp_mdeg;
				snprintf(source, source_len, "%s:%s", hwmon_name, temp_path);
			}
		}
		closedir(hwmon);
	}
	closedir(root);
	if (best_mdeg < 0)
		return -1;
	*out_c = best_mdeg / 1000;
	return 0;
}

static int get_temp_c(const char *sensor, int *out_c, char *source, size_t source_len)
{
	char temp_path[320];
	int temp_mdeg;

	if (!strcmp(sensor, "auto")) {
		return find_hottest_temp(source, source_len, out_c);
	}
	if (!strcmp(sensor, "k10temp")) {
		if (!find_named_hwmon("k10temp", temp_path, sizeof(temp_path)))
			goto read_temp;
	}
	if (strncmp(sensor, "/sys/", 5) == 0) {
		snprintf(temp_path, sizeof(temp_path), "%s", sensor);
		goto read_temp;
	}
	if (find_named_hwmon(sensor, temp_path, sizeof(temp_path)) == 0)
		goto read_temp;
	return -1;

read_temp:
	if (read_int_file(temp_path, &temp_mdeg))
		return -1;
	*out_c = temp_mdeg / 1000;
	snprintf(source, source_len, "%s", temp_path);
	return 0;
}

static void usage(const char *p)
{
	fprintf(stderr, "usage: %s [-i ms] [-H on_c] [-L off_c] [-s auto|k10temp|/path/temp_input] [-v] [-q hold_samples] [-n] [-f pattern] [-a default_pattern] [-C cool_pattern] [-c curve] [-y hysteresis_c]\n", p);
}

int main(int argc, char **argv)
{
	unsigned interval_ms = 1000;
	int on_temp = 55, off_temp = 45, temp_c, fan_on = 0, target_fan_on;
	int force_set = -1, read_failures = 0;
	int hold_logs = 0;
	int curve_stage = -1, next_stage, target_temp_c = -1;
	unsigned long pattern_arg;
	unsigned char default_pattern = FAN_PATTERN_DEFAULT;
	unsigned char cool_pattern = FAN_PATTERN_COOL;
	unsigned char current_pattern;
	const char *sensor = "auto";
	char source[384] = "unknown";
	char *endptr;
	int opt;
	int ret;

	while ((opt = getopt(argc, argv, "i:H:L:s:vq:nf:a:C:c:y:")) != -1) {
		switch (opt) {
		case 'i':
			interval_ms = strtoul(optarg, &endptr, 10);
			if (*endptr || interval_ms < 100)
				goto bad;
			break;
		case 'H':
			on_temp = (int)strtoul(optarg, &endptr, 10);
			if (*endptr)
				goto bad;
			break;
		case 'L':
			off_temp = (int)strtoul(optarg, &endptr, 10);
			if (*endptr)
				goto bad;
			break;
		case 's':
			sensor = optarg;
			break;
		case 'v':
			verbose = 1;
			break;
		case 'q':
			log_every = (int)strtoul(optarg, &endptr, 10);
			if (*endptr || log_every < 1)
				goto bad;
			break;
		case 'n':
			restore_on_exit = 0;
			break;
		case 'f':
			pattern_arg = strtoul(optarg, &endptr, 10);
			if (*endptr || pattern_arg > 255)
				goto bad;
			force_set = (int)pattern_arg;
			break;
		case 'a':
			pattern_arg = strtoul(optarg, &endptr, 10);
			if (*endptr || pattern_arg > 255)
				goto bad;
			default_pattern = (unsigned char)pattern_arg;
			break;
		case 'C':
			pattern_arg = strtoul(optarg, &endptr, 10);
			if (*endptr || pattern_arg > 255)
				goto bad;
			cool_pattern = (unsigned char)pattern_arg;
			break;
		case 'c':
			if (parse_curve(optarg))
				goto bad;
			break;
		case 'y':
			hysteresis_c = (int)strtoul(optarg, &endptr, 10);
			if (*endptr || hysteresis_c < 1 || hysteresis_c > 20)
				goto bad;
			break;
		default:
bad:
			usage(argv[0]);
			return 2;
		}
	}
	if (default_pattern == cool_pattern) {
		fprintf(stderr, "default and cool fan patterns must differ\n");
		return 2;
	}

	if (off_temp < 0 || on_temp > 200 || off_temp >= on_temp) {
		fprintf(stderr, "invalid thresholds: off must be < on and on <= 200\n");
		return 2;
	}
	if (argc - optind != 0) {
		usage(argv[0]);
		return 2;
	}

	if (force_set >= 0) {
		current_pattern = (unsigned char)force_set;
		if (set_fan_pattern(current_pattern))
			return 1;
		write_state(current_pattern, -1, on_temp, off_temp, default_pattern,
			    cool_pattern, -1, -1, "manual", "force");
		if (verbose)
			printf("fan event=force pattern=%u\n", current_pattern);
		return 0;
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	if (get_temp_c(sensor, &temp_c, source, sizeof(source))) {
		fprintf(stderr, "couldn't read temperature from sensor=%s\n", sensor);
		return 1;
	}

	if (curve_len > 0) {
		curve_stage = curve_stage_for_temp(temp_c, 0);
		current_pattern = curve[curve_stage].pattern;
		target_temp_c = curve[curve_stage].target_c;
	} else {
		target_fan_on = fan_on = temp_c >= on_temp;
		current_pattern = fan_on ? cool_pattern : default_pattern;
	}
	if (set_fan_pattern(current_pattern))
		return 1;
	if (curve_len > 0)
		set_target_temp(target_temp_c);
	write_state(current_pattern, temp_c, on_temp, off_temp, default_pattern,
		    cool_pattern, curve_stage, target_temp_c, source, "init");
	if (verbose)
		printf("fan event=init temp=%dC pattern=%u target_temp=%d curve_stage=%d on=%d on_temp=%d off_temp=%d log_every=%d sensor=%s source=%s\n",
		       temp_c, current_pattern, target_temp_c, curve_stage, fan_on, on_temp, off_temp, log_every, sensor, source);

	while (!stop) {
		int sleep_ms = (int)interval_ms;
		struct timespec ts = { .tv_sec = sleep_ms / 1000, .tv_nsec = (sleep_ms % 1000) * 1000000L };

		nanosleep(&ts, NULL);
		if (stop)
			break;

		if (get_temp_c(sensor, &temp_c, source, sizeof(source))) {
			if (++read_failures >= 3 && !fan_on && set_fan_pattern(cool_pattern) == 0) {
				fan_on = 1;
				current_pattern = cool_pattern;
				write_state(current_pattern, -1, on_temp, off_temp, default_pattern,
					    cool_pattern, curve_stage, target_temp_c, sensor, "sensor_fail_safe");
				if (verbose)
					printf("fan event=fail_safe pattern=%u sensor=%s\n", current_pattern, sensor);
			}
			continue;
		}
		read_failures = 0;

		if (curve_len > 0) {
			next_stage = curve_stage_for_temp(temp_c, curve_stage);
			if (next_stage != curve_stage) {
				curve_stage = next_stage;
				current_pattern = curve[curve_stage].pattern;
				target_temp_c = curve[curve_stage].target_c;
				if (set_fan_pattern(current_pattern)) {
					fprintf(stderr, "failed to set fan pattern=%u at temp=%dC\n", current_pattern, temp_c);
					continue;
				}
				set_target_temp(target_temp_c);
				write_state(current_pattern, temp_c, on_temp, off_temp, default_pattern,
					    cool_pattern, curve_stage, target_temp_c, source, "curve");
				if (verbose)
					printf("fan event=curve temp=%dC stage=%d pattern=%u target_temp=%d source=%s\n",
					       temp_c, curve_stage, current_pattern, target_temp_c, source);
			} else {
				write_state(current_pattern, temp_c, on_temp, off_temp, default_pattern,
					    cool_pattern, curve_stage, target_temp_c, source, "hold");
				if (verbose && ++hold_logs >= log_every) {
					hold_logs = 0;
					printf("fan event=hold temp=%dC stage=%d pattern=%u target_temp=%d source=%s\n",
					       temp_c, curve_stage, current_pattern, target_temp_c, source);
				}
			}
			continue;
		}

		target_fan_on = fan_on ? (temp_c > off_temp) : (temp_c >= on_temp);
		if (target_fan_on != fan_on) {
			fan_on = target_fan_on;
			current_pattern = fan_on ? cool_pattern : default_pattern;
			if (set_fan_pattern(current_pattern)) {
				fprintf(stderr, "failed to set fan pattern=%u at temp=%dC\n", current_pattern, temp_c);
				continue;
			}
			write_state(current_pattern, temp_c, on_temp, off_temp, default_pattern,
				    cool_pattern, curve_stage, target_temp_c, source, "toggle");
			if (verbose)
				printf("fan event=toggle temp=%dC pattern=%u fan=%d source=%s\n",
				       temp_c, current_pattern, fan_on, source);
		} else {
			write_state(current_pattern, temp_c, on_temp, off_temp, default_pattern,
				    cool_pattern, curve_stage, target_temp_c, source, "hold");
			if (verbose && ++hold_logs >= log_every) {
				hold_logs = 0;
				printf("fan event=hold temp=%dC pattern=%u fan=%d source=%s\n",
				       temp_c, current_pattern, fan_on, source);
			}
		}
	}

	if (restore_on_exit) {
		ret = set_fan_pattern(default_pattern);
		if (ret) {
			fprintf(stderr, "failed restore fan pattern=%u on exit\n", default_pattern);
			return ret;
		}
		write_state(default_pattern, temp_c, on_temp, off_temp, default_pattern,
			    cool_pattern, -1, -1, source, "restore");
		if (verbose)
			printf("fan event=restore temp=%dC pattern=%u\n", temp_c, default_pattern);
	}
	return 0;
}
