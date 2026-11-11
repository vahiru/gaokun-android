/* Windows：从一块盘上卸载 gk3w apply（或 LiveCD 安装器）装上去的那一套 Android，留下空闲空间。
 *
 *   gk3w uninstall --disk N --backup-dir DIR [--real-disk [--bitlocker-key yes]]
 *
 * 分区：misc、metadata、boot_a、boot_b、super、gk3rescue、userdata 里盘上有的那些。每个名字至多一个、必须是 Linux 数据类型，
 *   否则整个拒绝（有歧义不猜）。别的分区一个都不碰。
 * ESP：只清这一套安装自己的东西 ——
 *   - <mid>/android/ 与 <mid>/rescue/；<mid>/ 本身只在空了时删（它可能也是别的系统的 BLS 目录，见 gk3__esp_pick_mid）；
 *   - loader/entries/<mid>-android-{a,b}.conf、<mid>-recovery-{a,b}.conf、<mid>-rescue.conf、<mid>-rescue-b.conf，
 *     统一启动入口的 gk3boot-android-*.conf、gk3prev-android-*.conf、gk3boot-tools.conf；EFI/gk3boot/ 整个目录；
 *   - EFI/BOOT/BOOTAA64.EFI 与 loader/loader.conf：有 .before-gaokun3 备份就换回原件、删掉备份；
 *   - loader/ 与 EFI/systemd/：只在 apply 的标记（<mid>/android/gk3w-install.txt）说它们是那次装才有的、并且
 *     loader/entries 里已经没有别的 .conf 时才删。没有标记（LiveCD 装的）就一律留着。
 *   <mid>：ESP 根目录下带 android/ 子目录的 32 位十六进制目录（恰好一个）；没有的话退回按 *-android-a.conf 的前缀找。
 *   先删目录、最后删启动项：中途失败时启动项还在，再跑一次 uninstall 仍认得出这一套。
 * 真实磁盘：删 LoaderEntryDefault 与 LoaderEntryOneShot（它们可能指着刚删掉的启动项）。
 *
 * ★ 先清 ESP、再删分区：中途失败时，前者留下的是“开机直接进 Windows、盘上多几个没用的分区”，
 *   后者留下的是“开机默认去启动一个已经不存在的 Android”。 */
#include <windows.h>
#include <winioctl.h>

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../esp.h"
#include "../layout.h"
#include "../proto.h"
#include "disk.h"
#include "gk3core.h"
#include "win.h"

static const wchar_t *const OUR_NAMES[] = {L"misc", L"metadata", L"boot_a", L"boot_b", L"super", L"gk3rescue", L"userdata"};
#define N_OURS (sizeof OUR_NAMES / sizeof OUR_NAMES[0])

static void note_removed(const char *what, const wchar_t *rel)
{
    gk3w_line l = {0};
    char r[MAX_PATH * 3];
    gk3w_w2u(rel, r, sizeof r);
    gk3w_line_begin(&l, "UNINSTALL");
    gk3w_line_kv(&l, what, r);
    (void)gk3w_line_emit(&l, stdout);
    gk3w_line_free(&l);
}

/* 删一个文件；本来就没有不算错 */
static int rm_file(const wchar_t *root, const wchar_t *rel)
{
    wchar_t p[MAX_PATH * 2];
    _snwprintf(p, MAX_PATH * 2, L"%ls%ls", root, rel);
    if (!gk3w_exists(p))
        return 0;
    SetFileAttributesW(p, FILE_ATTRIBUTE_NORMAL);
    if (!DeleteFileW(p))
        return -1;
    note_removed("esp-file", rel);
    return 0;
}

/* 整个目录删掉（不跟随重解析点）；本来就没有不算错 */
static int rm_tree(const wchar_t *dir)
{
    wchar_t pat[MAX_PATH * 2];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int rc = 0;
    _snwprintf(pat, MAX_PATH * 2, L"%ls\\*", dir);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        return (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) ? 0 : -1;
    }
    do {
        wchar_t child[MAX_PATH * 2];
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        _snwprintf(child, MAX_PATH * 2, L"%ls\\%ls", dir, fd.cFileName);
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

/* ESP 根目录下带 android/ 子目录的 32 位十六进制目录：0 = 没有；1 = 恰好一个（写进 mid）；-1 = 不止一个 */
static int find_install_dir(const wchar_t *root, wchar_t mid[33])
{
    wchar_t pat[MAX_PATH * 2];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int n = 0;
    _snwprintf(pat, MAX_PATH * 2, L"%ls*", root);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    do {
        wchar_t sub[MAX_PATH * 2];
        char u[MAX_PATH * 3];
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        gk3w_w2u(fd.cFileName, u, sizeof u);
        if (!gk3w_is_machine_id(u))
            continue;
        _snwprintf(sub, MAX_PATH * 2, L"%ls%ls\\android", root, fd.cFileName);
        if (!(GetFileAttributesW(sub) != INVALID_FILE_ATTRIBUTES && (GetFileAttributesW(sub) & FILE_ATTRIBUTE_DIRECTORY)))
            continue;
        if (n)
            n = -1000;
        wcscpy(mid, fd.cFileName);
        n++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n < 0 ? -1 : n;
}

/* apply 的标记里 key=1 吗（没有标记或没有这一行都算 0） */
static int marker_flag(const wchar_t *root, const wchar_t *mid, const char *key)
{
    wchar_t p[MAX_PATH * 2];
    uint8_t *data;
    size_t len;
    char want[64];
    int hit = 0;
    _snwprintf(p, MAX_PATH * 2, L"%ls%ls\\android\\gk3w-install.txt", root, mid);
    if (gk3w_read_file(p, &data, &len, 4096))
        return 0;
    snprintf(want, sizeof want, "\n%s=1", key);
    data[len] = 0;
    hit = strstr((const char *)data, want) != NULL;
    free(data);
    return hit;
}

/* loader/entries 里还有没有 .conf（我们的清完之后，别人的启动项） */
static int entries_left(const wchar_t *root)
{
    wchar_t pat[MAX_PATH * 2];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    _snwprintf(pat, MAX_PATH * 2, L"%lsloader\\entries\\*.conf", root);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    FindClose(h);
    return 1;
}

static int rm_dir(const wchar_t *root, const wchar_t *rel)
{
    wchar_t p[MAX_PATH * 2];
    _snwprintf(p, MAX_PATH * 2, L"%ls%ls", root, rel);
    if (!gk3w_exists(p))
        return 0;
    if (rm_tree(p))
        return -1;
    note_removed("esp-dir", rel);
    return 0;
}

/* 有 <rel>.before-gaokun3 就把原件换回来、删掉备份 */
static int restore_backup(const wchar_t *root, const wchar_t *rel)
{
    wchar_t p[MAX_PATH * 2], b[MAX_PATH * 2];
    uint8_t *data;
    size_t len;
    _snwprintf(p, MAX_PATH * 2, L"%ls%ls", root, rel);
    _snwprintf(b, MAX_PATH * 2, L"%ls%ls.before-gaokun3", root, rel);
    if (!gk3w_exists(b))
        return 0;
    if (gk3w_read_file(b, &data, &len, 64u << 20))
        return -1;
    if (gk3w_write_file_verify(p, data, len)) {
        free(data);
        return -1;
    }
    free(data);
    SetFileAttributesW(b, FILE_ATTRIBUTE_NORMAL);
    if (!DeleteFileW(b))
        return -1;
    note_removed("esp-restored", rel);
    return 0;
}

/* ESP 上 loader/entries/<mid>-android-a.conf 的 <mid>：0 = 没有；1 = 恰好一个（写进 mid）；-1 = 不止一个 */
static int find_mid(const wchar_t *root, wchar_t mid[33])
{
    wchar_t pat[MAX_PATH * 2];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int n = 0;
    _snwprintf(pat, MAX_PATH * 2, L"%lsloader\\entries\\*-android-a.conf", root);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    do {
        wchar_t *dash = wcsstr(fd.cFileName, L"-android-a.conf");
        char u[64];
        if (!dash || dash - fd.cFileName != 32 || dash[15] != 0)
            continue;
        *dash = 0;
        gk3w_w2u(fd.cFileName, u, sizeof u);
        if (!gk3w_is_machine_id(u))
            continue;
        if (n && wcscmp(mid, fd.cFileName))
            n = -1000;   /* 两个不同的 mid */
        wcscpy(mid, fd.cFileName);
        n++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n < 0 ? -1 : n > 0;
}

/* 统一启动入口的条目：gk3boot-android-*.conf、gk3prev-android-*.conf、gk3boot-tools.conf */
static int rm_gk3boot_entries(const wchar_t *root)
{
    static const wchar_t *const pats[] = {L"gk3boot-android-*", L"gk3prev-android-*"};
    int rc = rm_file(root, L"loader\\entries\\gk3boot-tools.conf");
    for (size_t k = 0; k < 2; k++) {
        wchar_t pat[MAX_PATH * 2];
        WIN32_FIND_DATAW fd;
        HANDLE h;
        _snwprintf(pat, MAX_PATH * 2, L"%lsloader\\entries\\%ls", root, pats[k]);
        h = FindFirstFileW(pat, &fd);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        do {
            wchar_t rel[MAX_PATH];
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;
            _snwprintf(rel, MAX_PATH, L"loader\\entries\\%ls", fd.cFileName);
            if (rm_file(root, rel))
                rc = -1;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return rc;
}

int gk3w_cmd_uninstall(int argc, wchar_t **argv)
{
    uint64_t num;
    const wchar_t *backup_dir = gk3w_arg(argc, argv, L"--backup-dir"), *blkey = gk3w_arg(argc, argv, L"--bitlocker-key");
    int real = gk3w_flag(argc, argv, L"--real-disk"), rc = 1, n_esp = 0, have_mid;
    gk3w_touched T = GK3W_TOUCHED_NO;
    gk3w_disk d;
    GUID ids[N_OURS];
    size_t n_ids = 0;
    const PARTITION_INFORMATION_EX *esp_part = NULL;
    wchar_t root[MAX_PATH], mid[33] = L"";

    if (gk3w_arg_u64(argc, argv, L"--disk", &num) || num > 255 || !backup_dir)
        return gk3w_err(T, "usage", "usage: uninstall --disk N --backup-dir DIR [--real-disk [--bitlocker-key yes]]", 0);
    if (real) {
        gk3w_sys sys;
        gk3w_sys_read(&sys);
        if (gk3w_sys_gate(&sys, blkey && !wcscmp(blkey, L"yes")))
            return 1;
    }
    if (gk3w_disk_open((DWORD)num, real, &d, T))
        return 1;

    /* ── 认分区：每个名字至多一个、必须是 Linux 数据类型 ── */
    for (size_t k = 0; k < N_OURS; k++) {
        int hits = 0;
        for (DWORD i = 0; i < d.lay->PartitionCount; i++) {
            const PARTITION_INFORMATION_EX *p = &d.lay->PartitionEntry[i];
            wchar_t wn[GK3_GPT_NAME_CHARS + 1];
            if (p->PartitionStyle != PARTITION_STYLE_GPT || !p->PartitionLength.QuadPart)
                continue;
            memcpy(wn, p->Gpt.Name, sizeof p->Gpt.Name);
            wn[GK3_GPT_NAME_CHARS] = 0;
            if (wcscmp(wn, OUR_NAMES[k]))
                continue;
            if (memcmp(&p->Gpt.PartitionType, &GK3W_LINUX_DATA, sizeof(GUID))) {
                char nm[32];
                gk3w_w2u(OUR_NAMES[k], nm, sizeof nm);
                gk3w_err_kv(T, "bad-part-type", "name", nm, "a partition with this name is not of the Linux data type, so it is not ours; leaving it (disk not touched)", 0);
                goto out;
            }
            ids[n_ids] = p->Gpt.PartitionId;
            hits++;
        }
        if (hits > 1) {
            char nm[32];
            gk3w_w2u(OUR_NAMES[k], nm, sizeof nm);
            gk3w_err_kv(T, "partlabel-conflict", "names", nm, "more than one partition has this name; cannot tell which one to remove (disk not touched)", 0);
            goto out;
        }
        n_ids += (size_t)hits;
    }
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
    have_mid = find_install_dir(root, mid);
    if (!have_mid)
        have_mid = find_mid(root, mid);
    if (have_mid < 0) {
        gk3w_err(T, "esp-multiple-installs", "the EFI system partition holds more than one Android install; cannot tell which one to remove (disk not touched)", 0);
        goto out;
    }
    if (!n_ids && !have_mid) {
        gk3w_log("no Android partitions on the disk and no Android install on the EFI system partition: nothing to uninstall");
        rc = 0;
        goto out;
    }
    {
        char m8[64] = "(none)";
        if (have_mid)
            gk3w_w2u(mid, m8, sizeof m8);
        gk3w_log("%u partitions to remove; install directory on the ESP: %s", (unsigned)n_ids, m8);
    }
    if (gk3w_gpt_backup(&d, backup_dir, L"uninstall", T))
        goto out;

    /* ══ 从这里起开始动盘：先清 ESP ══ */
    T = GK3W_TOUCHED_YES;
    {
        int bad = 0, created_loader = 0, created_systemd = 0;
        if (have_mid) {
            static const wchar_t *const sfx[] = {L"-android-a.conf", L"-android-b.conf", L"-recovery-a.conf",
                                                 L"-recovery-b.conf", L"-rescue.conf",    L"-rescue-b.conf"};
            wchar_t rel[MAX_PATH], full[MAX_PATH * 2];
            created_loader = marker_flag(root, mid, "created_loader");
            created_systemd = marker_flag(root, mid, "created_systemd");
            /* 先删文件目录，最后删启动项 */
            _snwprintf(rel, MAX_PATH, L"%ls\\android", mid);
            bad |= rm_dir(root, rel);
            _snwprintf(rel, MAX_PATH, L"%ls\\rescue", mid);
            bad |= rm_dir(root, rel);
            _snwprintf(full, MAX_PATH * 2, L"%ls%ls", root, mid);
            if (!bad && RemoveDirectoryW(full))   /* 空了才删得掉：里面还有别的系统的东西就留着 */
                note_removed("esp-dir", mid);
            for (size_t k = 0; k < sizeof sfx / sizeof sfx[0]; k++) {
                _snwprintf(rel, MAX_PATH, L"loader\\entries\\%ls%ls", mid, sfx[k]);
                bad |= rm_file(root, rel);
            }
        }
        bad |= rm_gk3boot_entries(root);
        bad |= rm_dir(root, L"EFI\\gk3boot");
        bad |= restore_backup(root, L"EFI\\BOOT\\BOOTAA64.EFI");
        bad |= restore_backup(root, L"loader\\loader.conf");
        if (!bad && !entries_left(root)) {
            if (created_loader)
                bad |= rm_dir(root, L"loader");
            if (created_systemd)
                bad |= rm_dir(root, L"EFI\\systemd");
        }
        if (bad) {
            gk3w_err(T, "esp-write", "cleaning the EFI system partition failed: a file could not be removed or restored (partitions not removed yet)", GetLastError());
            goto out;
        }
    }

    /* ── 再删分区 ── */
    if (n_ids) {
        DWORD count = 0;
        DRIVE_LAYOUT_INFORMATION_EX *lay = calloc(1, d.laysz);
        if (!lay) {
            gk3w_err(T, "nomem", "out of memory", 0);
            goto out;
        }
        lay->PartitionStyle = PARTITION_STYLE_GPT;
        lay->Gpt = d.lay->Gpt;
        for (DWORD i = 0; i < d.lay->PartitionCount; i++) {
            const PARTITION_INFORMATION_EX *p = &d.lay->PartitionEntry[i];
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

    /* ── EFI 变量（真实磁盘）：可能指着刚删掉的启动项 ── */
    if (real) {
        gk3w_loader_var_delete(L"LoaderEntryDefault");
        gk3w_loader_var_delete(L"LoaderEntryOneShot");
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
    gk3w_disk_close(&d);
    return rc;
}
