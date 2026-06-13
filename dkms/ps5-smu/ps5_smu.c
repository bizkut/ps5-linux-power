// SPDX-License-Identifier: GPL-2.0
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>
#include <asm/amd/node.h>
#include "ps5_smu.h"

#define Q0_CMD 0x03B10A08
#define Q0_RSP 0x03B10A68
#define Q0_ARG 0x03B10A48
#define Q3_CMD 0x03B10A20
#define Q3_RSP 0x03B10A80
#define Q3_ARG 0x03B10A88

#define MSG_REQUEST_CORE_PSTATE 0x0b
#define MSG_QUERY_CORE_PSTATE   0x0c
#define MSG_REQUEST_GFXCLK      0x0e
#define MSG_QUERY_GFXCLK        0x0f
#define MSG_GET_CPU_VOLTAGE     0x36
#define MSG_GET_GPU_VOLTAGE     0x37

#define PS5_SMU_NODE 0
#define PS5_SMU_POLL_TRIES 200000

static DEFINE_MUTEX(ps5_smu_lock);

static int ps5_smn_read(u32 reg, u32 *val)
{
	return amd_smn_read(PS5_SMU_NODE, reg, val);
}

static int ps5_smn_write(u32 reg, u32 val)
{
	return amd_smn_write(PS5_SMU_NODE, reg, val);
}

static int ps5_smu_wait_nz(u32 reg, u32 *out)
{
	u32 val = 0;
	int i, ret;

	for (i = 0; i < PS5_SMU_POLL_TRIES; i++) {
		ret = ps5_smn_read(reg, &val);
		if (ret)
			return ret;
		if (val) {
			if (out)
				*out = val;
			return 0;
		}
		cpu_relax();
	}

	return -ETIMEDOUT;
}

static int ps5_smu_mbox(u32 cmd, u32 rsp, u32 arg_reg, u8 msg, u32 arg, u32 *status, u32 *val)
{
	u32 st = 0, out = 0;
	int ret = 0;

	mutex_lock(&ps5_smu_lock);
	ret = ps5_smu_wait_nz(rsp, NULL);
	if (ret)
		goto out_unlock;
	ret = ps5_smn_write(rsp, 0);
	if (ret)
		goto out_unlock;
	ret = ps5_smn_write(arg_reg, arg);
	if (ret)
		goto out_unlock;
	ret = ps5_smn_write(arg_reg + 4, 0);
	if (ret)
		goto out_unlock;
	ret = ps5_smn_write(cmd, msg);
	if (ret)
		goto out_unlock;
	ret = ps5_smu_wait_nz(rsp, &st);
	if (ret)
		goto out_unlock;
	ret = ps5_smn_read(arg_reg, &out);
	if (ret)
		goto out_unlock;
	if ((st & 0xff) != 1) {
		ret = -EIO;
		goto out_unlock;
	}
	if (status)
		*status = st;
	if (val)
		*val = out;
out_unlock:
	mutex_unlock(&ps5_smu_lock);
	return ret;
}

static int ps5_smu_cpu_pstate_set(const struct ps5_smu_cpu_pstate *req)
{
	u32 arg;

	if (!req->mask || req->pstate > 7)
		return -EINVAL;
	arg = (req->mask & 0xff) | ((req->pstate & 0x0f) << 16);
	return ps5_smu_mbox(Q0_CMD, Q0_RSP, Q0_ARG, MSG_REQUEST_CORE_PSTATE, arg, NULL, NULL);
}

static int ps5_smu_cpu_pstate_get(struct ps5_smu_cpu_pstate *req)
{
	u32 pstate = 0;
	int ret;

	if (req->core > 7)
		return -EINVAL;
	ret = ps5_smu_mbox(Q0_CMD, Q0_RSP, Q0_ARG, MSG_QUERY_CORE_PSTATE, req->core, NULL, &pstate);
	if (ret)
		return ret;
	req->pstate = pstate & 0x0f;
	return 0;
}

static int ps5_smu_gfxclk_set(const struct ps5_smu_gfxclk *req)
{
	if (req->mhz < 400 || req->mhz > 2380)
		return -EINVAL;
	return ps5_smu_mbox(Q0_CMD, Q0_RSP, Q0_ARG, MSG_REQUEST_GFXCLK, req->mhz, NULL, NULL);
}

static int ps5_smu_gfxclk_get(struct ps5_smu_gfxclk *req)
{
	return ps5_smu_mbox(Q0_CMD, Q0_RSP, Q0_ARG, MSG_QUERY_GFXCLK, 0, NULL, &req->mhz);
}

static int ps5_smu_voltage_get(struct ps5_smu_voltage *req)
{
	u8 msg;

	switch (req->rail) {
	case PS5_SMU_VOLTAGE_CPU:
		msg = MSG_GET_CPU_VOLTAGE;
		break;
	case PS5_SMU_VOLTAGE_GPU:
		msg = MSG_GET_GPU_VOLTAGE;
		break;
	default:
		return -EINVAL;
	}
	return ps5_smu_mbox(Q3_CMD, Q3_RSP, Q3_ARG, msg, 0, NULL, &req->millivolts);
}

static long ps5_smu_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	void __user *argp = (void __user *)arg;

	switch (cmd) {
	case PS5_SMU_CPU_PSTATE_SET:
	{
		struct ps5_smu_cpu_pstate req;

		if (copy_from_user(&req, argp, sizeof(req)))
			return -EFAULT;
		return ps5_smu_cpu_pstate_set(&req);
	}
	case PS5_SMU_CPU_PSTATE_GET:
	{
		struct ps5_smu_cpu_pstate req;
		int ret;

		if (copy_from_user(&req, argp, sizeof(req)))
			return -EFAULT;
		ret = ps5_smu_cpu_pstate_get(&req);
		if (ret)
			return ret;
		if (copy_to_user(argp, &req, sizeof(req)))
			return -EFAULT;
		return 0;
	}
	case PS5_SMU_GFXCLK_SET:
	{
		struct ps5_smu_gfxclk req;

		if (copy_from_user(&req, argp, sizeof(req)))
			return -EFAULT;
		return ps5_smu_gfxclk_set(&req);
	}
	case PS5_SMU_GFXCLK_GET:
	{
		struct ps5_smu_gfxclk req = {};
		int ret = ps5_smu_gfxclk_get(&req);

		if (ret)
			return ret;
		if (copy_to_user(argp, &req, sizeof(req)))
			return -EFAULT;
		return 0;
	}
	case PS5_SMU_VOLTAGE_GET:
	{
		struct ps5_smu_voltage req;
		int ret;

		if (copy_from_user(&req, argp, sizeof(req)))
			return -EFAULT;
		ret = ps5_smu_voltage_get(&req);
		if (ret)
			return ret;
		if (copy_to_user(argp, &req, sizeof(req)))
			return -EFAULT;
		return 0;
	}
	default:
		return -ENOTTY;
	}
}

static const struct file_operations ps5_smu_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = ps5_smu_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = ps5_smu_ioctl,
#endif
};

static struct miscdevice ps5_smu_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "ps5-smu",
	.fops = &ps5_smu_fops,
	.mode = 0600,
};

static int __init ps5_smu_init(void)
{
	return misc_register(&ps5_smu_miscdev);
}

static void __exit ps5_smu_exit(void)
{
	misc_deregister(&ps5_smu_miscdev);
}

module_init(ps5_smu_init);
module_exit(ps5_smu_exit);

MODULE_AUTHOR("PS5 Linux contributors");
MODULE_DESCRIPTION("PlayStation 5 curated SMU mailbox control");
MODULE_LICENSE("GPL");
