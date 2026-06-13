/* SPDX-License-Identifier: GPL-2.0 */
/*
 * smn.h - userspace SMU MP1 mailbox over PCI config space.
 *
 * Prefer the optional /dev/ps5-smu kernel transport when available. Otherwise
 * fall back to direct userspace PCI config-space SMN access: the SMN index/data
 * pair lives in PCI config space of 0000:00:00.0 (index @ 0xB8, data @ 0xBC).
 * Root required.
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
#include <stdlib.h>
#include <unistd.h>
#include <sys/file.h>
#include "dkms/ps5-smu/ps5_smu.h"

#define SMN_CFG   "/sys/bus/pci/devices/0000:00:00.0/config"
#define MP1_DEV   "/dev/mp1"
#define PS5_SMU_DEV "/dev/ps5-smu"
#define SMN_LOCK  "/run/lock/ps5-power.lock"
#define SMN_BOOST_STATE "/run/ps5-power.boost"
#define SMN_INDEX 0xB8
#define SMN_DATA  0xBC

/* queue 0 = normal mailbox; queue 3 = "test" mailbox (voltage telemetry) */
#define Q0_CMD 0x03B10A08
#define Q0_RSP 0x03B10A68
#define Q0_ARG 0x03B10A48
#define Q3_CMD 0x03B10A20
#define Q3_RSP 0x03B10A80
#define Q3_ARG 0x03B10A88

#ifndef _IO
#define _IOC_NRBITS	8
#define _IOC_TYPEBITS	8
#define _IOC_SIZEBITS	14
#define _IOC_DIRBITS	2
#define _IOC_NRSHIFT	0
#define _IOC_TYPESHIFT	(_IOC_NRSHIFT + _IOC_NRBITS)
#define _IOC_SIZESHIFT	(_IOC_TYPESHIFT + _IOC_TYPEBITS)
#define _IOC_DIRSHIFT	(_IOC_SIZESHIFT + _IOC_SIZEBITS)
#define _IOC_NONE	0U
#define _IOC(dir, type, nr, size) \
	(((dir) << _IOC_DIRSHIFT) | ((type) << _IOC_TYPESHIFT) | \
	 ((nr) << _IOC_NRSHIFT) | ((size) << _IOC_SIZESHIFT))
#define _IO(type, nr)	_IOC(_IOC_NONE, (type), (nr), 0)
#endif

#define MP1_IOC_MAGIC	'M'
#define MP1_BOOST_ENTER	_IO(MP1_IOC_MAGIC, 1)
#define MP1_BOOST_EXIT	_IO(MP1_IOC_MAGIC, 2)

#define SMN_BOOST_CPU	0x01
#define SMN_BOOST_GPU	0x02

#define SMN_POLL_TRIES 200000

static int smn_fd = -1;
static int ps5_smu_fd = -1;
static int smn_lock_fd = -1;

extern int ioctl(int fd, unsigned long request, ...);

static inline int cfg_wr(uint64_t off, uint32_t v) { return pwrite(smn_fd, &v, 4, off) == 4 ? 0 : -1; }
static inline int cfg_rd(uint64_t off, uint32_t *v) { return pread(smn_fd, v, 4, off) == 4 ? 0 : -1; }
static inline int smn_wr(uint32_t reg, uint32_t v) { return cfg_wr(SMN_INDEX, reg) ? -1 : cfg_wr(SMN_DATA, v); }
static inline int smn_rd(uint32_t reg, uint32_t *v) { return cfg_wr(SMN_INDEX, reg) ? -1 : cfg_rd(SMN_DATA, v); }

static int smn_boost_ioctl(int enable)
{
	int fd, ret;

	fd = open(MP1_DEV, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return -1;

	ret = ioctl(fd, enable ? MP1_BOOST_ENTER : MP1_BOOST_EXIT);
	close(fd);
	return ret < 0 ? -1 : 0;
}

static uint32_t smn_boost_read_state(int fd)
{
	char buf[32];
	ssize_t len;

	if (lseek(fd, 0, SEEK_SET) < 0)
		return 0;
	len = read(fd, buf, sizeof(buf) - 1);
	if (len <= 0)
		return 0;
	buf[len] = 0;
	return (uint32_t)strtoul(buf, NULL, 0);
}

static int smn_boost_write_state(int fd, uint32_t state)
{
	if (ftruncate(fd, 0) < 0 || lseek(fd, 0, SEEK_SET) < 0)
		return -1;
	return dprintf(fd, "0x%x\n", state) < 0 ? -1 : 0;
}

static int __attribute__((unused)) smn_boost_vote(uint32_t bit, int enable)
{
	uint32_t old_state, new_state;
	int fd, ret = 0;

	if (smn_lock_fd >= 0)
		flock(smn_lock_fd, LOCK_EX);

	fd = open(SMN_BOOST_STATE, O_CREAT | O_RDWR | O_CLOEXEC, 0644);
	if (fd < 0) {
		ret = -1;
		goto out_unlock;
	}

	old_state = smn_boost_read_state(fd);
	new_state = enable ? (old_state | bit) : (old_state & ~bit);
	if (old_state == new_state)
		goto out_close;

	if (!old_state && new_state)
		ret = smn_boost_ioctl(1);
	else if (old_state && !new_state)
		ret = smn_boost_ioctl(0);

	if (!ret)
		ret = smn_boost_write_state(fd, new_state);

out_close:
	close(fd);
out_unlock:
	if (smn_lock_fd >= 0)
		flock(smn_lock_fd, LOCK_UN);
	return ret;
}

static int smn_open(void)
{
	ps5_smu_fd = open(PS5_SMU_DEV, O_RDWR | O_CLOEXEC);
	if (ps5_smu_fd >= 0)
		return 0;

	smn_fd = open(SMN_CFG, O_RDWR);
	if (smn_fd < 0) {
		perror("open " PS5_SMU_DEV " or " SMN_CFG " (need root)");
		return -1;
	}
	/* best-effort lock; if it can't be created we still run (unlocked, PoC) */
	smn_lock_fd = open(SMN_LOCK, O_CREAT | O_RDWR, 0644);
	return 0;
}

static int smn_mbox_kernel(uint8_t msg, uint32_t arg, uint32_t *status, uint32_t *val)
{
	if (ps5_smu_fd < 0)
		return -3;

	switch (msg) {
	case 0x0b:
	{
		struct ps5_smu_cpu_pstate req = {
			.mask = arg & 0xff,
			.pstate = (arg >> 16) & 0x0f,
		};
		if (ioctl(ps5_smu_fd, PS5_SMU_CPU_PSTATE_SET, &req) < 0)
			return -1;
		if (status)
			*status = 1;
		return 0;
	}
	case 0x0c:
	{
		struct ps5_smu_cpu_pstate req = {
			.core = arg & 0xff,
		};
		if (ioctl(ps5_smu_fd, PS5_SMU_CPU_PSTATE_GET, &req) < 0)
			return -1;
		if (status)
			*status = 1;
		if (val)
			*val = req.pstate;
		return 0;
	}
	case 0x0e:
	{
		struct ps5_smu_gfxclk req = { .mhz = arg };
		if (ioctl(ps5_smu_fd, PS5_SMU_GFXCLK_SET, &req) < 0)
			return -1;
		if (status)
			*status = 1;
		return 0;
	}
	case 0x0f:
	{
		struct ps5_smu_gfxclk req = {};
		if (ioctl(ps5_smu_fd, PS5_SMU_GFXCLK_GET, &req) < 0)
			return -1;
		if (status)
			*status = 1;
		if (val)
			*val = req.mhz;
		return 0;
	}
	case 0x36:
	case 0x37:
	{
		struct ps5_smu_voltage req = {
			.rail = msg == 0x36 ? PS5_SMU_VOLTAGE_CPU : PS5_SMU_VOLTAGE_GPU,
		};
		if (ioctl(ps5_smu_fd, PS5_SMU_VOLTAGE_GET, &req) < 0)
			return -1;
		if (status)
			*status = 1;
		if (val)
			*val = req.millivolts;
		return 0;
	}
	default:
		return -3;
	}
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

	ret = smn_mbox_kernel(msg, arg, status, val);
	if (ret != -3)
		return ret;

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
