/* SPDX-License-Identifier: GPL-2.0 */
/*
 * smn.h - userspace SMU MP1 mailbox over PCI config space.
 *
 * Talks to the same mailbox the kernel reaches via amd_smn_read/write, but from
 * userspace: the SMN index/data pair lives in PCI config space of 0000:00:00.0
 * (index @ 0xB8, data @ 0xBC). This is the cyan-skillfish-governor transport,
 * verified working on PS5. Root required (writes /sys/.../config).
 *
 * Serialization: all ps5-power userspace tools take an flock on SMN_LOCK so they
 * don't interleave the (non-atomic) index->data sequence against EACH OTHER.
 * CAVEAT (PoC): this does NOT coordinate with in-kernel SMN users (k10temp,
 * EDAC, ...) that share the same 0xB8/0xBC pair -- tiny race window remains.
 * That is the price of dropping the kernel module; acceptable for a PoC.
 */
#ifndef PS5_SMN_H
#define PS5_SMN_H
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>

#define SMN_CFG   "/sys/bus/pci/devices/0000:00:00.0/config"
#define SMN_LOCK  "/run/lock/ps5-power.lock"
#define SMN_INDEX 0xB8
#define SMN_DATA  0xBC

/* queue 0 = normal mailbox; queue 3 = "test" mailbox (voltage telemetry) */
#define Q0_CMD 0x03B10A08
#define Q0_RSP 0x03B10A68
#define Q0_ARG 0x03B10A48
#define Q3_CMD 0x03B10A20
#define Q3_RSP 0x03B10A80
#define Q3_ARG 0x03B10A88

#define SMN_POLL_TRIES 200000

static int smn_fd = -1;
static int smn_lock_fd = -1;

static inline int cfg_wr(uint64_t off, uint32_t v) { return pwrite(smn_fd, &v, 4, off) == 4 ? 0 : -1; }
static inline int cfg_rd(uint64_t off, uint32_t *v) { return pread(smn_fd, v, 4, off) == 4 ? 0 : -1; }
static inline int smn_wr(uint32_t reg, uint32_t v) { return cfg_wr(SMN_INDEX, reg) ? -1 : cfg_wr(SMN_DATA, v); }
static inline int smn_rd(uint32_t reg, uint32_t *v) { return cfg_wr(SMN_INDEX, reg) ? -1 : cfg_rd(SMN_DATA, v); }

static int smn_open(void)
{
	smn_fd = open(SMN_CFG, O_RDWR);
	if (smn_fd < 0) {
		perror("open " SMN_CFG " (need root)");
		return -1;
	}
	/* best-effort lock; if it can't be created we still run (unlocked, PoC) */
	smn_lock_fd = open(SMN_LOCK, O_CREAT | O_RDWR, 0644);
	return 0;
}

static int smn_wait_nz(uint32_t reg, uint32_t *out)
{
	uint32_t v = 0;
	int i;

	for (i = 0; i < SMN_POLL_TRIES; i++) {
		if (smn_rd(reg, &v))
			return -1;
		if (v) {
			if (out)
				*out = v;
			return 0;
		}
	}
	return -2;
}

/*
 * One mailbox transaction on the given queue (cmd/rsp/arg register triple).
 * Returns 0 if the transport succeeded; the SMU status byte is in *status
 * (0x01 = OK) and the arg readback in *val. Serialized via flock so two
 * ps5-power tools never interleave the index/data writes.
 */
static int smn_mbox(uint32_t cmd, uint32_t rsp, uint32_t areg,
		    uint8_t msg, uint32_t arg, uint32_t *status, uint32_t *val)
{
	uint32_t s = 0, v = 0;
	int ret = 0;

	if (smn_lock_fd >= 0)
		flock(smn_lock_fd, LOCK_EX);

	/* mailbox must not be busy (rsp nonzero = previous response present) */
	if (smn_wait_nz(rsp, NULL)) { ret = -1; goto out; }
	if (smn_wr(rsp, 0) || smn_wr(areg, arg) || smn_wr(areg + 4, 0) || smn_wr(cmd, msg)) {
		ret = -1; goto out;
	}
	if (smn_wait_nz(rsp, &s)) { ret = -2; goto out; }   /* timeout */
	if (smn_rd(areg, &v)) { ret = -1; goto out; }

	if (status) *status = s;
	if (val)    *val = v;
out:
	if (smn_lock_fd >= 0)
		flock(smn_lock_fd, LOCK_UN);
	return ret;
}

#endif /* PS5_SMN_H */
