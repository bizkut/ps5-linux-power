// SPDX-License-Identifier: GPL-2.0
/*
 * ps5_dffreq_probe - read-only FCLK/UCLK-per-DF-P-state probe (PS5, SMU)
 *
 * Maps raw df_pstate 0..3 -> FCLK MHz / UCLK MHz, READ-ONLY. Uses the SMU TEST
 * mailbox (different registers than the normal one), modelled on the built-in
 * mp1fw_test_sendmsg(). Sends only two read-only Query messages; no write path.
 *
 * Message IDs (proven from the FW 7.61 TESTSMC name table):
 *   0x38 TESTSMC_MSG_QueryFclkFreqOfDfPstate
 *   0x39 TESTSMC_MSG_QueryUclkFreqOfDfPstate
 * (These are TESTSMC IDs, distinct from PPSMC 0x12/0x13.)
 *
 * Results print to dmesg. Measured on the tested unit:
 *   df0 250/225   df1 250/225   df2 750/425   df3 1200/875   (FCLK/UCLK MHz)
 *
 * SMN errors return -EIO (no panic). Do NOT run boost or another MP1 user
 * concurrently (shared mailbox). No device node; just load to print, then rmmod.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <asm/amd/node.h>

/* TEST mailbox registers (SMN addresses). */
#define MP1_C2PMSG_72	0x3b10a20	/* test msgid trigger        */
#define MP1_C2PMSG_96	0x3b10a80	/* test status / response    */
#define MP1_C2PMSG_98	0x3b10a88	/* test arg / response-value */

#define TESTSMC_MSG_QueryFclkFreqOfDfPstate	0x38
#define TESTSMC_MSG_QueryUclkFreqOfDfPstate	0x39

#define SMN_WAIT_ITERS	200000
#define SMN_WAIT_UDELAY	10
#define DF_PSTATE_COUNT	4

static DEFINE_MUTEX(ps5dff_lock);

static int smn_rd(u32 reg, u32 *val)
{
	int ret = amd_smn_read(0, reg, val);

	if (ret)
		pr_err("ps5dff: amd_smn_read(0x%08x) failed: %d\n", reg, ret);
	return ret;
}

static int smn_wr(u32 reg, u32 val)
{
	int ret = amd_smn_write(0, reg, val);

	if (ret)
		pr_err("ps5dff: amd_smn_write(0x%08x) failed: %d\n", reg, ret);
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

/* One TEST-mailbox transaction. arg = df_pstate; *resp_val = returned freq. */
static int test_sendmsg(u8 msgid, u32 arg, u32 *resp_val)
{
	u32 resp, rv;
	int ret;

	ret = smn_wait_nonzero(MP1_C2PMSG_96);
	if (ret) {
		pr_err("ps5dff: test mbox busy before msg 0x%x arg 0x%x\n", msgid, arg);
		return ret;
	}
	if (smn_wr(MP1_C2PMSG_96, 0) ||
	    smn_wr(MP1_C2PMSG_98, arg) ||
	    smn_wr(MP1_C2PMSG_72, msgid))
		return -EIO;

	ret = smn_wait_nonzero(MP1_C2PMSG_96);
	if (ret) {
		pr_err("ps5dff: test mbox timeout after msg 0x%x arg 0x%x\n", msgid, arg);
		return ret;
	}
	if (smn_rd(MP1_C2PMSG_96, &resp) || smn_rd(MP1_C2PMSG_98, &rv))
		return -EIO;
	if (resp != 1) {
		pr_warn("ps5dff: msg 0x%x arg 0x%x -> bad resp 0x%08x\n", msgid, arg, resp);
		return -EIO;
	}
	if (resp_val)
		*resp_val = rv;
	return 0;
}

static void df_freq_sweep(void)
{
	u32 ps;

	pr_info("ps5dff: --- FCLK/UCLK per df_pstate (read-only TEST mbox) ---\n");
	for (ps = 0; ps < DF_PSTATE_COUNT; ps++) {
		u32 fclk = 0, uclk = 0;
		int rf, ru;

		rf = test_sendmsg(TESTSMC_MSG_QueryFclkFreqOfDfPstate, ps, &fclk);
		ru = test_sendmsg(TESTSMC_MSG_QueryUclkFreqOfDfPstate, ps, &uclk);

		if (rf || ru)
			pr_info("ps5dff:  df_pstate %u -> FCLK %s, UCLK %s\n", ps,
				rf ? "ERR" : "ok", ru ? "ERR" : "ok");
		else
			pr_info("ps5dff:  df_pstate %u -> FCLK %u, UCLK %u\n", ps, fclk, uclk);
	}
}

static int __init ps5dff_init(void)
{
	pr_info("ps5dff: read-only DF FCLK/UCLK probe (TEST mbox, queries only)\n");
	mutex_lock(&ps5dff_lock);
	df_freq_sweep();
	mutex_unlock(&ps5dff_lock);
	pr_info("ps5dff: done (see dmesg); no device registered, rmmod to remove\n");
	return 0;
}

static void __exit ps5dff_exit(void)
{
	pr_info("ps5dff: unloaded\n");
}

module_init(ps5dff_init);
module_exit(ps5dff_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("ps5-cpufreq-project");
MODULE_DESCRIPTION("PS5 read-only DF FCLK/UCLK-per-DfPstate probe (TEST mailbox)");
