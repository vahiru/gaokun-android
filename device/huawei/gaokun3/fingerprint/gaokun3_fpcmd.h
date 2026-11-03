/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _GAOKUN3_FPCMD_H
#define _GAOKUN3_FPCMD_H

#ifdef __KERNEL__
#include <linux/ioctl.h>
#include <linux/types.h>
typedef __u32 gaokun3_fpcmd_u32;
typedef __u8 gaokun3_fpcmd_u8;
typedef __aligned_u64 gaokun3_fpcmd_u64;
#else
#include <stdint.h>
#include <sys/ioctl.h>
typedef uint32_t gaokun3_fpcmd_u32;
typedef uint8_t gaokun3_fpcmd_u8;
typedef uint64_t gaokun3_fpcmd_u64;
#endif

#define GAOKUN3_FPCMD_IOC_MAGIC 'G'

struct gaokun3_fpcmd_capture {
	gaokun3_fpcmd_u32 image_bytes;
	gaokun3_fpcmd_u32 reserved;
	gaokun3_fpcmd_u64 user_ptr;
};

#define GAOKUN3_FPCMD_IOC_LOAD \
	_IO(GAOKUN3_FPCMD_IOC_MAGIC, 0x00)
#define GAOKUN3_FPCMD_IOC_UNLOAD \
	_IO(GAOKUN3_FPCMD_IOC_MAGIC, 0x01)
#define GAOKUN3_FPCMD_IOC_INIT \
	_IO(GAOKUN3_FPCMD_IOC_MAGIC, 0x02)
#define GAOKUN3_FPCMD_IOC_PROBE \
	_IO(GAOKUN3_FPCMD_IOC_MAGIC, 0x03)
#define GAOKUN3_FPCMD_IOC_WAKE \
	_IO(GAOKUN3_FPCMD_IOC_MAGIC, 0x04)
#define GAOKUN3_FPCMD_IOC_CAPTURE \
	_IOWR(GAOKUN3_FPCMD_IOC_MAGIC, 0x05, struct gaokun3_fpcmd_capture)
#define GAOKUN3_FPCMD_IOC_GET_FINGER_STATUS \
	_IOR(GAOKUN3_FPCMD_IOC_MAGIC, 0x06, gaokun3_fpcmd_u8)

#endif
