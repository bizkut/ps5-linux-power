// SPDX-License-Identifier: GPL-2.0
/*
 * ps5_dfpstate_ctl - PS5 DF (memory/fabric) P-state control via SMU MP1
 *
 * This is the *real* power lever on PS5-Linux. Lowering the DF P-state from the
 * default df3 (FCLK 1200 / UCLK 875) to df2 (750/425) drops wall power by ~15%
 * on the tested unit. Below df2 the GPU/SoC dominates and is not controllable
 * from Linux.
 *
 * Talks to the normal MP1 mailbox via amd_smn_read/write and exposes /dev/ps5dfc:
 *   QueryDfPstate   (0x13)  read current df_pstate            (arg ignored)
 *   RequestDfPstate (0x12)  set df_pstate                     (arg = state 0..3)
 *
 * DF P-state -> FCLK/UCLK (measured read-only via TESTSMC 0x38/0x39):
 *   df0 250/225   df1 250/225   df2 750/425   df3 1200/875 (MHz)
 * Sony's "mempstate" maps inverted: df_pstate = 3 - mempstate.
 *
 * !!! WARNING: df0/df1 ARE RISKY !!!
 *   RequestDfPstate(1) was observed to DEADLOCK the SMU mailbox once (every
 *   later query returns "busy"; only a reboot recovers; the system otherwise
 *   stays up). A later attempt from df2 succeeded, so it is state-dependent and
 *   not reliable. The extra saving vs df2 is only ~5 W. All states 0..3 are
 *   writable here, but treat df0/df1 as experimental: short idle test only,
 *   no load, be ready to reboot.
 *
 * Guards: default is NO-OP (a SET needs an explicit force flag); the
 * current state is read before every write; the boot value is captured as the
 * restore baseline and re-applied on rmmod; after a write QueryDfPstate is
 * polled until it matches (else immediate restore). SMN errors return -EIO,
 * never panic. The MP1 mailbox is shared: do NOT run boost (UniversalMode) or
 * the CPU-pstate module's writes concurrently.
 *
 * Unload (restores baseline):  rmmod ps5_dfpstate_ctl
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <asm/amd/node.h>

/* Normal MP1 C2PMSG mailbox registers (SMN addresses). */
#define MP1_C2PMSG_66		0x3b10a08	/* message-id trigger   */
#define MP1_C2PMSG_82		0x3b10a48	/* arg / response value */
#define MP1_C2PMSG_90		0x3b10a68	/* status / response    */

#define PPSMC_MSG_RequestDfPstate	0x12	/* WRITE */
#define PPSMC_MSG_QueryDfPstate		0x13	/* read  */

#define SMN_WAIT_ITERS	200000
#define SMN_WAIT_UDELAY	10

#define DF_PSTATE_MAX	3		/* valid states 0..3 (all writable) */
#define DF_POLL_TRIES	2000
#define DF_POLL_UDELAY	1500

#define PS5DFC_IOC_MAGIC	'F'
#define PS5DFC_FORCE_MAGIC	0x5005	/* required for a real write */

struct ps5dfc_req {
	__u32 state;	/* SET in: target 0..3 | GET out: current 0..3 */
	__u32 force;	/* SET in: must == PS5DFC_FORCE_MAGIC to write  */
	__u32 prev;	/* SET out: state read before the write         */
	__u32 readback;	/* SET out: state read back after the write     */
};
#define PS5DFC_GET	_IOWR(PS5DFC_IOC_MAGIC, 0x01, struct ps5dfc_req)
#define PS5DFC_SET	_IOWR(PS5DFC_IOC_MAGIC, 0x02, struct ps5dfc_req)

static DEFINE_MUTEX(ps5dfc_lock);
static u32 df_baseline = 0xffffffff;	/* captured at init = restore target */
static bool df_dirty;			/* true once moved off baseline */

static int smn_rd(u32 reg, u32 *val)
{
	int ret = amd_smn_read(0, reg, val);

	if (ret)
		pr_err("ps5dfc: amd_smn_read(0x%08x) failed: %d\n", reg, ret);
	return ret;
}

static int smn_wr(u32 reg, u32 val)
{
	int ret = amd_smn_write(0, reg, val);

	if (ret)
		pr_err("ps5dfc: amd_smn_write(0x%08x) failed: %d\n", reg, ret);
	return ret;
}

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

/* One MP1 mailbox transaction. Mirrors the built-in mp1fw_sendmsg(); no panic. */
static int mp1_sendmsg(u8 msgid, u32 arg, u32 *resp_val)
{
	u32 resp, rv;
	int ret;

	ret = smn_wait_nonzero(MP1_C2PMSG_90);
	if (ret) {
		pr_err("ps5dfc: mailbox busy before msg 0x%x (arg 0x%x)\n", msgid, arg);
		return ret;
	}
	if (smn_wr(MP1_C2PMSG_90, 0) ||
	    smn_wr(MP1_C2PMSG_82, arg) ||
	    smn_wr(MP1_C2PMSG_66, msgid))
		return -EIO;

	ret = smn_wait_nonzero(MP1_C2PMSG_90);
	if (ret) {
		pr_err("ps5dfc: timeout after msg 0x%x (arg 0x%x)\n", msgid, arg);
		return ret;
	}
	if (smn_rd(MP1_C2PMSG_90, &resp) || smn_rd(MP1_C2PMSG_82, &rv))
		return -EIO;
	if (resp != 1) {
		pr_warn("ps5dfc: msg 0x%x arg 0x%x -> bad resp 0x%08x\n", msgid, arg, resp);
		return -EIO;
	}
	if (resp_val)
		*resp_val = rv;
	return 0;
}

static int df_query(u32 *cur)
{
	return mp1_sendmsg(PPSMC_MSG_QueryDfPstate, 0, cur);
}

/* Send RequestDfPstate(target) then poll QueryDfPstate until it matches. */
static int df_request_verified(u32 target)
{
	u32 cur = 0xffffffff;
	int ret, i;

	ret = mp1_sendmsg(PPSMC_MSG_RequestDfPstate, target, NULL);
	if (ret)
		return ret;

	for (i = 0; i < DF_POLL_TRIES; i++) {
		ret = df_query(&cur);
		if (ret)
			return ret;
		if (cur == target)
			return 0;
		udelay(DF_POLL_UDELAY);
	}
	pr_err("ps5dfc: poll timeout: target %u, last read %u\n", target, cur);
	return -ETIMEDOUT;
}

static void df_restore_baseline(void)
{
	int ret;

	if (!df_dirty || df_baseline > DF_PSTATE_MAX)
		return;
	ret = df_request_verified(df_baseline);
	if (ret)
		pr_err("ps5dfc: RESTORE to baseline %u FAILED: %d\n", df_baseline, ret);
	else
		pr_info("ps5dfc: restored df_pstate -> baseline %u\n", df_baseline);
	df_dirty = false;
}

static long ps5dfc_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct ps5dfc_req q;
	int ret;

	if (copy_from_user(&q, (void __user *)arg, sizeof(q)))
		return -EFAULT;

	mutex_lock(&ps5dfc_lock);

	switch (cmd) {
	case PS5DFC_GET:
		ret = df_query(&q.state);
		break;

	case PS5DFC_SET:
		if (q.state > DF_PSTATE_MAX) {
			ret = -EINVAL;
			break;
		}
		if (q.state < 2 && q.force == PS5DFC_FORCE_MAGIC)
			pr_warn("ps5dfc: writing df%u is experimental (mailbox-deadlock risk)\n",
				q.state);
		ret = df_query(&q.prev);	/* always read current first */
		if (ret)
			break;
		if (q.force != PS5DFC_FORCE_MAGIC) {	/* no force => no write */
			q.readback = q.prev;
			pr_info("ps5dfc: SET dry-run (no force): target %u, current %u, NOT written\n",
				q.state, q.prev);
			ret = 0;
			break;
		}
		pr_info("ps5dfc: SET force: %u -> %u (baseline %u)\n",
			q.prev, q.state, df_baseline);
		ret = df_request_verified(q.state);
		if (ret) {
			pr_err("ps5dfc: write to %u failed (%d) -> restoring baseline\n",
			       q.state, ret);
			df_restore_baseline();
			break;
		}
		df_dirty = (q.state != df_baseline);
		ret = df_query(&q.readback);
		if (!ret)
			pr_info("ps5dfc: SET done: df_pstate now %u\n", q.readback);
		break;

	default:
		ret = -ENOTTY;
		break;
	}

	mutex_unlock(&ps5dfc_lock);
	if (ret)
		return ret;
	if (copy_to_user((void __user *)arg, &q, sizeof(q)))
		return -EFAULT;
	return 0;
}

static const struct file_operations ps5dfc_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl	= ps5dfc_ioctl,
};

static struct miscdevice ps5dfc_dev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "ps5dfc",
	.fops	= &ps5dfc_fops,
};

static int __init ps5dfc_init(void)
{
	u32 cur = 0xffffffff;
	int ret;

	mutex_lock(&ps5dfc_lock);
	ret = df_query(&cur);
	mutex_unlock(&ps5dfc_lock);
	if (ret) {
		pr_err("ps5dfc: initial QueryDfPstate failed: %d -- refusing to load\n", ret);
		return ret;
	}
	if (cur > DF_PSTATE_MAX) {
		pr_err("ps5dfc: unexpected baseline df_pstate %u -- refusing to load\n", cur);
		return -EIO;
	}
	df_baseline = cur;
	df_dirty = false;

	ret = misc_register(&ps5dfc_dev);
	if (ret) {
		pr_err("ps5dfc: misc_register failed: %d\n", ret);
		return ret;
	}
	pr_info("ps5dfc: ready (/dev/ps5dfc). baseline df=%u, writes allowed: df0..df%u "
		"(df0/df1 experimental), force=0x%x\n",
		df_baseline, DF_PSTATE_MAX, PS5DFC_FORCE_MAGIC);
	return 0;
}

static void __exit ps5dfc_exit(void)
{
	mutex_lock(&ps5dfc_lock);
	df_restore_baseline();
	mutex_unlock(&ps5dfc_lock);
	misc_deregister(&ps5dfc_dev);
	pr_info("ps5dfc: unloaded\n");
}

module_init(ps5dfc_init);
module_exit(ps5dfc_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("ps5-cpufreq-project");
MODULE_DESCRIPTION("PS5 DF (memory/fabric) P-state control via SMU MP1");
