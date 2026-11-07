/* SPDX-License-Identifier: GPL-2.0 */
/* Host-side template store reference implementation.
 *
 * The TA exports one opaque blob per finger (tplType 121).  The blob carries
 * its own magic, a header-derived CRC16 tail, and no geometry/config fields
 * (D19/D20; summarised in docs/dev/fingerprint-driver-design.md, section 4).  This module is the host
 * contract:
 *
 *   - validate magic / layout / CRC before ever calling FtIniAlgTplData_v1;
 *   - wrap the blob in a small container that records finger, geometry,
 *     slot size and a caller-supplied config digest;
 *   - write it atomically with mode 0600.
 *
 * The CRC is corruption detection only.  Confidentiality and tamper resistance
 * stay the job of the storage layer (file permissions, encryption, rollback
 * protection); see D19.
 */
#ifndef FT_TPL_STORE_H
#define FT_TPL_STORE_H

#include <stddef.h>
#include <stdint.h>

#define FT_TPL_MAGIC            0x1415927Du
#define FT_TPL_HEADER_BYTES     216u
#define FT_TPL_HEADER_SIZE_FLD  208u
#define FT_TPL_SLOT_COUNT       96u
#define FT_TPL_FEATURE_BYTES    44u
#define FT_TPL_CRC_BYTES        2u
#define FT_TPL_MIN_SLOT_BYTES   489u
#define FT_TPL_MAX_SLOT_BYTES   65536u
#define FT_TPL_MAX_BLOB_BYTES   (4u * 1024u * 1024u)
#define FT_TPL_CONFIG_DIGEST    32u
#define FT_TPL_MAX_FINGER       5u

#define FT_STORE_MAGIC          "FPT1"
#define FT_STORE_VERSION        1u
#define FT_STORE_HDR_BYTES      64u

enum ft_tpl_status {
        FT_TPL_OK = 0,
        FT_TPL_ERR_ARG = -1,
        FT_TPL_ERR_MAGIC = -2,
        FT_TPL_ERR_LAYOUT = -3,
        FT_TPL_ERR_CRC = -4,
        FT_TPL_ERR_IO = -5,
        FT_TPL_ERR_META = -6,
        FT_TPL_ERR_VERSION = -7,
        FT_TPL_ERR_NOMEM = -8,
};

struct ft_tpl_meta {
        uint8_t finger;
        uint16_t width;
        uint16_t height;
        uint32_t slot_bytes;
        uint8_t config_digest[FT_TPL_CONFIG_DIGEST];
        uint32_t blob_len;
};

/* FtCalCrc16: reflected 0x8005 (0xA001), init 0, no final reflection/xor. */
unsigned ft_tpl_crc16(const uint8_t *data, size_t len);

/* Validate the raw TA blob for the given geometry/slot size.  Does not use the
 * environment: the caller must already know width/height/slot_bytes from the
 * sensor config used for enrollment. */
enum ft_tpl_status ft_tpl_validate(const uint8_t *blob, size_t len,
                                   uint16_t width, uint16_t height,
                                   uint32_t slot_bytes);

/* Write meta + blob to path atomically.  path is replaced on success. */
enum ft_tpl_status ft_tpl_store_write(const char *path,
                                      const struct ft_tpl_meta *meta,
                                      const uint8_t *blob, size_t len);

/* Read and validate a store file.  If expect is non-NULL, finger/geometry/
 * slot_bytes/config_digest must match it.  On success *blob_out is malloc'ed
 * and must be released with ft_tpl_free(). */
enum ft_tpl_status ft_tpl_store_read(const char *path,
                                     const struct ft_tpl_meta *expect,
                                     uint8_t **blob_out, size_t *len_out);

void ft_tpl_free(void *ptr);
const char *ft_tpl_status_str(enum ft_tpl_status status);

#endif /* FT_TPL_STORE_H */
