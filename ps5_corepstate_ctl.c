// SPDX-License-Identifier: GPL-2.0
/*
 * ps5_corepstate_ctl - PS5 CPU core P-state control via the SMU MP1 mailbox
 *
 * Out-of-tree module for PS5-Linux. There is no cpufreq/amd_pstate on this
 * platform; the SMU owns CPU clocking. This module talks to the same MP1
 * mailbox the built-in ps5 driver uses, via amd_smn_read/write, and exposes
 * two PPSMC messages through /dev/ps5cpc:
 *
 *   QueryCorePstate   (0x0c)  read a core's current p-state
 *   RequestCorePstate (0x0b)  set a core's p-state
 *
 * Argument packing (FW 7.61):  arg = (core_sel & 0xff) | ((pstate & 0xf) << 16)
 *
 * IMPORTANT asymmetry:
 *   QUERY: core_sel is a core *ID*      (0..7)
 *   SET:   core_sel is a core *BITMASK* (bit i = physical core i; all = 0xff)
 *
 * P-state table on the tested unit (read via AMD PStateDef MSRs):
 *   P0 3200  P1 2560  P2 2327  P3 1969  P4 1829  P5 1600  P6 1280  P7 800 MHz
 *   NOTE: every p-state reports the same VID (1.05 V). RequestCorePstate only
 *   changes FID/DID, never voltage, so lowering the clock saves little power.
 *
 * Safety: SMN failures return -EIO (never panic). Every core moved off P0 is
 * restored to P0 automatically on rmmod. Do NOT run "boost" (UniversalMode) or
 * any other MP1 user concurrently -- the mailbox is shared.
 *
 * Unload (restores all touched cores to P0):  rmmod ps5_corepstate_ctl
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <asm/amd/node.h>

/* Normal MP1 C2PMSG mailbox registers (SMN addresses, same as built-in driver) */
#define MP1_C2PMSG_66		0x3b10a08	/* message-id trigger   */
#define MP1_C2PMSG_82		0x3b10a48	/* arg / response value */
#define MP1_C2PMSG_90		0x3b10a68	/* status / response    */

#define PPSMC_MSG_RequestCorePstate	0x0b
#define PPSMC_MSG_QueryCorePstate	0x0c

/* Mailbox wait budget (matches the built-in driver: ~2 s worst case). */
#define SMN_WAIT_ITERS		200000
#define SMN_WAIT_UDELAY		10

#define MAX_CORE	7	/* physical cores 0..7 */
#define CORE_MASK_ALL	0xff
#define MAX_PSTATE	7	/* p-state ids 0..7 (0 = full clock = restore) */

#define PS5CPC_IOC_MAGIC	'Q'
struct ps5cpc_req {
	__u32 core_sel;		/* GET: core id 0..7 | SET: core bitmask     */
	__u32 pstate;		/* SET in: 0..7      | GET out: current id    */
	__u32 readback;		/* SET out: p-state of lowest core in mask    */
	__u32 flags;		/* reserved, 0                                */
};
#define PS5CPC_GET	_IOWR(PS5CPC_IOC_MAGIC, 0x01, struct ps5cpc_req)
#define PS5CPC_SET	_IOWR(PS5CPC_IOC_MAGIC, 0x02, struct ps5cpc_req)

static DEFINE_MUTEX(ps5cpc_lock);
static u32 touched_mask;	/* bit i set => core i was moved off P0 */

static int smn_rd(u32 reg, u32 *val)
{
	int ret = amd_smn_read(0, reg, val);

	if (ret)
		pr_err("ps5cpc: amd_smn_read(0x%08x) failed: %d\n", reg, ret);
	return ret;
}

static int smn_wr(u32 reg, u32 val)
{
	int ret = amd_smn_write(0, reg, val);

	if (ret)
		pr_err("ps5cpc: amd_smn_write(0x%08x) failed: %d\n", reg, ret);
	return ret;
}

/* Wait until the status register reads non-zero (mailbox ready). */
static int smn_wait_nonzero(u32 reg)
{
	u32 val;
	int i, ret;

	for (i = 0; i < SMN_WAIT_ITERS; i++) {
		ret = smn_rd(reg, &val);
		if (ret)
			return -EIO;
		if (val)
			return 0;
		udelay(SMN_WAIT_UDELAY);
	}
	return -ETIMEDOUT;
}

/* One MP1 mailbox transaction. Mirrors the built-in mp1fw_sendmsg(), no panic. */
static int mp1_sendmsg(u8 msgid, u32 arg, u32 *resp_val)
{
	u32 resp, rv;
	int ret;

	ret = smn_wait_nonzero(MP1_C2PMSG_90);
	if (ret) {
		pr_err("ps5cpc: mailbox busy before msg 0x%x (arg 0x%08x)\n", msgid, arg);
		return ret;
	}
	if (smn_wr(MP1_C2PMSG_90, 0) ||		/* clear status      */
	    smn_wr(MP1_C2PMSG_82, arg) ||	/* write argument    */
	    smn_wr(MP1_C2PMSG_66, msgid))	/* trigger message   */
		return -EIO;

	ret = smn_wait_nonzero(MP1_C2PMSG_90);
	if (ret) {
		pr_err("ps5cpc: timeout after msg 0x%x (arg 0x%08x)\n", msgid, arg);
		return ret;
	}
	if (smn_rd(MP1_C2PMSG_90, &resp) || smn_rd(MP1_C2PMSG_82, &rv))
		return -EIO;
	if (resp != 1) {
		pr_warn("ps5cpc: msg 0x%x arg 0x%08x -> bad resp 0x%08x\n",
			msgid, arg, resp);
		return -EIO;
	}
	if (resp_val)
		*resp_val = rv;
	return 0;
}

/* QUERY: core_sel is a core id 0..7. */
static int core_query(u32 core_id, u32 *pstate)
{
	return mp1_sendmsg(PPSMC_MSG_QueryCorePstate, core_id & 0xff, pstate);
}

/* SET: core_mask is a bitmask; pstate 0..7 (0 = full clock). */
static int core_request(u32 core_mask, u32 pstate)
{
	u32 arg = (core_mask & 0xff) | ((pstate & 0x0f) << 16);

	return mp1_sendmsg(PPSMC_MSG_RequestCorePstate, arg, NULL);
}

/* Restore every core we touched back to P0 (best-effort). */
static void restore_all(void)
{
	u32 i;

	for (i = 0; i <= MAX_CORE; i++) {
		if (!(touched_mask & BIT(i)))
			continue;
		if (core_request(BIT(i), 0))
			pr_err("ps5cpc: restore core %u -> P0 FAILED\n", i);
		else
			pr_info("ps5cpc: restored core %u -> P0\n", i);
	}
	touched_mask = 0;
}

static long ps5cpc_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct ps5cpc_req q;
	int ret;

	if (copy_from_user(&q, (void __user *)arg, sizeof(q)))
		return -EFAULT;

	mutex_lock(&ps5cpc_lock);

	switch (cmd) {
	case PS5CPC_GET:
		if (q.core_sel > MAX_CORE) {
			ret = -EINVAL;
			break;
		}
		ret = core_query(q.core_sel, &q.pstate);
		break;

	case PS5CPC_SET: {
		u32 lowest;

		if (q.core_sel == 0 || q.core_sel > CORE_MASK_ALL ||
		    q.pstate > MAX_PSTATE) {
			ret = -EINVAL;
			break;
		}
		ret = core_request(q.core_sel, q.pstate);
		if (ret)
			break;
		touched_mask |= (q.core_sel & CORE_MASK_ALL);
		/* read back the lowest core in the mask (QUERY wants an id) */
		lowest = __ffs(q.core_sel);
		ret = core_query(lowest, &q.readback);
		if (!ret)
			pr_info("ps5cpc: SET mask 0x%02x -> P%u (core %u readback P%u)\n",
				q.core_sel, q.pstate, lowest, q.readback);
		break;
	}

	default:
		ret = -ENOTTY;
		break;
	}

	mutex_unlock(&ps5cpc_lock);
	if (ret)
		return ret;
	if (copy_to_user((void __user *)arg, &q, sizeof(q)))
		return -EFAULT;
	return 0;
}

static const struct file_operations ps5cpc_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl	= ps5cpc_ioctl,
};

static struct miscdevice ps5cpc_dev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "ps5cpc",
	.fops	= &ps5cpc_fops,
};

static int __init ps5cpc_init(void)
{
	int ret = misc_register(&ps5cpc_dev);

	if (ret) {
		pr_err("ps5cpc: misc_register failed: %d\n", ret);
		return ret;
	}
	pr_info("ps5cpc: ready (/dev/ps5cpc; auto-restore to P0 on unload)\n");
	return 0;
}

static void __exit ps5cpc_exit(void)
{
	mutex_lock(&ps5cpc_lock);
	restore_all();
	mutex_unlock(&ps5cpc_lock);
	misc_deregister(&ps5cpc_dev);
	pr_info("ps5cpc: unloaded\n");
}

module_init(ps5cpc_init);
module_exit(ps5cpc_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("ps5-cpufreq-project");
MODULE_DESCRIPTION("PS5 CPU core P-state control via SMU MP1 (GET + guarded SET)");
