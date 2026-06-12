// SPDX-License-Identifier: GPL-2.0
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "ps5_icc_fan.h"

#define DEV "/dev/ps5-fan"

static void usage(const char *p)
{
	fprintf(stderr,
		"usage: %s pattern <0..255>|mode-get <zone>|mode-set <zone> <1..6>|temp <zone>|servo <zone>|target-temp <zone> <celsius>\n",
		p);
}

static unsigned long parse_u8(const char *s, const char *name)
{
	char *end;
	unsigned long v = strtoul(s, &end, 0);

	if (*end || v > 255) {
		fprintf(stderr, "invalid %s: %s\n", name, s);
		exit(2);
	}
	return v;
}

static int open_dev(void)
{
	int fd = open(DEV, O_RDWR | O_CLOEXEC);

	if (fd < 0) {
		perror("open " DEV);
		exit(1);
	}
	return fd;
}

int main(int argc, char **argv)
{
	int fd, ret;

	if (argc < 2) {
		usage(argv[0]);
		return 2;
	}

	fd = open_dev();
	if (!strcmp(argv[1], "pattern")) {
		struct ps5_fan_pattern pattern;

		if (argc != 3) {
			usage(argv[0]);
			return 2;
		}
		pattern.pattern = parse_u8(argv[2], "pattern");
		ret = ioctl(fd, PS5_FAN_CHANGE_SERVO_PATTERN, &pattern);
		if (ret)
			perror("PS5_FAN_CHANGE_SERVO_PATTERN");
	} else if (!strcmp(argv[1], "mode-get")) {
		struct ps5_fan_mode mode = {};

		if (argc != 3) {
			usage(argv[0]);
			return 2;
		}
		mode.zone = parse_u8(argv[2], "zone");
		ret = ioctl(fd, PS5_FAN_MODE_GET, &mode);
		if (ret)
			perror("PS5_FAN_MODE_GET");
		else
			printf("zone=%u mode=%u\n", mode.zone, mode.mode);
	} else if (!strcmp(argv[1], "mode-set")) {
		struct ps5_fan_mode mode = {};

		if (argc != 4) {
			usage(argv[0]);
			return 2;
		}
		mode.zone = parse_u8(argv[2], "zone");
		mode.mode = parse_u8(argv[3], "mode");
		ret = ioctl(fd, PS5_FAN_MODE_SET, &mode);
		if (ret)
			perror("PS5_FAN_MODE_SET");
	} else if (!strcmp(argv[1], "temp")) {
		struct ps5_fan_temp temp = {};

		if (argc != 3) {
			usage(argv[0]);
			return 2;
		}
		temp.zone = parse_u8(argv[2], "zone");
		ret = ioctl(fd, PS5_FAN_ZONE_TEMP_GET, &temp);
		if (ret)
			perror("PS5_FAN_ZONE_TEMP_GET");
		else
			printf("zone=%u temp=%u.%02uC\n", temp.zone, temp.temperature_c,
			       (unsigned int)temp.fraction * 100 / 256);
	} else if (!strcmp(argv[1], "servo")) {
		struct ps5_fan_servo servo = {};
		int i;

		if (argc != 3) {
			usage(argv[0]);
			return 2;
		}
		servo.zone = parse_u8(argv[2], "zone");
		ret = ioctl(fd, PS5_FAN_SERVO_GET, &servo);
		if (ret) {
			perror("PS5_FAN_SERVO_GET");
		} else {
			printf("zone=%u\n", servo.zone);
			for (i = 0; i < 6; i++)
				printf("setting%d=0x%08x\n", i, servo.setting[i]);
		}
	} else if (!strcmp(argv[1], "target-temp")) {
		struct ps5_fan_target_temp target = {};

		if (argc != 4) {
			usage(argv[0]);
			return 2;
		}
		target.zone = parse_u8(argv[2], "zone");
		target.temperature_c = parse_u8(argv[3], "temperature");
		ret = ioctl(fd, PS5_FAN_TARGET_TEMP_SET, &target);
		if (ret)
			perror("PS5_FAN_TARGET_TEMP_SET");
	} else {
		usage(argv[0]);
		return 2;
	}

	close(fd);
	return ret ? 1 : 0;
}
