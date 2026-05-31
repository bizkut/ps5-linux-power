// SPDX-License-Identifier: GPL-2.0
/*
 * pstate_msr - read-only dump of AMD family-17h CPU P-state MSRs
 *
 * Independent verification of what ps5_corepstate_ctl actually did: reads the
 * active p-state per physical core and the full PStateDef table, decoded to
 * MHz and Volt. This is how we found that all PS5 CPU p-states share the same
 * VID (1.05 V) -- i.e. only frequency changes, not voltage.
 *
 * Read-only: opens /dev/cpu/N/msr (needs root + the `msr` module loaded).
 *
 *   pstate_msr          active p-state per core
 *   pstate_msr defs     also dump the full P0..P7 definition table
 *
 * MSRs (family 17h):
 *   0xC0010061 PStateCurLim   [2:0]=current, [6:4]=max
 *   0xC0010063 PStateStat     [2:0]=currently active p-state index
 *   0xC0010064..6B PStateDef0..7   CpuFid[7:0] CpuDfsId[13:8] CpuVid[21:14] En[63]
 * Decode:  MHz = CpuFid/CpuDfsId * 200 ;  V = 1.55 - CpuVid*0.00625
 */

#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#define MSR_PSTATE_CUR_LIMIT 0xC0010061
#define MSR_PSTATE_STATUS    0xC0010063
#define MSR_PSTATE_DEF0      0xC0010064

static int rd(int cpu, uint32_t reg, uint64_t *v)
{
	char path[64];
	int fd;
	ssize_t n;

	snprintf(path, sizeof(path), "/dev/cpu/%d/msr", cpu);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	n = pread(fd, v, 8, reg);
	close(fd);
	return n == 8 ? 0 : -1;
}

static uint32_t pdef_fid(uint64_t d) { return d & 0xff; }
static uint32_t pdef_did(uint64_t d) { return (d >> 8) & 0x3f; }
static uint32_t pdef_vid(uint64_t d) { return (d >> 14) & 0xff; }

static double pdef_mhz(uint64_t d)
{
	uint32_t did = pdef_did(d);

	return did ? (double)pdef_fid(d) / did * 200.0 : 0.0;
}

static double pdef_volt(uint64_t d)
{
	return 1.55 - pdef_vid(d) * 0.00625;
}

int main(int argc, char **argv)
{
	/* one logical CPU per physical core on this 8c/16t part: 0,2,4,...,14 */
	const int cores[] = {0, 2, 4, 6, 8, 10, 12, 14};
	int show_defs = (argc > 1);
	uint64_t v;
	int i, p;

	(void)argv;
	printf("Active p-state per physical core:\n");
	for (i = 0; i < 8; i++) {
		uint32_t cur;
		uint64_t def;

		if (rd(cores[i], MSR_PSTATE_STATUS, &v)) {
			printf("core%d (cpu%d): MSR read failed (root? msr module?)\n",
			       i, cores[i]);
			continue;
		}
		cur = v & 0x7;
		if (rd(cores[i], MSR_PSTATE_DEF0 + cur, &def)) {
			printf("core%d: P%u (def read failed)\n", i, cur);
			continue;
		}
		printf("  core%d: P%u -> %7.1f MHz @ %.4f V (VID=0x%02x)\n",
		       i, cur, pdef_mhz(def), pdef_volt(def), pdef_vid(def));
	}

	if (show_defs) {
		printf("\nP-state definition table (cpu0):\n");
		for (p = 0; p < 8; p++) {
			if (rd(0, MSR_PSTATE_DEF0 + p, &v))
				break;
			printf("  P%d: en=%llu %7.1f MHz @ %.4f V "
			       "(fid=0x%02x did=0x%02x vid=0x%02x)\n",
			       p, (unsigned long long)(v >> 63),
			       pdef_mhz(v), pdef_volt(v),
			       pdef_fid(v), pdef_did(v), pdef_vid(v));
		}
		if (!rd(0, MSR_PSTATE_CUR_LIMIT, &v))
			printf("  CurLimit: cur=%llu max=%llu\n",
			       (unsigned long long)(v & 7),
			       (unsigned long long)((v >> 4) & 7));
	}
	return 0;
}
