// SPDX-License-Identifier: GPL-2.0
/*
 * ps5_gpu - PS5 GPU GFXCLK set/get.
 *
 *   ps5_gpu get                 read current GFXCLK
 *   ps5_gpu set <mhz> force     set GFXCLK 400..2380 ('force' required), poll-verify
 *   ps5_gpu reset               back to 2000 MHz (default)
 *
 * Messages (queue 0, component-0 path): 0x0e RequestGfxclk arg=MHz,
 * 0x0f QueryGfxclk arg=0. GFXCLK is the dominant power lever (2000->1500 ~ -30 W).
 *
 * One-shot CLI: no auto-restore. To go back to default: ps5_gpu reset.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "smn.h"

#define MSG_REQUEST_GFXCLK 0x0e
#define MSG_QUERY_GFXCLK   0x0f
#define GFX_MIN 400U
#define GFX_MAX 2380U
#define GFX_DEFAULT 2000U
#define VERIFY_TRIES 2000
#define VERIFY_UDELAY 1500

static int gfx_query(uint32_t *mhz)
{
	uint32_t st = 0;
	if (smn_mbox(Q0_CMD, Q0_RSP, Q0_ARG, MSG_QUERY_GFXCLK, 0, &st, mhz))
		return -1;
	if (st != 1) { fprintf(stderr, "QueryGfxclk status=0x%02x\n", st); return -1; }
	return 0;
}

static int gfx_set_verified(uint32_t mhz)
{
	uint32_t st = 0, cur = 0;
	int i;

	if (smn_mbox(Q0_CMD, Q0_RSP, Q0_ARG, MSG_REQUEST_GFXCLK, mhz, &st, NULL))
		return -1;
	if (st != 1) { fprintf(stderr, "RequestGfxclk status=0x%02x\n", st); return -1; }
	for (i = 0; i < VERIFY_TRIES; i++) {
		if (gfx_query(&cur)) return -1;
		if (cur == mhz) return 0;
		usleep(VERIFY_UDELAY);
	}
	fprintf(stderr, "verify timeout: target %u, last %u\n", mhz, cur);
	return -1;
}

static void usage(const char *p)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s get\n"
		"  %s set <mhz> force   (%u..%u)\n"
		"  %s reset             (-> %u MHz)\n",
		p, p, GFX_MIN, GFX_MAX, p, GFX_DEFAULT);
}

int main(int argc, char **argv)
{
	uint32_t cur = 0, target;

	if (argc < 2) { usage(argv[0]); return 2; }
	if (smn_open()) return 1;

	if (!strcmp(argv[1], "get")) {
		if (gfx_query(&cur)) return 1;
		printf("gfxclk=%u MHz\n", cur);
	} else if (!strcmp(argv[1], "reset")) {
		if (gfx_query(&cur)) return 1;
		if (gfx_set_verified(GFX_DEFAULT)) { fprintf(stderr, "reset failed\n"); return 1; }
		printf("gfxclk %u -> %u MHz (reset)\n", cur, GFX_DEFAULT);
	} else if (!strcmp(argv[1], "set")) {
		if (argc < 3) { usage(argv[0]); return 2; }
		target = (uint32_t)strtoul(argv[2], NULL, 0);
		if (target < GFX_MIN || target > GFX_MAX) {
			fprintf(stderr, "mhz must be %u..%u\n", GFX_MIN, GFX_MAX); return 2;
		}
		if (gfx_query(&cur)) return 1;
		if (argc < 4 || strcmp(argv[3], "force")) {
			printf("dry-run: %u -> %u MHz (NOT written; add 'force')\n", cur, target);
			return 0;
		}
		if (gfx_set_verified(target)) { fprintf(stderr, "set failed\n"); return 1; }
		printf("gfxclk %u -> %u MHz (written)\n", cur, target);
	} else { usage(argv[0]); return 2; }

	return 0;
}
