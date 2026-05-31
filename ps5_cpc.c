// SPDX-License-Identifier: GPL-2.0
/*
 * ps5_cpc - userspace CLI for ps5_corepstate_ctl (/dev/ps5cpc)
 *
 *   ps5_cpc get <core>            read a core's current p-state   (core id 0..7)
 *   ps5_cpc set <mask> <pstate>   set p-state for a core BITMASK  (pstate 0..7)
 *
 * The mask selects cores by bit:  core0=0x01 core1=0x02 ... all 8 cores = 0xff.
 * pstate 0 = full/default clock (used to restore). The kernel module also
 * auto-restores every touched core to P0 on rmmod.
 *
 * Examples:
 *   sudo ps5_cpc set 0xff 7   # all cores -> P7 (800 MHz)
 *   sudo ps5_cpc set 0xff 0   # all cores -> P0 (full clock)
 *   sudo ps5_cpc get 0        # read core 0
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define PS5CPC_IOC_MAGIC 'Q'
struct ps5cpc_req {
	uint32_t core_sel;
	uint32_t pstate;
	uint32_t readback;
	uint32_t flags;
};
#define PS5CPC_GET _IOWR(PS5CPC_IOC_MAGIC, 0x01, struct ps5cpc_req)
#define PS5CPC_SET _IOWR(PS5CPC_IOC_MAGIC, 0x02, struct ps5cpc_req)

static int parse_u32(const char *s, uint32_t max, uint32_t *out)
{
	char *end = NULL;
	unsigned long v;

	errno = 0;
	v = strtoul(s, &end, 0);
	if (errno || end == s || *end != '\0' || v > max)
		return -1;
	*out = (uint32_t)v;
	return 0;
}

static void usage(const char *p)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s get <core>            core id 0..7\n"
		"  %s set <mask> <pstate>   mask 0x01..0xff, pstate 0..7\n"
		"                           (core0=0x01 core1=0x02 ... all=0xff)\n",
		p, p);
}

int main(int argc, char **argv)
{
	struct ps5cpc_req q;
	int fd, ret;

	if (argc < 3) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}
	memset(&q, 0, sizeof(q));

	/* GET takes a core id (max 7); SET takes a bitmask (max 0xff). */
	if (parse_u32(argv[2], strcmp(argv[1], "set") == 0 ? 0xff : 7, &q.core_sel)) {
		fprintf(stderr, "invalid core/mask: %s\n", argv[2]);
		return EXIT_FAILURE;
	}

	fd = open("/dev/ps5cpc", O_RDWR);
	if (fd < 0) {
		perror("open /dev/ps5cpc (is ps5_corepstate_ctl loaded?)");
		return EXIT_FAILURE;
	}

	if (strcmp(argv[1], "get") == 0) {
		ret = ioctl(fd, PS5CPC_GET, &q);
		if (ret < 0) {
			perror("ioctl GET");
			close(fd);
			return EXIT_FAILURE;
		}
		printf("core %u: P%u\n", q.core_sel, q.pstate);
	} else if (strcmp(argv[1], "set") == 0) {
		if (argc < 4 || parse_u32(argv[3], 7, &q.pstate)) {
			fprintf(stderr, "invalid pstate (0..7; 0 = full clock)\n");
			close(fd);
			return EXIT_FAILURE;
		}
		ret = ioctl(fd, PS5CPC_SET, &q);
		if (ret < 0) {
			perror("ioctl SET");
			close(fd);
			return EXIT_FAILURE;
		}
		printf("mask 0x%02x -> P%u (readback P%u)\n",
		       q.core_sel, q.pstate, q.readback);
	} else {
		usage(argv[0]);
		close(fd);
		return EXIT_FAILURE;
	}

	close(fd);
	return EXIT_SUCCESS;
}
