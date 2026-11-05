/*
 * gk3probe.efi —— 统一启动入口的只读探针（见 tools/gk3boot/README.md）。
 *
 * 作为 systemd-boot 的 efi 条目、经 LoaderEntryOneShot 进入；只读地收集入口要依赖的
 * 固件事实，写进自己所在 ESP 的 \EFI\gk3boot\probe\log-<n>.txt（n 递增、不覆盖），
 * 同时打到屏幕；最后 ResetSystem(EfiResetCold) —— 下一次启动自动回到 ESP default 的 Android，
 * 日志从 Android 里只读挂 ESP 取（步骤见 tools/gk3boot/README.md）。
 *
 * 纪律（为了能无人值守上机）：
 *   - 只读：不写 misc / 任何块设备 / 除自己日志以外的任何文件，不改 EFI 变量；
 *     块设备包装连 write 回调都不给（gk3_blk_from_bio）。
 *   - 任何一步出错：记一行 "!! …"、计数，然后继续下一步；绝不 return 错误码给固件
 *     （华为 BootFail 计数，设计稿 §2.1、E-K1），也不 return SUCCESS（会停在不倒计时的菜单上）。
 *   - SetWatchdogTimer(60 s) 兜底：哪一步真的挂死，60 秒后固件复位（E6 之前还不知道本机看门狗是否真会复位）。
 *   - 风险最大的一步（缓冲区 LoadImage + StartImage）放在最后，之前每一节结束都把日志 Flush 到盘上，
 *     这样即使它把机器带走，前面的结果也已经在 ESP 上了。
 *
 * LoadOptions（systemd-boot 条目的 options 行）：
 *   gk3probe.hold=<秒>       复位前在屏幕上停留多久，默认 3，最大 30（方便拍照）
 *   gk3probe.keyscan=<毫秒>   开头扫按键的时长，默认 2000，最大 10000（不阻塞等待，轮询）
 *   gk3probe.noreset=1       不复位，改为 return EFI_SUCCESS 回 systemd-boot（只给 QEMU 调试用；
 *                            真机上会停在不倒计时的菜单里，别用）
 */
#include "gk3efi.h"

#include "child_abi.h"

#ifndef GK3PROBE_VERSION
#define GK3PROBE_VERSION "dev"
#endif

/* child_blob.S：内嵌的测试 PE（child.c 编出来的 gk3probe-child.efi） */
extern const uint8_t gk3_child_pe[];
extern const uint8_t gk3_child_pe_end[];

#define WATCHDOG_SEC 60
#define WATCHDOG_CODE 0x10001          /* 0x0000–0xFFFF 留给固件（UEFI 规范 SetWatchdogTimer） */
#define LOG_CAP (512u * 1024u)

static uint64_t T0;
static unsigned n_err;
static unsigned opt_hold = 3, opt_keyscan = 2000, opt_noreset = 0;
static EFI_LOADED_IMAGE_PROTOCOL *self_li;

#define MS() ((unsigned long long)(gk3_us_since(T0) / 1000u))
#define FAIL(...)                                                                                                 \
    do {                                                                                                          \
        n_err++;                                                                                                  \
        gk3_logf("!! " __VA_ARGS__);                                                                              \
        gk3_logf("\n");                                                                                           \
    } while (0)

static void sect(const char *name)
{
    gk3_log_sync();
    gk3_logf("\n== %s  [t=%llu ms] ==\n", name, MS());
}

static unsigned long long us_to_ms_x10(uint64_t us)   /* 毫秒 ×10，打成 "12.3" 用 */
{
    return (unsigned long long)(us / 100u);
}
#define MSF(us) us_to_ms_x10(us) / 10, us_to_ms_x10(us) % 10

static void hex_str(const uint8_t *p, size_t n, char *out, size_t cap)
{
    size_t o = 0;
    for (size_t i = 0; i < n && o + 3 <= cap; i++)
        o += (size_t)gk3_snprintf(out + o, cap - o, "%02x", p[i]);
    if (cap)
        out[o < cap ? o : cap - 1] = 0;
}

/* ------------------------------------------------------------------ 0. 选项 */

static unsigned parse_opt(const char *s, const char *key, unsigned dflt, unsigned max)
{
    size_t kl = gk3_strlen(key), sl = gk3_strlen(s);
    for (const char *p = s; p + kl <= s + sl; p++) {
        if ((p == s || p[-1] == ' ') && !gk3_memcmp(p, key, kl)) {
            unsigned v = 0;
            const char *q = p + kl;
            if (*q < '0' || *q > '9')
                return dflt;
            while (*q >= '0' && *q <= '9' && v < 1000000u)
                v = v * 10 + (unsigned)(*q++ - '0');
            return v > max ? max : v;
        }
    }
    return dflt;
}

static void step_header(void)
{
    static char opts[1024];
    EFI_TIME tm;
    gk3_logf("gk3probe %s  (read-only probe, docs/boot-entry-design.md E3)\n", GK3PROBE_VERSION);
    gk3_logf("timer: cntfrq=%llu Hz\n", (unsigned long long)gk3_tick_hz());
    gk3_memset(&tm, 0, sizeof(tm));
    EFI_STATUS st = gk3_rt->GetTime(&tm, NULL);
    if (EFI_ERROR(st))
        FAIL("GetTime: %s", gk3_efi_strerror(st));
    else
        gk3_logf("rtc: %04u-%02u-%02u %02u:%02u:%02u (tz %d, daylight 0x%x)\n", tm.Year, tm.Month, tm.Day, tm.Hour,
                 tm.Minute, tm.Second, (int)tm.TimeZone, tm.Daylight);

    st = gk3_bs->SetWatchdogTimer(WATCHDOG_SEC, WATCHDOG_CODE, 0, NULL);
    if (EFI_ERROR(st))
        FAIL("SetWatchdogTimer(%u s): %s", WATCHDOG_SEC, gk3_efi_strerror(st));
    else
        gk3_logf("watchdog: armed %u s\n", WATCHDOG_SEC);

    st = gk3_bs->HandleProtocol(gk3_image, (EFI_GUID *)&gk3_guid_loaded_image, (void **)&self_li);
    if (EFI_ERROR(st) || !self_li) {
        self_li = NULL;
        FAIL("LoadedImage(self): %s", gk3_efi_strerror(st));
        return;
    }
    opts[0] = 0;
    if (self_li->LoadOptions && self_li->LoadOptionsSize >= 2)
        gk3_ucs2_to_ascii(self_li->LoadOptions, self_li->LoadOptionsSize / 2, opts, sizeof(opts));
    gk3_logf("load_options: (%u bytes) \"%s\"\n", self_li->LoadOptionsSize, opts);
    opt_hold = parse_opt(opts, "gk3probe.hold=", 3, 30);
    opt_keyscan = parse_opt(opts, "gk3probe.keyscan=", 2000, 10000);
    opt_noreset = parse_opt(opts, "gk3probe.noreset=", 0, 1);
    gk3_logf("options: hold=%u s keyscan=%u ms noreset=%u\n", opt_hold, opt_keyscan, opt_noreset);
}

/* ------------------------------------------------------------------ 1. 按键（最先做：看 systemd-boot 之后 ConIn 里还剩什么） */

typedef struct {
    EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *ex;
    SIMPLE_INPUT_INTERFACE *in;
    unsigned errs;
    bool dead;
} key_src;

static void step_keys(void)
{
    static char dp[512];
    key_src src[16];
    unsigned ns = 0, nkeys = 0;
    UINTN n_in = 0, n_ex = 0;
    EFI_HANDLE *h_in = NULL, *h_ex = NULL;
    EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *con_ex = NULL;
    EFI_STATUS st;

    sect("keys (ConIn / ConInEx)");
    gk3_memset(src, 0, sizeof(src));
    gk3_logf("conin: ST->ConIn=%s ConsoleInHandle=%p\n", gk3_st->ConIn ? "present" : "ABSENT",
             gk3_st->ConsoleInHandle);
    if (gk3_st->ConsoleInHandle) {
        st = gk3_bs->HandleProtocol(gk3_st->ConsoleInHandle, (EFI_GUID *)&gk3_guid_text_in_ex, (void **)&con_ex);
        gk3_logf("conin_ex on ConsoleInHandle: %s\n", EFI_ERROR(st) ? gk3_efi_strerror(st) : "present");
        if (EFI_ERROR(st))
            con_ex = NULL;
        gk3_dp_text(gk3_dp_of(gk3_st->ConsoleInHandle), dp, sizeof(dp));
        gk3_logf("conin handle dp: %s\n", dp);
    }
    st = gk3_handles(&gk3_guid_text_in, &n_in, &h_in);
    gk3_logf("SimpleTextIn handles: %llu (%s)\n", (unsigned long long)n_in, gk3_efi_strerror(st));
    st = gk3_handles(&gk3_guid_text_in_ex, &n_ex, &h_ex);
    gk3_logf("SimpleTextInEx handles: %llu (%s)\n", (unsigned long long)n_ex, gk3_efi_strerror(st));

    /* 逐个物理设备轮询（跳过 ConsoleInHandle 那个 ConSplitter 虚拟句柄）：这样能知道键从哪个设备来 */
    for (UINTN i = 0; i < n_ex && ns < 16; i++) {
        void *p = NULL;
        gk3_dp_text(gk3_dp_of(h_ex[i]), dp, sizeof(dp));
        bool is_con = h_ex[i] == gk3_st->ConsoleInHandle;
        gk3_logf("  inex[%llu]%s %s\n", (unsigned long long)i, is_con ? " (ConsoleIn)" : "", dp);
        if (is_con || EFI_ERROR(gk3_bs->HandleProtocol(h_ex[i], (EFI_GUID *)&gk3_guid_text_in_ex, &p)) || !p)
            continue;
        gk3_logf("    -> src %u\n", ns);
        src[ns++].ex = p;
    }
    for (UINTN i = 0; i < n_in && ns < 16; i++) {
        void *p = NULL, *q = NULL;
        bool is_con = h_in[i] == gk3_st->ConsoleInHandle;
        bool has_ex = !EFI_ERROR(gk3_bs->HandleProtocol(h_in[i], (EFI_GUID *)&gk3_guid_text_in_ex, &q));
        gk3_dp_text(gk3_dp_of(h_in[i]), dp, sizeof(dp));
        gk3_logf("  in[%llu]%s%s %s\n", (unsigned long long)i, is_con ? " (ConsoleIn)" : "", has_ex ? " (has Ex)" : "",
                 dp);
        if (is_con || has_ex || EFI_ERROR(gk3_bs->HandleProtocol(h_in[i], (EFI_GUID *)&gk3_guid_text_in, &p)) || !p)
            continue;
        gk3_logf("    -> src %u\n", ns);
        src[ns++].in = p;
    }
    if (!ns) {
        if (con_ex)
            src[ns++].ex = con_ex;
        else if (gk3_st->ConIn)
            src[ns++].in = gk3_st->ConIn;
        if (ns)
            gk3_logf("  no physical input handles; src 0 = ConsoleIn %s\n", con_ex ? "(Ex)" : "(SimpleTextIn)");
    }
    gk3_free(h_in);
    gk3_free(h_ex);

    gk3_log_sync();
    gk3_logf("keyscan.begin sources=%u window=%u ms (press keys now: vol-up / vol-down / power / keyboard)\n", ns,
             opt_keyscan);
    uint64_t t = gk3_ticks();
    while (ns && gk3_us_since(t) / 1000u < opt_keyscan && nkeys < 200) {
        for (unsigned i = 0; i < ns; i++) {
            if (src[i].dead)
                continue;
            EFI_KEY_DATA kd;
            gk3_memset(&kd, 0, sizeof(kd));
            if (src[i].ex)
                st = src[i].ex->ReadKeyStrokeEx(src[i].ex, &kd);
            else
                st = src[i].in->ReadKeyStroke(src[i].in, &kd.Key);
            if (st == EFI_SUCCESS) {
                nkeys++;
                gk3_logf("key t=%llu ms src=%u scan=0x%04x unicode=0x%04x shift=0x%08x toggle=0x%02x\n",
                         (unsigned long long)(gk3_us_since(t) / 1000u), i, kd.Key.ScanCode, kd.Key.UnicodeChar,
                         kd.KeyState.KeyShiftState, kd.KeyState.KeyToggleState);
            } else if (st != EFI_NOT_READY) {
                if (++src[i].errs == 1)
                    gk3_logf("  src %u: ReadKeyStroke: %s\n", i, gk3_efi_strerror(st));
                if (src[i].errs > 20)
                    src[i].dead = true;
            }
        }
        gk3_bs->Stall(10000);
    }
    gk3_logf("keyscan.end keys=%u\n", nkeys);
}

/* ------------------------------------------------------------------ 2. 自身镜像 + 日志文件 */

static void step_image(void)
{
    static char dp[1024];
    sect("image (self)");
    if (!self_li) {
        FAIL("no LoadedImage for self");
        return;
    }
    gk3_logf("image: base=%p size=0x%llx code_type=%u data_type=%u parent=%p\n", self_li->ImageBase,
             (unsigned long long)self_li->ImageSize, self_li->ImageCodeType, self_li->ImageDataType,
             self_li->ParentHandle);
    gk3_dp_text(self_li->FilePath, dp, sizeof(dp));
    gk3_logf("file_path: %s\n", dp);
    gk3_dp_text(gk3_dp_of(self_li->DeviceHandle), dp, sizeof(dp));
    gk3_logf("device_path: %s\n", dp);

    void *pi = NULL;
    EFI_STATUS st = gk3_bs->HandleProtocol(self_li->DeviceHandle, (EFI_GUID *)&gk3_guid_partition_info, &pi);
    if (EFI_ERROR(st) || !pi) {
        gk3_logf("partition_info: absent (%s)\n", gk3_efi_strerror(st));
    } else {
        const uint8_t *p = pi;     /* PartitionInfo.h:33-54：Revision u32 / Type u32 / System u8 / Reserved[7] / Info */
        char g[37], name[40];
        gk3_logf("partition_info: rev=0x%x type=%u system=%u\n", gk3_le32(p), gk3_le32(p + 4), p[8]);
        if (gk3_le32(p + 4) == 2) {
            const uint8_t *e = p + 16;
            gk3_guid_str(e + 16, g);
            gk3_ucs2_to_ascii((const CHAR16 *)(e + 56), 36, name, sizeof(name));
            gk3_logf("partition_info.gpt: name=\"%s\" partuuid=%s lba=%llu-%llu\n", name, g,
                     (unsigned long long)gk3_le64(e + 32), (unsigned long long)gk3_le64(e + 40));
        }
    }
}

static EFI_FILE_PROTOCOL *open_dir(EFI_FILE_PROTOCOL *at, const CHAR16 *name)
{
    EFI_FILE_PROTOCOL *d = NULL;
    /* 先只读打开；没有才建（建目录是唯一一次"写目录"，只发生在第一次跑） */
    EFI_STATUS st = at->Open(at, &d, (CHAR16 *)name, EFI_FILE_MODE_READ, 0);
    if (st == EFI_NOT_FOUND)
        st = at->Open(at, &d, (CHAR16 *)name, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                      EFI_FILE_DIRECTORY);
    if (EFI_ERROR(st)) {
        static char a[64];
        gk3_ucs2_to_ascii(name, 64, a, sizeof(a));
        FAIL("open dir %s: %s", a, gk3_efi_strerror(st));
        return NULL;
    }
    return d;
}

static EFI_FILE_PROTOCOL *g_logdir;
static unsigned g_log_n;
static CHAR16 g_log_name[32];

static void name16(CHAR16 *out, unsigned n)
{
    char a[32];
    int k = gk3_snprintf(a, sizeof(a), "log-%u.txt", n);
    for (int i = 0; i <= k && i < 31; i++)
        out[i] = (CHAR16)a[i];
    out[31] = 0;
}

static void step_open_log(void)
{
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
    EFI_FILE_PROTOCOL *root = NULL, *d1, *d2, *f = NULL;
    sect("log file");
    if (!self_li) {
        FAIL("no LoadedImage: cannot locate ESP, log stays on screen only");
        return;
    }
    EFI_STATUS st = gk3_bs->HandleProtocol(self_li->DeviceHandle, (EFI_GUID *)&gk3_guid_simple_fs, (void **)&fs);
    if (EFI_ERROR(st) || !fs) {
        FAIL("SimpleFileSystem on own device: %s", gk3_efi_strerror(st));
        return;
    }
    st = fs->OpenVolume(fs, &root);
    if (EFI_ERROR(st) || !root) {
        FAIL("OpenVolume: %s", gk3_efi_strerror(st));
        return;
    }
    d1 = open_dir(root, u"EFI");
    d2 = d1 ? open_dir(d1, u"gk3boot") : NULL;
    g_logdir = d2 ? open_dir(d2, u"probe") : NULL;
    if (d2)
        d2->Close(d2);
    if (d1)
        d1->Close(d1);
    root->Close(root);
    if (!g_logdir)
        return;
    /* n 递增、不覆盖：找第一个不存在的 log-<n>.txt */
    for (g_log_n = 0; g_log_n < 10000; g_log_n++) {
        EFI_FILE_PROTOCOL *t = NULL;
        name16(g_log_name, g_log_n);
        st = g_logdir->Open(g_logdir, &t, g_log_name, EFI_FILE_MODE_READ, 0);
        if (st == EFI_NOT_FOUND)
            break;
        if (EFI_ERROR(st)) {
            FAIL("probe log-%u.txt: %s", g_log_n, gk3_efi_strerror(st));
            return;
        }
        t->Close(t);
    }
    if (g_log_n >= 10000) {
        FAIL("10000 logs already exist, not writing");
        return;
    }
    st = g_logdir->Open(g_logdir, &f, g_log_name, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
    if (EFI_ERROR(st) || !f) {
        FAIL("create log-%u.txt: %s", g_log_n, gk3_efi_strerror(st));
        return;
    }
    gk3_lg.file = f;
    gk3_logf("log_file: \\EFI\\gk3boot\\probe\\log-%u.txt\n", g_log_n);
    gk3_log_sync();
    if (gk3_lg.file_failed)
        FAIL("first write to log file: %s (continuing, screen only)", gk3_efi_strerror(gk3_lg.file_err));
}

/* ------------------------------------------------------------------ 3. 固件 */

static const char *smbios_str(const uint8_t *s, const uint8_t *end, uint8_t idx)
{
    static char out[4][80];
    static unsigned k;
    char *o = out[k++ & 3];
    const uint8_t *p = s;
    o[0] = 0;
    if (!idx)
        return "";
    for (uint8_t i = 1; p < end && *p; i++) {
        const uint8_t *q = p;
        while (q < end && *q)
            q++;
        if (i == idx) {
            size_t n = 0;
            for (; p < q && n + 1 < 80; p++)
                o[n++] = (*p >= 0x20 && *p < 0x7f) ? (char)*p : '?';
            o[n] = 0;
            return o;
        }
        p = q + 1;
    }
    return "?";
}

static void smbios(void *ep3, void *ep2)
{
    const uint8_t *tbl = NULL;
    uint64_t len = 0;
    if (ep3 && !gk3_memcmp(ep3, "_SM3_", 5)) {
        const uint8_t *e = ep3;      /* SMBIOS 3.x 入口：0x0c 最大长度 u32，0x10 表地址 u64 */
        tbl = (const uint8_t *)(uintptr_t)gk3_le64(e + 0x10);
        len = gk3_le32(e + 0x0c);
        gk3_logf("smbios3: version %u.%u table=%p max_len=%llu\n", e[7], e[8], tbl, (unsigned long long)len);
    } else if (ep2 && !gk3_memcmp(ep2, "_SM_", 4)) {
        const uint8_t *e = ep2;      /* 2.x：0x16 表长 u16，0x18 表地址 u32 */
        tbl = (const uint8_t *)(uintptr_t)gk3_le32(e + 0x18);
        len = gk3_le16(e + 0x16);
        gk3_logf("smbios: version %u.%u table=%p len=%llu\n", e[6], e[7], tbl, (unsigned long long)len);
    } else {
        gk3_logf("smbios: no entry point\n");
        return;
    }
    if (!tbl || !len || len > 1024 * 1024) {
        FAIL("smbios: implausible table");
        return;
    }
    const uint8_t *p = tbl, *end = tbl + len;
    for (unsigned n = 0; n < 512 && p + 4 <= end; n++) {
        uint8_t type = p[0], hl = p[1];
        if (hl < 4 || p + hl > end)
            break;
        const uint8_t *s = p + hl, *q = s;
        while (q + 1 < end && (q[0] || q[1]))
            q++;
        if (type == 0)
            gk3_logf("smbios.bios: vendor=\"%s\" version=\"%s\" date=\"%s\"\n", smbios_str(s, q + 1, p[4]),
                     smbios_str(s, q + 1, p[5]), smbios_str(s, q + 1, hl > 8 ? p[8] : 0));
        else if (type == 1)
            gk3_logf("smbios.system: maker=\"%s\" product=\"%s\" version=\"%s\" sku=\"%s\" family=\"%s\"\n",
                     smbios_str(s, q + 1, p[4]), smbios_str(s, q + 1, p[5]), smbios_str(s, q + 1, p[6]),
                     smbios_str(s, q + 1, hl > 0x19 ? p[0x19] : 0), smbios_str(s, q + 1, hl > 0x1a ? p[0x1a] : 0));
        else if (type == 127)
            break;
        p = q + 2;
    }
}

static void step_firmware(void)
{
    static char v[160];
    void *ep3 = NULL, *ep2 = NULL;
    sect("firmware");
    gk3_ucs2_to_ascii(gk3_st->FirmwareVendor, 128, v, sizeof(v));
    gk3_logf("vendor: \"%s\" revision=0x%08x uefi=%u.%u\n", v, gk3_st->FirmwareRevision, gk3_st->Hdr.Revision >> 16,
             gk3_st->Hdr.Revision & 0xffff);
    gk3_logf("config_tables: %llu\n", (unsigned long long)gk3_st->NumberOfTableEntries);
    for (UINTN i = 0; i < gk3_st->NumberOfTableEntries && i < 64; i++) {
        EFI_CONFIGURATION_TABLE *t = &gk3_st->ConfigurationTable[i];
        char g[37];
        const char *nm = gk3_guid_name(&t->VendorGuid);
        gk3_guid_str((const uint8_t *)&t->VendorGuid, g);
        gk3_logf("  table %s %s @%p\n", g, nm ? nm : "?", t->VendorTable);
        if (gk3_guid_eq(&t->VendorGuid, &gk3_guid_smbios3))
            ep3 = t->VendorTable;
        if (gk3_guid_eq(&t->VendorGuid, &gk3_guid_smbios))
            ep2 = t->VendorTable;
    }
    smbios(ep3, ep2);
}

/* ------------------------------------------------------------------ 4. EFI 变量（只读） */

static void show_var(const CHAR16 *name, const EFI_GUID *g, int kind)   /* kind 0=hex 1=UCS-2 串 2=u64 3=u8 4=u16 5=串列 */
{
    static uint8_t buf[4096];
    static char a[600];
    char nm[48];
    UINTN sz = sizeof(buf);
    UINT32 attr = 0;
    gk3_ucs2_to_ascii(name, 47, nm, sizeof(nm));
    EFI_STATUS st = gk3_getvar(name, g, &attr, buf, &sz);
    if (st == EFI_NOT_FOUND) {
        gk3_logf("var %s: (not set)\n", nm);
        return;
    }
    if (EFI_ERROR(st)) {
        gk3_logf("var %s: %s (size %llu)\n", nm, gk3_efi_strerror(st), (unsigned long long)sz);
        return;
    }
    if (kind == 1) {
        gk3_ucs2_to_ascii((const CHAR16 *)buf, sz / 2, a, sizeof(a));
    } else if (kind == 5) {     /* NUL 分隔的 UCS-2 串列（LoaderEntries）→ 逗号分隔 */
        size_t o = 0;
        for (UINTN i = 0; i + 1 < sz && o + 2 < sizeof(a); i += 2) {
            uint16_t ch = gk3_le16(buf + i);
            a[o++] = ch == 0 ? ',' : (ch >= 0x20 && ch < 0x7f) ? (char)ch : '?';
        }
        while (o && a[o - 1] == ',')
            o--;
        a[o] = 0;
    }
    else if (kind == 2 && sz == 8)
        gk3_snprintf(a, sizeof(a), "0x%llx", (unsigned long long)gk3_le64(buf));
    else if (kind == 3 && sz == 1)
        gk3_snprintf(a, sizeof(a), "%u", buf[0]);
    else if (kind == 4 && sz == 2)
        gk3_snprintf(a, sizeof(a), "0x%04x", gk3_le16(buf));
    else
        hex_str(buf, sz < 64 ? sz : 64, a, sizeof(a));
    gk3_logf("var %s: attr=0x%x size=%llu \"%s\"\n", nm, attr, (unsigned long long)sz, a);
}

static void show_boot_option(uint16_t num)
{
    static uint8_t buf[4096];
    static char desc[160], dp[600];
    CHAR16 name[9] = u"Boot0000";
    static const char hx[] = "0123456789ABCDEF";
    UINTN sz = sizeof(buf);
    UINT32 attr;
    for (int i = 0; i < 4; i++)
        name[4 + i] = (CHAR16)hx[(num >> (12 - 4 * i)) & 15];
    EFI_STATUS st = gk3_getvar(name, &gk3_guid_global_var, &attr, buf, &sz);
    if (EFI_ERROR(st)) {
        gk3_logf("var Boot%04X: %s\n", num, gk3_efi_strerror(st));
        return;
    }
    /* EFI_LOAD_OPTION：Attributes u32 / FilePathListLength u16 / Description（UCS-2，NUL 结尾）/ FilePathList / OptionalData */
    if (sz < 8) {
        FAIL("Boot%04X: too short (%llu)", num, (unsigned long long)sz);
        return;
    }
    uint32_t oattr = gk3_le32(buf);
    uint16_t fpl = gk3_le16(buf + 4);
    size_t i = 6;
    while (i + 1 < sz && (buf[i] || buf[i + 1]))
        i += 2;
    gk3_ucs2_to_ascii((const CHAR16 *)(buf + 6), (i - 6) / 2, desc, sizeof(desc));
    i += 2;
    if (i + fpl <= sz && fpl >= 4) {
        /* 设备路径的最后一个节点必须是结束节点，否则别解析 */
        const uint8_t *e = buf + i + fpl - 4;
        if (e[0] == 0x7f && e[1] == 0xff)
            gk3_dp_text((const EFI_DEVICE_PATH_PROTOCOL *)(buf + i), dp, sizeof(dp));
        else
            gk3_snprintf(dp, sizeof(dp), "(unterminated)");
    } else {
        gk3_snprintf(dp, sizeof(dp), "(bad length %u)", fpl);
    }
    gk3_logf("var Boot%04X: attr=0x%x opt_attr=0x%x desc=\"%s\" optional=%llu dp=%s\n", num, attr, oattr, desc,
             (unsigned long long)(sz > i + fpl ? sz - i - fpl : 0), dp);
}

static void step_vars(void)
{
    static const struct {
        const CHAR16 *n;
        int kind;
    } loader[] = {
        {u"LoaderBootCountPath", 1}, {u"LoaderEntrySelected", 1}, {u"LoaderDevicePartUUID", 1},
        {u"LoaderEntryDefault", 1},  {u"LoaderEntryOneShot", 1},  {u"LoaderEntryLastBooted", 1},
        {u"LoaderInfo", 1},          {u"LoaderFirmwareInfo", 1},  {u"LoaderFirmwareType", 1},
        {u"LoaderImageIdentifier", 1}, {u"LoaderFeatures", 2},    {u"LoaderTimeInitUSec", 1},
        {u"LoaderTimeExecUSec", 1},  {u"LoaderConfigTimeout", 1}, {u"LoaderConfigTimeoutOneShot", 1},
        {u"LoaderEntries", 5},
    };
    static uint8_t order[512];
    sect("efi variables (read-only)");
    for (size_t i = 0; i < sizeof(loader) / sizeof(loader[0]); i++)
        show_var(loader[i].n, &gk3_guid_loader, loader[i].kind);
    show_var(u"SecureBoot", &gk3_guid_global_var, 3);
    show_var(u"SetupMode", &gk3_guid_global_var, 3);
    show_var(u"OsIndicationsSupported", &gk3_guid_global_var, 2);
    show_var(u"Timeout", &gk3_guid_global_var, 4);
    show_var(u"BootNext", &gk3_guid_global_var, 4);
    show_var(u"BootCurrent", &gk3_guid_global_var, 4);
    show_var(u"BootOrder", &gk3_guid_global_var, 0);

    UINTN sz = sizeof(order);
    UINT32 attr;
    if (!EFI_ERROR(gk3_getvar(u"BootOrder", &gk3_guid_global_var, &attr, order, &sz)))
        for (UINTN i = 0; i + 1 < sz && i < 64; i += 2)
            show_boot_option(gk3_le16(order + i));
    uint8_t cur[2];
    sz = 2;
    if (!EFI_ERROR(gk3_getvar(u"BootCurrent", &gk3_guid_global_var, &attr, cur, &sz)) && sz == 2) {
        bool in_order = false;
        UINTN osz = sizeof(order);
        if (!EFI_ERROR(gk3_getvar(u"BootOrder", &gk3_guid_global_var, &attr, order, &osz)))
            for (UINTN i = 0; i + 1 < osz; i += 2)
                in_order |= gk3_le16(order + i) == gk3_le16(cur);
        if (!in_order)
            show_boot_option(gk3_le16(cur));
    }
}

/* ------------------------------------------------------------------ 5. 整盘与 GPT */

static struct {
    bool ok;
    EFI_BLOCK_IO_PROTOCOL *bio;
    gk3_bio_ctx ctx;
    gk3_blk dev;
    gk3_gpt gpt;
    uint8_t *entries;
} disk;

static void step_disk(void)
{
    static char dp[1024];
    UINTN n = 0, matches = 0;
    EFI_HANDLE *hs = NULL;
    sect("disk (own ESP -> whole disk -> GPT)");
    if (!self_li) {
        FAIL("no LoadedImage");
        return;
    }
    EFI_DEVICE_PATH_PROTOCOL *esp = gk3_dp_of(self_li->DeviceHandle);
    const EFI_DEVICE_PATH_PROTOCOL *hd = gk3_dp_find_hd(esp);
    if (!esp || !hd) {
        FAIL("own device path has no HD node");
        return;
    }
    size_t prefix = (size_t)((const uint8_t *)hd - (const uint8_t *)esp);
    {
        const uint8_t *h = (const uint8_t *)hd;
        char g[37];
        gk3_guid_str(h + 24, g);
        gk3_logf("esp: partition=%u start=%llu size=%llu sig_type=%u partuuid=%s\n", gk3_le32(h + 4),
                 (unsigned long long)gk3_le64(h + 8), (unsigned long long)gk3_le64(h + 16), h[41], g);
    }

    EFI_STATUS st = gk3_handles(&gk3_guid_block_io, &n, &hs);
    gk3_logf("blockio handles: %llu (%s)\n", (unsigned long long)n, gk3_efi_strerror(st));
    for (UINTN i = 0; i < n; i++) {
        EFI_BLOCK_IO_PROTOCOL *b = NULL;
        if (EFI_ERROR(gk3_bs->HandleProtocol(hs[i], (EFI_GUID *)&gk3_guid_block_io, (void **)&b)) || !b || !b->Media)
            continue;
        EFI_DEVICE_PATH_PROTOCOL *d = gk3_dp_of(hs[i]);
        gk3_dp_text(d, dp, sizeof(dp));
        bool whole = !b->Media->LogicalPartition;
        gk3_logd("  blk[%llu] %s bs=%u last=%llu ro=%u rm=%u present=%u %s\n", (unsigned long long)i,
                 whole ? "DISK" : "part", b->Media->BlockSize, (unsigned long long)b->Media->LastBlock,
                 b->Media->ReadOnly, b->Media->RemovableMedia, b->Media->MediaPresent, dp);
        if (whole && d && gk3_dp_size(d) == prefix + 4 && !gk3_memcmp(d, esp, prefix)) {
            matches++;
            if (!disk.bio) {
                disk.bio = b;
                gk3_logf("whole_disk: %s\n", dp);
                gk3_logf("whole_disk.media: id=%u bs=%u last=%llu (%llu MiB) io_align=%u ro=%u removable=%u "
                         "write_caching=%u rev=0x%llx\n",
                         b->Media->MediaId, b->Media->BlockSize, (unsigned long long)b->Media->LastBlock,
                         (unsigned long long)((b->Media->LastBlock + 1) * b->Media->BlockSize >> 20),
                         b->Media->IoAlign, b->Media->ReadOnly, b->Media->RemovableMedia, b->Media->WriteCaching,
                         (unsigned long long)b->Revision);
            }
        }
    }
    gk3_free(hs);
    if (matches != 1)
        FAIL("whole-disk candidates for own ESP: %llu (expected 1)", (unsigned long long)matches);
    if (!disk.bio)
        return;
    if (disk.bio->Media->BlockSize < 512 || disk.bio->Media->BlockSize > 4096 || disk.bio->Media->IoAlign > 4096) {
        FAIL("unsupported block size %u / io_align %u", disk.bio->Media->BlockSize, disk.bio->Media->IoAlign);
        return;
    }
    gk3_blk_from_bio(&disk.dev, &disk.ctx, disk.bio);

    /* 主 GPT：先读头算出表多大，再一步读完（gk3_gpt_read 会再读一遍头，便宜） */
    uint32_t bs = disk.dev.block_size;
    uint8_t *hdr = gk3_alloc_pages(bs);
    if (!hdr) {
        FAIL("alloc");
        return;
    }
    uint64_t t = gk3_ticks();
    gk3_gpt g0;
    if (disk.dev.read(disk.dev.ctx, 1, 1, hdr)) {
        FAIL("read LBA1: %s", gk3_efi_strerror(disk.ctx.last_err));
        return;
    }
    gk3_err e = gk3_gpt_parse_header(hdr, bs, &g0);
    if (e) {
        FAIL("GPT header: %s", gk3_strerror(e));
        return;
    }
    size_t elen = ((size_t)g0.num_entries * g0.entry_size + bs - 1) / bs * bs;
    disk.entries = gk3_alloc_pages(elen);
    if (!disk.entries) {
        FAIL("alloc entries %llu", (unsigned long long)elen);
        return;
    }
    e = gk3_gpt_read(&disk.dev, &disk.gpt, hdr, disk.entries, elen);
    uint64_t us = gk3_us_since(t);
    if (e) {
        FAIL("gk3_gpt_read: %s (efi %s)", gk3_strerror(e), gk3_efi_strerror(disk.ctx.last_err));
        return;
    }
    char g[37];
    gk3_guid_str(disk.gpt.disk_guid, g);
    gk3_logf("gpt: disk=%s entries=%u x %u @LBA%llu usable=%llu-%llu alt=%llu used=%u read=%llu.%llu ms\n", g,
             disk.gpt.num_entries, disk.gpt.entry_size, (unsigned long long)disk.gpt.entries_lba,
             (unsigned long long)disk.gpt.first_usable, (unsigned long long)disk.gpt.last_usable,
             (unsigned long long)disk.gpt.alt_lba, gk3_gpt_count(&disk.gpt), MSF(us));
    for (uint32_t i = 0; i < disk.gpt.num_entries; i++) {
        gk3_gpt_part p;
        char tg[37], pg[37];
        if (gk3_gpt_get(&disk.gpt, i, &p))
            continue;
        gk3_guid_str(p.type_guid, tg);
        gk3_guid_str(p.part_guid, pg);
        gk3_logf("  p%-3u %-14s %10llu-%-10llu attrs=%016llx %s\n", p.index, p.name[0] ? p.name : "(non-ascii)",
                 (unsigned long long)p.first_lba, (unsigned long long)p.last_lba, (unsigned long long)p.attrs, pg);
        gk3_logd("       type=%s\n", tg);
    }
    static const char *const names[] = {"misc", "boot_a", "boot_b", "super", "userdata", "metadata", "esp"};
    bool all = true;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        gk3_gpt_part p;
        e = gk3_gpt_find(&disk.gpt, names[i], &p);
        gk3_logf("unique %-9s %s%s", names[i], e ? gk3_strerror(e) : "OK", e ? "\n" : "");
        if (!e)
            gk3_logf(" p%u\n", p.index);
        if (e && i < 5)      /* metadata / esp 只记录；入口硬要求的是前五个 */
            all = false;
    }
    gk3_logf("unique_required(misc,boot_a,boot_b,super,userdata): %s\n", all ? "YES" : "NO");
    if (!all)
        n_err++;
    /* 自己所在的 ESP 在 GPT 里对得上吗（HD 节点的分区号与签名） */
    const uint8_t *h = (const uint8_t *)hd;
    gk3_gpt_part ep;
    if (h[41] == 2 && !gk3_gpt_get(&disk.gpt, gk3_le32(h + 4) - 1, &ep))
        gk3_logf("esp_in_gpt: p%u name=\"%s\" partuuid %s\n", ep.index, ep.name,
                 !gk3_memcmp(ep.part_guid, h + 24, 16) ? "MATCHES device path" : "DIFFERS from device path");
    else
        FAIL("own ESP partition number not found in GPT");
    disk.ok = true;
}

/* ------------------------------------------------------------------ 6. misc 与 boot_a / boot_b */

static bool part_of(const char *name, gk3_gpt_part *p)
{
    gk3_err e = gk3_gpt_find(&disk.gpt, name, p);
    if (e) {
        FAIL("%s: %s", name, gk3_strerror(e));
        return false;
    }
    return true;
}

static void step_misc(void)
{
    gk3_gpt_part p;
    static const char *const selk[] = {"boot", "bcab_invalid", "noslot", "merging"};
    sect("misc (read-only)");
    if (!disk.ok || !part_of("misc", &p))
        return;
    uint32_t bs = disk.dev.block_size;
    uint64_t pbytes = (p.last_lba - p.first_lba + 1) * bs;
    if (pbytes < GK3_MISC_READ_SIZE) {
        FAIL("misc is only %llu bytes (< 64 KiB)", (unsigned long long)pbytes);
        return;
    }
    uint8_t *m = gk3_alloc_pages(GK3_MISC_READ_SIZE);
    if (!m) {
        FAIL("alloc");
        return;
    }
    uint64_t t = gk3_ticks();
    int r = disk.dev.read(disk.dev.ctx, p.first_lba, GK3_MISC_READ_SIZE / bs, m);
    uint64_t us = gk3_us_since(t);
    if (r) {
        FAIL("read misc: %s", gk3_efi_strerror(disk.ctx.last_err));
        gk3_free_pages(m, GK3_MISC_READ_SIZE);
        return;
    }
    gk3_sha1_ctx c;
    uint8_t d[20];
    char hx[48];
    gk3_sha1_init(&c);
    gk3_sha1_update(&c, m, GK3_MISC_READ_SIZE);
    gk3_sha1_final(&c, d);
    hex_str(d, 20, hx, sizeof(hx));
    gk3_logf("misc: p%u lba=%llu size=%llu KiB read 64 KiB in %llu.%llu ms sha1(0-64K)=%s\n", p.index,
             (unsigned long long)p.first_lba, (unsigned long long)(pbytes >> 10), MSF(us), hx);

    gk3_bcb_info bi;
    gk3_bcb_classify(m, &bi);
    gk3_logf("bcb: kind=%s command=\"%s\" args=%u zero=%u\n", gk3_bcb_kind_name(bi.kind), bi.command, bi.n_args,
             gk3_is_zero(m, GK3_MISC_BCB_SIZE));
    const uint8_t *bc = m + GK3_MISC_BCAB_OFF;
    gk3_err e = gk3_bcab_validate(bc);
    hex_str(bc, 32, hx, sizeof(hx));
    gk3_logf("bcab: %s suffix=\"%.3s\" nb_slot=%u merge=%u raw=%s\n", e ? gk3_strerror(e) : "valid",
             (bc[0] >= 0x20 && bc[0] < 0x7f) ? (const char *)bc : "", gk3_bcab_nb_slot(bc),
             gk3_bcab_merge_status(bc), hx);
    for (unsigned i = 0; i < 2; i++) {
        gk3_slot_info s;
        gk3_bcab_get_slot(bc, i, &s);
        gk3_logf("  _%c priority=%u tries=%u successful=%u verity_corrupted=%u bootable=%u\n", 'a' + i, s.priority,
                 s.tries, s.successful, s.verity_corrupted, gk3_slot_bootable(&s));
    }
    e = gk3_rec_validate(m + GK3_MISC_GK3_OFF);
    gk3_logf("gk3rec: %s zero=%u\n", e ? gk3_strerror(e) : "valid", gk3_is_zero(m + GK3_MISC_GK3_OFF, GK3_REC_SIZE));
    gk3_vab v;
    gk3_vab_parse(m + GK3_MISC_SYSTEM_OFF, &v);
    gk3_logf("vab: %s version=%u merge_status=%u source_slot=%u\n", v.valid ? "valid" : "invalid", v.version,
             v.merge_status, v.source_slot);
    /* 入口在这份 misc 上会怎么选 —— 在副本上算，不写（与 gk3-misc select 同一算法、hint=a） */
    uint8_t copy[32];
    gk3_sel s;
    gk3_memcpy(copy, bc, 32);
    gk3_select_slot(copy, 0, v.valid ? v.merge_status : GK3_MERGE_UNKNOWN, &s);
    gk3_logf("would_select (NOT written): %s slot=_%c active=_%c fallback=%u decrement=%u (%u->%u)\n", selk[s.kind],
             'a' + s.slot, 'a' + s.active, s.fallback, s.decremented, s.tries_before, s.tries_after);
    gk3_free_pages(m, GK3_MISC_READ_SIZE);
}

static bool has_substr(const char *s, const char *k)
{
    size_t kl = gk3_strlen(k), sl = gk3_strlen(s);
    for (size_t i = 0; i + kl <= sl; i++)
        if (!gk3_memcmp(s + i, k, kl))
            return true;
    return false;
}

static void step_boot(const char *name)
{
    gk3_gpt_part p;
    static char cl[1600];
    char hx[48], want[48];
    sect(name);
    if (!disk.ok || !part_of(name, &p))
        return;
    uint32_t bs = disk.dev.block_size;
    uint64_t pbytes = (p.last_lba - p.first_lba + 1) * bs;
    uint32_t hblocks = (4096 + bs - 1) / bs;
    uint8_t *h = gk3_alloc_pages(4096);
    if (!h) {
        FAIL("alloc");
        return;
    }
    uint64_t t = gk3_ticks();
    if (disk.dev.read(disk.dev.ctx, p.first_lba, hblocks, h)) {
        FAIL("read %s header: %s", name, gk3_efi_strerror(disk.ctx.last_err));
        gk3_free_pages(h, 4096);
        return;
    }
    uint64_t us_h = gk3_us_since(t);
    gk3_bootimg b;
    gk3_err e = gk3_bootimg_parse(h, 4096, pbytes, &b);
    if (e) {
        FAIL("%s: header %s (first bytes %02x %02x %02x %02x)", name, gk3_strerror(e), h[0], h[1], h[2], h[3]);
        gk3_free_pages(h, 4096);
        return;
    }
    hex_str(b.id, 20, want, sizeof(want));
    gk3_logf("%s: p%u header v%u page=%u kernel=%u ramdisk=%u second=%u dtbo=%u dtb=%u total=%llu os=0x%08x "
             "read_hdr=%llu.%llu ms\n",
             name, p.index, b.version, b.page_size, b.kernel_size, b.ramdisk_size, b.second_size,
             b.recovery_dtbo_size, b.dtb_size, (unsigned long long)b.total_size, b.os_version, MSF(us_h));
    gk3_logf("%s: id=%s name=\"%s\"\n", name, want, b.name);
    long cn = gk3_bootimg_cmdline(&b, cl, sizeof(cl));
    gk3_logd("%s: cmdline(%ld)=\"%s\"\n", name, cn, cn >= 0 ? cl : "");
    gk3_logf("%s: cmdline %ld chars, slot_suffix in cmdline: %s\n", name, cn,
             cn > 0 && has_substr(cl, "androidboot.slot_suffix=") ? "yes" : "no");
    gk3_free_pages(h, 4096);

    /* 整份读进来复算 SHA1(id)：入口每次开机都要做的事，量一下真机上的耗时 */
    if (b.total_size > 128ull * 1024 * 1024) {
        FAIL("%s: total %llu too large, skip", name, (unsigned long long)b.total_size);
        return;
    }
    size_t rd = (size_t)((b.total_size + bs - 1) / bs * bs);
    uint8_t *img = gk3_alloc_pages(rd);
    if (!img) {
        FAIL("%s: alloc %llu", name, (unsigned long long)rd);
        return;
    }
    t = gk3_ticks();
    int r = disk.dev.read(disk.dev.ctx, p.first_lba, (uint32_t)(rd / bs), img);
    uint64_t us_r = gk3_us_since(t);
    if (r) {
        FAIL("%s: read %llu bytes: %s", name, (unsigned long long)rd, gk3_efi_strerror(disk.ctx.last_err));
        gk3_free_pages(img, rd);
        return;
    }
    uint8_t got[20];
    t = gk3_ticks();
    e = gk3_bootimg_verify_id(&b, img, rd, got);
    uint64_t us_s = gk3_us_since(t);
    hex_str(got, 20, hx, sizeof(hx));
    gk3_logf("%s: read %llu bytes in %llu.%llu ms (%llu MB/s), sha1(id) in %llu.%llu ms: %s%s%s\n", name,
             (unsigned long long)rd, MSF(us_r), us_r ? (unsigned long long)(rd / us_r) : 0ull, MSF(us_s),
             e ? "MISMATCH got=" : "OK ", e ? hx : "", e ? "" : "(matches header)");
    if (e)
        n_err++;
    const uint8_t *k = img + b.kernel_off;
    gk3_logf("%s: kernel magic %02x %02x .. %02x %02x %02x %02x (%s)\n", name, k[0], k[1], k[4], k[5], k[6], k[7],
             (k[0] == 'M' && k[1] == 'Z' && !gk3_memcmp(k + 4, "zimg", 4)) ? "MZ + zimg = EFI zboot"
             : (k[0] == 'M' && k[1] == 'Z')                               ? "MZ (PE)"
                                                                         : "not PE");
    gk3_free_pages(img, rd);
}

/* ------------------------------------------------------------------ 7. 显示 */

static void step_display(void)
{
    static char dp[512];
    UINTN n = 0;
    EFI_HANDLE *hs = NULL;
    sect("display (GOP / ConOut, no mode change)");
    EFI_STATUS st = gk3_handles(&gk3_guid_gop, &n, &hs);
    gk3_logf("gop handles: %llu (%s)\n", (unsigned long long)n, gk3_efi_strerror(st));
    for (UINTN i = 0; i < n && i < 8; i++) {
        EFI_GRAPHICS_OUTPUT_PROTOCOL *g = NULL;
        if (EFI_ERROR(gk3_bs->HandleProtocol(hs[i], (EFI_GUID *)&gk3_guid_gop, (void **)&g)) || !g || !g->Mode)
            continue;
        gk3_dp_text(gk3_dp_of(hs[i]), dp, sizeof(dp));
        gk3_logf("gop[%llu]%s: max_mode=%u mode=%u fb=0x%llx fb_size=0x%llx %s\n", (unsigned long long)i,
                 hs[i] == gk3_st->ConsoleOutHandle ? " (ConsoleOut)" : "", g->Mode->MaxMode, g->Mode->Mode,
                 (unsigned long long)g->Mode->FrameBufferBase, (unsigned long long)g->Mode->FrameBufferSize, dp);
        for (UINT32 m = 0; m < g->Mode->MaxMode && m < 48; m++) {
            UINTN sz = 0;
            EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *inf = NULL;
            st = g->QueryMode(g, m, &sz, &inf);
            if (EFI_ERROR(st) || !inf) {
                gk3_logf("  mode %u: %s\n", m, gk3_efi_strerror(st));
                continue;
            }
            /* 当前模式上屏幕，其余只进文件（QEMU 的 virtio-gpu 有 37 个模式，屏幕上刷不下） */
            (m == g->Mode->Mode ? gk3_logf : gk3_logd)("  mode %u: %ux%u fmt=%u ppsl=%u%s\n", m,
                                                        inf->HorizontalResolution, inf->VerticalResolution,
                                                        inf->PixelFormat, inf->PixelsPerScanLine,
                                                        m == g->Mode->Mode ? "  <- current" : "");
            gk3_free(inf);
        }
    }
    gk3_free(hs);

    SIMPLE_TEXT_OUTPUT_INTERFACE *co = gk3_st->ConOut;
    if (!co || !co->Mode) {
        FAIL("ConOut absent");
        return;
    }
    gk3_logf("conout: max_mode=%d mode=%d attr=0x%x cursor=%d,%d\n", co->Mode->MaxMode, co->Mode->Mode,
             co->Mode->Attribute, co->Mode->CursorColumn, co->Mode->CursorRow);
    for (INT32 m = 0; m < co->Mode->MaxMode && m < 16; m++) {
        UINTN cols = 0, rows = 0;
        st = co->QueryMode(co, (UINTN)m, &cols, &rows);
        gk3_logf("  text mode %d: %s %llux%llu%s\n", m, EFI_ERROR(st) ? gk3_efi_strerror(st) : "ok",
                 (unsigned long long)cols, (unsigned long long)rows, m == co->Mode->Mode ? "  <- current" : "");
    }
    /* 设计稿 E3：CJK 与破折号能不能显示。EFI_WARN_UNKNOWN_GLYPH = 固件字体里没有 */
    st = co->OutputString(co, (CHAR16 *)u"cjk_test: 中文 — 引导菜单\r\n");
    gk3_logf("cjk_test OutputString: %s\n", gk3_efi_strerror(st));
}

/* ------------------------------------------------------------------ 8. 内存图 */

static const char *mem_type(UINT32 t)
{
    static const char *const n[] = {"Reserved", "LoaderCode", "LoaderData", "BSCode", "BSData", "RTCode",
                                    "RTData", "Conventional", "Unusable", "ACPIReclaim", "ACPINVS", "MMIO",
                                    "MMIOPort", "PalCode", "Persistent", "Unaccepted"};
    return t < sizeof(n) / sizeof(n[0]) ? n[t] : "Other";
}

static void step_memmap(void)
{
    UINTN sz = 0, key = 0, dsz = 0;
    UINT32 dver = 0;
    uint8_t *buf = NULL;
    EFI_STATUS st = EFI_BUFFER_TOO_SMALL;
    sect("memory map");
    for (int tries = 0; tries < 4 && st == EFI_BUFFER_TOO_SMALL; tries++) {
        gk3_free(buf);
        buf = NULL;
        if (sz) {
            sz += 16 * (dsz ? dsz : 48);
            buf = gk3_alloc(sz);
            if (!buf) {
                FAIL("alloc %llu", (unsigned long long)sz);
                return;
            }
        }
        st = gk3_bs->GetMemoryMap(&sz, (EFI_MEMORY_DESCRIPTOR *)buf, &key, &dsz, &dver);
    }
    if (EFI_ERROR(st) || !buf || dsz < sizeof(EFI_MEMORY_DESCRIPTOR)) {
        FAIL("GetMemoryMap: %s", gk3_efi_strerror(st));
        gk3_free(buf);
        return;
    }
    uint64_t pages[17] = {0}, cnt[17] = {0}, total = 0, top = 0, big = 0, big_at = 0;
    unsigned nrt = 0;
    UINTN nd = sz / dsz;
    for (UINTN i = 0; i < nd; i++) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)(buf + i * dsz);
        unsigned ty = d->Type < 16 ? d->Type : 16;
        pages[ty] += d->NumberOfPages;
        cnt[ty]++;
        total += d->NumberOfPages;
        uint64_t end = d->PhysicalStart + d->NumberOfPages * 4096;
        if (end > top)
            top = end;
        if (d->Type == EfiConventionalMemory && d->NumberOfPages > big) {
            big = d->NumberOfPages;
            big_at = d->PhysicalStart;
        }
        if (d->Attribute & EFI_MEMORY_RUNTIME)
            nrt++;
        gk3_logd("  %-12s 0x%011llx-0x%011llx %8llu pages attr=0x%llx\n", mem_type(d->Type),
                 (unsigned long long)d->PhysicalStart, (unsigned long long)(end - 1),
                 (unsigned long long)d->NumberOfPages, (unsigned long long)d->Attribute);
    }
    gk3_logf("memmap: %llu descriptors (desc_size=%llu ver=%u) total=%llu MiB top=0x%llx runtime_desc=%u\n",
             (unsigned long long)nd, (unsigned long long)dsz, dver, (unsigned long long)(total >> 8),
             (unsigned long long)top, nrt);
    for (unsigned t = 0; t < 17; t++)
        if (cnt[t])
            gk3_logf("  %-12s %4llu desc %8llu MiB\n", t < 16 ? mem_type(t) : "Other", (unsigned long long)cnt[t],
                     (unsigned long long)(pages[t] >> 8));
    gk3_logf("largest_conventional: %llu MiB @0x%llx\n", (unsigned long long)(big >> 8), (unsigned long long)big_at);
    gk3_free(buf);
}

/* ------------------------------------------------------------------ 9. USB 与其他协议：只 LocateHandleBuffer / LocateProtocol，不调用 */

static void step_protocols(void)
{
    static char dp[512];
    static const struct {
        const EFI_GUID *g;
        const char *n;
    } hp[] = {
        {&gk3_guid_usb_device, "EFI_USB_DEVICE_PROTOCOL(qcom d9d9ce48)"},
        {&gk3_guid_usbfn_io, "EFI_USBFN_IO_PROTOCOL"},
        {&gk3_guid_usb_io, "EFI_USB_IO_PROTOCOL"},
        {&gk3_guid_usb2_hc, "EFI_USB2_HC_PROTOCOL"},
        {&gk3_guid_rng, "EFI_RNG_PROTOCOL"},
        {&gk3_guid_tcg2, "EFI_TCG2_PROTOCOL"},
        {&gk3_guid_dt_fixup, "EFI_DT_FIXUP_PROTOCOL"},
        {&gk3_guid_memory_attribute, "EFI_MEMORY_ATTRIBUTE_PROTOCOL"},
    };
    sect("protocols (LocateHandleBuffer only, nothing is called)");
    for (size_t i = 0; i < sizeof(hp) / sizeof(hp[0]); i++) {
        UINTN n = 0;
        EFI_HANDLE *hs = NULL;
        EFI_STATUS st = gk3_handles(hp[i].g, &n, &hs);
        if (st == EFI_NOT_FOUND || (!EFI_ERROR(st) && n == 0)) {
            gk3_logf("%s: absent\n", hp[i].n);
        } else if (EFI_ERROR(st)) {
            gk3_logf("%s: %s\n", hp[i].n, gk3_efi_strerror(st));
        } else {
            gk3_logf("%s: %llu handle(s)\n", hp[i].n, (unsigned long long)n);
            for (UINTN k = 0; k < n && k < 16; k++) {
                gk3_dp_text(gk3_dp_of(hs[k]), dp, sizeof(dp));
                gk3_logf("  [%llu] %s\n", (unsigned long long)k, dp);
            }
        }
        gk3_free(hs);
    }
}

/* ------------------------------------------------------------------ 10. 缓冲区 LoadImage + StartImage（E4 门槛的前置） */

/* 本探针自己的厂商 GUID：给缓冲区镜像一个可辨认的设备路径（同 systemd-boot linux.c:43-69 的做法） */
static const EFI_GUID payload_guid = {0x646eb8a6, 0x6379, 0x4025, {0x86, 0x78, 0x1d, 0x5b, 0x68, 0x1c, 0xa7, 0x93}};

static void loadimage_variant(const char *label, bool with_dp)
{
    static char mark[256], dp[512];
    struct __attribute__((packed)) {
        uint8_t type, sub;
        uint16_t len;
        EFI_GUID g;
        uint8_t et, es;
        uint16_t elen;
    } vdp = {4, 3, 20, payload_guid, 0x7f, 0xff, 4};
    gk3_child_ctx ctx;
    EFI_HANDLE h = NULL;
    size_t size = (size_t)(gk3_child_pe_end - gk3_child_pe);

    gk3_memset(&ctx, 0, sizeof(ctx));
    gk3_memset(mark, 0, sizeof(mark));
    ctx.magic = GK3_CHILD_MAGIC;
    ctx.version = GK3_CHILD_VERSION;
    ctx.size = sizeof(ctx);
    ctx.buf = mark;
    ctx.buf_len = sizeof(mark);

    uint64_t t = gk3_ticks();
    EFI_STATUS st = gk3_bs->LoadImage(FALSE, gk3_image, with_dp ? (EFI_DEVICE_PATH_PROTOCOL *)&vdp : NULL,
                                      (void *)gk3_child_pe, size, &h);
    uint64_t us_l = gk3_us_since(t);
    gk3_logf("loadimage[%s]: LoadImage %s in %llu.%llu ms handle=%p\n", label, gk3_efi_strerror(st), MSF(us_l), h);
    if (EFI_ERROR(st)) {
        n_err++;
        if (h && st == EFI_SECURITY_VIOLATION) {
            EFI_STATUS u = gk3_bs->UnloadImage(h);
            gk3_logf("loadimage[%s]: UnloadImage after SECURITY_VIOLATION: %s\n", label, gk3_efi_strerror(u));
        }
        return;
    }
    EFI_LOADED_IMAGE_PROTOCOL *cli = NULL;
    st = gk3_bs->HandleProtocol(h, (EFI_GUID *)&gk3_guid_loaded_image, (void **)&cli);
    if (EFI_ERROR(st) || !cli) {
        FAIL("loadimage[%s]: child LoadedImage: %s", label, gk3_efi_strerror(st));
        gk3_bs->UnloadImage(h);
        return;
    }
    gk3_dp_text(cli->FilePath, dp, sizeof(dp));
    gk3_logf("loadimage[%s]: child base=%p size=0x%llx code_type=%u file_path=%s\n", label, cli->ImageBase,
             (unsigned long long)cli->ImageSize, cli->ImageCodeType, dp);
    cli->LoadOptions = &ctx;
    cli->LoadOptionsSize = sizeof(ctx);
    gk3_log_sync();              /* StartImage 万一把机器带走，前面的结果已经在盘上了 */

    UINTN xs = 0;
    CHAR16 *xd = NULL;
    t = gk3_ticks();
    st = gk3_bs->StartImage(h, &xs, &xd);
    uint64_t us_s = gk3_us_since(t);
    bool ok = !EFI_ERROR(st) && ctx.written > 0 && !gk3_memcmp(mark, GK3_CHILD_MARKER, sizeof(GK3_CHILD_MARKER) - 1);
    gk3_logf("loadimage[%s]: StartImage %s in %llu.%llu ms exit_data=%llu marker=\"%s\"\n", label,
             gk3_efi_strerror(st), MSF(us_s), (unsigned long long)xs, mark);
    gk3_logf("loadimage[%s]: child saw load_options_size=%u parent=0x%llx -> %s\n", label, ctx.child_load_options_size,
             (unsigned long long)ctx.child_parent_handle,
             ok ? "PASS (buffer LoadImage + StartImage + LoadOptions work)" : "FAIL");
    if (!ok)
        n_err++;
    if (xd)
        gk3_bs->FreePool(xd);
}

static void step_loadimage(void)
{
    size_t size = (size_t)(gk3_child_pe_end - gk3_child_pe);
    sect("buffer LoadImage + StartImage (test PE)");
    gk3_logf("test_pe: %llu bytes, starts with %02x %02x\n", (unsigned long long)size, gk3_child_pe[0],
             gk3_child_pe[1]);
    loadimage_variant("vendor-dp", true);
    loadimage_variant("null-dp", false);
}

/* ------------------------------------------------------------------ 收尾 */

static void finish(void)
{
    sect("done");
    gk3_logf("probe.done errors=%u total=%llu ms\n", n_err, MS());
    gk3_log_sync();
    if (gk3_lg.file) {
        EFI_FILE_PROTOCOL *f = gk3_lg.file;
        bool failed = gk3_lg.file_failed;
        EFI_STATUS cst = f->Close(f);
        gk3_lg.file = NULL;
        /* 读回核对（只上屏幕：文件已经关了） */
        bool same = false;
        if (!failed && !EFI_ERROR(cst) && g_logdir) {
            EFI_FILE_PROTOCOL *r = NULL;
            if (!EFI_ERROR(g_logdir->Open(g_logdir, &r, g_log_name, EFI_FILE_MODE_READ, 0)) && r) {
                uint8_t *back = gk3_alloc(gk3_lg.synced + 16);
                UINTN n = gk3_lg.synced + 16;
                if (back && !EFI_ERROR(r->Read(r, &n, back)))
                    same = n == gk3_lg.synced && !gk3_memcmp(back, gk3_lg.buf, n);
                gk3_free(back);
                r->Close(r);
            }
        }
        if (failed)
            gk3_screenf("!! log file write failed: %s (log is incomplete)\r\n", gk3_efi_strerror(gk3_lg.file_err));
        gk3_screenf("log file log-%u.txt: %llu bytes, close=%s, read-back %s\n", g_log_n,
                    (unsigned long long)gk3_lg.synced, gk3_efi_strerror(cst), same ? "MATCHES" : "DIFFERS/FAILED");
    } else {
        gk3_screenf("!! no log file was written (see errors above)\n");
    }
    if (g_logdir)
        g_logdir->Close(g_logdir);
    if (opt_noreset) {
        gk3_screenf("noreset=1: returning EFI_SUCCESS to the boot manager (debug only)\n");
        return;
    }
    for (unsigned s = opt_hold; s > 0; s--) {
        gk3_screenf("cold reset in %u s ...\n", s);
        gk3_bs->Stall(1000000);
    }
    gk3_screenf("gk3probe: ResetSystem(EfiResetCold)\n");
    gk3_rt->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);
    /* 不该回来。回来了就等看门狗（绝不把错误返回给固件） */
    gk3_screenf("!! ResetSystem returned; waiting for the %u s watchdog\n", WATCHDOG_SEC);
    for (;;)
        gk3_bs->Stall(1000000);
}

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    gk3efi_init(image, st);
    T0 = gk3_ticks();
    gk3_log_init(LOG_CAP);
    gk3_screenf("\n");         /* systemd-boot 的菜单可能把光标留在行中间；只上屏幕，不进文件 */
    step_header();
    step_keys();
    step_image();
    step_open_log();
    step_firmware();
    step_vars();
    step_disk();
    step_misc();
    step_boot("boot_a");
    step_boot("boot_b");
    step_display();
    step_memmap();
    step_protocols();
    step_loadimage();
    finish();
    return EFI_SUCCESS;     /* 只有 noreset=1 走到这里 */
}
