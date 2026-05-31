// SPDX-License-Identifier: GPL-2.0
/*
 * ps5_dfc - userspace CLI for ps5_dfpstate_ctl (/dev/ps5dfc)
 *
 *   ps5_dfc get                 read current df_pstate (0..3)
 *   ps5_dfc noop <state>        dry-run: read current, do NOT write
 *   ps5_dfc set  <state> force  REAL write (state 0..3), poll-verified
 *
 * "force" is required for a real write; otherwise it's a no-op.
 * df2 (750/425) and df3 (1200/875) are the safe states. df0/df1 (250/225) are
 * experimental: RequestDfPstate(1) once deadlocked the SMU mailbox (reboot to
 * recover) and only saves ~5 W more than df2 -- short idle test only.
 * The module restores the boot baseline df_pstate on rmmod.
 *
 * Examples:
 *   sudo ps5_dfc get            # -> df_pstate=3 (Linux default)
 *   sudo ps5_dfc set 2 force    # df3 -> df2  (~15% less wall power)
 *   sudo ps5_dfc set 3 force    # back to default
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define PS5DFC_IOC_MAGIC 'F'
#define PS5DFC_FORCE_MAGIC 0x5005
struct ps5dfc_req {
	uint32_t state;
	uint32_t force;
	uint32_t prev;
	uint32_t readback;
};
#define PS5DFC_GET _IOWR(PS5DFC_IOC_MAGIC, 0x01, struct ps5dfc_req)
#define PS5DFC_SET _IOWR(PS5DFC_IOC_MAGIC, 0x02, struct ps5dfc_req)

static void usage(const char *p)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s get                read current df_pstate\n"
		"  %s noop <state>       dry-run, no write (state 0..3)\n"
		"  %s set  <state> force REAL write (state 0..3; df0/df1 experimental)\n",
		p, p, p);
}

int main(int argc, char **argv)
{
	struct ps5dfc_req q;
	int fd, ret;

	if (argc < 2) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}
	memset(&q, 0, sizeof(q));

	fd = open("/dev/ps5dfc", O_RDWR);
	if (fd < 0) {
		perror("open /dev/ps5dfc (is ps5_dfpstate_ctl loaded?)");
		return EXIT_FAILURE;
	}

	if (strcmp(argv[1], "get") == 0) {
		ret = ioctl(fd, PS5DFC_GET, &q);
		if (ret < 0) {
			perror("ioctl GET");
			close(fd);
			return EXIT_FAILURE;
		}
		printf("df_pstate=%u\n", q.state);
	} else if (strcmp(argv[1], "noop") == 0 || strcmp(argv[1], "set") == 0) {
		if (argc < 3) {
			usage(argv[0]);
			close(fd);
			return EXIT_FAILURE;
		}
		q.state = (uint32_t)strtoul(argv[2], NULL, 0);
		if (q.state > 3) {
			fprintf(stderr, "state must be 0..3\n");
			close(fd);
			return EXIT_FAILURE;
		}
		if (strcmp(argv[1], "set") == 0 && argc >= 4 &&
		    strcmp(argv[3], "force") == 0)
			q.force = PS5DFC_FORCE_MAGIC;

		ret = ioctl(fd, PS5DFC_SET, &q);
		if (ret < 0) {
			perror("ioctl SET");
			close(fd);
			return EXIT_FAILURE;
		}
		printf("target=%u current=%u readback=%u %s\n",
		       q.state, q.prev, q.readback,
		       q.force == PS5DFC_FORCE_MAGIC ? "(written)" : "(no-op, not written)");
	} else {
		usage(argv[0]);
		close(fd);
		return EXIT_FAILURE;
	}

	close(fd);
	return EXIT_SUCCESS;
}
