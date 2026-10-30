/* SPDX-License-Identifier: GPL-2.0 */
#ifndef GAOKUN3_FP_FRAME_STATUS_H
#define GAOKUN3_FP_FRAME_STATUS_H
/* Fixed no-image diagnostic for the pinned ST firmware. 88/index1d returns
 * object+c in its low bit and also sets object+e (host acknowledgement), so
 * this query is not described as a side-effect-free register read.
 * A valid lowbit1 means MCUframe_available, not finger presence or image
 * quality. Stop at that point without reading SRAM or consuming the frame.
 * Requires fp-init-sequence.h and the caller's serialized shared buffers.
 */
static int fp_frame_status(u32 app_id, bool *uncertain)
{
 static const u8 packet[] = {2, 0, 3, 0x88, 0x1d, 1, 0x97};
 unsigned int n;
 u8 status = 0;
 int ret;

 if (*uncertain) return -EIO;
 for (n = 0; n < 10; ++n) {
  ret = fpinit_exchange(app_id, packet, sizeof(packet), &status, 1, uncertain);
  if (ret) return ret;
  pr_info("fpstatus: sample=%u raw1d=0x%02x MCUframe_available=%u\n",
          n + 1, (unsigned int)status, (unsigned int)(status & 1));
  if (status & 1) {
   pr_info("fpstatus: MCU frame flag observed; stopping without SRAM read or frame consumption\n");
   return 0;
  }
  /* Gap after a completed validated transaction, not a claim of a precise
   * sampling frequency: fpinit_exchange itself has10+10+1ms of delay. */
  if (n != 9) msleep(100);
 }
 pr_info("fpstatus: ten valid samples without MCU frame flag; no finger-presence inference\n");
 return 0;
}
#endif
