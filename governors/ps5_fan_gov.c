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
#define FAN_PATTERN_DEFAULT 0
#define FAN_PATTERN_COOL 1

static volatile sig_atomic_t stop;
static int verbose;
static int restore_on_exit = 1;

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
	fprintf(f, "sensor=%s\n", sensor ? sensor : "unknown");
	fprintf(f, "reason=%s\n", reason ? reason : "unknown");
	fclose(f);
	return rename(tmp, FAN_STATE);
}

static int set_fan_pattern_locked(int fd, unsigned char pattern)
{
	int ret = ioctl(fd, ICC_FAN_CHANGE_SERVO_PATTERN, &pattern);
	if (ret < 0)
		perror("ICC_FAN_CHANGE_SERVO_PATTERN");
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

	fd = open(ICC_DEV, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror("open " ICC_DEV);
		goto out;
	}
	ret = set_fan_pattern_locked(fd, pattern);
	close(fd);
out:
	flock(lock_fd, LOCK_UN);
	close(lock_fd);
	return ret;
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
	fprintf(stderr, "usage: %s [-i ms] [-H on_c] [-L off_c] [-s auto|k10temp|/path/temp_input] [-v] [-n] [-f pattern] [-a default_pattern] [-C cool_pattern]\n", p);
}

int main(int argc, char **argv)
{
	unsigned interval_ms = 1000;
	int on_temp = 55, off_temp = 45, temp_c, fan_on = 0, target_fan_on;
	int force_set = -1, read_failures = 0;
	unsigned long pattern_arg;
	unsigned char default_pattern = FAN_PATTERN_DEFAULT;
	unsigned char cool_pattern = FAN_PATTERN_COOL;
	unsigned char current_pattern;
	const char *sensor = "auto";
	char source[384] = "unknown";
	char *endptr;
	int opt;
	int ret;

	while ((opt = getopt(argc, argv, "i:H:L:s:vnf:a:C:")) != -1) {
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
			    cool_pattern, "manual", "force");
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

	target_fan_on = fan_on = temp_c >= on_temp;
	current_pattern = fan_on ? cool_pattern : default_pattern;
	if (set_fan_pattern(current_pattern))
		return 1;
	write_state(current_pattern, temp_c, on_temp, off_temp, default_pattern,
		    cool_pattern, source, "init");
	if (verbose)
		printf("fan event=init temp=%dC pattern=%u on=%d on_temp=%d off_temp=%d sensor=%s source=%s\n",
		       temp_c, current_pattern, fan_on, on_temp, off_temp, sensor, source);

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
					    cool_pattern, sensor, "sensor_fail_safe");
				if (verbose)
					printf("fan event=fail_safe pattern=%u sensor=%s\n", current_pattern, sensor);
			}
			continue;
		}
		read_failures = 0;

		target_fan_on = fan_on ? (temp_c > off_temp) : (temp_c >= on_temp);
		if (target_fan_on != fan_on) {
			fan_on = target_fan_on;
			current_pattern = fan_on ? cool_pattern : default_pattern;
			if (set_fan_pattern(current_pattern)) {
				fprintf(stderr, "failed to set fan pattern=%u at temp=%dC\n", current_pattern, temp_c);
				continue;
			}
			write_state(current_pattern, temp_c, on_temp, off_temp, default_pattern,
				    cool_pattern, source, "toggle");
			if (verbose)
				printf("fan event=toggle temp=%dC pattern=%u fan=%d source=%s\n",
				       temp_c, current_pattern, fan_on, source);
		} else {
			write_state(current_pattern, temp_c, on_temp, off_temp, default_pattern,
				    cool_pattern, source, "hold");
			if (verbose)
				printf("fan event=hold temp=%dC pattern=%u fan=%d source=%s\n",
				       temp_c, current_pattern, fan_on, source);
		}
	}

	if (restore_on_exit) {
		ret = set_fan_pattern(default_pattern);
		if (ret) {
			fprintf(stderr, "failed restore fan pattern=%u on exit\n", default_pattern);
			return ret;
		}
		write_state(default_pattern, temp_c, on_temp, off_temp, default_pattern,
			    cool_pattern, source, "restore");
		if (verbose)
			printf("fan event=restore temp=%dC pattern=%u\n", temp_c, default_pattern);
	}
	return 0;
}
