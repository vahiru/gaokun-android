/*
 * gk3boot.efi —— 统一启动入口（见 tools/gk3boot/README.md）：观察模式 + 动作模式。
 *
 *   systemd-boot（默认条目 gk3boot-android-<x>[+N].conf，或经 OneShot）→ gk3boot → 定位本盘 → 读 misc → 选槽
 *   →（动作模式：扣 tries、写 GK3 记录，写后读回）→ 读 boot_<x> 整份、校验 SHA1(id) → 拼 cmdline → H2 交接（handoff.c）。
 *
 * 两种模式：
 *   观察模式 gk3.observe=1：决策照算，"会怎么做"只记日志；不写 misc、不扣 tries、不消费 BCB（块设备包装连 write 回调都不给）；
 *     每次开机写一份 ESP 日志；唯一允许的写 = fail-open 时的 LoaderEntryOneShot（见下）。
 *   动作模式（gk3.observe=0 或不带 observe）：
 *     - 选中的槽未成功 → tries−1、重算 CRC、写回 misc+0x800、读回核对（gk3_blk_write_bytes_verify）；写不进去 → fail-open；
 *       tries 用完（tries 0 且未成功 = SetSlotAsUnbootable 的状态）下次自然落到另一槽 = 自动回滚（event=fallback）；
 *     - 两槽都不可启动 / VAB 合并中且 active 槽不可启动（不许换槽，§4.3.2-4）→ 本该进执行端，执行端（S7）还没有 → fail-open；
 *     - GK3 记录（misc+8 KiB）：boot_streak +1（开机完成由 Android 侧清零 —— S9 还没做），回落 / 看到 BCB 记事件；
 *       写后读回，写不进去只记日志、照常启动（记录是参考信息）。阈值动作（bootloop 进菜单）不启用：执行端没就绪；
 *     - BCB：分派开关默认关（E-K7：必须与执行端、迁移同版发布）—— 有命令只记录、不消费、不清除、照常启动 Android；
 *       gk3.dispatch=1 打开后（S7c，README §15）：首跑迁移（没有迁移标记 ⇒ 清存量 BCB、只记录不执行）、
 *       按 gk3_dispatch_plan 去执行端 / 清掉 / 3 次上限；两槽都不可启动、合并中不许换槽、已确认的槽连续
 *       GK3_BOOTLOOP_THRESHOLD 次没开机完成 ⇒ 也去执行端（why=noslot / merging / bootloop）；
 *     - 执行端（S7c）：同一个 boot_x 里的 zboot 内核 + 本目录的 fastboot.img 作 initrd（同一套 H2 交接），
 *       cmdline 由 gk3_cmdline_fastboot 拼。执行端缺失 / 读不出 / 内核读不出 ⇒ 记日志、照常启动 Android（不消费 BCB）；
 *       进执行端那一次不扣 tries、不加 boot_streak；bootloader / fastboot / recovery 类 BCB 进之前清掉，wipe 类留给执行端；
 *     - ESP 日志只在异常时写（正常路径对 ESP 零写入，§4.12），屏幕上也不打字（§4.3.1）。
 *     - 双系统（S15，README §16，⬜ 未上机）：读 LoaderEntryDefault（不合法就删）、应用 GK3 的 set_default 请求、
 *       消费 next=windows / next=sdboot-menu / clean_poweroff（Android 有待办时作废），默认是 Windows 时交接前
 *       预置 LoaderEntryOneShot=*-android-<x>.conf。规则在 libgk3core 的 dual.c，这里只碰变量和复位（见"双系统"一节）。
 *
 * fail-open（§4.12 阶梯第 1 步）：任何一步失败 → 记日志 → 写 LoaderEntryOneShot = 本 ESP 上的直连条目
 * <machine-id>-android-<x>.conf（x = 目标槽；没有就用另一槽的）→ ResetSystem(EfiResetCold)。写变量失败也照样复位。
 * 这样 gk3boot 即使是默认条目，失败一次后下一次也直接走直连条目（OneShot 读后即删，boot.c:1637-1640），不会原地循环；
 * 连续失败由 systemd-boot 的条目计数（+3）兜底。阶梯第 2 步（写变量失败时把自己的条目改名 +0）没做（README §11 限制）。
 * 绝不 return 错误码给 systemd-boot（boot.c:2971-2973 会原样交给固件，华为 BootFail 计数，§2.1），也不 return SUCCESS
 * （会停在不倒计时的菜单上）。
 *
 * LoadOptions（条目的 options 行，空格分隔）：
 *   gk3.observe=0|1   1 = 观察模式；0 或不带 = 动作模式
 *   gk3.dispatch=0|1  BCB 分派开关（缺省 = 编译期 GK3BOOT_DISPATCH_DEFAULT，出厂 0）
 *   gk3.action=fastboot|menu  直接进执行端（gk3boot-tools.conf 用，why=fastboot|menu；不看分派开关、不碰 BCB）
 *   gk3.fbtcp=0|1     执行端打开 TCP 5554（开发用，不认证；原样传给执行端的 cmdline）
 *   gk3.slot=a|b      强制启动这一槽（测试用；决策照算照记；动作模式下不扣 tries）
 *   gk3.hint=a|b      BCAB 无效时按它启动（§4.3.2-1）、fail-open 在选槽之前发生时的目标槽；
 *                     缺省取自己条目名里的 -android-<x>，再没有就是 a
 *   gk3.mid=<32 位十六进制>  fail-open 时只认这个 machine-id 的直连条目（缺省：在 \loader\entries 里自己找）
 *   gk3.hold=<秒>     fail-open 复位前在屏幕上停多久，缺省 5，最大 30
 */
#include "gk3efi.h"

#include "handoff.h"

#ifndef GK3BOOT_VERSION
#define GK3BOOT_VERSION "dev"
#endif
#ifndef GK3BOOT_DISPATCH_DEFAULT
#define GK3BOOT_DISPATCH_DEFAULT 0     /* E-K7：只有与执行端（S7）、迁移同版发布时才改成 1 */
#endif

#define WATCHDOG_SEC 120               /* 设计稿 §4.2 第 0 步 */
#define WATCHDOG_CODE 0x10002          /* 0x0000–0xFFFF 留给固件 */
#define LOG_CAP (256u * 1024u)
#define LOG_MAX 1000u                  /* boot-0 … boot-999；满了只上屏幕（README 的上机步骤里有清理） */
#define BOOTIMG_MAX (128ull * 1024 * 1024)

static uint64_t T0;
static EFI_LOADED_IMAGE_PROTOCOL *self_li;
static gk3_logfile g_log;
static bool g_log_tried;               /* 已经试过建日志文件（成功与否） */
static const char *g_entry;            /* 自己条目的文件名（LoaderBootCountPath / LoaderEntrySelected），可为 NULL */

static struct {
    bool observe;              /* 观察模式 */
    bool dispatch;             /* BCB 分派开关 */
    int force_slot;            /* -1 = 不强制 */
    int hint;                  /* -1 = 没给 gk3.hint */
    unsigned hold;
    char mid[33];              /* gk3.mid，空 = 自己找 */
    char action[12];           /* gk3.action：fastboot / menu，空 = 不直接进执行端 */
    bool fbtcp;                /* gk3.fbtcp=1 */
} opt = {false, GK3BOOT_DISPATCH_DEFAULT, -1, -1, 5, "", "", false};

static unsigned g_hint;                /* 生效的 hint */
static unsigned g_target;              /* fail-open 写 OneShot 用的目标槽：先是 hint，选完槽后是要启动的槽 */

#define MS() ((unsigned long long)(gk3_us_since(T0) / 1000u))
#define MSF(us) (unsigned long long)((us) / 1000u), (unsigned long long)((us) / 100u % 10u)

static void hex_str(const uint8_t *p, size_t n, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; i < n && o + 3 <= cap; i++)
        o += (size_t)gk3_snprintf(out + o, cap - o, "%02x", p[i]);
    if (cap)
        out[o < cap ? o : cap - 1] = 0;
}

static int str_cmp(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}

/* ------------------------------------------------------------------ 日志 */

/* 日志文件按需建：观察模式一开始就建；动作模式只在第一次出异常（notable / anomaly / fail-open）时建 ——
 * 内存里的缓冲从头记着，建文件时整段写进去（gk3_lg.synced 从 0 开始），所以异常之前的经过也在文件里。 */
static void log_ensure(void)
{
    if (g_log.open)
        return;
    if (g_log_tried) {
        gk3_log_reopen(&g_log);        /* 交接失败回来时文件已关；从没建成过（path 空）就什么也不做 */
        return;
    }
    g_log_tried = true;
    if (!self_li)
        return;
    if (gk3_log_open_seq(&g_log, self_li->DeviceHandle, u"\\EFI\\gk3boot\\log", "boot-", LOG_MAX)) {
        gk3_logf("log_file: \\EFI\\gk3boot\\log\\boot-%u.txt\n", g_log.n);
        gk3_log_sync();
    } else {
        gk3_logf("!! no log file (screen only); continuing\n");
    }
}

/* 动作模式里"值得留一份 ESP 日志"的事：prefix "!!" = 出错（写失败等），"note:" = 不正常但按设计处理了
 * （回落、BCAB 无效、BCB 没消费、强制槽…）。观察模式下日志本来就开着，这里只是多记一行。 */
static void __attribute__((format(printf, 2, 3))) flag_log(const char *prefix, const char *fmt, ...)
{
    static char line[600];
    va_list ap;
    va_start(ap, fmt);
    gk3_vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    gk3_logf("%s %s\n", prefix, line);
    log_ensure();
    gk3_log_sync();
}
#define notable(...) flag_log("note:", __VA_ARGS__)
#define anomaly(...) flag_log("!!", __VA_ARGS__)

/* ------------------------------------------------------------------ fail-open */

static void pre_start(void)
{
    gk3_logd("gk3boot.result=handoff t=%llu ms (log closed before StartImage)\n", MS());
    gk3_log_close(&g_log);
}

/* 直连条目：\loader\entries\<32 位十六进制>-android-<x>.conf（安装器的写法，scripts/live/installer-lib.sh:921）。
 * gk3boot 自己的条目 gk3boot-android-<x>[+N].conf 前缀不是 machine-id，不会被当成直连条目。
 * 有多个（多份安装共用 ESP）时取 id 最大的那个 —— 两份直连条目 sort-key / version 相同时，systemd-boot 的
 * default 通配命中的也是它（boot.c:1707-1745 按 -strverscmp(id) 排，:1771-1782 取第一个匹配）。 */
typedef struct {
    unsigned slot;
    unsigned n;
    char best[48];
} direct_ctx;

static bool is_hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }

static bool direct_cb(const char *name, bool is_dir, void *vctx)
{
    direct_ctx *c = vctx;
    char low[48];
    size_t n = gk3_strlen(name);
    if (is_dir || n != 32 + 9 + 1 + 5)
        return true;
    for (size_t i = 0; i <= n; i++)
        low[i] = (name[i] >= 'A' && name[i] <= 'Z') ? (char)(name[i] + 32) : name[i];   /* systemd-boot 的 id 是小写 */
    for (size_t i = 0; i < 32; i++)
        if (!is_hex(low[i]))
            return true;
    if (gk3_memcmp(low + 32, "-android-", 9) || low[41] != (char)('a' + c->slot) || gk3_memcmp(low + 42, ".conf", 6))
        return true;
    if (opt.mid[0] && gk3_memcmp(low, opt.mid, 32))
        return true;
    c->n++;
    if (!c->best[0] || str_cmp(low, c->best) > 0)
        gk3_memcpy(c->best, low, n + 1);
    return true;
}

/* 阶梯第 1 步。观察模式下这也是唯一允许的写（一个 NV 变量，systemd-boot 下次读到就删）。 */
static void oneshot_direct(void)
{
    direct_ctx c;
    unsigned want = g_target & 1;
    EFI_STATUS st = EFI_SUCCESS;

    if (!self_li) {
        gk3_logf("!! fail-open: no LoadedImage, cannot look for the direct entry; LoaderEntryOneShot NOT written\n");
        return;
    }
    for (unsigned k = 0; k < 2; k++) {
        gk3_memset(&c, 0, sizeof(c));
        c.slot = want ^ k;
        st = gk3_dir_each(self_li->DeviceHandle, u"\\loader\\entries", direct_cb, &c);
        if (EFI_ERROR(st) || c.n)
            break;
    }
    if (EFI_ERROR(st) && !c.n) {
        gk3_logf("!! fail-open: list \\loader\\entries: %s; LoaderEntryOneShot NOT written\n", gk3_efi_strerror(st));
        return;
    }
    if (!c.n) {
        gk3_logf("!! fail-open: no <machine-id>-android-{a,b}.conf on this ESP%s; LoaderEntryOneShot NOT written\n",
                 opt.mid[0] ? " for gk3.mid" : "");
        return;
    }
    if (c.slot != want)
        gk3_logf("fail-open: no direct entry for _%c, using _%c's\n", 'a' + want, 'a' + c.slot);
    if (c.n > 1)
        gk3_logf("fail-open: %u direct entries for _%c, taking %s (same one systemd-boot's default glob picks)\n", c.n,
                 'a' + c.slot, c.best);

    CHAR16 v[48];
    size_t n = gk3_strlen(c.best);
    for (size_t i = 0; i <= n; i++)
        v[i] = (CHAR16)(unsigned char)c.best[i];
    UINTN size = (n + 1) * sizeof(CHAR16);
    /* 与 scripts/boot-oneshot.sh 同一格式：属性 NV|BS|RT = 0x07，UTF-16LE + 结尾 NUL */
    const UINT32 attr = EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS;
    st = gk3_setvar(u"LoaderEntryOneShot", &gk3_guid_loader, attr, v, size);
    if (EFI_ERROR(st)) {
        gk3_logf("!! fail-open: SetVariable(LoaderEntryOneShot=%s): %s -- resetting anyway\n", c.best,
                 gk3_efi_strerror(st));
        return;
    }
    static uint8_t back[160];
    UINTN bsz = sizeof(back);
    UINT32 battr = 0;
    st = gk3_getvar(u"LoaderEntryOneShot", &gk3_guid_loader, &battr, back, &bsz);
    bool same = !EFI_ERROR(st) && bsz == size && !gk3_memcmp(back, v, size) && battr == attr;
    gk3_logf("fail-open: LoaderEntryOneShot=%s written (attr 0x%x, %llu bytes), read back %s\n", c.best, attr,
             (unsigned long long)size, same ? "OK" : EFI_ERROR(st) ? gk3_efi_strerror(st) : "MISMATCH");
}

static void __attribute__((noreturn, format(printf, 2, 3))) fail_open(const char *stage, const char *fmt, ...)
{
    static char why[512];
    va_list ap;
    va_start(ap, fmt);
    gk3_vsnprintf(why, sizeof(why), fmt, ap);
    va_end(ap);

    gk3_lg.screen = true;              /* 动作模式平时不上屏幕；失败要让人看得见 */
    log_ensure();
    gk3_logf("\n!! FAIL-OPEN at %s: %s\n", stage, why);
    gk3_logf("gk3boot.result=fail-open stage=%s target=_%c t=%llu ms\n", stage, 'a' + (g_target & 1), MS());
    oneshot_direct();
    gk3_logf("fail-open: nothing else is written on this path (misc untouched from here on); "
             "ResetSystem(EfiResetCold) -> the direct entry (or, if no OneShot was written, systemd-boot's default).\n");
    gk3_log_close(&g_log);
    if (gk3_lg.file_failed)
        gk3_screenf("!! log file write failed: %s (log is incomplete)\n", gk3_efi_strerror(gk3_lg.file_err));
    for (unsigned s = opt.hold; s > 0; s--) {
        gk3_screenf("cold reset in %u s ...\n", s);
        gk3_bs->Stall(1000000);
    }
    gk3_rt->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);
    /* 不该回来。回来了就等看门狗（绝不把错误返回给固件） */
    gk3_screenf("!! ResetSystem returned; waiting for the %u s watchdog\n", WATCHDOG_SEC);
    for (;;)
        gk3_bs->Stall(1000000);
}

/* ------------------------------------------------------------------ 选项 */

/* 找 " key=" 形式的 token，值拷进 out（到空白为止）；没有返回 false */
static bool opt_get(const char *s, const char *key, char *out, size_t cap)
{
    size_t kl = gk3_strlen(key), sl = gk3_strlen(s);
    for (size_t i = 0; i + kl <= sl; i++) {
        if ((i == 0 || s[i - 1] == ' ') && !gk3_memcmp(s + i, key, kl)) {
            size_t n = 0;
            for (const char *q = s + i + kl; *q && *q != ' ' && n + 1 < cap; q++)
                out[n++] = *q;
            out[n] = 0;
            return true;
        }
    }
    return false;
}

static int slot_letter(const char *v)
{
    if ((v[0] == 'a' || v[0] == 'b') && !v[1])
        return v[0] - 'a';
    if (v[0] == '_' && (v[1] == 'a' || v[1] == 'b') && !v[2])
        return v[1] - 'a';
    return -1;
}

/* 0/1 开关；不是 0/1 → -1 */
static int opt_bool(const char *v)
{
    return (v[0] == '0' || v[0] == '1') && !v[1] ? v[0] - '0' : -1;
}

/* systemd-boot 设的 LoaderBootCountPath（带计数的条目才有，"\loader\entries\x+2-1.conf"）取文件名；
 * 没有就用 LoaderEntrySelected（条目 id，小写，boot.c:1540-1541、:2697）。值不合规就是 NULL。 */
static const char *entry_name(void)
{
    static char out[128];
    static uint8_t buf[512];
    UINTN sz = sizeof(buf) - 2;
    char a[256];
    const char *base;
    gk3_memset(buf, 0, sizeof(buf));
    if (EFI_ERROR(gk3_getvar(u"LoaderBootCountPath", &gk3_guid_loader, NULL, buf, &sz))) {
        sz = sizeof(buf) - 2;
        gk3_memset(buf, 0, sizeof(buf));
        if (EFI_ERROR(gk3_getvar(u"LoaderEntrySelected", &gk3_guid_loader, NULL, buf, &sz)))
            return NULL;
    }
    gk3_ucs2_to_ascii((const CHAR16 *)buf, sz / 2, a, sizeof(a));
    base = a;
    for (const char *q = a; *q; q++)
        if (*q == '\\' || *q == '/')
            base = q + 1;
    size_t n = 0;
    for (; base[n] && n + 1 < sizeof(out); n++) {
        char c = base[n];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
              c == '+' || c == '-'))
            return NULL;
        out[n] = c;
    }
    out[n] = 0;
    return n ? out : NULL;
}

/* 条目名里的 "-android-a." / "-android-a+" → 0（gk3boot-android-a+3.conf、<mid>-android-a.conf 都认）；没有 → -1 */
static int slot_from_entry(const char *e)
{
    if (!e)
        return -1;
    for (const char *p = e; *p; p++)
        if (!gk3_memcmp(p, "-android-", 9) && (p[9] == 'a' || p[9] == 'b') && (p[10] == '.' || p[10] == '+'))
            return p[9] - 'a';
    return -1;
}

static void step_header(void)
{
    static char opts[1024];
    char v[40];
    EFI_STATUS st;
    int b, eb;

    gk3_logf("gk3boot %s  (S5: observe + action modes; docs/boot-entry-design.md E5)\n", GK3BOOT_VERSION);
    st = gk3_bs->SetWatchdogTimer(WATCHDOG_SEC, WATCHDOG_CODE, 0, NULL);
    gk3_logf("watchdog: %u s %s\n", WATCHDOG_SEC, EFI_ERROR(st) ? gk3_efi_strerror(st) : "armed");

    st = gk3_bs->HandleProtocol(gk3_image, (EFI_GUID *)&gk3_guid_loaded_image, (void **)&self_li);
    if (EFI_ERROR(st) || !self_li) {
        self_li = NULL;
        fail_open("self", "LoadedImage(self): %s", gk3_efi_strerror(st));
    }
    opts[0] = 0;
    if (self_li->LoadOptions && self_li->LoadOptionsSize >= 2)
        gk3_ucs2_to_ascii(self_li->LoadOptions, self_li->LoadOptionsSize / 2, opts, sizeof(opts));
    gk3_logf("load_options: \"%s\"\n", opts);
    g_entry = entry_name();

    /* hint 先定（选项解析失败的 fail-open 也要知道往哪个槽回落）：gk3.hint > 条目名里的 -android-<x> > a */
    eb = slot_from_entry(g_entry);
    g_target = eb >= 0 ? (unsigned)eb : 0;
    if (opt_get(opts, "gk3.hint=", v, sizeof(v)) && (opt.hint = slot_letter(v)) < 0)
        fail_open("options", "gk3.hint=%s is not a|b", v);
    g_hint = opt.hint >= 0 ? (unsigned)opt.hint : g_target;
    g_target = g_hint;

    if (opt_get(opts, "gk3.observe=", v, sizeof(v))) {
        if ((b = opt_bool(v)) < 0)
            fail_open("options", "gk3.observe=%s is not 0|1", v);
        opt.observe = b;
    }
    if (opt_get(opts, "gk3.dispatch=", v, sizeof(v))) {
        if ((b = opt_bool(v)) < 0)
            fail_open("options", "gk3.dispatch=%s is not 0|1", v);
        opt.dispatch = b;
    }
    if (opt_get(opts, "gk3.slot=", v, sizeof(v)) && (opt.force_slot = slot_letter(v)) < 0)
        fail_open("options", "gk3.slot=%s is not a|b", v);
    if (opt_get(opts, "gk3.mid=", v, sizeof(v))) {
        bool okm = gk3_strlen(v) == 32;
        for (size_t i = 0; okm && i < 32; i++)
            okm = is_hex(v[i]);
        if (!okm)
            fail_open("options", "gk3.mid=%s is not 32 lowercase hex digits", v);
        gk3_memcpy(opt.mid, v, 33);
    }
    if (opt_get(opts, "gk3.action=", v, sizeof(v))) {
        if (str_cmp(v, "fastboot") && str_cmp(v, "menu"))
            fail_open("options", "gk3.action=%s is not fastboot|menu", v);
        gk3_memcpy(opt.action, v, gk3_strlen(v) + 1);
    }
    if (opt_get(opts, "gk3.fbtcp=", v, sizeof(v))) {
        if ((b = opt_bool(v)) < 0)
            fail_open("options", "gk3.fbtcp=%s is not 0|1", v);
        opt.fbtcp = b;
    }
    if (opt_get(opts, "gk3.hold=", v, sizeof(v))) {
        unsigned h = 0;
        for (const char *q = v; *q >= '0' && *q <= '9' && h < 1000; q++)
            h = h * 10 + (unsigned)(*q - '0');
        opt.hold = h > 30 ? 30 : h;
    }
    gk3_logf("mode: %s dispatch=%s force_slot=%c hint=_%c%s hold=%u s entry=%s\n", opt.observe ? "observe" : "action",
             opt.dispatch ? "on" : "off", opt.force_slot < 0 ? '-' : 'a' + opt.force_slot, 'a' + g_hint,
             opt.hint >= 0 ? "" : eb >= 0 ? " (from entry name)" : " (default)", opt.hold, g_entry ? g_entry : "-");
    if (opt.action[0] || opt.fbtcp)
        gk3_logf("executor options: action=%s fbtcp=%u\n", opt.action[0] ? opt.action : "-", opt.fbtcp);
    if (opt.observe) {
        /* 观察模式：上屏幕（把到这里为止的几行补打出来）、每次都留日志 */
        gk3_lg.screen = true;
        gk3_screenf("\n%s", gk3_lg.buf ? gk3_lg.buf : "");   /* 开头的换行：systemd-boot 可能把光标留在行中间 */
        log_ensure();
    } else {
        /* 动作模式：正常路径不上屏幕、不写 ESP（§4.3.1、§4.12）；日志先只在内存里，出异常时才落盘 */
        if (opt.force_slot >= 0)
            notable("gk3.slot=%c forces the slot in action mode: no tries are written for it", 'a' + opt.force_slot);
    }
}

/* ------------------------------------------------------------------ 1. 本盘 + GPT（同探针 step_disk，设计稿 §4.2 第 1 步） */

static struct {
    EFI_BLOCK_IO_PROTOCOL *bio;
    gk3_bio_ctx ctx;
    gk3_blk dev;
    gk3_gpt gpt;
    uint8_t *entries;
    char esp_uuid[37];         /* 自己 ESP 的 PARTUUID（HD 节点），执行端 cmdline 的 gk3.esp */
} disk;

static void step_disk(void)
{
    static char dp[1024];
    UINTN n = 0, matches = 0;
    EFI_HANDLE *hs = NULL;
    uint64_t t = gk3_ticks();

    EFI_DEVICE_PATH_PROTOCOL *esp = gk3_dp_of(self_li->DeviceHandle);
    const EFI_DEVICE_PATH_PROTOCOL *hd = gk3_dp_find_hd(esp);
    if (!esp || !hd)
        fail_open("disk", "own device path has no HD node");
    size_t prefix = (size_t)((const uint8_t *)hd - (const uint8_t *)esp);
    gk3_dp_text(esp, dp, sizeof(dp));
    gk3_logd("esp_dp: %s\n", dp);

    EFI_STATUS st = gk3_handles(&gk3_guid_block_io, &n, &hs);
    if (EFI_ERROR(st))
        fail_open("disk", "LocateHandleBuffer(BlockIo): %s", gk3_efi_strerror(st));
    for (UINTN i = 0; i < n; i++) {
        EFI_BLOCK_IO_PROTOCOL *b = NULL;
        if (EFI_ERROR(gk3_bs->HandleProtocol(hs[i], (EFI_GUID *)&gk3_guid_block_io, (void **)&b)) || !b || !b->Media)
            continue;
        EFI_DEVICE_PATH_PROTOCOL *d = gk3_dp_of(hs[i]);
        /* 整盘 = 自己 ESP 的设备路径去掉 HD 节点（前缀 + 结束节点） */
        if (!b->Media->LogicalPartition && d && gk3_dp_size(d) == prefix + 4 && !gk3_memcmp(d, esp, prefix)) {
            matches++;
            if (!disk.bio) {
                disk.bio = b;
                gk3_dp_text(d, dp, sizeof(dp));
                gk3_logd("whole_disk: %s bs=%u last=%llu ro=%u\n", dp, b->Media->BlockSize,
                         (unsigned long long)b->Media->LastBlock, b->Media->ReadOnly);
            }
        }
    }
    gk3_free(hs);
    /* §4.2 第 1 步：本盘找不到（或不唯一）时设计稿要扫所有整盘；这一版直接 fail-open */
    if (matches != 1)
        fail_open("disk", "whole-disk candidates for own ESP: %llu (expected 1)", (unsigned long long)matches);
    if (disk.bio->Media->BlockSize < 512 || disk.bio->Media->BlockSize > 4096 || disk.bio->Media->IoAlign > 4096)
        fail_open("disk", "unsupported block size %u / io_align %u", disk.bio->Media->BlockSize,
                  disk.bio->Media->IoAlign);
    /* 观察模式只读（write / flush 都是 NULL）；动作模式才给写回调，且只写 misc 的两处（step_misc） */
    if (opt.observe)
        gk3_blk_from_bio(&disk.dev, &disk.ctx, disk.bio);
    else
        gk3_blk_from_bio_rw(&disk.dev, &disk.ctx, disk.bio);

    uint32_t bs = disk.dev.block_size;
    uint8_t *hdr = gk3_alloc_pages(bs);
    gk3_gpt g0;
    if (!hdr)
        fail_open("disk", "alloc");
    if (disk.dev.read(disk.dev.ctx, 1, 1, hdr))
        fail_open("disk", "read LBA1: %s", gk3_efi_strerror(disk.ctx.last_err));
    gk3_err e = gk3_gpt_parse_header(hdr, bs, &g0);
    if (e)
        fail_open("disk", "GPT header: %s", gk3_strerror(e));
    size_t elen = ((size_t)g0.num_entries * g0.entry_size + bs - 1) / bs * bs;
    if (!(disk.entries = gk3_alloc_pages(elen)))
        fail_open("disk", "alloc entries %llu", (unsigned long long)elen);
    e = gk3_gpt_read(&disk.dev, &disk.gpt, hdr, disk.entries, elen);
    if (e)
        fail_open("disk", "GPT: %s (efi %s)", gk3_strerror(e), gk3_efi_strerror(disk.ctx.last_err));

    /* 入口硬要求的五个名字各恰好一个（同探针 unique_required；metadata 只记录） */
    static const char *const req[] = {"misc", "boot_a", "boot_b", "super", "userdata"};
    const char *bad = NULL;
    e = gk3_gpt_require_unique(&disk.gpt, req, sizeof(req) / sizeof(req[0]), &bad);
    if (e)
        fail_open("disk", "GPT name %s: %s", bad ? bad : "?", gk3_strerror(e));
    gk3_gpt_part mp;
    gk3_logd("gpt: metadata %s\n", gk3_gpt_find(&disk.gpt, "metadata", &mp) ? "not unique/absent" : "unique");
    /* 自己的 ESP 在这张表里对得上吗（HD 节点的分区号与 PARTUUID）—— 对不上说明设备路径前缀骗了我们 */
    const uint8_t *h = (const uint8_t *)hd;
    gk3_gpt_part ep;
    if (h[41] != 2 || gk3_gpt_get(&disk.gpt, gk3_le32(h + 4) - 1, &ep) || gk3_memcmp(ep.part_guid, h + 24, 16))
        fail_open("disk", "own ESP (partition %u) not found in this GPT", gk3_le32(h + 4));
    gk3_guid_str(ep.part_guid, disk.esp_uuid);
    char g[37];
    gk3_guid_str(disk.gpt.disk_guid, g);
    gk3_logf("disk: gpt %s, %u partitions, misc/boot_a/boot_b/super/userdata unique, esp=p%u  [%llu.%llu ms]\n", g,
             gk3_gpt_count(&disk.gpt), ep.index, MSF(gk3_us_since(t)));
    gk3_free_pages(hdr, bs);
}

static void part(const char *name, gk3_gpt_part *p)
{
    gk3_err e = gk3_gpt_find(&disk.gpt, name, p);
    if (e)
        fail_open("disk", "%s: %s", name, gk3_strerror(e));
}

/* ------------------------------------------------------------------ 2. misc：解码、选槽；动作模式下扣 tries、写 GK3 记录、BCB 分派 */

static const char *const selk[] = {"boot", "bcab_invalid", "noslot", "merging"};

/* §4.3.4：分派打开、执行端也在时会怎么处理这份 BCB（观察模式与分派关时只记录） */
static const char *bcb_would(gk3_bcb_kind k)
{
    switch (k) {
    case GK3_BCB_NONE: return "normal boot";
    case GK3_BCB_BOOTLOADER: return "clear BCB, then executor why=bootloader";
    case GK3_BCB_FASTBOOT: return "clear BCB, then executor why=fastboot";
    case GK3_BCB_WIPE: return "executor why=wipe (BCB cleared by executor after wiping; 3-entry cap)";
    case GK3_BCB_PROMPT_WIPE: return "executor why=prompt_wipe (never auto-cleared)";
    case GK3_BCB_RECOVERY: return "clear BCB, then executor recovery menu why=recovery";
    case GK3_BCB_UNKNOWN: return "record in GK3, clear, boot normally";
    }
    return "?";
}

static unsigned g_slot;
static const char *g_event = "none";
static char g_streak[4];               /* 动作模式写进 GK3 的 boot_streak（cmdline 用）；空 = 不报 */

static struct {
    gk3_gpt_part p;                    /* misc 分区 */
    uint64_t blocks;
    uint8_t *scratch;                  /* 两块，gk3_blk_write_bytes_verify 用 */
} misc;

/* 写 misc 分区内 [off, off+len)，写后逐块读回（gk3_blk_write_bytes_verify），再按字节读一遍与 data 比对 */
static gk3_err misc_write(uint32_t off, const uint8_t *data, size_t len, uint8_t *back)
{
    uint32_t bs = disk.dev.block_size;
    gk3_err e = gk3_blk_write_bytes_verify(&disk.dev, misc.p.first_lba, misc.blocks, off, data, len, misc.scratch,
                                           2 * bs);
    if (e)
        return e;
    e = gk3_blk_read_bytes(&disk.dev, misc.p.first_lba, misc.blocks, off, back, len, misc.scratch, 2 * bs);
    if (e)
        return e;
    return gk3_memcmp(back, data, len) ? GK3_EVERIFY : GK3_OK;
}

/* 整份 BCB 清零写回（= clear_bootloader_message） */
static gk3_err bcb_clear_write(void)
{
    static uint8_t z[GK3_MISC_BCB_SIZE], back[GK3_MISC_BCB_SIZE];
    gk3_memset(z, 0, sizeof(z));
    return misc_write(GK3_MISC_BCB_OFF, z, sizeof(z), back);
}

/* ------------------------------------------------------------------ 3. boot_<x>：整份读进来、校验、拆段 */

static struct {
    uint8_t *img;
    size_t img_len;
    gk3_bootimg b;
    uint8_t hdr[4096];
    unsigned slot;
} boot;

/* 读 boot_<slot> 整份、校验 SHA1(id)、内核是 PE、有 ramdisk、dtb 是 FDT。成功返回 NULL；失败返回原因（静态缓冲），
 * 已分配的镜像缓冲释放掉。Android 与执行端共用（§4.4.1：执行端用同一个 boot_x 里的内核）。 */
static const char *load_boot(unsigned slot)
{
    static char why[300];
    gk3_gpt_part p;
    char name[8] = "boot_a", hx[48], want[48];
    name[5] = (char)('a' + slot);
    if (boot.img) {
        gk3_free_pages(boot.img, boot.img_len);
        boot.img = NULL;
    }
    gk3_err e = gk3_gpt_find(&disk.gpt, name, &p);
    if (e) {
        gk3_snprintf(why, sizeof(why), "%s: %s", name, gk3_strerror(e));
        return why;
    }
    uint32_t bs = disk.dev.block_size;
    uint64_t pbytes = (p.last_lba - p.first_lba + 1) * bs;
    uint8_t *h = gk3_alloc_pages(4096);
    if (!h)
        return "alloc";
    if (disk.dev.read(disk.dev.ctx, p.first_lba, 4096 / bs, h)) {
        gk3_free_pages(h, 4096);
        gk3_snprintf(why, sizeof(why), "read %s header: %s", name, gk3_efi_strerror(disk.ctx.last_err));
        return why;
    }
    gk3_memcpy(boot.hdr, h, 4096);
    gk3_free_pages(h, 4096);
    e = gk3_bootimg_parse(boot.hdr, sizeof(boot.hdr), pbytes, &boot.b);
    if (e) {
        gk3_snprintf(why, sizeof(why), "%s header: %s (first bytes %02x %02x %02x %02x)", name, gk3_strerror(e),
                     boot.hdr[0], boot.hdr[1], boot.hdr[2], boot.hdr[3]);
        return why;
    }
    gk3_bootimg *b = &boot.b;
    if (b->version != 2) {
        gk3_snprintf(why, sizeof(why), "%s: header v%u, this build expects v2 (kernel+ramdisk+dtb in one image, §2.2)",
                     name, b->version);
        return why;
    }
    if (b->total_size > BOOTIMG_MAX) {
        gk3_snprintf(why, sizeof(why), "%s: total %llu bytes is implausible", name, (unsigned long long)b->total_size);
        return why;
    }
    hex_str(b->id, 20, want, sizeof(want));
    gk3_logf("%s: p%u v%u page=%u kernel=%u ramdisk=%u dtb=%u total=%llu id=%s\n", name, p.index, b->version,
             b->page_size, b->kernel_size, b->ramdisk_size, b->dtb_size, (unsigned long long)b->total_size, want);

    boot.img_len = (size_t)((b->total_size + bs - 1) / bs * bs);
    if (!(boot.img = gk3_alloc_pages(boot.img_len))) {
        gk3_snprintf(why, sizeof(why), "alloc %llu", (unsigned long long)boot.img_len);
        return why;
    }
    uint64_t t = gk3_ticks();
    if (disk.dev.read(disk.dev.ctx, p.first_lba, (uint32_t)(boot.img_len / bs), boot.img)) {
        gk3_snprintf(why, sizeof(why), "read %s (%llu bytes): %s", name, (unsigned long long)boot.img_len,
                     gk3_efi_strerror(disk.ctx.last_err));
        goto bad;
    }
    uint64_t us_r = gk3_us_since(t);
    uint8_t got[20];
    t = gk3_ticks();
    e = gk3_bootimg_verify_id(b, boot.img, boot.img_len, got);
    uint64_t us_s = gk3_us_since(t);
    hex_str(got, 20, hx, sizeof(hx));
    /* §4.3.3：SHA1 不对时完整版会换另一个可启动的槽（不写 misc）、两个都坏走 H1；这一版 Android 路径直接 fail-open
     * —— 直连条目启动的是 ESP 上那份内核，效果上就是 H1 */
    if (e) {
        gk3_snprintf(why, sizeof(why), "%s: SHA1(id) MISMATCH: header %s, computed %s", name, want, hx);
        goto bad;
    }
    gk3_logf("%s: read %llu.%llu ms, sha1(id) %llu.%llu ms: OK\n", name, MSF(us_r), MSF(us_s));

    const uint8_t *k = boot.img + b->kernel_off, *d = boot.img + b->dtb_off;
    if (k[0] != 'M' || k[1] != 'Z') {
        gk3_snprintf(why, sizeof(why), "%s: kernel is not a PE image (%02x %02x)", name, k[0], k[1]);
        goto bad;
    }
    gk3_logf("%s: kernel %s\n", name, !gk3_memcmp(k + 4, "zimg", 4) ? "EFI zboot PE" : "PE (EFI stub)");
    if (!b->ramdisk_size) {
        gk3_snprintf(why, sizeof(why), "%s: no ramdisk", name);
        goto bad;
    }
    /* FDT 头：magic 0xd00dfeed、totalsize（大端）不超过段长 */
    uint32_t fdt_magic = (uint32_t)d[0] << 24 | (uint32_t)d[1] << 16 | (uint32_t)d[2] << 8 | d[3];
    uint32_t fdt_size = (uint32_t)d[4] << 24 | (uint32_t)d[5] << 16 | (uint32_t)d[6] << 8 | d[7];
    if (b->dtb_size < 40 || fdt_magic != 0xd00dfeedu || fdt_size > b->dtb_size || fdt_size < 40) {
        gk3_snprintf(why, sizeof(why), "%s: dtb is not a valid FDT (magic %08x size %u / %u)", name, fdt_magic,
                     fdt_size, b->dtb_size);
        goto bad;
    }
    boot.slot = slot;
    return NULL;
bad:
    gk3_free_pages(boot.img, boot.img_len);
    boot.img = NULL;
    return why;
}

/* ------------------------------------------------------------------ 执行端（S7c，§4.4.1） */

#define FASTBOOT_IMG_MAX (16u * 1024 * 1024)   /* 预算 4 MiB（build.sh 断言）；读到离谱的大小就不用 */

static struct {
    const char *why;           /* 非 NULL = 这次进执行端 */
    void *img;                 /* fastboot.img（initrd） */
    size_t len;
    CHAR16 *cmdline16;
} ex;

/* 准备执行端：读本目录的 fastboot.img、读同一个 boot_x 的内核（slot 不行就另一槽）、拼 cmdline。
 * 只读，不写盘。成功返回 NULL；失败返回原因（调用方照常启动 Android / fail-open）。 */
static const char *prepare_executor(const char *why, unsigned slot)
{
    static char err[400], base[GK3_BOOT_ARGS_SIZE + GK3_BOOT_EXTRA_ARGS_SIZE + 8], out[4096], mid[37];
    static CHAR16 path[160];
    char pa[200];
    const char *e1;
    EFI_STATUS st;

    if (!self_li || !gk3_image_dir(self_li->FilePath, path, sizeof(path) / sizeof(path[0]) - 16))
        return "cannot tell this loader's own directory (LoadedImage FilePath)";
    {
        static const CHAR16 fn[] = u"fastboot.img";
        size_t n = gk3_strlen16(path);
        for (size_t i = 0; i < sizeof(fn) / sizeof(fn[0]); i++)
            path[n + i] = fn[i];
    }
    gk3_ucs2_to_ascii(path, sizeof(path) / sizeof(path[0]), pa, sizeof(pa));
    uint64_t t = gk3_ticks();
    st = gk3_file_read(self_li->DeviceHandle, path, &ex.img, &ex.len, FASTBOOT_IMG_MAX);
    if (EFI_ERROR(st)) {
        gk3_snprintf(err, sizeof(err), "%s: %s", pa, st == EFI_NOT_FOUND ? "not on the ESP (no executor in this "
                     "deployment)" : gk3_efi_strerror(st));
        return err;
    }
    const uint8_t *z = ex.img;
    if (ex.len < 18 || z[0] != 0x1f || z[1] != 0x8b) {
        gk3_snprintf(err, sizeof(err), "%s: not a gzip cpio (%02x %02x, %llu bytes)", pa, z[0], z[1],
                     (unsigned long long)ex.len);
        goto bad;
    }
    gk3_logf("executor: %s %llu bytes [%llu.%llu ms]\n", pa, (unsigned long long)ex.len, MSF(gk3_us_since(t)));
    /* 内核：先 slot（Android 这次本来要启动的 / active 槽），不行就另一槽（§4.4.1 的三级回落，ESP 副本那一级没做） */
    if ((e1 = load_boot(slot)) != NULL) {
        const char *e2;
        gk3_logf("executor: kernel from boot_%c failed (%s); trying boot_%c\n", 'a' + slot, e1, 'a' + (slot ^ 1));
        if ((e2 = load_boot(slot ^ 1)) != NULL) {
            gk3_snprintf(err, sizeof(err), "no usable kernel: boot_%c: %s / boot_%c: %s", 'a' + slot, e1,
                         'a' + (slot ^ 1), e2);
            goto bad;
        }
    }
    long bn = gk3_bootimg_cmdline(&boot.b, base, sizeof(base));
    if (bn < 0) {
        gk3_snprintf(err, sizeof(err), "boot_%c cmdline does not fit", 'a' + boot.slot);
        goto bad;
    }
    gk3_guid_str(misc.p.part_guid, mid);
    gk3_fastboot_args fa = {why, slot, GK3BOOT_VERSION, mid, disk.esp_uuid[0] ? disk.esp_uuid : NULL, opt.dispatch,
                            opt.fbtcp};
    gk3_err ge = gk3_cmdline_fastboot(base, &fa, out, sizeof(out));
    if (ge) {
        gk3_snprintf(err, sizeof(err), "gk3_cmdline_fastboot: %s", gk3_strerror(ge));
        goto bad;
    }
    size_t n = gk3_strlen(out);
    if (!(ex.cmdline16 = gk3_alloc((n + 1) * sizeof(CHAR16))) ||
        gk3_ascii_to_ucs2(out, (uint16_t *)ex.cmdline16, n + 1)) {
        gk3_snprintf(err, sizeof(err), "cmdline alloc / ucs2");
        goto bad;
    }
    gk3_logf("executor: kernel boot_%c, cmdline(%llu): %s\n", 'a' + boot.slot, (unsigned long long)n, out);
    ex.why = why;
    return NULL;
bad:
    if (ex.img)
        gk3_free_pages(ex.img, ex.len);
    ex.img = NULL;
    ex.len = 0;
    return err;
}

/* ------------------------------------------------------------------ 动作模式：GK3 记录 + 分派 */

static uint8_t g_rec[GK3_REC_SIZE];

static void rec_prepare(const uint8_t *m)
{
    const uint8_t *old = m + GK3_MISC_GK3_OFF;
    gk3_err re = gk3_rec_validate(old);
    if (re) {
        /* 不是迁移（迁移要分派开关开着才做，§4.10）：只建一份空记录，不置迁移标记 */
        gk3_rec_init(g_rec);
        gk3_logf("gk3rec: new v1 record (was: %s%s); migration marker NOT set here\n", gk3_strerror(re),
                 gk3_is_zero(old, GK3_REC_SIZE) ? ", all zero" : ", NOT all zero");
        if (!gk3_is_zero(old, GK3_REC_SIZE))
            notable("gk3rec: 8 KiB area held non-zero data that is not a valid GK3 record (%s); overwriting",
                    gk3_strerror(re));
    } else {
        gk3_memcpy(g_rec, old, GK3_REC_SIZE);
    }
}

static gk3_err rec_write(const char *what)
{
    static uint8_t back[GK3_REC_SIZE];
    gk3_rec_seal(g_rec);
    gk3_err e = misc_write(GK3_MISC_GK3_OFF, g_rec, GK3_REC_SIZE, back);
    if (e)
        anomaly("gk3rec: write misc+0x2000 failed: %s (efi %s); continuing (the record is advisory)", gk3_strerror(e),
                gk3_efi_strerror(disk.ctx.last_err));
    else
        gk3_logf("gk3rec: written (%s), boot_streak=%u ok_streak=%u flags=0x%x (read back OK)\n", what,
                 gk3_rec_boot_streak(g_rec), gk3_rec_ok_streak(g_rec), gk3_rec_flags(g_rec));
    return e;
}

/* ------------------------------------------------------------------ 双系统（S15，设计稿 §4.9.3 / §4.9.4 / §4.4.4）
 *
 * 规则本身在 libgk3core（core/src/dual.c，主机单测 test/test_dual.c）；这里只做 UEFI 那一半：读 / 写 / 删 systemd-boot 的
 * LoaderEntryDefault 与 LoaderEntryOneShot、看 \EFI\Microsoft\Boot\bootmgfw.efi 在不在、复位。
 *   - 只在动作模式下写（观察模式只打 "would"，§4.12）；纯 Android（变量不存在、记录里没有双系统意图）时
 *     只多一次 GetVariable(LoaderEntryDefault)，不多写任何东西。
 *   - 去 Windows 的那一次：先把清掉标记的 GK3 记录写回 misc（读回），再写 OneShot=Windows 条目，冷复位；
 *     不扣 tries、不加 boot_streak、不预置。任何一步失败都改为照常启动 Android（不会循环：标记要么已清，要么还在、下次再试一次）。
 *     复位用 EfiResetCold，不是设计稿写的 Warm：Cold 是 fail-open 已在本机固件上验过的那条路（E6），NV 变量在复位前已落盘。
 *   - 正常路径不写 ESP 日志（每次切 Windows 都留一份会慢慢吃掉共用 ESP，U17）；只有写失败、不合法的默认值才算异常。 */

static const CHAR16 kBootmgfw[] = u"\\EFI\\Microsoft\\Boot\\bootmgfw.efi";
#define VAR_ATTR (EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS)

static struct {
    int win;                   /* -1 = 还没看；0 / 1 = ESP 上有没有 bootmgfw.efi */
    gk3_defvar var;            /* LoaderEntryDefault 进来时的样子 */
    char value[96];            /* 它的原值（ASCII，非 ASCII 记成 '?'） */
    bool default_windows;      /* 处理完 set_default / 不合法值之后：默认是不是 Windows */
    bool preset;               /* 交接前预置 LoaderEntryOneShot = *-android-<x>.conf */
    bool menu;                 /* next=sdboot-menu：已清掉，return EFI_SUCCESS 回 systemd-boot 菜单 */
} dual = {-1, GK3_DEFVAR_ABSENT, "", false, false, false};

static bool win_present(void)
{
    if (dual.win < 0) {
        EFI_STATUS st = self_li ? gk3_file_exists(self_li->DeviceHandle, kBootmgfw) : EFI_NOT_FOUND;
        dual.win = !EFI_ERROR(st);
        gk3_logf("dual: \\EFI\\Microsoft\\Boot\\bootmgfw.efi %s\n", dual.win ? "present" : gk3_efi_strerror(st));
    }
    return dual.win;
}

/* Windows 条目的 id：安装器自写的 type1 条目 gk3-windows.conf（U22，id = 文件名，boot.c:1540-1541）优先，
 * 否则 systemd-boot 自动生成的 auto-windows（boot.c:2146-2148） */
static const char *win_entry_id(void)
{
    return self_li && !EFI_ERROR(gk3_file_exists(self_li->DeviceHandle, u"\\loader\\entries\\gk3-windows.conf"))
               ? "gk3-windows.conf"
               : "auto-windows";
}

/* systemd-boot 厂商 GUID 下的字符串变量：val 非 NULL = 写（属性 0x07、UTF-16LE + NUL，同 scripts/boot-oneshot.sh），
 * NULL = 删（size 0；本来就没有算成功）。写 / 删之后都读回核对。 */
static EFI_STATUS loader_var_set(const CHAR16 *name, const char *val)
{
    static CHAR16 v[96];
    static uint8_t back[200];
    UINTN bsz = sizeof(back);
    UINT32 battr = 0;
    EFI_STATUS st;
    if (!val) {
        st = gk3_setvar(name, &gk3_guid_loader, VAR_ATTR, NULL, 0);
        if (EFI_ERROR(st) && st != EFI_NOT_FOUND)
            return st;
        st = gk3_getvar(name, &gk3_guid_loader, NULL, back, &bsz);
        return st == EFI_NOT_FOUND ? EFI_SUCCESS : EFI_ERROR(st) && st != EFI_BUFFER_TOO_SMALL ? st : EFI_DEVICE_ERROR;
    }
    size_t n = gk3_strlen(val);
    if (n + 1 > sizeof(v) / sizeof(v[0]))
        return EFI_BAD_BUFFER_SIZE;
    for (size_t i = 0; i <= n; i++)
        v[i] = (CHAR16)(unsigned char)val[i];
    UINTN size = (n + 1) * sizeof(CHAR16);
    st = gk3_setvar(name, &gk3_guid_loader, VAR_ATTR, v, size);
    if (EFI_ERROR(st))
        return st;
    st = gk3_getvar(name, &gk3_guid_loader, &battr, back, &bsz);
    if (EFI_ERROR(st))
        return st;
    return bsz == size && !gk3_memcmp(back, v, size) && battr == VAR_ATTR ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

static void read_default_var(void)
{
    static uint8_t buf[200];
    UINTN sz = sizeof(buf) - 2;
    gk3_memset(buf, 0, sizeof(buf));
    EFI_STATUS st = gk3_getvar(u"LoaderEntryDefault", &gk3_guid_loader, NULL, buf, &sz);
    dual.value[0] = 0;
    if (st == EFI_NOT_FOUND) {
        dual.var = GK3_DEFVAR_ABSENT;
        return;
    }
    if (st == EFI_BUFFER_TOO_SMALL) {      /* 存在、但长得不像任何条目 id */
        gk3_memcpy(dual.value, "(too long)", 11);
        dual.var = GK3_DEFVAR_OTHER;
        return;
    }
    if (EFI_ERROR(st)) {                   /* 读不出：什么都不做（不去删一个看不见的东西） */
        gk3_logf("dual: GetVariable(LoaderEntryDefault): %s; treated as absent\n", gk3_efi_strerror(st));
        dual.var = GK3_DEFVAR_ABSENT;
        return;
    }
    gk3_ucs2_to_ascii((const CHAR16 *)buf, sz / 2, dual.value, sizeof(dual.value));
    dual.var = gk3_defvar_classify(dual.value);
}

static const char *const defvar_name[] = {"absent", "windows-entry", "NOT-LEGAL"};
static const char *const setdef_name[] = {"none", "windows", "android"};

/* §4.9.3 a、b（动作模式，g_rec 已备好）：删不合法的 LoaderEntryDefault、应用 set_default 请求、更新默认系统缓存 */
static void dual_defaults(void)
{
    gk3_def_plan p;
    gk3_setdef req = gk3_rec_set_default_req(g_rec);
    read_default_var();
    bool win = (dual.var == GK3_DEFVAR_WINDOWS || req == GK3_SETDEF_WINDOWS) ? win_present() : false;
    gk3_dual_plan_default(g_rec, dual.var, win, &p);
    dual.default_windows = p.default_windows;
    if (dual.var != GK3_DEFVAR_ABSENT || req != GK3_SETDEF_NONE)
        gk3_logf("dual: LoaderEntryDefault=\"%s\" (%s) set_default=%s -> %s; default %s\n", dual.value,
                 defvar_name[dual.var], setdef_name[req],
                 p.action == GK3_DEFACT_DELETE ? "delete" : p.action == GK3_DEFACT_SET_WINDOWS ? "set Windows" : "keep",
                 p.default_windows ? "Windows" : "Android");
    EFI_STATUS st = EFI_SUCCESS;
    const char *id = NULL;
    if (p.action == GK3_DEFACT_DELETE)
        st = loader_var_set(u"LoaderEntryDefault", NULL);
    else if (p.action == GK3_DEFACT_SET_WINDOWS)
        st = loader_var_set(u"LoaderEntryDefault", id = win_entry_id());
    if (EFI_ERROR(st)) {
        dual.default_windows = p.default_windows_before;
        if (p.applied != GK3_SETDEF_NONE)
            gk3_rec_event_add(g_rec, GK3_EV_INTENT_DROPPED, GK3_DROP_VAR_FAILED, GK3_INTENT_SET_DEFAULT);
        anomaly("dual: %s LoaderEntryDefault failed: %s; the default stays %s", id ? "writing" : "deleting",
                gk3_efi_strerror(st), dual.default_windows ? "Windows" : "Android");
    } else if (p.action != GK3_DEFACT_NONE) {
        gk3_logf("dual: LoaderEntryDefault %s%s (read back OK)\n", id ? "= " : "deleted", id ? id : "");
        if (p.reset)
            notable("dual: LoaderEntryDefault=\"%s\" is not a Windows entry id (§4.9.3): deleted (= default Android), "
                    "event default_reset", dual.value);
    }
    gk3_rec_put_default_os(g_rec, dual.default_windows ? GK3_OS_WINDOWS : GK3_OS_ANDROID);
}

/* §4.9.3 d：标记已在 g_rec 里清掉。先写 misc（读回），再写 OneShot = Windows 条目，冷复位。失败就返回（调用方照常启动 Android） */
static void go_windows(gk3_intent why)
{
    const char *id = win_entry_id();
    const char *what = why == GK3_INTENT_NEXT_WINDOWS ? "next=windows" : "clean_poweroff (default Windows)";
    if (rec_write("to-windows")) {
        anomaly("dual: %s: GK3 record not written, so the mark is not cleared on disk; NOT going to Windows "
                "(booting Android)", what);
        return;
    }
    EFI_STATUS st = loader_var_set(u"LoaderEntryOneShot", id);
    if (EFI_ERROR(st)) {
        gk3_rec_event_add(g_rec, GK3_EV_INTENT_DROPPED, GK3_DROP_VAR_FAILED, why);
        anomaly("dual: %s: SetVariable(LoaderEntryOneShot=%s): %s; booting Android instead", what, id,
                gk3_efi_strerror(st));
        return;
    }
    gk3_logf("dual: %s -> LoaderEntryOneShot=%s written, read back OK; tries / streak untouched; "
             "ResetSystem(EfiResetCold) (t=%llu ms)\n", what, id, MS());
    gk3_log_close(&g_log);
    gk3_rt->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);
    gk3_screenf("!! ResetSystem returned; waiting for the %u s watchdog\n", WATCHDOG_SEC);
    for (;;)
        gk3_bs->Stall(1000000);
}

/* §4.9.3 e：默认是 Windows 时，交接前预置 OneShot = *-android-<x>.conf（通配：计数用完的入口条目照常排到最后，
 * boot.c:1771-1784 只做 fnmatch）。x 取这次要启动的槽（与设计稿的 hint 只在入口计数全用完、落到直连条目时有差别，
 * 那时落到正在跑的这个槽更稳）。失败只记异常：之后 Android 里的重启会落进 Windows，不挡这次启动。 */
static void preset_oneshot(void)
{
    char v[24];
    if (!dual.preset)
        return;
    gk3_snprintf(v, sizeof(v), "*-android-%c.conf", 'a' + (g_slot & 1));
    EFI_STATUS st = loader_var_set(u"LoaderEntryOneShot", v);
    if (EFI_ERROR(st))
        anomaly("dual: default is Windows but presetting LoaderEntryOneShot=%s failed: %s (a reboot from Android "
                "will land in Windows)", v, gk3_efi_strerror(st));
    else
        gk3_logf("dual: default is Windows: LoaderEntryOneShot=%s preset (read back OK)\n", v);
}

/* 观察模式：只说会怎么做 */
static void dual_observe(const uint8_t *rec)
{
    read_default_var();
    bool win = dual.var == GK3_DEFVAR_WINDOWS ? win_present() : false;
    bool valid = !gk3_rec_validate(rec);
    gk3_next_kind nk = valid ? gk3_rec_next(rec, NULL) : GK3_NEXT_NONE;
    bool cp = valid && gk3_rec_clean_poweroff(rec);
    gk3_setdef req = valid ? gk3_rec_set_default_req(rec) : GK3_SETDEF_NONE;
    if (dual.var == GK3_DEFVAR_ABSENT && nk != GK3_NEXT_WINDOWS && nk != GK3_NEXT_SDBOOT_MENU && !cp && !req)
        return;
    gk3_logf("would (action): dual: LoaderEntryDefault=\"%s\" (%s%s) set_default=%s next=%u clean_poweroff=%u "
             "(nothing written in observe mode; no OneShot preset)\n", dual.value, defvar_name[dual.var],
             dual.var == GK3_DEFVAR_WINDOWS ? (win ? ", bootmgfw present" : ", bootmgfw ABSENT") : "",
             setdef_name[req], nk, cp);
}

/* 启动 Android 的那一次（动作模式）：boot_streak +1、ok_streak、回落事件、分派关时的 BCB 记录。bcb_done = 这份 BCB
 * 已被分派处理（清掉了），不再按"看到但没消费"记 */
static void rec_android(const uint8_t *m, const gk3_bcb_info *bi, const gk3_sel *s, bool bcb_done)
{
    uint8_t prev = gk3_rec_boot_streak(g_rec);
    uint8_t ok = prev ? gk3_rec_ok_streak(g_rec) : 0;   /* 上一次开机完成过（HAL 清了 boot_streak）⇒ 从 0 数 */
    bool confirmed = opt.force_slot < 0 && s->kind == GK3_SEL_BOOT && !s->decremented;
    gk3_rec_set_ok_streak(g_rec, confirmed ? (uint8_t)(ok == 255 ? 255 : ok + 1) : 0);
    uint8_t streak = gk3_rec_inc_boot_streak(g_rec);
    gk3_snprintf(g_streak, sizeof(g_streak), "%u", streak);

    /* 回落：只在"进入回落"的那一次记事件、写 ESP 日志（之后 active 槽一直是那个 tries 0 的槽，每次都会算成回落） */
    bool fb = opt.force_slot < 0 && s->kind == GK3_SEL_BOOT && s->fallback;
    if (fb && !(gk3_rec_flags(g_rec) & GK3_REC_F_IN_FALLBACK)) {
        gk3_rec_event_add(g_rec, GK3_EV_FALLBACK, (uint8_t)g_slot, s->active);
        gk3_rec_set_flag(g_rec, GK3_REC_F_IN_FALLBACK, true);
        notable("fallback: active slot _%c is not bootable (tries exhausted, not marked successful) -> booting _%c; "
                "GK3 event fallback recorded", 'a' + s->active, 'a' + g_slot);
    } else if (fb) {
        gk3_logf("fallback: still on _%c (active _%c unbootable); already recorded\n", 'a' + g_slot, 'a' + s->active);
    } else {
        gk3_rec_set_flag(g_rec, GK3_REC_F_IN_FALLBACK, false);
    }

    /* BCB 看到了但没消费（分派关，或分派开但执行端不在）：同一份只记一次事件、写一次日志 */
    if (bi->kind == GK3_BCB_NONE || bcb_done) {
        gk3_rec_set_bcb_seen(g_rec, 0);
    } else {
        uint32_t crc = gk3_crc32(0, m, GK3_MISC_BCB_SIZE);
        if (!crc)
            crc = 1;                   /* 0 留给"没有" */
        if (gk3_rec_bcb_seen(g_rec) != crc) {
            gk3_rec_event_add(g_rec, GK3_EV_BCB_IGNORED, 0xff, (uint32_t)bi->kind);
            gk3_rec_set_bcb_seen(g_rec, crc);
            if (!opt.dispatch)
                notable("bcb: kind=%s command=\"%s\" present; dispatch is off (E-K7): NOT consumed, NOT cleared, "
                        "booting Android (dispatch would: %s)", gk3_bcb_kind_name(bi->kind), bi->command,
                        bcb_would(bi->kind));
            else
                notable("bcb: kind=%s command=\"%s\" NOT consumed (executor unavailable), booting Android",
                        gk3_bcb_kind_name(bi->kind), bi->command);
        } else {
            gk3_logf("bcb: same BCB as before (crc %08x), still not consumed; already recorded\n", crc);
        }
    }
    rec_write("android");
}

/* 分派（动作模式、gk3.dispatch=1）对 BCB 的"清掉、照常启动"三种：迁移 / 未知命令 / wipe 上限。
 * BCB 先清（写后读回），清成了才把相应的标记 / 事件记进 g_rec —— 尤其迁移：BCB 没清掉就置了迁移标记，
 * 下一次会把一份存量 BCB 当成新请求执行，那正是迁移要防的事（§4.10）。返回 true = 清掉了。 */
static bool dispatch_clear(const uint8_t *m, const gk3_bcb_info *bi, const gk3_disp_plan *dp)
{
    if (bi->kind != GK3_BCB_NONE) {
        gk3_err e = bcb_clear_write();
        if (e) {
            anomaly("dispatch: clearing BCB \"%s\" (%s) failed: %s (efi %s); nothing recorded, booting Android",
                    bi->command, gk3_disp_name(dp->action), gk3_strerror(e), gk3_efi_strerror(disk.ctx.last_err));
            return false;
        }
    }
    switch (dp->action) {
    case GK3_DISP_MIGRATE:
        gk3_rec_migrate(g_rec, m, GK3_DISPATCH_VER);
        notable("migration (§4.10): first run with dispatch on; existing BCB \"%s\" (%s) %s, NOT executed; "
                "marker set (dispatch_ver %u)", bi->command, gk3_bcb_kind_name(bi->kind),
                bi->kind == GK3_BCB_NONE ? "was empty" : "cleared", GK3_DISPATCH_VER);
        if (bi->kind != GK3_BCB_NONE)
            g_event = "bcb_dropped";
        break;
    case GK3_DISP_CLEAR:
        gk3_rec_event_add(g_rec, GK3_EV_BCB_DROPPED, 0xff, (uint32_t)bi->kind);
        g_event = "bcb_dropped";
        notable("dispatch: BCB \"%s\" (%s) is not a request we handle: cleared, booting Android", bi->command,
                gk3_bcb_kind_name(bi->kind));
        break;
    case GK3_DISP_WIPE_CAP:
        gk3_rec_event_add(g_rec, GK3_EV_WIPE_FAILED, 0xff, dp->count);
        gk3_rec_dispatch_reset(g_rec);
        g_event = "wipe_failed";
        notable("dispatch: the same wipe BCB entered the executor %u times without being cleared: cleared it, "
                "event wipe_failed, booting Android", dp->count - 1);
        break;
    default:
        break;
    }
    return true;
}

/* 进执行端之前的写：BCB（bootloader / fastboot / recovery 类先清）、GK3 记录（分派计数 / 事件）。
 * 不扣 tries、不加 boot_streak（执行端那一次不算 Android 的启动尝试）。 */
static void commit_executor(const gk3_bcb_info *bi, const gk3_disp_plan *dp, bool from_bcb)
{
    if (from_bcb && dp->clear_bcb_first) {
        gk3_err e = bcb_clear_write();
        if (e)
            anomaly("executor: clearing BCB \"%s\" before entering failed: %s (efi %s); the executor clears it too",
                    bi->command, gk3_strerror(e), gk3_efi_strerror(disk.ctx.last_err));
        else
            gk3_logf("executor: BCB \"%s\" (%s) cleared before entering (read back OK)\n", bi->command,
                     gk3_bcb_kind_name(bi->kind));
    }
    gk3_rec_set_bcb_seen(g_rec, 0);
    rec_write("executor");
}

/* ------------------------------------------------------------------ 2'. step_misc */

static void step_misc(void)
{
    char hx[80];
    part("misc", &misc.p);
    uint32_t bs = disk.dev.block_size;
    misc.blocks = misc.p.last_lba - misc.p.first_lba + 1;
    uint64_t pbytes = misc.blocks * bs;
    if (pbytes < GK3_MISC_READ_SIZE)
        fail_open("misc", "misc is only %llu bytes (< 64 KiB)", (unsigned long long)pbytes);
    uint8_t *m = gk3_alloc_pages(GK3_MISC_READ_SIZE);
    if (!m || !(misc.scratch = gk3_alloc_pages(2 * bs)))
        fail_open("misc", "alloc");
    uint64_t t = gk3_ticks();
    if (disk.dev.read(disk.dev.ctx, misc.p.first_lba, GK3_MISC_READ_SIZE / bs, m))
        fail_open("misc", "read misc (p%u, 64 KiB): %s", misc.p.index, gk3_efi_strerror(disk.ctx.last_err));
    gk3_logf("misc: p%u read 64 KiB [%llu.%llu ms]\n", misc.p.index, MSF(gk3_us_since(t)));

    /* BCB */
    gk3_bcb_info bi;
    gk3_bcb_classify(m, &bi);
    gk3_logf("bcb: kind=%s command=\"%s\" args=%u\n", gk3_bcb_kind_name(bi.kind), bi.command, bi.n_args);
    /* GK3 记录与首跑迁移（§4.10） */
    const uint8_t *rec = m + GK3_MISC_GK3_OFF;
    gk3_err re = gk3_rec_validate(rec);
    bool migrated = !re && gk3_rec_migrated(rec);
    if (re)
        gk3_logf("gk3rec: %s\n", gk3_strerror(re));
    else
        gk3_logf("gk3rec: valid%s boot_streak=%u ok_streak=%u flags=0x%x\n", migrated ? " migrated" : " not-migrated",
                 gk3_rec_boot_streak(rec), gk3_rec_ok_streak(rec), gk3_rec_flags(rec));
    if (opt.observe) {
        if (opt.action[0])
            gk3_logf("would (action): gk3.action=%s -> executor (NOT done)\n", opt.action);
        else if (!migrated)
            gk3_logf("would (action): first-run migration: %sset marker (NOT done)\n",
                     bi.kind == GK3_BCB_NONE ? "BCB empty, " : "clear BCB without executing it, ");
        else
            gk3_logf("would (action): BCB -> %s (NOT done)\n", bcb_would(bi.kind));
        dual_observe(rec);
    }

    /* BCAB + virtual_ab → 选槽（§4.3.2）；在副本上算，动作模式下写回的就是这份副本 */
    const uint8_t *bc = m + GK3_MISC_BCAB_OFF;
    gk3_err be = gk3_bcab_validate(bc);
    hex_str(bc, 32, hx, sizeof(hx));
    gk3_logd("bcab_raw: %s\n", hx);
    gk3_logf("bcab: %s", be ? gk3_strerror(be) : "valid");
    if (!be)
        for (unsigned i = 0; i < 2; i++) {
            gk3_slot_info si;
            gk3_bcab_get_slot(bc, i, &si);
            gk3_logf("  _%c=%u/%u%s%s", 'a' + i, si.priority, si.tries, si.successful ? "/ok" : "",
                     gk3_slot_bootable(&si) ? "" : "/unbootable");
        }
    gk3_logf("\n");
    gk3_vab v;
    gk3_vab_parse(m + GK3_MISC_SYSTEM_OFF, &v);
    uint8_t merge = v.valid ? v.merge_status : GK3_MERGE_UNKNOWN;
    gk3_logf("vab: %s merge_status=%u source=_%c\n", v.valid ? "valid" : "invalid", v.merge_status,
             v.source_slot < 2 ? 'a' + v.source_slot : '?');

    static uint8_t copy[32], back[32];
    gk3_sel s;
    gk3_memcpy(copy, bc, 32);
    gk3_select_slot(copy, g_hint, merge, &s);
    gk3_logf("decision: %s slot=_%c active=_%c fallback=%u\n", selk[s.kind], 'a' + s.slot, 'a' + s.active, s.fallback);
    if (opt.observe) {
        if (s.decremented) {
            hex_str(copy, 32, hx, sizeof(hx));
            gk3_logf("would (action): write misc+0x800: _%c tries %u -> %u (NOT written; new bcab %s)\n", 'a' + s.slot,
                     s.tries_before, s.tries_after, hx);
        } else if (s.kind == GK3_SEL_BOOT) {
            gk3_logf("would (action): no misc write (slot already successful)\n");
        }
        gk3_logf("would (action): GK3 boot_streak +1 (NOT written)\n");
    }

    /* 要启动 / 进执行端的槽 */
    if (opt.force_slot >= 0) {
        g_slot = (unsigned)opt.force_slot;
        g_event = "forced";
        gk3_logf("slot: _%c (forced by gk3.slot; decision above is %s _%c)\n", 'a' + g_slot, selk[s.kind],
                 'a' + s.slot);
    } else {
        g_slot = s.kind == GK3_SEL_BCAB_INVALID ? g_hint : s.slot;
        g_event = s.kind == GK3_SEL_BOOT && s.fallback ? "fallback" : s.kind == GK3_SEL_BCAB_INVALID ? "bcab_invalid"
                                                                                                    : "none";
    }
    g_target = g_slot;

    /* —— 动作模式：执行端的去向（S7c）—— */
    bool bcb_done = false, bcb_pending = false;
    if (!opt.observe) {
        static uint8_t rec_save[GK3_REC_SIZE];
        gk3_disp_plan dp;
        const char *why = NULL;
        bool from_bcb = false;
        uint8_t ok_prev = 0;

        gk3_memset(&dp, 0, sizeof(dp));
        rec_prepare(m);
        /* S15 §4.9.3 a、b：默认系统（不合法的 LoaderEntryDefault、set_default 请求、缓存）。在存 rec_save 之前做：
         * 下面"执行端不可用 → 退回进来时的记录"不该把已经写了变量的那一步也退掉 */
        dual_defaults();
        /* §4.2 第 4 步、§4.4.4：next=sdboot-menu（执行端留下的"下次停在 systemd-boot 菜单"）早于 BCB 分派 */
        if (gk3_rec_next(g_rec, NULL) == GK3_NEXT_SDBOOT_MENU) {
            gk3_dual_boot db;
            gk3_dual_plan_boot(g_rec, dual.default_windows, false, true, &db);
            if (!rec_write("sdboot-menu")) {
                gk3_logf("dual: next=sdboot-menu consumed: returning EFI_SUCCESS to systemd-boot (menu without "
                         "countdown, boot.c:2971-2976); tries / streak untouched\n");
                gk3_screenf("gk3boot: next=sdboot-menu -> back to the boot menu\n");
                dual.menu = true;
                gk3_free_pages(m, GK3_MISC_READ_SIZE);
                return;
            }
            /* 写不进去：不回菜单（否则每次开机都停在菜单），照常往下走；内存里已清，rec_android 会再试着写 */
        }
        gk3_memcpy(rec_save, g_rec, GK3_REC_SIZE);
        if (opt.action[0]) {
            why = opt.action;               /* gk3boot-tools.conf：用户在 systemd-boot 菜单里选的，不看分派开关、不碰 BCB */
        } else if (opt.dispatch) {
            gk3_dispatch_plan(&bi, g_rec, (uint8_t)g_slot, &dp);
            gk3_logf("dispatch: action=%s why=%s count=%u%s\n", gk3_disp_name(dp.action), gk3_bcb_kind_name(dp.why),
                     dp.count, dp.clear_bcb_first ? " clear_bcb_first" : "");
            switch (dp.action) {
            case GK3_DISP_MIGRATE:
            case GK3_DISP_CLEAR:
            case GK3_DISP_WIPE_CAP:
                if (dispatch_clear(m, &bi, &dp))
                    bcb_done = true;
                else
                    gk3_memcpy(g_rec, rec_save, GK3_REC_SIZE);
                break;
            case GK3_DISP_EXECUTOR:
                why = gk3_bcb_kind_name(dp.why);
                from_bcb = true;
                bcb_pending = true;       /* S15：这份 BCB 这一版会执行 = Android 待办（执行端不可用时也算） */
                break;
            case GK3_DISP_NONE:
                break;
            }
            if (!why && opt.force_slot < 0 && s.kind == GK3_SEL_NOSLOT)
                why = "noslot";
            if (!why && opt.force_slot < 0 && s.kind == GK3_SEL_MERGING)
                why = "merging";
            /* bootloop（§4.3.3）：已确认的槽连续 N 次没走到开机完成（HAL 没清 boot_streak）⇒ 这一次去执行端菜单 */
            ok_prev = gk3_rec_boot_streak(g_rec) ? gk3_rec_ok_streak(g_rec) : 0;
            if (!why && opt.force_slot < 0 && s.kind == GK3_SEL_BOOT && !s.decremented &&
                ok_prev >= GK3_BOOTLOOP_THRESHOLD)
                why = "bootloop";
        }
        if (why) {
            const char *err = prepare_executor(why, g_slot);
            if (!err) {
                if (!str_cmp(why, "bootloop")) {
                    gk3_rec_event_add(g_rec, GK3_EV_BOOTLOOP, (uint8_t)g_slot, ok_prev);
                    gk3_rec_set_ok_streak(g_rec, 0);
                } else if (!str_cmp(why, "noslot")) {
                    gk3_rec_event_add(g_rec, GK3_EV_NOSLOT, 0xff, s.active);
                } else if (!str_cmp(why, "merging")) {
                    gk3_rec_event_add(g_rec, GK3_EV_REFUSED_MERGING, (uint8_t)s.slot, GK3_MERGE_MERGING);
                }
                /* S15：进执行端 = Android 待办 ⇒ next=windows / clean_poweroff 作废；默认 Windows 时同样预置 OneShot
                 * （执行端里 reboot 之后回 Android，而不是落进默认的 Windows） */
                {
                    gk3_dual_boot db;
                    gk3_dual_plan_boot(g_rec, dual.default_windows, true, true, &db);
                    dual.preset = db.preset_oneshot;
                    if (db.dropped)
                        gk3_logf("dual: entering the executor: intents dropped (mask %u)\n", db.dropped);
                }
                notable("executor: why=%s%s slot=_%c (kernel boot_%c); tries NOT decremented, boot_streak NOT counted",
                        why, from_bcb ? " (from BCB)" : opt.action[0] ? " (gk3.action)" : "", 'a' + g_slot,
                        'a' + boot.slot);
                commit_executor(&bi, &dp, from_bcb);
                gk3_free_pages(m, GK3_MISC_READ_SIZE);
                return;
            }
            /* 执行端缺失 / 读不出：照常启动 Android，不消费 BCB（分派计数也不记：用进来之前的那份记录） */
            gk3_memcpy(g_rec, rec_save, GK3_REC_SIZE);
            if (s.kind == GK3_SEL_NOSLOT || s.kind == GK3_SEL_MERGING)
                fail_open("decision", "%s: the executor (why=%s) is unavailable: %s", selk[s.kind], why, err);
            /* 按设计处理掉的情况（部署里没有执行端是合法状态）："note:" 而不是 "!!" */
            notable("executor (why=%s) unavailable: %s -- booting Android instead%s", why, err,
                    from_bcb ? " (BCB left as is)" : "");
        }
    }

    /* 不进执行端：Android（两槽都不可启动 / 合并中不许换槽 → 直连条目） */
    if (opt.force_slot < 0) {
        if (s.kind == GK3_SEL_NOSLOT)
            /* 本该进执行端（why=noslot）；分派关 / 观察模式 → 直连条目（今天的路，不猜槽：用 active 槽） */
            fail_open("decision", "noslot: neither slot is bootable; executor not used (%s)",
                      opt.observe ? "observe mode" : "dispatch off");
        if (s.kind == GK3_SEL_MERGING)
            /* §4.3.2-4：合并中不换槽 —— 不回落到另一槽 → active 槽的直连条目 */
            fail_open("decision", "merging: active slot _%c is not bootable while a snapshot merge is in progress; "
                      "refusing to fall back to _%c (§4.3.2-4); executor not used (%s)", 'a' + s.slot,
                      'a' + (s.slot ^ 1), opt.observe ? "observe mode" : "dispatch off");
        if (s.kind == GK3_SEL_BCAB_INVALID && !opt.observe)
            notable("bcab invalid (%s): booting hint _%c without writing misc (the HAL re-initialises it)",
                    gk3_strerror(s.bcab_err), 'a' + g_hint);
        gk3_logf("slot: _%c (event=%s)\n", 'a' + g_slot, g_event);
    }

    if (!opt.observe) {
        /* 0) S15 §4.9.3 c、d、e：去不去 Windows。"Android 待办" = 这份 BCB 这一版会执行、强制槽、BCAB 无效（HAL 要重建）、
         *    选中的槽在扣 tries（OTA 后的新槽）、上一次没走到开机完成（HAL 没清 boot_streak）；next=slot:x 由 libgk3core 算。
         *    分派关时 BCB 只记录不执行，不算待办（否则一份留着的 BCB 会永远挡住"重启到 Windows"）。 */
        bool pending = bcb_pending || opt.force_slot >= 0 || s.kind != GK3_SEL_BOOT || s.decremented ||
                       gk3_rec_boot_streak(g_rec) > 0;
        gk3_next_kind nk = gk3_rec_next(g_rec, NULL);
        bool cp = gk3_rec_clean_poweroff(g_rec), want = nk == GK3_NEXT_WINDOWS || cp;
        gk3_dual_boot db;
        gk3_dual_plan_boot(g_rec, dual.default_windows, want ? win_present() : false, pending, &db);
        if (want)
            gk3_logf("dual: next=%u clean_poweroff=%u default=%s pending=%u -> %s%s\n", nk, cp,
                     dual.default_windows ? "Windows" : "Android", pending,
                     db.kind == GK3_DUAL_WINDOWS ? "Windows" : "Android",
                     db.dropped ? " (intents dropped, event intent_dropped)" : "");
        if (db.kind == GK3_DUAL_WINDOWS)
            go_windows(db.why);            /* 只在失败时回来 */
        dual.preset = dual.default_windows;
        /* 1) 扣 tries（§4.3.2-3）：写 → Flush → 读回比对；写不进去就不启动这一槽（没扣到 tries 的未确认槽可能一直起不来） */
        if (s.decremented && opt.force_slot < 0) {
            gk3_err e = misc_write(GK3_MISC_BCAB_OFF, copy, 32, back);
            if (!e && gk3_bcab_validate(back))
                e = GK3_ECRC;
            if (e)
                fail_open("misc-write", "BCAB _%c tries %u -> %u: %s (efi %s)", 'a' + s.slot, s.tries_before,
                          s.tries_after, gk3_strerror(e), gk3_efi_strerror(disk.ctx.last_err));
            hex_str(back, 32, hx, sizeof(hx));
            gk3_logf("misc: wrote +0x800: _%c tries %u -> %u (read back OK, crc OK; bcab %s)\n", 'a' + s.slot,
                     s.tries_before, s.tries_after, hx);
            if (!s.tries_after)
                gk3_logf("misc: _%c now tries 0 and not successful = unbootable (the SetSlotAsUnbootable state); "
                         "unless Android marks it successful, the next boot falls back to _%c\n",
                         'a' + s.slot, 'a' + (s.slot ^ 1));
        } else {
            gk3_logf("misc: BCAB not written (%s)\n", opt.force_slot >= 0 ? "forced slot"
                                                      : s.kind == GK3_SEL_BOOT ? "slot already successful"
                                                                               : "BCAB invalid");
        }
        /* 2) GK3 记录 */
        rec_android(m, &bi, &s, bcb_done);
    }
    gk3_free_pages(m, GK3_MISC_READ_SIZE);
}

static void step_boot(void)
{
    const char *e = load_boot(g_slot);
    if (e)
        fail_open("boot", "%s", e);
}

/* ------------------------------------------------------------------ 4. cmdline（§4.3.1） */

static CHAR16 *g_cmdline16;

static void step_cmdline(void)
{
    static char base[GK3_BOOT_ARGS_SIZE + GK3_BOOT_EXTRA_ARGS_SIZE + 8], out[4096];
    gk3_android_args a = {g_slot, "gk3boot-" GK3BOOT_VERSION, g_event, g_entry, opt.observe ? "observe" : "action",
                          !opt.observe && g_streak[0] ? g_streak : NULL, !opt.observe && opt.dispatch ? "1" : NULL};
    long bn = gk3_bootimg_cmdline(&boot.b, base, sizeof(base));
    if (bn < 0)
        fail_open("cmdline", "boot.img cmdline does not fit");
    gk3_err e = gk3_cmdline_android(base, &a, out, sizeof(out));
    if (e)
        fail_open("cmdline", "gk3_cmdline_android: %s", gk3_strerror(e));
    size_t n = gk3_strlen(out);
    if (!(g_cmdline16 = gk3_alloc((n + 1) * sizeof(CHAR16))))
        fail_open("cmdline", "alloc");
    e = gk3_ascii_to_ucs2(out, (uint16_t *)g_cmdline16, n + 1);
    if (e)
        fail_open("cmdline", "ucs2: %s", gk3_strerror(e));
    gk3_logf("cmdline.base(%ld): %s\n", bn, base);
    gk3_logf("cmdline(%llu): %s\n", (unsigned long long)n, out);
}

/* ------------------------------------------------------------------ main */

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    gk3efi_init(image, st);
    T0 = gk3_ticks();
    gk3_log_init(LOG_CAP);
    gk3_lg.screen = false;      /* 模式定下来之前先不上屏幕（动作模式整个正常路径都不上） */
    step_header();
    step_disk();
    step_misc();
    if (dual.menu) {
        /* S15 next=sdboot-menu：唯一允许 return SUCCESS 的情况（§4.12 第 5 条）—— systemd-boot 停在不倒计时的菜单上。
         * 先撤掉自己的 120 s 看门狗（systemd-boot 等按键时自己会再设 5 分钟的，console.c:94-96） */
        gk3_bs->SetWatchdogTimer(0, 0, 0, NULL);
        gk3_logd("gk3boot.result=sdboot-menu t=%llu ms\n", MS());
        gk3_log_close(&g_log);
        return EFI_SUCCESS;
    }
    gk3_bootimg *b = &boot.b;
    gk3_linux L = {
        .dtb_len = 0,
    };
    const char *stage = "?";
    if (ex.why) {
        /* 执行端：同一个 boot_x 的内核与 dtb，initrd 换成 fastboot.img（§4.4.1） */
        L.kernel = boot.img + b->kernel_off;
        L.kernel_len = b->kernel_size;
        L.initrd = ex.img;
        L.initrd_len = ex.len;
        L.dtb = boot.img + b->dtb_off;
        L.dtb_len = b->dtb_size;
        L.cmdline = ex.cmdline16;
        preset_oneshot();
        gk3_logf("gk3boot: booting the executor (why=%s) with boot_%c's kernel via H2 (t=%llu ms)\n", ex.why,
                 'a' + boot.slot, MS());
        EFI_STATUS r = gk3_linux_boot(&L, pre_start, &stage);
        /* BCB 可能已经清了；交接失败是入口内部错误 → fail-open 阶梯（§4.12） */
        fail_open("handoff", "executor %s: %s", stage, gk3_efi_strerror(r));
    }
    step_boot();
    step_cmdline();
    L.kernel = boot.img + b->kernel_off;
    L.kernel_len = b->kernel_size;
    L.initrd = boot.img + b->ramdisk_off;
    L.initrd_len = b->ramdisk_size;
    L.dtb = boot.img + b->dtb_off;
    L.dtb_len = b->dtb_size;
    L.cmdline = g_cmdline16;
    preset_oneshot();
    gk3_logf("gk3boot: booting boot_%c via H2 (t=%llu ms)\n", 'a' + g_slot, MS());
    EFI_STATUS r = gk3_linux_boot(&L, pre_start, &stage);
    /* 只有失败才回到这里 */
    fail_open("handoff", "%s: %s", stage, gk3_efi_strerror(r));
    return EFI_SUCCESS;     /* 不可达 */
}
