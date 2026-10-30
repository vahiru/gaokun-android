/* SPDX-License-Identifier: GPL-2.0 */
#ifndef GAOKUN3_FP_ACTIVE_SCAN_H
#define GAOKUN3_FP_ACTIVE_SCAN_H
/* Fixed frame-gated MCU capture for the verified40x196 ST firmware.
 * Include after fp-init-sequence.h. The caller owns shared-buffer lifetime,
 * verifies dimensions, and serializes debugfs access.
 * Only88/index1d and8b are issued:87 is a no-op in this firmware,20 is a
 * constant reply, and the old Windows ST/scan sequence is not a fallback.
 * Saved bytes are encoded MCU data, NOT a validated decoded fingerprint.
 * No user-selected commands, enrollment, template or flash operations.
 */
#define FP_SCAN_DATA_BYTES 15680
#define FP_SCAN_TA_REPLY_BYTES (FP_SCAN_DATA_BYTES + 17)
#if !defined(FP_RESPONSE_CAPACITY) || FP_RESPONSE_CAPACITY < FP_SCAN_TA_REPLY_BYTES
#error "Active scan requires a verified response allocation of at least 15697 bytes"
#endif

static u8 fp_scan_data[FP_SCAN_DATA_BYTES];
static bool fp_scan_valid;

static int fpscan_read_frame(u32 app_id, bool *uncertain)
{
 static const u8 read_sram[] = {2, 0, 5, 0x8b, 0x34, 0, 0x3d, 0x40, 0xc7};
 u8 *rx = (u8 *)response + 12;
 u8 check = 0;
 unsigned int i, length;
 int ret;

 memset(request, 0, 4096); memset(response, 0xa5, FP_RESPONSE_CAPACITY);
 ((__le32 *)request)[0] = cpu_to_le32(0x8021);
 ((__le32 *)request)[1] = cpu_to_le32(sizeof(read_sram));
 memcpy((u8 *)request + 12, read_sram, sizeof(read_sram));
 ret = qcom_scm_qseecom_app_send(app_id, request, 21, response, 12);
 if (ret) { *uncertain = true; return ret; }
 msleep(10);
 if (le32_to_cpu(((__le32 *)request)[0]) != 0x5aa5 ||
     le32_to_cpu(((__le32 *)request)[2])) return -EPROTO;
 /* Retain ReadSram's20ms after the write plus10ms per successful SCM call.
  * The old outer GetImage10ms is intentionally omitted after a freshly
  * validated frame flag to reduce expiry; no speculative SRAM read occurs. */
 msleep(20);
 memset(request, 0, 4096); memset(response, 0xa5, FP_RESPONSE_CAPACITY);
 ((__le32 *)request)[0] = cpu_to_le32(0x8020);
 ((__le32 *)request)[2] = cpu_to_le32(FP_SCAN_DATA_BYTES + 5);
 ret = qcom_scm_qseecom_app_send(app_id, request, 12, response,
                               FP_SCAN_TA_REPLY_BYTES);
 if (ret) { *uncertain = true; return ret; }
 msleep(10);
 if (le32_to_cpu(((__le32 *)request)[0]) != 0x5aa5 ||
     le32_to_cpu(((__le32 *)request)[2])) return -EPROTO;
 length = ((unsigned int)rx[1] << 8) | rx[2];
 /* Validate exact bounds before indexing checksum. Windows did not impose
  * a constant byte0; retain the proven length/status/XOR contract only. */
 if (length != FP_SCAN_DATA_BYTES + 1 || rx[3] != 4) return -EPROTO;
 for (i = 1; i <= length + 2; ++i) check ^= rx[i];
 if (check != rx[length + 3]) return -EBADMSG;
 memcpy(fp_scan_data, rx + 4, sizeof(fp_scan_data));
 fp_scan_valid = true;
 pr_info("fpscan: frame-gated transfer of %u encoded bytes passed framing; decoding not verified\n",
         (unsigned int)FP_SCAN_DATA_BYTES);
 return 0;
}

static int fp_active_scan_bounded(u32 app_id, bool *uncertain, unsigned int limit)
{
 static const u8 frame_status[] = {2, 0, 3, 0x88, 0x1d, 1, 0x97};
 u8 status = 0;
 unsigned int n;
 int ret;

 fp_scan_valid = false;
 memzero_explicit(fp_scan_data, sizeof(fp_scan_data));
 if (limit < 10 || limit > 200) return -EINVAL;
 if (*uncertain) return -EIO;
 for (n = 0; n < limit; ++n) {
  /* index1d acknowledges the host by setting object+e. Only its lowbit
   * reflects MCUframe_available; other bits and constant20 are not gates. */
  ret = fpinit_exchange(app_id, frame_status, sizeof(frame_status),
                        &status, 1, uncertain);
  if (ret) return ret;
  pr_info("fpscan: sample=%u raw1d=0x%02x MCUframe_available=%u\n",
          n + 1, (unsigned int)status, (unsigned int)(status & 1));
  if (status & 1)
   return fpscan_read_frame(app_id, uncertain);
  if (n + 1 != limit) msleep(100);
 }
 if (limit == 10)
  pr_info("fpscan: no MCU frame flag in ten valid samples; SRAM not read\n");
 else
  pr_info("fpscan: no MCU frame flag in %u valid samples; SRAM not read\n", limit);
 return -ENODATA;
}

static int fp_active_scan(u32 app_id, bool *uncertain)
{
 return fp_active_scan_bounded(app_id, uncertain, 10);
}
#endif
