/* libgk3core：双系统（Windows + Android）的决定（见 tools/gk3boot/README.md）。
 *
 * 只决定、不写盘：两个函数都只改调用方给的 GK3 记录缓冲区（清掉消费了的标记、记事件），要不要写 LoaderEntryDefault /
 * LoaderEntryOneShot、写回 misc、复位，由调用方（gk3boot.efi）照 out 去做。这样整套规则能在主机上逐条测
 * （test/test_dual.c），UEFI 那边只剩"读变量、看文件在不在、写变量"。
 *
 * 判定顺序：
 *   a. LoaderEntryDefault 不是合法值就删（合法 = 不存在，或 Windows 条目 id）；
 *   b. 应用 set_default=windows|android（写或删 LoaderEntryDefault），不复位，接着往下走；
 *   c. Android 有待办 ⇒ next=windows、clean_poweroff 作废并记事件，照常启动 Android；
 *   d. 没有待办、且 (next=windows 或 (clean_poweroff 且默认是 Windows)) 且 bootmgfw 在 ⇒ 先清标记，再 OneShot=Windows、复位；
 *   e. 其余照常启动 Android；默认是 Windows 时交接前预置 OneShot。
 * next=sdboot-menu（执行端"Other systems"的退化出口）早于 BCB 分派处理：清掉、返回菜单；
 * 它同时让 clean_poweroff 作废（"有 Android 待办"的一种）。 */
#include "gk3core.h"

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static bool ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (lower(*a) != lower(*b))
            return false;
    return *a == *b;
}

gk3_defvar gk3_defvar_classify(const char *value)
{
    if (!value)
        return GK3_DEFVAR_ABSENT;
    /* 空串 / 读不出来的值 systemd-boot 也会落回 loader.conf（boot.c:1797-1810），留着没有意义：当不合法删掉 */
    if (ieq(value, "auto-windows") || ieq(value, "gk3-windows.conf"))
        return GK3_DEFVAR_WINDOWS;
    return GK3_DEFVAR_OTHER;
}

void gk3_dual_plan_default(uint8_t *rec, gk3_defvar var, bool windows_present, gk3_def_plan *out)
{
    gk3_setdef req = gk3_rec_set_default_req(rec);

    gk3_memset(out, 0, sizeof(*out));
    /* 变量是 Windows id 但 bootmgfw 不在：systemd-boot 匹配不到条目、落回 loader.conf（boot.c:1797-1810）= 实际进 Android。
     * 是合法值，不删（Windows 的文件回来了它就又生效） */
    out->default_windows_before = var == GK3_DEFVAR_WINDOWS && windows_present;
    out->default_windows = out->default_windows_before;
    if (var == GK3_DEFVAR_OTHER) {
        out->action = GK3_DEFACT_DELETE;
        out->reset = true;
        out->default_windows = false;
        gk3_rec_event_add(rec, GK3_EV_DEFAULT_RESET, 0xff, 0);
    }
    if (req == GK3_SETDEF_NONE)
        return;
    gk3_rec_put_set_default_req(rec, GK3_SETDEF_NONE);
    out->applied = req;
    if (req == GK3_SETDEF_WINDOWS) {
        if (!windows_present) {
            out->dropped = true;
            gk3_rec_event_add(rec, GK3_EV_INTENT_DROPPED, GK3_DROP_NO_WINDOWS, GK3_INTENT_SET_DEFAULT);
            return;
        }
        /* 已经是 Windows id（且没被上面当成不合法删掉）就不重写：auto-windows 与 gk3-windows.conf 都算"已是" */
        out->action = var == GK3_DEFVAR_WINDOWS ? GK3_DEFACT_NONE : GK3_DEFACT_SET_WINDOWS;
        out->default_windows = true;
        gk3_rec_event_add(rec, GK3_EV_DEFAULT_SET, 0, GK3_OS_WINDOWS);
    } else {
        /* android：变量在（Windows id 或不合法值）就删 */
        out->action = var == GK3_DEFVAR_ABSENT ? GK3_DEFACT_NONE : GK3_DEFACT_DELETE;
        out->default_windows = false;
        gk3_rec_event_add(rec, GK3_EV_DEFAULT_SET, 0, GK3_OS_ANDROID);
    }
}

static void drop(uint8_t *rec, gk3_dual_boot *out, gk3_intent what, gk3_drop_why why)
{
    out->dropped |= (uint8_t)(1u << (what - 1));
    gk3_rec_event_add(rec, GK3_EV_INTENT_DROPPED, (uint8_t)why, (uint32_t)what);
}

void gk3_dual_plan_boot(uint8_t *rec, bool default_windows, bool windows_present, bool android_pending,
                        gk3_dual_boot *out)
{
    gk3_next_kind nk = gk3_rec_next(rec, NULL);
    bool cp = gk3_rec_clean_poweroff(rec);

    gk3_memset(out, 0, sizeof(*out));
    if (nk == GK3_NEXT_SDBOOT_MENU) {
        gk3_rec_set_next(rec, GK3_NEXT_NONE, 0);
        if (cp) {
            gk3_rec_set_flag(rec, GK3_REC_F_CLEAN_POWEROFF, false);
            drop(rec, out, GK3_INTENT_CLEAN_POWEROFF, GK3_DROP_ANDROID_PENDING);
        }
        out->kind = GK3_DUAL_SDBOOT_MENU;
        return;
    }
    if (nk == GK3_NEXT_SLOT)
        android_pending = true;    /* next=slot:x 归入口的另一段（"Boot other slot"），这里只把它当待办 */

    bool want_next = nk == GK3_NEXT_WINDOWS;
    bool want_off = cp && default_windows;
    if (want_next)
        gk3_rec_set_next(rec, GK3_NEXT_NONE, 0);
    if (cp)
        gk3_rec_set_flag(rec, GK3_REC_F_CLEAN_POWEROFF, false);   /* 默认不是 Windows 时静默清（本不该有，有也无害） */

    if (want_next || want_off) {
        gk3_drop_why why = android_pending    ? GK3_DROP_ANDROID_PENDING
                           : !windows_present ? GK3_DROP_NO_WINDOWS
                                              : (gk3_drop_why)0;
        if (why) {
            if (want_next)
                drop(rec, out, GK3_INTENT_NEXT_WINDOWS, why);
            if (want_off)
                drop(rec, out, GK3_INTENT_CLEAN_POWEROFF, why);
        } else {
            out->kind = GK3_DUAL_WINDOWS;
            out->why = want_next ? GK3_INTENT_NEXT_WINDOWS : GK3_INTENT_CLEAN_POWEROFF;
            gk3_rec_event_add(rec, GK3_EV_TO_WINDOWS, 0xff, (uint32_t)out->why);
            return;
        }
    }
    out->kind = GK3_DUAL_ANDROID;
    out->preset_oneshot = default_windows;
}
