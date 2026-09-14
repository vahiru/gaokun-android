/* SPDX-License-Identifier: GPL-2.0 */
#ifndef HX_FRAME_STATUS_H
#define HX_FRAME_STATUS_H

#ifdef HX_ALGO_HOST_TEST
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint8_t hx_frame_u8;
#else
#include <linux/types.h>
typedef u8 hx_frame_u8;
#endif

/* The Linux event-stack format has a four-byte header.  This differs from
 * the seven-byte frame returned by the Windows THP interface. */
#define HX_FRAME_HEADER_BYTES 4U
#define HX_FRAME_MATRIX_BYTES (60U * 40U * 2U)
#define HX_FRAME_SUFFIX_BYTES 256U
#define HX_FRAME_DIAG_VALID 0x00bbU

struct hx_frame_status {
	bool has_finger;
	bool retry;
};

static inline unsigned int hx_frame_get_le16(const hx_frame_u8 *p)
{
	return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

static inline bool
hx_parse_master_frame_status(const hx_frame_u8 *buf, size_t len,
			     struct hx_frame_status *status)
{
	const size_t suffix = HX_FRAME_HEADER_BYTES + HX_FRAME_MATRIX_BYTES;
	unsigned int x, y;

	if (!buf || !status || len < suffix + HX_FRAME_SUFFIX_BYTES)
		return false;
	/* Captured native event-stack master frames consistently use this sync
	 * word.  Rejecting a shifted/non-master frame is safer than interpreting
	 * its payload as a 40x60 capacitance matrix. */
	if (buf[0] != 0x5a || buf[1] != 0xa5)
		return false;
	if (hx_frame_get_le16(buf + suffix + 6) != HX_FRAME_DIAG_VALID)
		return false;

	x = hx_frame_get_le16(buf + suffix + 108);
	y = hx_frame_get_le16(buf + suffix + 110);
	status->has_finger = !((x & 0xff) == 0xff &&
			       (y & 0xff) == 0xff);
	status->retry = hx_frame_get_le16(buf + suffix) != 0;
	return true;
}

#endif /* HX_FRAME_STATUS_H */
