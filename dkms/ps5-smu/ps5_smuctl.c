// SPDX-License-Identifier: GPL-2.0
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "ps5_smu.h"

#define DEV "/dev/ps5-smu"

static void usage(const char *p)
{
	fprintf(stderr,
		"usage: %s cpu-get <core>|cpu-set <mask> <pstate>|gpu-get|gpu-set <mhz>|voltage\n",
		p);
}

static unsigned long parse_u32(const char *s, const char *name)
{
	char *end;
	unsigned long v = strtoul(s, &end, 0);

	if (*end) {
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
	int fd, ret = 0;

	if (argc < 2) {
		usage(argv[0]);
		return 2;
	}

	fd = open_dev();
	if (!strcmp(argv[1], "cpu-get")) {
		struct ps5_smu_cpu_pstate req = {};

		if (argc != 3) {
			usage(argv[0]);
			return 2;
		}
		req.core = parse_u32(argv[2], "core");
		ret = ioctl(fd, PS5_SMU_CPU_PSTATE_GET, &req);
		if (ret)
			perror("PS5_SMU_CPU_PSTATE_GET");
		else
			printf("core %u: P%u\n", req.core, req.pstate);
	} else if (!strcmp(argv[1], "cpu-set")) {
		struct ps5_smu_cpu_pstate req = {};

		if (argc != 4) {
			usage(argv[0]);
			return 2;
		}
		req.mask = parse_u32(argv[2], "mask");
		req.pstate = parse_u32(argv[3], "pstate");
		ret = ioctl(fd, PS5_SMU_CPU_PSTATE_SET, &req);
		if (ret)
			perror("PS5_SMU_CPU_PSTATE_SET");
	} else if (!strcmp(argv[1], "gpu-get")) {
		struct ps5_smu_gfxclk req = {};

		ret = ioctl(fd, PS5_SMU_GFXCLK_GET, &req);
		if (ret)
			perror("PS5_SMU_GFXCLK_GET");
		else
			printf("gfxclk=%u MHz\n", req.mhz);
	} else if (!strcmp(argv[1], "gpu-set")) {
		struct ps5_smu_gfxclk req = {};

		if (argc != 3) {
			usage(argv[0]);
			return 2;
		}
		req.mhz = parse_u32(argv[2], "mhz");
		ret = ioctl(fd, PS5_SMU_GFXCLK_SET, &req);
		if (ret)
			perror("PS5_SMU_GFXCLK_SET");
	} else if (!strcmp(argv[1], "voltage")) {
		struct ps5_smu_voltage cpu = { .rail = PS5_SMU_VOLTAGE_CPU };
		struct ps5_smu_voltage gpu = { .rail = PS5_SMU_VOLTAGE_GPU };

		ret = ioctl(fd, PS5_SMU_VOLTAGE_GET, &cpu);
		if (ret) {
			perror("PS5_SMU_VOLTAGE_GET cpu");
		} else {
			ret = ioctl(fd, PS5_SMU_VOLTAGE_GET, &gpu);
			if (ret)
				perror("PS5_SMU_VOLTAGE_GET gpu");
			else
				printf("cpu rail %u mV, gpu rail %u mV\n", cpu.millivolts, gpu.millivolts);
		}
	} else {
		usage(argv[0]);
		return 2;
	}

	close(fd);
	return ret ? 1 : 0;
}
