/* Windows：从一块盘上卸载 Android，回到只有 Windows。顺序与规则同 scripts/windows/gaokun3-setup.ps1 的
 * -RemoveAndroid（Invoke-RemoveAndroid）：
 *
 *   gk3w uninstall --disk N --backup-dir DIR [--real-disk [--bitlocker-key yes]]
 *
 * 0 预检（只读；不过就什么都不改）：
 *   - 分区：misc、metadata、boot_a、boot_b、super、gk3rescue、userdata 中盘上有的。每个名字至多一个、必须是 Linux 数据
 *     类型；开头 8 KiB 是 NTFS / BitLocker / FAT / exFAT 的拒绝；misc 不大于 GK3W_MISC_MIB；boot_a、boot_b、super 里
 *     至少一个认得出（ANDROID! / LP 元数据）—— 同 Test-RemovalCandidates。都认不出时，只在名字、先后、大小与相邻关系
 *     和安装器排出来的一模一样时才删（apply 建好分区、还没写 super / boot 就中断了：内容还认不出，但这样一组分区
 *     不可能是别人的）。
 *   - ESP：loader/entries 里有不是我们写的 .conf（gk3w_our_entry_name），或 EFI/Linux 下有 .efi（UKI），就算 ESP 上
 *     还有别的系统（foreign）。
 * 1 还原引导（没有 foreign 时）。记 cur = 现在的 EFI/BOOT/BOOTAA64.EFI，W = EFI/Microsoft/Boot/bootmgfw.efi，
 *   B = BOOTAA64.EFI.before-gaokun3（apply 覆盖回落路径前留的原件；B 是 systemd-boot 时当作没有：那是我们自己
 *   早先装的，或者一个已经没有配置的 systemd-boot，换回去只会得到一个空菜单）。按顺序取第一条成立的：
 *   a. 有 .before-gaokun3（不论是不是 systemd-boot），而 cur 既不是 systemd-boot、也不是 Windows 启动管理器、也不
 *      等于 W 或 B：装完之后别的系统换上了自己的引导程序（例如 grub-install --removable）—— 回落路径留着。
 *      .before-gaokun3 删了不丢（systemd-boot，或 Windows 启动管理器而 W 还在）就删，否则下次安装不会再留 cur 的
 *      原件；是别的系统的原件就也留着（下次安装见到这种情况会拒绝，见 apply.c）。没有 .before-gaokun3 时同样的 cur 归 d。
 *   b. B 不是 Windows 启动管理器（与 W 不同、版本资源也不是）：装之前回落路径上是别的系统的引导程序 —— 换回 B，
 *      不能把它唯一的一份删掉。
 *   c. 有 B，或 cur 是 systemd-boot：换成【当前的】W（Windows 之后更新过的启动管理器只在 EFI/Microsoft/Boot 里，
 *      B 是装机那天的拷贝）；没有 W 才用 B；两样都没有就拒绝（删完没有东西能启动）。
 *   d. cur 不存在而有 W（例如改名替换时掉电）：放回 W，同 PS1。其余（没有可用的 B，cur 已经是 Windows 启动管理器
 *      或别的引导程序）：这一套安装没动过回落路径（例如 apply 在写 ESP 之前就中断了），原样留着；PS1 则一律换成 W。
 *   回落路径上已经是要换的那个文件时不写；要写时先查 ESP 空闲够不够，写成 .tmp 再改名。
 *   必须在删分区之前：反过来的话 systemd-boot 仍默认进一个已经不存在的 Android。
 * 2 删 ESP 上我们的东西：我们的启动项；含 android/ 或 rescue/ 的 <mid>/ 里的这两个目录（<mid>/ 空了才删）；
 *   EFI/gk3boot、EFI/gaokun3；没有 foreign 时再删 loader/、EFI/systemd/，最后、并且前面全部成功时才删
 *   BOOTAA64.EFI.before-gaokun3（中途失败时它还在，重跑仍有还原的来源；1a 里它是别的系统的原件时不删）。
 *   有 foreign 时这三样留着、回落路径也不动（放谁由用户决定），只把 loader.conf 从 .before-gaokun3 换回来。
 * 3 删变量（真实磁盘）：LoaderEntryDefault、LoaderEntryOneShot；有 foreign 时只删看得出是我们写的值。
 * 4 删分区：先重读分区表、与预检时的逐项比对，变过就停（不能拿旧表盖掉别的程序刚做的改动）。
 * 中途失败时再跑一次：每一步都按 ESP 与分区表的现状来认，做过的步骤再做一遍结果不变。
 *
 * 与 -RemoveAndroid 的差别（原型阶段）：GK3LIVE 不删、C: 不扩、BitLocker 不暂停（改为要求 --bitlocker-key yes，
 * 共用 ESP、不动回落路径时也要求）；免 U 盘安装旧版留下的 BCD 启动项不删（EFI/gaokun3 照样删）；
 * 盘上没有 Android 分区时也清 ESP（PS1 拒绝）；<mid>/ 不整个删（它也可能是别的系统的 BLS 目录）；EFI/Linux 下的
 * UKI 也算 foreign；装机时的分区组也认（见上）；回落路径原来不是 Windows 时换回原件（见上）。 */
#include <windows.h>
#include <winioctl.h>

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../esp.h"
#include "../layout.h"
#include "../plan.h"
#include "../proto.h"
#include "disk.h"
#include "gk3core.h"
#include "win.h"

static const wchar_t *const OUR_NAMES[] = {L"misc", L"metadata", L"boot_a", L"boot_b", L"super", L"gk3rescue", L"userdata"};
#define N_OURS (sizeof OUR_NAMES / sizeof OUR_NAMES[0])
#define HEAD_BYTES 8192u

static const wchar_t FALLBACK[] = L"EFI\\BOOT\\BOOTAA64.EFI";
static const wchar_t FALLBACK_BAK[] = L"EFI\\BOOT\\BOOTAA64.EFI.before-gaokun3";
static const wchar_t BOOTMGFW[] = L"EFI\\Microsoft\\Boot\\bootmgfw.efi";

typedef struct {
    const wchar_t *root;        /* \\?\Volume{…}\ */
    DWORD first_err;            /* 第一次失败的 Win32 错误码 */
    int bad;
} esp_ctx;

static void note(const char *what, const wchar_t *rel)
{
    gk3w_line l = {0};
    char r[MAX_PATH * 3];
    gk3w_w2u(rel, r, sizeof r);
    gk3w_line_begin(&l, "UNINSTALL");
    gk3w_line_kv(&l, what, r);
    (void)gk3w_line_emit(&l, stdout);
    gk3w_line_free(&l);
}

static void fail(esp_ctx *e, const char *what, const wchar_t *rel)
{
    DWORD err = GetLastError();
    char r[MAX_PATH * 3];
    gk3w_w2u(rel, r, sizeof r);
    gk3w_log("cannot %s %s (%lu)", what, r, (unsigned long)err);
    if (!e->bad)
        e->first_err = err;
    e->bad = 1;
}

static void full_path(const esp_ctx *e, const wchar_t *rel, wchar_t *out)
{
    _snwprintf(out, MAX_PATH * 2, L"%ls%ls", e->root, rel);
    out[MAX_PATH * 2 - 1] = 0;
}

static int is_dir(const wchar_t *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

/* 删一个文件；本来就没有不算错 */
static void rm_file(esp_ctx *e, const wchar_t *rel)
{
    wchar_t p[MAX_PATH * 2];
    full_path(e, rel, p);
    if (!gk3w_exists(p))
        return;
    SetFileAttributesW(p, FILE_ATTRIBUTE_NORMAL);
    if (!DeleteFileW(p))
        fail(e, "remove", rel);
    else
        note("esp-file", rel);
}

/* 整个目录删掉（不跟随重解析点）；本来就没有不算错 */
static int rm_tree(const wchar_t *dir)
{
    wchar_t pat[MAX_PATH * 2];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int rc = 0;
    _snwprintf(pat, MAX_PATH * 2, L"%ls\\*", dir);
    pat[MAX_PATH * 2 - 1] = 0;
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        return (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) ? 0 : -1;
    }
    do {
        wchar_t child[MAX_PATH * 2];
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        _snwprintf(child, MAX_PATH * 2, L"%ls\\%ls", dir, fd.cFileName);
        child[MAX_PATH * 2 - 1] = 0;
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            if (rm_tree(child))
                rc = -1;
        } else {
            SetFileAttributesW(child, FILE_ATTRIBUTE_NORMAL);
            if (!DeleteFileW(child))
                rc = -1;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (!rc && !RemoveDirectoryW(dir))
        rc = -1;
    return rc;
}

static void rm_dir(esp_ctx *e, const wchar_t *rel)
{
    wchar_t p[MAX_PATH * 2];
    full_path(e, rel, p);
    if (!gk3w_exists(p))
        return;
    if (rm_tree(p))
        fail(e, "remove", rel);
    else
        note("esp-dir", rel);
}

/* 32 位十六进制（不分大小写，同 PS1 的 '^[0-9a-fA-F]{32}$'） */
static int is_hex32(const wchar_t *s)
{
    size_t n = 0;
    for (; s[n]; n++)
        if (!((s[n] >= L'0' && s[n] <= L'9') || (s[n] >= L'a' && s[n] <= L'f') || (s[n] >= L'A' && s[n] <= L'F')))
            return 0;
    return n == 32;
}

/* <mid>/ 里有 android/ 或 rescue/ 吗 */
static int is_install_dir(const esp_ctx *e, const wchar_t *mid)
{
    wchar_t rel[MAX_PATH], p[MAX_PATH * 2];
    _snwprintf(rel, MAX_PATH, L"%ls\\android", mid);
    full_path(e, rel, p);
    if (is_dir(p))
        return 1;
    _snwprintf(rel, MAX_PATH, L"%ls\\rescue", mid);
    full_path(e, rel, p);
    return is_dir(p);
}

/* ESP 现状（只读）：我们的启动项、安装目录、别的系统的启动项 */
typedef struct {
    unsigned our_entries, install_dirs, foreign;
    int bootmgfw, fallback_bak, gk3boot, gaokun3;
} esp_scan;

static void scan_esp(const esp_ctx *e, esp_scan *s)
{
    wchar_t pat[MAX_PATH * 2], p[MAX_PATH * 2];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    memset(s, 0, sizeof *s);
    full_path(e, L"loader\\entries\\*", pat);
    h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            char u[MAX_PATH * 3];
            size_t n;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;
            gk3w_w2u(fd.cFileName, u, sizeof u);
            n = strlen(u);
            if (gk3w_our_entry_name(u))
                s->our_entries++;
            else if (n >= 5 && !_stricmp(u + n - 5, ".conf")) {   /* 同 PS1：只有 .conf 算别人的启动项 */
                s->foreign++;
                gk3w_log("boot entry of another system: loader/entries/%s", u);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    full_path(e, L"EFI\\Linux\\*.efi", pat);
    h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            char u[MAX_PATH * 3];
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;
            gk3w_w2u(fd.cFileName, u, sizeof u);
            s->foreign++;
            gk3w_log("unified kernel image of another system: EFI/Linux/%s", u);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    full_path(e, L"*", pat);
    h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && is_hex32(fd.cFileName) && is_install_dir(e, fd.cFileName))
                s->install_dirs++;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    full_path(e, BOOTMGFW, p);
    s->bootmgfw = gk3w_exists(p);
    full_path(e, FALLBACK_BAK, p);
    s->fallback_bak = gk3w_exists(p);
    full_path(e, L"EFI\\gk3boot", p);
    s->gk3boot = gk3w_exists(p);
    full_path(e, L"EFI\\gaokun3", p);
    s->gaokun3 = gk3w_exists(p);
}

/* 第 2 步的前半：我们的启动项、<mid>/android|rescue */
static void rm_entries_and_dirs(esp_ctx *e)
{
    wchar_t pat[MAX_PATH * 2];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    full_path(e, L"loader\\entries\\*", pat);
    h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            char u[MAX_PATH * 3];
            wchar_t rel[MAX_PATH];
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;
            gk3w_w2u(fd.cFileName, u, sizeof u);
            if (!gk3w_our_entry_name(u))
                continue;
            _snwprintf(rel, MAX_PATH, L"loader\\entries\\%ls", fd.cFileName);
            rel[MAX_PATH - 1] = 0;
            rm_file(e, rel);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    full_path(e, L"*", pat);
    h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            wchar_t rel[MAX_PATH], p[MAX_PATH * 2];
            int was_bad = e->bad;
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !is_hex32(fd.cFileName) || !is_install_dir(e, fd.cFileName))
                continue;
            _snwprintf(rel, MAX_PATH, L"%ls\\android", fd.cFileName);
            rm_dir(e, rel);
            _snwprintf(rel, MAX_PATH, L"%ls\\rescue", fd.cFileName);
            rm_dir(e, rel);
            full_path(e, fd.cFileName, p);
            if (e->bad == was_bad && RemoveDirectoryW(p))   /* 空了才删得掉：里面还有别的系统的东西就留着 */
                note("esp-dir", fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

/* 有 <rel>.before-gaokun3 就把它换回 <rel>、删掉备份（有 foreign 时的 loader.conf） */
static void restore_backup(esp_ctx *e, const wchar_t *rel)
{
    wchar_t p[MAX_PATH * 2], b[MAX_PATH * 2];
    uint8_t *data;
    size_t len;
    full_path(e, rel, p);
    _snwprintf(b, MAX_PATH * 2, L"%ls.before-gaokun3", p);
    b[MAX_PATH * 2 - 1] = 0;
    if (!gk3w_exists(b))
        return;
    if (gk3w_read_file(b, &data, &len, 64u << 20)) {
        fail(e, "read the backup of", rel);
        return;
    }
    if (gk3w_replace_file_verify(p, data, len)) {
        free(data);
        fail(e, "restore", rel);
        return;
    }
    free(data);
    note("esp-restored", rel);
    SetFileAttributesW(b, FILE_ATTRIBUTE_NORMAL);
    if (!DeleteFileW(b))
        fail(e, "remove the backup of", rel);
}

/* 两段内容是否相同（任一为 NULL 算不同） */
static int same(const uint8_t *a, size_t al, const uint8_t *b, size_t bl)
{
    return a && b && al == bl && !memcmp(a, b, al);
}

/* 分区开头 HEAD_BYTES 字节认内容；读不了返回 -1 */
static int read_kind(gk3w_disk *d, const PARTITION_INFORMATION_EX *p, uint8_t *buf, gk3w_content_kind *k)
{
    uint64_t len = (uint64_t)p->PartitionLength.QuadPart;
    DWORD n = len < HEAD_BYTES ? (DWORD)(len / d->bs * d->bs) : HEAD_BYTES;
    if (!n || gk3w_pread(d->h, buf, n, (uint64_t)p->StartingOffset.QuadPart))
        return -1;
    *k = gk3w_content_kind_of(buf, n);
    return 0;
}

/* 我们的分区按起点排好后，是不是正好是安装器排出来的那一组（plan.c 的 gk3w_plan_alongside）：misc、metadata、
 * boot_a、boot_b、super、[gk3rescue、]userdata 依次首尾相接，前面几个大小分毫不差，userdata 不小于下限，misc 起点按
 * 1 MiB 对齐。have / first / bytes 按 OUR_NAMES 的下标。 */
static int apply_shaped(const int *have, const uint64_t *first, const uint64_t *bytes)
{
    /* OUR_NAMES 的下标：misc 0、metadata 1、boot_a 2、boot_b 3、super 4、gk3rescue 5、userdata 6 */
    static const unsigned order[] = {0, 1, 2, 3, 4, 5, 6};
    static const uint64_t mib[] = {GK3W_MISC_MIB, GK3W_METADATA_MIB, GK3W_BOOT_MIB, GK3W_BOOT_MIB, GK3W_SUPER_MIB, GK3W_RESCUE_MIB, 0};
    uint64_t next = 0;
    int started = 0;
    for (size_t i = 0; i < sizeof order / sizeof order[0]; i++) {
        unsigned k = order[i];
        if (!have[k]) {
            if (k == 5)
                continue;   /* gk3rescue 可有可无 */
            return 0;
        }
        if (started && first[k] != next)
            return 0;
        if (!started && (first[k] & ((1u << 20) - 1)))
            return 0;
        if (mib[k] ? bytes[k] != (mib[k] << 20) : bytes[k] < ((uint64_t)GK3W_USERDATA_MIN_MIB << 20))
            return 0;
        next = first[k] + bytes[k];
        started = 1;
    }
    return 1;
}

int gk3w_cmd_uninstall(int argc, wchar_t **argv)
{
    uint64_t num;
    const wchar_t *backup_dir = gk3w_arg(argc, argv, L"--backup-dir"), *blkey = gk3w_arg(argc, argv, L"--bitlocker-key");
    int real = gk3w_flag(argc, argv, L"--real-disk"), rc = 1, n_esp = 0, confirmed = 0, have[N_OURS] = {0}, restore_original = 0, need_restore = 0, keep_bak_file = 0;
    uint64_t pfirst[N_OURS] = {0}, pbytes[N_OURS] = {0};
    gk3w_touched T = GK3W_TOUCHED_NO;
    gk3w_disk d;
    GUID ids[N_OURS];
    size_t n_ids = 0;
    const PARTITION_INFORMATION_EX *esp_part = NULL;
    DRIVE_LAYOUT_INFORMATION_EX *snap = NULL;
    uint8_t *head = NULL, *winboot = NULL;
    size_t winboot_len = 0;
    wchar_t root[MAX_PATH];
    esp_ctx e = {root, 0, 0};
    esp_scan s;

    if (gk3w_arg_u64(argc, argv, L"--disk", &num) || num > 255 || !backup_dir)
        return gk3w_err(T, "usage", "usage: uninstall --disk N --backup-dir DIR [--real-disk [--bitlocker-key yes]]", 0);
    if (real) {
        gk3w_sys sys;
        gk3w_sys_read(&sys);
        if (gk3w_sys_gate(&sys, blkey && !wcscmp(blkey, L"yes"), GK3W_GATE_UNINSTALL))
            return 1;
    }
    if (gk3w_disk_open((DWORD)num, real, &d, T))
        return 1;
    snap = malloc(d.laysz);
    head = _aligned_malloc(HEAD_BYTES, 4096);
    if (!snap || !head) {
        gk3w_err(T, "nomem", "out of memory", 0);
        goto out;
    }
    memcpy(snap, d.lay, d.laysz);

    /* ── 0 预检：分区 ── */
    for (size_t k = 0; k < N_OURS; k++) {
        int hits = 0;
        char nm[32];
        gk3w_w2u(OUR_NAMES[k], nm, sizeof nm);
        for (DWORD i = 0; i < d.lay->PartitionCount; i++) {
            const PARTITION_INFORMATION_EX *p = &d.lay->PartitionEntry[i];
            wchar_t wn[GK3_GPT_NAME_CHARS + 1];
            gk3w_content_kind kind;
            if (p->PartitionStyle != PARTITION_STYLE_GPT || !p->PartitionLength.QuadPart)
                continue;
            memcpy(wn, p->Gpt.Name, sizeof p->Gpt.Name);
            wn[GK3_GPT_NAME_CHARS] = 0;
            if (wcscmp(wn, OUR_NAMES[k]))
                continue;
            if (memcmp(&p->Gpt.PartitionType, &GK3W_LINUX_DATA, sizeof(GUID))) {
                gk3w_err_kv(T, "bad-part-type", "name", nm, "a partition with this name is not of the Linux data type, so it is not ours; leaving it (disk not touched)", 0);
                goto out;
            }
            if (hits) {
                gk3w_err_kv(T, "partlabel-conflict", "names", nm, "more than one partition has this name; cannot tell which one to remove (disk not touched)", 0);
                goto out;
            }
            if (read_kind(&d, p, head, &kind)) {
                gk3w_err_kv(T, "disk-read", "name", nm, "cannot read the start of the partition (disk not touched)", GetLastError());
                goto out;
            }
            gk3w_log("partition %s: %llu MiB, content %s", nm, (unsigned long long)(p->PartitionLength.QuadPart >> 20), gk3w_content_kind_name(kind));
            if (kind == GK3W_KIND_NTFS || kind == GK3W_KIND_BITLOCKER || kind == GK3W_KIND_FAT || kind == GK3W_KIND_EXFAT) {
                gk3w_err_kv(T, "foreign-content", "name", nm, "this partition holds a Windows file system, so it is not an Android partition (disk not touched)", 0);
                goto out;
            }
            if (k == 0 && (uint64_t)p->PartitionLength.QuadPart > ((uint64_t)GK3W_MISC_MIB << 20)) {
                gk3w_err_kv(T, "misc-too-big", "name", nm, "misc is larger than the installer ever creates it (disk not touched)", 0);
                goto out;
            }
            if (((k == 2 || k == 3) && kind == GK3W_KIND_ANDROID_BOOT) || (k == 4 && kind == GK3W_KIND_LP))
                confirmed++;
            ids[n_ids++] = p->Gpt.PartitionId;
            have[k] = 1;
            pfirst[k] = (uint64_t)p->StartingOffset.QuadPart;
            pbytes[k] = (uint64_t)p->PartitionLength.QuadPart;
            hits++;
        }
    }
    if (n_ids && !confirmed) {
        if (!apply_shaped(have, pfirst, pbytes)) {
            gk3w_err(T, "android-unconfirmed", "none of boot_a, boot_b and super holds an Android boot image or LP metadata, and the partitions are not laid out the way the installer creates them; refusing to remove partitions by name alone (disk not touched)", 0);
            goto out;
        }
        gk3w_log("no Android content found yet, but the partitions are laid out exactly as the installer creates them (an interrupted install): removing them");
    }

    /* ── 0 预检：ESP ── */
    for (DWORD i = 0; i < d.lay->PartitionCount; i++) {
        const PARTITION_INFORMATION_EX *p = &d.lay->PartitionEntry[i];
        if (p->PartitionStyle == PARTITION_STYLE_GPT && p->PartitionLength.QuadPart &&
            !memcmp(&p->Gpt.PartitionType, &GK3W_ESP_TYPE, sizeof(GUID))) {
            esp_part = p;
            n_esp++;
        }
    }
    if (n_esp != 1) {
        gk3w_err(T, n_esp ? "esp-ambiguous" : "esp-missing", n_esp ? "this disk has more than one EFI system partition (disk not touched)" : "this disk has no EFI system partition (disk not touched)", 0);
        goto out;
    }
    if (gk3w_find_volume(d.num, (uint64_t)esp_part->StartingOffset.QuadPart, root, MAX_PATH)) {
        gk3w_err(T, "esp-mount", "cannot find the volume of the EFI system partition (disk not touched)", 0);
        goto out;
    }
    esp_part = NULL;
    scan_esp(&e, &s);
    /* 真实磁盘：同 apply —— gk3w 运行在这块盘上已经启动的 Windows 里，bootmgfw.efi 不在说明 Windows 引导不完整 */
    if (real && !s.bootmgfw) {
        gk3w_err(T, "no-windows-bootmgr", "EFI/Microsoft/Boot/bootmgfw.efi is missing from the EFI system partition: the Windows boot files of this disk are incomplete; repair them first (disk not touched)", 0);
        goto out;
    }
    if (!n_ids && !s.our_entries && !s.install_dirs && !s.gk3boot && !s.gaokun3 && !s.fallback_bak) {
        gk3w_log("no Android partitions on the disk and no Android files on the EFI system partition: nothing to uninstall");
        rc = 0;
        goto out;
    }
    if (!s.foreign) {
        wchar_t p[MAX_PATH * 2];
        uint8_t *bak = NULL, *cur = NULL;
        size_t bak_len = 0, cur_len = 0;
        ULARGE_INTEGER avail;
        full_path(&e, FALLBACK, p);
        if (gk3w_exists(p) && gk3w_read_file(p, &cur, &cur_len, 64u << 20)) {
            gk3w_err(T, "esp-unreadable", "cannot read EFI/BOOT/BOOTAA64.EFI (disk not touched)", GetLastError());
            goto out;
        }
        if (s.bootmgfw) {
            full_path(&e, BOOTMGFW, p);
            if (gk3w_read_file(p, &winboot, &winboot_len, 64u << 20)) {
                gk3w_err(T, "esp-unreadable", "cannot read the Windows Boot Manager on the EFI system partition (disk not touched)", GetLastError());
                free(cur);
                goto out;
            }
        }
        if (s.fallback_bak) {
            full_path(&e, FALLBACK_BAK, p);
            if (gk3w_read_file(p, &bak, &bak_len, 64u << 20)) {
                free(cur);
                gk3w_err(T, "esp-unreadable", "cannot read EFI/BOOT/BOOTAA64.EFI.before-gaokun3 (disk not touched)", GetLastError());
                goto out;
            }
        }
        {
            /* 回落路径怎么还原：开头注释里 1a–1d 的判断表 */
            uint8_t *target = NULL;
            size_t target_len = 0;
            switch (gk3w_fallback_decide(cur, cur_len, winboot, winboot_len, bak, bak_len)) {
            case GK3W_FB_LEAVE_OTHER:
                /* .before-gaokun3 删了不丢就删（否则下次安装不会再留现在这个引导程序的原件）；是别人的原件就留着 */
                keep_bak_file = !gk3w_fallback_bak_disposable(bak, bak_len, winboot, winboot_len);
                gk3w_log("EFI/BOOT/BOOTAA64.EFI now holds another system's boot loader (neither systemd-boot nor the Windows Boot Manager): left as it is%s",
                         keep_bak_file ? "; EFI/BOOT/BOOTAA64.EFI.before-gaokun3 (the different loader that was there before the install) is kept too" : "");
                break;
            case GK3W_FB_RESTORE_ORIGINAL:
                restore_original = 1;
                target = bak, target_len = bak_len;
                gk3w_log("before the install, EFI/BOOT/BOOTAA64.EFI was not the Windows Boot Manager (another system's boot loader?): restoring that original file");
                break;
            case GK3W_FB_USE_W:
                target = winboot, target_len = winboot_len;
                break;
            case GK3W_FB_USE_BAK:
                target = bak, target_len = bak_len;
                break;
            case GK3W_FB_NO_WINDOWS:
                free(cur);
                free(bak);
                gk3w_err(T, "no-windows-boot", "EFI/BOOT/BOOTAA64.EFI is systemd-boot, and the EFI system partition has neither the Windows Boot Manager nor a usable .before-gaokun3 backup to put back: nothing would be left to boot (disk not touched)", 0);
                goto out;
            case GK3W_FB_UNTOUCHED:
                if (cur)
                    gk3w_log("EFI/BOOT/BOOTAA64.EFI is not a file this install put there: left as it is");
                break;
            }
            /* 之后 winboot 就是要写进回落路径的内容（NULL = 不写） */
            if (target != winboot) {
                free(winboot);
                winboot = target;
                winboot_len = target_len;
                if (target == bak)
                    bak = NULL;
            }
            free(bak);
        }
        /* 回落路径上已经是要换回的那个文件（重跑时）就不用写 */
        need_restore = winboot && !same(cur, cur_len, winboot, winboot_len);
        free(cur);
        /* .tmp 与旧文件要同时放得下；不够就什么都不改 */
        if (need_restore) {
            BOOL ok = GetDiskFreeSpaceExW(root, &avail, NULL, NULL);
            if (!ok || avail.QuadPart < (ULONGLONG)winboot_len + (1u << 20)) {
                gk3w_err(T, "esp-full", "not enough free space on the EFI system partition to restore EFI/BOOT/BOOTAA64.EFI (disk not touched)", ok ? 0 : GetLastError());
                goto out;
            }
        }
    } else {
        gk3w_log("the EFI system partition is shared with another system: our entries are removed; loader/, EFI/systemd and the fallback EFI/BOOT/BOOTAA64.EFI stay as they are");
    }
    gk3w_log("%u partitions to remove; %u of our boot entries, %u install directories on the ESP", (unsigned)n_ids, s.our_entries, s.install_dirs);
    if (gk3w_gpt_backup(&d, backup_dir, L"uninstall", T))
        goto out;

    /* ══ 从这里起开始动盘 ══ */
    T = GK3W_TOUCHED_YES;

    /* ── 1 还原引导 ── */
    if (!s.foreign) {
        wchar_t p[MAX_PATH * 2], dir[MAX_PATH * 2];
        full_path(&e, FALLBACK, p);
        if (!need_restore) {
            if (winboot)
                gk3w_log("EFI/BOOT/BOOTAA64.EFI already holds the file to restore");
        } else {
            full_path(&e, L"EFI\\BOOT", dir);
            if (gk3w_mkdirs(dir, wcslen(root)) || gk3w_replace_file_verify(p, winboot, winboot_len)) {
                DWORD err = GetLastError();
                gk3w_err(T, "esp-write", "restoring EFI/BOOT/BOOTAA64.EFI failed (nothing else changed yet; run uninstall again)", err);
                goto out;
            }
            note("esp-restored", FALLBACK);
            if (restore_original)
                gk3w_log("EFI/BOOT/BOOTAA64.EFI = the file it held before the install (from the .before-gaokun3 copy)");
            else
                gk3w_log("EFI/BOOT/BOOTAA64.EFI = Windows Boot Manager (from %s)", s.bootmgfw ? "EFI/Microsoft/Boot/bootmgfw.efi" : "the .before-gaokun3 copy made at install time");
        }
    }

    /* ── 2 删 ESP 上我们的东西 ── */
    rm_entries_and_dirs(&e);
    rm_dir(&e, L"EFI\\gk3boot");
    rm_dir(&e, L"EFI\\gaokun3");
    /* 中断留下的临时文件（apply 写备份、本命令换回落路径时的 .tmp） */
    rm_file(&e, L"EFI\\BOOT\\BOOTAA64.EFI.before-gaokun3.tmp");
    rm_file(&e, L"EFI\\BOOT\\BOOTAA64.EFI.tmp");
    rm_file(&e, L"loader\\loader.conf.before-gaokun3.tmp");
    if (!s.foreign) {
        rm_dir(&e, L"loader");
        rm_dir(&e, L"EFI\\systemd");
        if (!e.bad && !keep_bak_file)   /* 最后删：前面有一步失败时留着它，重跑仍有还原的来源 */
            rm_file(&e, FALLBACK_BAK);
    } else {
        restore_backup(&e, L"loader\\loader.conf");
    }
    if (e.bad) {
        gk3w_err(T, "esp-write", "cleaning the EFI system partition failed: a file could not be removed or restored (partitions not removed yet; run uninstall again)", e.first_err);
        goto out;
    }

    /* ── 3 删变量（真实磁盘）：可能指着刚删掉的启动项 ── */
    if (real) {
        int (*ours)(const char *) = s.foreign ? gk3w_our_var_value : NULL;
        gk3w_loader_var_delete(L"LoaderEntryDefault", ours);
        gk3w_loader_var_delete(L"LoaderEntryOneShot", ours);
    }

    /* ── 4 删分区 ── */
    if (n_ids) {
        DWORD count = 0;
        DRIVE_LAYOUT_INFORMATION_EX *lay;
        if (gk3w_disk_read_layout(&d)) {
            gk3w_err(T, "verify-layout", "re-reading the partition table before removing partitions failed (EFI system partition already cleaned, partitions not removed)", GetLastError());
            goto out;
        }
        if (d.lay->PartitionCount != snap->PartitionCount ||
            memcmp(&d.lay->Gpt, &snap->Gpt, sizeof snap->Gpt) ||
            memcmp(d.lay->PartitionEntry, snap->PartitionEntry, snap->PartitionCount * sizeof(PARTITION_INFORMATION_EX))) {
            gk3w_err(T, "layout-changed", "the partition table changed during the uninstall; run uninstall again (EFI system partition already cleaned, partitions not removed)", 0);
            goto out;
        }
        lay = calloc(1, d.laysz);
        if (!lay) {
            gk3w_err(T, "nomem", "out of memory", 0);
            goto out;
        }
        lay->PartitionStyle = PARTITION_STYLE_GPT;
        lay->Gpt = snap->Gpt;
        for (DWORD i = 0; i < snap->PartitionCount; i++) {
            const PARTITION_INFORMATION_EX *p = &snap->PartitionEntry[i];
            int ours = 0;
            if (p->PartitionStyle != PARTITION_STYLE_GPT || !p->PartitionLength.QuadPart)
                continue;
            for (size_t k = 0; k < n_ids; k++)
                if (!memcmp(&p->Gpt.PartitionId, &ids[k], sizeof(GUID)))
                    ours = 1;
            if (!ours)
                lay->PartitionEntry[count++] = *p;
        }
        if (gk3w_disk_set_layout(&d, lay, count, T)) {
            free(lay);
            goto out;
        }
        free(lay);
        if (gk3w_disk_read_layout(&d)) {
            gk3w_err(T, "verify-layout", "re-reading the partition table after removing partitions failed", GetLastError());
            goto out;
        }
        for (size_t k = 0; k < n_ids; k++)
            if (gk3w_disk_find_part(&d, &ids[k])) {
                gk3w_err(T, "verify-layout", "after removing and re-reading, a partition is still there", 0);
                goto out;
            }
        gk3w_log("removed %u partitions", (unsigned)n_ids);
    }

    /* ── 报告现在的空闲区（给接下来的 apply 选） ── */
    {
        gk3w_span parts[GK3W_MAX_PARTS], spans[GK3W_MAX_PARTS + 1];
        size_t np = 0, nf = 0;
        uint64_t uf, ul;
        for (DWORD i = 0; i < d.lay->PartitionCount && np < GK3W_MAX_PARTS; i++) {
            const PARTITION_INFORMATION_EX *p = &d.lay->PartitionEntry[i];
            if (p->PartitionStyle == PARTITION_STYLE_GPT && p->PartitionLength.QuadPart)
                parts[np++] = (gk3w_span){(uint64_t)p->StartingOffset.QuadPart / d.bs,
                                          ((uint64_t)p->StartingOffset.QuadPart + (uint64_t)p->PartitionLength.QuadPart) / d.bs - 1};
        }
        gk3w_disk_usable(&d, &uf, &ul);
        if (gk3w_free_spans(uf, ul, parts, np, d.bs, 16u << 20, spans, GK3W_MAX_PARTS + 1, &nf) == GK3W_LAYOUT_OK)
            for (size_t i = 0; i < nf && i < GK3W_MAX_PARTS + 1; i++) {
                gk3w_line l = {0};
                gk3w_line_begin(&l, "FREE");
                gk3w_line_u64(&l, "disk", d.num);
                gk3w_line_u64(&l, "start", spans[i].first);
                gk3w_line_u64(&l, "end", spans[i].last);
                gk3w_line_u64(&l, "size_mib", gk3w_span_bytes(spans[i], d.bs) >> 20);
                (void)gk3w_line_emit(&l, stdout);
                gk3w_line_free(&l);
            }
    }
    rc = 0;
out:
    free(snap);
    free(winboot);
    _aligned_free(head);
    gk3w_disk_close(&d);
    return rc;
}
