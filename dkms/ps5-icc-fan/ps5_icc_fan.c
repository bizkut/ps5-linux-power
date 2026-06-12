// SPDX-License-Identifier: GPL-2.0
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include "ps5_icc_fan.h"

#define ICC_MSG_MIN_SIZE 0x20
#define ICC_MSG_MAX_SIZE 0x7f0
#define ICC_SERVICE_ID_FAN 0x0a
#define ICC_SERVICE_ID_THERMAL 0x0b

struct icc_msg {
	u8 magic;
	u8 service_id;
	u16 msg_type;
	u16 unk_04;
	u16 id;
	u16 length;
	u16 checksum;
	u8 data[];
};

extern int icc_query(u8 *query, u8 *reply);

static DEFINE_MUTEX(ps5_fan_lock);

static int ps5_icc_query(u8 service_id, u16 msg_type, u16 length, u8 *data, size_t data_len)
{
	u8 *buf;
	struct icc_msg *msg;
	int ret;

	if (length < ICC_MSG_MIN_SIZE || length > ICC_MSG_MAX_SIZE)
		return -EINVAL;
	if (data_len > ICC_MSG_MAX_SIZE - sizeof(*msg))
		return -EINVAL;

	buf = kzalloc(ICC_MSG_MAX_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	msg = (struct icc_msg *)buf;
	msg->service_id = service_id;
	msg->msg_type = msg_type;
	msg->length = length;
	memcpy(msg->data, data, data_len);

	mutex_lock(&ps5_fan_lock);
	ret = icc_query(buf, buf);
	mutex_unlock(&ps5_fan_lock);
	if (ret)
		goto out;

	if (data_len)
		memcpy(data, msg->data, data_len);
out:
	kfree(buf);
	return ret;
}

static int fan_change_servo_pattern(u8 pattern)
{
	u8 data[4] = { pattern, pattern, pattern, pattern };

	return ps5_icc_query(ICC_SERVICE_ID_FAN, 0x0b, ICC_MSG_MIN_SIZE, data, sizeof(data));
}

static int fan_mode_get(struct ps5_fan_mode *mode)
{
	u8 data[0x20] = {};
	int ret;

	if (mode->zone > 3)
		return -EINVAL;

	data[0] = mode->zone;
	ret = ps5_icc_query(ICC_SERVICE_ID_FAN, 0x03, ICC_MSG_MIN_SIZE, data, sizeof(data));
	if (ret)
		return ret;

	mode->mode = data[2];
	return 0;
}

static int fan_mode_set(const struct ps5_fan_mode *mode)
{
	u8 data[2] = { mode->zone, mode->mode };

	if (mode->zone > 3 || mode->mode < 1 || mode->mode > 6)
		return -EINVAL;

	return ps5_icc_query(ICC_SERVICE_ID_FAN, 0x02, ICC_MSG_MIN_SIZE, data, sizeof(data));
}

static int fan_zone_temp_get(struct ps5_fan_temp *temp)
{
	u8 data[0x20] = {};
	int ret;

	if (temp->zone > 3)
		return -EINVAL;

	data[0] = temp->zone;
	ret = ps5_icc_query(ICC_SERVICE_ID_THERMAL, 0x01, ICC_MSG_MIN_SIZE, data, sizeof(data));
	if (ret)
		return ret;

	temp->fraction = data[2];
	temp->temperature_c = data[3];
	return 0;
}

static int fan_servo_get(struct ps5_fan_servo *servo)
{
	u8 data[0x20] = {};
	int ret, i;

	if (servo->zone > 3)
		return -EINVAL;

	data[0] = servo->zone;
	ret = ps5_icc_query(ICC_SERVICE_ID_FAN, 0x07, ICC_MSG_MIN_SIZE, data, sizeof(data));
	if (ret)
		return ret;

	for (i = 0; i < 6; i++)
		memcpy(&servo->setting[i], data + 4 + i * 4, sizeof(servo->setting[i]));

	return 0;
}

static int fan_target_temp_set(const struct ps5_fan_target_temp *target)
{
	u8 data[0x20] = {};
	u32 setval;

	if (target->zone > 3 || target->temperature_c > 110)
		return -EINVAL;

	setval = (u32)target->temperature_c << 8;
	data[0] = target->zone;
	data[1] = 0;
	memcpy(data + 4, &setval, sizeof(setval));

	return ps5_icc_query(ICC_SERVICE_ID_FAN, 0x06, 0x40, data, sizeof(data));
}

static long ps5_fan_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	void __user *argp = (void __user *)arg;

	switch (cmd) {
	case PS5_FAN_CHANGE_SERVO_PATTERN:
	{
		struct ps5_fan_pattern pattern;

		if (copy_from_user(&pattern, argp, sizeof(pattern)))
			return -EFAULT;
		return fan_change_servo_pattern(pattern.pattern);
	}
	case PS5_FAN_MODE_GET:
	{
		struct ps5_fan_mode mode;
		int ret;

		if (copy_from_user(&mode, argp, sizeof(mode)))
			return -EFAULT;
		ret = fan_mode_get(&mode);
		if (ret)
			return ret;
		if (copy_to_user(argp, &mode, sizeof(mode)))
			return -EFAULT;
		return 0;
	}
	case PS5_FAN_MODE_SET:
	{
		struct ps5_fan_mode mode;

		if (copy_from_user(&mode, argp, sizeof(mode)))
			return -EFAULT;
		return fan_mode_set(&mode);
	}
	case PS5_FAN_ZONE_TEMP_GET:
	{
		struct ps5_fan_temp temp;
		int ret;

		if (copy_from_user(&temp, argp, sizeof(temp)))
			return -EFAULT;
		ret = fan_zone_temp_get(&temp);
		if (ret)
			return ret;
		if (copy_to_user(argp, &temp, sizeof(temp)))
			return -EFAULT;
		return 0;
	}
	case PS5_FAN_SERVO_GET:
	{
		struct ps5_fan_servo servo;
		int ret;

		if (copy_from_user(&servo, argp, sizeof(servo)))
			return -EFAULT;
		ret = fan_servo_get(&servo);
		if (ret)
			return ret;
		if (copy_to_user(argp, &servo, sizeof(servo)))
			return -EFAULT;
		return 0;
	}
	case PS5_FAN_TARGET_TEMP_SET:
	{
		struct ps5_fan_target_temp target;

		if (copy_from_user(&target, argp, sizeof(target)))
			return -EFAULT;
		return fan_target_temp_set(&target);
	}
	default:
		return -ENOTTY;
	}
}

static const struct file_operations ps5_fan_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = ps5_fan_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = ps5_fan_ioctl,
#endif
};

static struct miscdevice ps5_fan_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "ps5-fan",
	.fops = &ps5_fan_fops,
	.mode = 0600,
};

static int __init ps5_fan_init(void)
{
	return misc_register(&ps5_fan_miscdev);
}

static void __exit ps5_fan_exit(void)
{
	misc_deregister(&ps5_fan_miscdev);
}

module_init(ps5_fan_init);
module_exit(ps5_fan_exit);

MODULE_AUTHOR("PS5 Linux contributors");
MODULE_DESCRIPTION("PlayStation 5 curated ICC fan control");
MODULE_LICENSE("GPL");
