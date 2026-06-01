// SPDX-License-Identifier: GPL-2.0
/*
 * ps5_df - PS5 DF (Data Fabric / memory) P-state set/get.
 *
 *   ps5_df get                 read current df_pstate
 *   ps5_df set <state> force   set df_pstate 0..3 ('force' required), poll-verify
 *
 * Messages (queue 0): 0x12 RequestDfPstate arg=state, 0x13 QueryDfPstate arg=0.
 * df_pstate -> FCLK/UCLK (measured): 3=1200/875 (boot default), 2=750/425,
 * 1=250/225, 0=250/225. df3->df2 saves ~19 W at the wall.
 *
 * !!! WARNING: RequestDfPstate has DEADLOCKED the SMU mailbox in testing
 * (df1, and once even df3->df2). Recovery needed a reboot. Use df2/df3;
 * treat df0/df1 as experimental: short test only, be ready to reboot. !!!
 *
 * One-shot CLI: no auto-restore. To go back to default: ps5_df set 3 force.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "smn.h"

#define MSG_REQUEST_DF 0x12
#define MSG_QUERY_DF   0x13
#define DF_MAX 3
#define VERIFY_TRIES 2000
#define VERIFY_UDELAY 1500

static int df_query(uint32_t *cur)
{
	uint32_t st = 0;
	if (smn_mbox(Q0_CMD, Q0_RSP, Q0_ARG, MSG_QUERY_DF, 0, &st, cur))
		return -1;
	if (st != 1) { fprintf(stderr, "QueryDfPstate status=0x%02x\n", st); return -1; }
	return 0;
}

static int df_set_verified(uint32_t target)
{
	uint32_t st = 0, cur = 0xffffffff;
	int i;

	if (smn_mbox(Q0_CMD, Q0_RSP, Q0_ARG, MSG_REQUEST_DF, target, &st, NULL))
		return -1;
	if (st != 1) { fprintf(stderr, "RequestDfPstate status=0x%02x\n", st); return -1; }
	for (i = 0; i < VERIFY_TRIES; i++) {
		if (df_query(&cur)) return -1;
		if (cur == target) return 0;
		usleep(VERIFY_UDELAY);
	}
	fprintf(stderr, "verify timeout: target %u, last %u\n", target, cur);
	return -1;
}

static void usage(const char *p)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s get\n"
		"  %s set <state> force   (state 0..3; 3=default, 2=-19W; 0/1 EXPERIMENTAL)\n",
		p, p);
}

int main(int argc, char **argv)
{
	uint32_t cur = 0, target;

	if (argc < 2) { usage(argv[0]); return 2; }
	if (smn_open()) return 1;

	if (!strcmp(argv[1], "get")) {
		if (df_query(&cur)) return 1;
		printf("df_pstate=%u\n", cur);
	} else if (!strcmp(argv[1], "set")) {
		if (argc < 3) { usage(argv[0]); return 2; }
		target = (uint32_t)strtoul(argv[2], NULL, 0);
		if (target > DF_MAX) { fprintf(stderr, "state 0..3\n"); return 2; }
		if (df_query(&cur)) return 1;
		if (argc < 4 || strcmp(argv[3], "force")) {
			printf("dry-run: df%u -> df%u (NOT written; add 'force')\n", cur, target);
			return 0;
		}
		if (target < 2)
			fprintf(stderr, "WARNING: df%u is experimental (deadlock risk, may need reboot)\n", target);
		if (df_set_verified(target)) { fprintf(stderr, "set failed\n"); return 1; }
		printf("df%u -> df%u (written)\n", cur, target);
	} else { usage(argv[0]); return 2; }

	return 0;
}
