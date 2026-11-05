/* gk3-fastbootd：给 /init 用的子命令（README §13.3）。/init 先停掉常驻实例再调（同一块盘只有一个写者）。
 *
 *   --wipe-data            恢复出厂。自己判"免二次确认"：成立就擦 → 0；不成立 → 3（/init 显示确认页）
 *   --wipe-data --confirm  用户已在确认页确认：照擦 → 0
 *   --clear-bcb            只清 BCB → 0
 * 两种 --wipe-data 都先过守卫（合并中 / 更新待验证 → 4）；没有目标盘、读写出错 → 5。
 * stdout 只打一两行英文（/init 原样显示在界面上）；过程照常进日志（stderr → /run/gk3/fastbootd.log）。
 *
 * 恢复出厂的语义（见 tools/gk3boot/README.md）：
 *   userdata：BLKDISCARD（尽力）+ 开头 / 末尾各 1 MiB 写零，读回确认开头 4 KiB 全零；metadata：整块清零；
 *   misc 只清 BCB（0–2 KiB），而且等两块都擦完才清 —— 断电后下一次进入会重做（清零是幂等的）；
 *   不碰 super、boot_x、ESP、BCAB、VAB。Android 下次开机由 fs_mgr 按 formattable 重建（本机未实测，E10）。
 * 免二次确认（§4.4.3）：迁移标记已存在，且 gk3.why=wipe，且这份 BCB 是 wipe，且它的 SHA-1 与 gk3boot 分派时
 *   记进 GK3 的摘要一致、记录的 why 也是 wipe —— 标记之后出现的 BCB 只可能是 Android 写的。
 * 守卫（fastboot-design §4.6.2）：VAB 合并中（MERGING）或更新已装、待验证（SNAPSHOTTED，有效值，当前槽 ≠ 源槽）⇒ 拒绝。
 *   设计稿对 SNAPSHOTTED 的目标做法是"目标槽设为不可启动、切回源槽、再擦"，要先核实 libsnapshot 的源槽 /
 *   forward-merge 取法（§4.6.2 写明"核实之前：拒绝"）—— 这一版按"核实之前"做。
 *   拒绝时【不保留】延期执行的清除请求：BCB 里的 wipe / prompt_wipe 清掉，GK3 记一条 refused_merging（aux = VAB 状态），
 *   Android 开机后由 boot_control HAL 发通知（Gk3Boot.cpp 认这个事件）。屏幕上 /init 显示 stdout 的那句。 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "fbd.h"

static uint8_t m[GK3_MISC_READ_SIZE];

static int read_misc(void)
{
    if (!G.disk.ok) {
        printf("No usable target disk: %s\n", G.disk.err);
        return -1;
    }
    if (fb_misc_read(&G.disk, m)) {
        printf("Cannot read misc: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

/* 整份 BCB 清零、写后读回（= recovery 的 clear_bootloader_message） */
static int clear_bcb(void)
{
    uint8_t z[GK3_MISC_BCB_SIZE];
    gk3_bcb_clear(z);
    return fb_misc_write(&G.disk, GK3_MISC_BCB_OFF, z, sizeof(z));
}

/* GK3 记录里追加一条事件；记录无效就不记（记录是参考信息，入口下次开机会重建） */
static void rec_event(gk3_ev_code code, uint32_t aux)
{
    static uint8_t rec[GK3_REC_SIZE];
    memcpy(rec, m + GK3_MISC_GK3_OFF, GK3_REC_SIZE);
    if (gk3_rec_validate(rec) != GK3_OK) {
        fb_log("gk3rec: no valid record, event %s not recorded", gk3_ev_name(code));
        return;
    }
    gk3_rec_event_add(rec, code, G.cur_slot < 0 ? 0xff : (uint8_t)G.cur_slot, aux);
    gk3_rec_seal(rec);
    if (fb_misc_write(&G.disk, GK3_MISC_GK3_OFF, rec, GK3_REC_SIZE))
        fb_log("gk3rec: writing event %s failed: %s", gk3_ev_name(code), strerror(errno));
    else
        fb_log("gk3rec: event %s (aux %u) recorded", gk3_ev_name(code), aux);
}

/* 免二次确认的判据；不成立时 why 写原因 */
static bool auto_ok(const gk3_bcb_info *bi, char *why, size_t n)
{
    const uint8_t *rec = m + GK3_MISC_GK3_OFF;
    uint8_t d[20];
    if (strcmp(G.why, "wipe")) {
        snprintf(why, n, "not entered for a factory reset request (gk3.why=%s)", G.why[0] ? G.why : "-");
        return false;
    }
    if (bi->kind != GK3_BCB_WIPE) {
        snprintf(why, n, "the request in misc is not a factory reset (%s)", gk3_bcb_kind_name(bi->kind));
        return false;
    }
    if (gk3_rec_validate(rec) != GK3_OK || !gk3_rec_migrated(rec)) {
        snprintf(why, n, "no migration marker in misc");
        return false;
    }
    gk3_rec_dispatch_digest(rec, d);
    if (gk3_rec_dispatch_why(rec) != GK3_BCB_WIPE || !gk3_rec_dispatch_count(rec) || memcmp(d, bi->digest, 20)) {
        snprintf(why, n, "the request does not match what the boot loader recorded");
        return false;
    }
    return true;
}

int fb_sub_wipe(bool confirm)
{
    gk3_bcb_info bi;
    gk3_vab v;
    uint8_t st;
    char why[160];
    fb_ctx *c;

    if (read_misc())
        return FB_EXIT_FAILED;
    gk3_bcb_classify(m, &bi);
    gk3_vab_parse(m + GK3_MISC_SYSTEM_OFF, &v);
    st = gk3_vab_effective(&v, G.cur_slot < 0 ? 0u : (unsigned)G.cur_slot);
    fb_log("wipe-data%s: BCB %s '%s', VAB %s, gk3.why=%s", confirm ? " --confirm" : "", gk3_bcb_kind_name(bi.kind),
           bi.command, fb_merge_name(st), G.why[0] ? G.why : "-");

    if (st == GK3_MERGE_MERGING || st == GK3_MERGE_SNAPSHOTTED) {
        if (bi.kind == GK3_BCB_WIPE || bi.kind == GK3_BCB_PROMPT_WIPE) {
            if (clear_bcb())
                fb_log("refused wipe: clearing the BCB failed: %s", strerror(errno));
            else
                fb_log("refused wipe: BCB cleared (no deferred wipe)");
        }
        rec_event(GK3_EV_REFUSED_MERGING, st);
        if (st == GK3_MERGE_MERGING)
            printf("Factory reset refused: a system update is being merged. Start Android, let the update finish,\n"
                   "then request the factory reset again. Nothing was erased.\n");
        else
            printf("Factory reset refused: a system update is installed but not yet verified. Start Android once,\n"
                   "then request the factory reset again. Nothing was erased.\n");
        return FB_EXIT_REFUSED;
    }
    if (!confirm && !auto_ok(&bi, why, sizeof(why))) {
        fb_log("wipe-data: confirmation required: %s", why);
        printf("Request not verified (%s): confirmation required.\n", why);
        return FB_EXIT_CONFIRM;
    }

    fb_status("erasing user data");
    c = fb_ctx_local();
    if (fb_erase_part(c, FB_P_USERDATA) || fb_erase_part(c, FB_P_METADATA)) {
        printf("Factory reset FAILED: %s\n", fb_ctx_last_fail(c));
        return FB_EXIT_FAILED;
    }
    /* 两块都擦完才清 BCB（断电续做） */
    if (bi.kind != GK3_BCB_NONE && clear_bcb()) {
        printf("User data erased, but clearing the request in misc failed (%s).\n", strerror(errno));
        return FB_EXIT_FAILED;
    }
    fb_log("wipe-data: userdata and metadata erased, BCB cleared");
    fb_status("user data erased");
    printf("User data erased. Android will set itself up again on the next start.\n");
    return 0;
}

int fb_sub_clear_bcb(void)
{
    gk3_bcb_info bi;
    if (read_misc())
        return FB_EXIT_FAILED;
    gk3_bcb_classify(m, &bi);
    if (gk3_is_zero(m, GK3_MISC_BCB_SIZE)) {
        printf("Boot request already empty.\n");
        return 0;
    }
    if (clear_bcb()) {
        printf("Clearing the boot request failed: %s\n", strerror(errno));
        return FB_EXIT_FAILED;
    }
    fb_log("clear-bcb: BCB '%s' (%s) cleared", bi.command, gk3_bcb_kind_name(bi.kind));
    printf("Boot request '%s' cleared.\n", bi.command);
    return 0;
}
