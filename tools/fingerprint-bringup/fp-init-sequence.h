/* SPDX-License-Identifier: GPL-2.0 */
#ifndef GAOKUN3_FP_INIT_SEQUENCE_H
#define GAOKUN3_FP_INIT_SEQUENCE_H
/* Legacy Windows initialization/sleep/wake protocol experiment.
 * IMPORTANT: matching-version ST firmware ignores87 payload and returns
 * constant5aa5 for88/index20. ACK/marker tests are NOT hardware-state proof. Include after the
 * fpversion module's request/response buffers and kernel declarations.
 * No user-selected registers, raw commands, flash or template operations.
 * Evidence: docs/dev/fingerprint/sc-init-sequence-2026-10-30.md.
 * Each MCU reply (including empty ACKs) must pass length/status/XOR checks.
 */

static int fpinit_exchange(u32 app_id, const u8 *packet, unsigned int size,
                          u8 *out, unsigned int out_size, bool *uncertain)
{
 u8 *rx = (u8 *)response + 12;
 unsigned int i, length;
 u8 check = 0;
 int ret;

 /* All callers below use at most eight request bytes and two reply bytes.
  * Bounds also prevent accidental extension into an arbitrary raw interface. */
 if (size < 5 || size > 8 || out_size > 2 || (out_size && !out))
  return -EINVAL;
 memset(request, 0, 4096); memset(response, 0xa5, 4096);
 ((__le32 *)request)[0] = cpu_to_le32(0x8021);
 ((__le32 *)request)[1] = cpu_to_le32(size);
 memcpy((u8 *)request + 12, packet, size);
 ret = qcom_scm_qseecom_app_send(app_id, request, size + 12, response, 12);
 if (ret) { *uncertain = true; return ret; }
 /* Windows TA_Commication waits 10ms after each IOCTL, independently of
  * the higher-level delays below. SCM failures stop immediately here (the
  * Windows error path also delays); they are not safe retry candidates. */
 msleep(10);
 if (le32_to_cpu(((__le32 *)request)[0]) != 0x5aa5 ||
     le32_to_cpu(((__le32 *)request)[2])) return -EPROTO;

 memset(request, 0, 4096); memset(response, 0xa5, 4096);
 ((__le32 *)request)[0] = cpu_to_le32(0x8020);
 ((__le32 *)request)[2] = cpu_to_le32(out_size + 5);
 ret = qcom_scm_qseecom_app_send(app_id, request, 12,
                               response, out_size + 17);
 if (ret) { *uncertain = true; return ret; }
 /* Windows TA_Commication waits 10ms after each IOCTL, independently of
  * the higher-level delays below. SCM failures stop immediately here (the
  * Windows error path also delays); they are not safe retry candidates. */
 msleep(10);
 /* Windows ReadInfo_* waits 1ms after reading; WriteInfo/CustomDataWrite
  * empty ACKs have no additional delay. This also covers the one indirect89
  * read, without adding a second 1ms in its caller. */
 if (out_size) msleep(1);
 pr_info("fpinit: MCU cmd=0x%02x index=0x%02x reply_bytes=%u\n",
         packet[3], size > 5 ? packet[4] : 0, out_size + 5);
 print_hex_dump(KERN_INFO, "fpinit MCU reply: ", DUMP_PREFIX_OFFSET,
                16, 1, rx, out_size + 5, false);
 if (le32_to_cpu(((__le32 *)request)[0]) != 0x5aa5 ||
     le32_to_cpu(((__le32 *)request)[2])) return -EPROTO;
 length = ((unsigned int)rx[1] << 8) | rx[2];
 if (length != out_size + 1 || rx[3] != 4) return -EPROTO;
 for (i = 1; i <= length + 2; ++i) check ^= rx[i];
 if (check != rx[length + 3]) return -EBADMSG;
 if (out_size) memcpy(out, rx + 4, out_size);
 return 0;
}

static int fpinit_state(u32 app_id, u16 *state, bool *uncertain)
{
 static const u8 packet[] = {2, 0, 3, 0x88, 0x20, 2, 0xa9};
 u8 value[2];
 int ret = fpinit_exchange(app_id, packet, sizeof(packet), value, 2, uncertain);
 if (!ret) {
  *state = (u16)value[0] | ((u16)value[1] << 8);
  pr_info("fpinit: valid direct20 state=0x%04x\n", *state);
 }
 return ret;
}

static int fpinit_write(u32 app_id, unsigned int step, bool *uncertain)
{
 /* Only the four literal WriteInfo packets used by this initialization. */
 static const u8 packets[][8] = {
  {2, 0, 4, 0x87, 0x01, 1, 0x01, 0x82},
  {2, 0, 4, 0x87, 0x30, 1, 0xbb, 0x09},
  {2, 0, 4, 0x87, 0x1f, 1, 0x01, 0x9c},
  {2, 0, 4, 0x87, 0x1e, 1, 0x01, 0x9d},
 };
 int ret;
 if (step >= ARRAY_SIZE(packets)) return -EINVAL;
 ret = fpinit_exchange(app_id, packets[step], 8, NULL, 0, uncertain);
 if (!ret)
  pr_info("fpinit: direct87 index=0x%02x value=0x%02x MCU ACK verified\n",
          packets[step][4], packets[step][6]);
 return ret;
}

static int fpinit_wake(u32 app_id, bool *uncertain)
{
 static const u8 packet[] = {2, 0, 3, 0x8e, 1, 0x70, 0xfc};
 u16 state = 0;
 unsigned int n;
 int ret = fpinit_state(app_id, &state, uncertain);
 if (ret) return ret;
 if (state == 0x5aa5) return 0;
 for (n = 0; n < 10; ++n) {
  ret = fpinit_exchange(app_id, packet, sizeof(packet), NULL, 0, uncertain);
  if (ret) return ret;
  msleep(5);
  ret = fpinit_exchange(app_id, packet, sizeof(packet), NULL, 0, uncertain);
  if (ret) return ret;
  msleep(10);
  ret = fpinit_state(app_id, &state, uncertain);
  if (ret) return ret;
  if (state == 0x5aa5) {
   pr_info("fpinit: wake verified after %u attempt(s)\n", n + 1);
   return 0;
  }
 }
 pr_err("fpinit: wake timeout; never observed valid ready state\n");
 return -ETIMEDOUT;
}

static __maybe_unused int init_sleep_wake(u32 app_id, bool *uncertain)
{
 static const u8 sleep_finish[] = {2, 0, 3, 0x89, 0x1d, 1, 0x96};
 u16 state = 0;
 u8 finish = 0;
 unsigned int n;
 int ret;

 if (*uncertain) return -EIO;
 /* query_info must have succeeded upstream. Recheck immediately before any
  * configuration write; do not convert a failed/invalid read into asleep. */
 ret = fpinit_state(app_id, &state, uncertain);
 if (ret) return ret;
 if (state != 0x5aa5) return -EPROTO;
 ret = fpinit_write(app_id, 0, uncertain);
 if (ret) return ret;
 ret = fpinit_write(app_id, 1, uncertain);
 if (ret) return ret;
 /* config_device_mode(0) ensures power_mode(0) again after these writes. */
 ret = fpinit_wake(app_id, uncertain);
 if (ret) return ret;
 ret = fpinit_write(app_id, 2, uncertain);
 if (ret) return ret;
 ret = fpinit_write(app_id, 3, uncertain);
 if (ret) return ret;
 for (n = 0; n < 50; ++n) {
  msleep(3);
  ret = fpinit_state(app_id, &state, uncertain);
  if (ret) return ret;
  if (state == 0x5aa5) break;
 }
 if (n == 50) {
  /* Windows continues after this polling limit. The diagnostic deliberately
   * stops here to avoid hiding which transition failed. */
  pr_err("fpinit: sleep precondition timeout; stopping before indirect89\n");
  return -ETIMEDOUT;
 }
 /* Windows 0x9b74-0x9c20 retries the complete indirect89 transaction,
  * at most three attempts. Only a completed SCM operation with invalid MCU
  * framing is retryable here; an uncertain secure call must not be repeated. */
 for (n = 0; n < 3; ++n) {
  ret = fpinit_exchange(app_id, sleep_finish, sizeof(sleep_finish),
                        &finish, 1, uncertain);
  pr_info("fpinit: indirect89 attempt=%u ret=%d uncertain=%u\n",
          n + 1, ret, (unsigned int)*uncertain);
  if (*uncertain) return ret ? ret : -EIO;
  if (!ret) break;
  if (ret != -EPROTO && ret != -EBADMSG) return ret;
 }
 if (ret) {
  /* Three valid SCM exchanges can still yield invalid indirect89 frames.
   * Inspect direct20 once to locate the state transition, without issuing
   * wake/configuration writes or turning this failure into success. */
  if (!*uncertain && (ret == -EPROTO || ret == -EBADMSG)) {
   int original = ret;
   u16 diagnostic_state = 0;
   int diagnostic_ret = fpinit_state(app_id, &diagnostic_state, uncertain);
   pr_info("fpinit: indirect89 exhausted original=%d diagnostic20_ret=%d valid=%u state=0x%04x uncertain=%u\n",
           original, diagnostic_ret, (unsigned int)(diagnostic_ret == 0),
           diagnostic_state, (unsigned int)*uncertain);
   return original;
  }
  return ret;
 }
 pr_info("fpinit: indirect89 index1d valid payload=0x%02x\n", finish);
 ret = fpinit_state(app_id, &state, uncertain);
 if (ret) return ret;
 if (state == 0x5aa5) {
  pr_err("fpinit: sleep transition failed; still ready\n");
  return -EPROTO;
 }
 pr_info("fpinit: sleep verified with valid non-ready state=0x%04x\n", state);
 ret = fpinit_wake(app_id, uncertain);
 if (ret) return ret;
 pr_info("fpinit: fixed initialization, sleep and final awake verified\n");
 return 0;
}
#endif
