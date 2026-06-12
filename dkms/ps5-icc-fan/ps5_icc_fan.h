/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _PS5_ICC_FAN_UAPI_H
#define _PS5_ICC_FAN_UAPI_H

#ifdef __KERNEL__
#include <linux/ioctl.h>
#include <linux/types.h>
#else
#include <stdint.h>
#include <sys/ioctl.h>
typedef uint8_t __u8;
typedef uint32_t __u32;
#endif

#define PS5_FAN_IOC_MAGIC 'F'

struct ps5_fan_mode {
	__u8 zone;
	__u8 mode;
};

struct ps5_fan_temp {
	__u8 zone;
	__u8 temperature_c;
	__u8 fraction;
};

struct ps5_fan_servo {
	__u8 zone;
	__u32 setting[6];
};

struct ps5_fan_target_temp {
	__u8 zone;
	__u8 temperature_c;
};

struct ps5_fan_pattern {
	__u8 pattern;
};

#define PS5_FAN_CHANGE_SERVO_PATTERN _IOW(PS5_FAN_IOC_MAGIC, 1, struct ps5_fan_pattern)
#define PS5_FAN_MODE_GET             _IOWR(PS5_FAN_IOC_MAGIC, 2, struct ps5_fan_mode)
#define PS5_FAN_MODE_SET             _IOW(PS5_FAN_IOC_MAGIC, 3, struct ps5_fan_mode)
#define PS5_FAN_ZONE_TEMP_GET        _IOWR(PS5_FAN_IOC_MAGIC, 4, struct ps5_fan_temp)
#define PS5_FAN_SERVO_GET            _IOWR(PS5_FAN_IOC_MAGIC, 5, struct ps5_fan_servo)
#define PS5_FAN_TARGET_TEMP_SET      _IOW(PS5_FAN_IOC_MAGIC, 6, struct ps5_fan_target_temp)

#endif
