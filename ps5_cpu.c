// SPDX-License-Identifier: GPL-2.0
/*
 * ps5_cpu - PS5 CPU core P-state set/get + rail-voltage read.
 *
 *   ps5_cpu get [core]            read pstate of a core (id 0..7, default 0) + rail mV
 *   ps5_cpu set <mask> <pstate>   set pstate 0..7 for a core BITMASK (0x01..0xff)
 *
 * Messages (queue 0): 0x0b RequestCorePstate arg=(mask&0xff)|((pstate&0xf)<<16),
 * 0x0c QueryCorePstate arg=core_id. Voltage (queue 3, read-only): 0x36 cpu, 0x37 gpu.
 * The SMU sets CPU VID automatically per pstate; voltage is read-only here.
 *
 * NOTE: unlike the kernel module there is no auto-restore on exit -- this is a
 * one-shot CLI. To go back to full clock: ps5_cpu set 0xff 0.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "smn.h"

#define MSG_REQUEST_CORE_PSTATE 0x0b
#define MSG_QUERY_CORE_PSTATE   0x0c
#define MSG_GET_CPU_VOLTAGE     0x36
#define MSG_GET_GPU_VOLTAGE     0x37

static int core_query(uint32_t id, uint32_t *pstate)
{
	uint32_t st = 0;
	if (smn_mbox(Q0_CMD, Q0_RSP, Q0_ARG, MSG_QUERY_CORE_PSTATE, id & 0xff, &st, pstate))
		return -1;
	if (st != 1) { fprintf(stderr, "QueryCorePstate status=0x%02x\n", st); return -1; }
	return 0;
}

static int core_request(uint32_t mask, uint32_t pstate)
{
	uint32_t arg = (mask & 0xff) | ((pstate & 0x0f) << 16), st = 0;
	if (smn_mbox(Q0_CMD, Q0_RSP, Q0_ARG, MSG_REQUEST_CORE_PSTATE, arg, &st, NULL))
		return -1;
	if (st != 1) { fprintf(stderr, "RequestCorePstate status=0x%02x\n", st); return -1; }
	return 0;
}

static void read_voltages(uint32_t *cpu_mv, uint32_t *gpu_mv)
{
	uint32_t st = 0;
	if (smn_mbox(Q3_CMD, Q3_RSP, Q3_ARG, MSG_GET_CPU_VOLTAGE, 0, &st, cpu_mv) || st != 1)
		*cpu_mv = 0;
	if (smn_mbox(Q3_CMD, Q3_RSP, Q3_ARG, MSG_GET_GPU_VOLTAGE, 0, &st, gpu_mv) || st != 1)
		*gpu_mv = 0;
}

static void usage(const char *p)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s get [core]            (core id 0..7, default 0)\n"
		"  %s set <mask> <pstate>   (mask 0x01..0xff, pstate 0..7)\n"
		"    pstate: 0=3200 1=2560 2=2327 3=1969 4=1829 5=1600 6=1280 7=800 MHz\n",
		p, p);
}

int main(int argc, char **argv)
{
	uint32_t pstate = 0, cpu_mv = 0, gpu_mv = 0;

	if (argc < 2) { usage(argv[0]); return 2; }
	if (smn_open()) return 1;

	if (!strcmp(argv[1], "get")) {
		uint32_t core = (argc >= 3) ? (uint32_t)strtoul(argv[2], NULL, 0) : 0;
		if (core > 7) { fprintf(stderr, "core id 0..7\n"); return 2; }
		if (core_query(core, &pstate)) return 1;
		read_voltages(&cpu_mv, &gpu_mv);
		printf("core %u: P%u | cpu rail %u mV, gpu rail %u mV\n", core, pstate, cpu_mv, gpu_mv);
	} else if (!strcmp(argv[1], "set")) {
		uint32_t mask, ps;
		if (argc < 4) { usage(argv[0]); return 2; }
		mask = (uint32_t)strtoul(argv[2], NULL, 0);
		ps   = (uint32_t)strtoul(argv[3], NULL, 0);
		if (mask == 0 || mask > 0xff || ps > 7) {
			fprintf(stderr, "mask 0x01..0xff, pstate 0..7\n"); return 2;
		}
		if (core_request(mask, ps)) return 1;
		core_query(__builtin_ctz(mask), &pstate);
		read_voltages(&cpu_mv, &gpu_mv);
		printf("mask 0x%02x -> P%u | cpu rail %u mV, gpu rail %u mV\n", mask, pstate, cpu_mv, gpu_mv);
	} else { usage(argv[0]); return 2; }

	return 0;
}
