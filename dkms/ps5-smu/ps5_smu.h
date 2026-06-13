/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _PS5_SMU_UAPI_H
#define _PS5_SMU_UAPI_H

#ifdef __KERNEL__
#include <linux/ioctl.h>
#include <linux/types.h>
#else
#include <stdint.h>
#include <sys/ioctl.h>
typedef uint8_t __u8;
typedef uint32_t __u32;
#endif

#define PS5_SMU_IOC_MAGIC 'S'

#define PS5_SMU_VOLTAGE_CPU 0
#define PS5_SMU_VOLTAGE_GPU 1

struct ps5_smu_cpu_pstate {
	__u8 core;
	__u8 mask;
	__u8 pstate;
	__u8 reserved;
};

struct ps5_smu_gfxclk {
	__u32 mhz;
};

struct ps5_smu_voltage {
	__u8 rail;
	__u8 reserved[3];
	__u32 millivolts;
};

#define PS5_SMU_CPU_PSTATE_SET _IOW(PS5_SMU_IOC_MAGIC, 1, struct ps5_smu_cpu_pstate)
#define PS5_SMU_CPU_PSTATE_GET _IOWR(PS5_SMU_IOC_MAGIC, 2, struct ps5_smu_cpu_pstate)
#define PS5_SMU_GFXCLK_SET     _IOW(PS5_SMU_IOC_MAGIC, 3, struct ps5_smu_gfxclk)
#define PS5_SMU_GFXCLK_GET     _IOWR(PS5_SMU_IOC_MAGIC, 4, struct ps5_smu_gfxclk)
#define PS5_SMU_VOLTAGE_GET    _IOWR(PS5_SMU_IOC_MAGIC, 5, struct ps5_smu_voltage)

#endif
