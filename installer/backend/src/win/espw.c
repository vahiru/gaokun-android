/* Windows：ESP 上的文件读写，以及把回落路径换成 systemd-boot 的那一步（说明见 espw.h）。 */
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../esp.h"
#include "disk.h"
#include "espw.h"
#include "gk3core.h"
#include "win.h"

void gk3w_esp_path(const gk3w_esp *e, const char *rel, wchar_t *out, size_t cap)
{
    wchar_t w[MAX_PATH];
    gk3w_u2w(rel, w, MAX_PATH);
    for (wchar_t *p = w; *p; p++)
        if (*p == L'/')
            *p = L'\\';
    _snwprintf(out, cap, L"%ls%ls", e->root, w);
}

/* ESP 上 rel 这个文件与 data 是否逐字节相同；不存在算不同 */
int gk3w_esp_same(const gk3w_esp *e, const char *rel, const uint8_t *data, size_t len)
{
    wchar_t p[MAX_PATH * 2];
    uint8_t *old;
    size_t olen;
    int same;
    gk3w_esp_path(e, rel, p, MAX_PATH * 2);
    if (gk3w_read_file(p, &old, &olen, 256u << 20))
        return 0;
    same = olen == len && !memcmp(old, data, len);
    free(old);
    return same;
}

int64_t gk3w_esp_size_of(const gk3w_esp *e, const char *rel)
{
    wchar_t p[MAX_PATH * 2];
    WIN32_FILE_ATTRIBUTE_DATA a;
    gk3w_esp_path(e, rel, p, MAX_PATH * 2);
    if (!GetFileAttributesExW(p, GetFileExInfoStandard, &a))
        return -1;
    return (int64_t)(((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow);
}

/* 往 ESP 上写这一组文件要多占多少 KiB（同名旧文件只算变大的部分，按 4 KiB 簇向上取整） */
int64_t gk3w_esp_delta(const gk3w_esp *e, const char *rel, uint64_t newsz)
{
    int64_t old = gk3w_esp_size_of(e, rel);
    if (old < 0)
        old = 0;
    return (int64_t)newsz > old ? ((int64_t)newsz - old + 4095) / 4096 * 4 : 0;
}

int gk3w_esp_has_android_entries(const gk3w_esp *e)
{
    wchar_t pat[MAX_PATH * 2];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    _snwprintf(pat, MAX_PATH * 2, L"%lsloader\\entries\\*-android-*.conf", e->root);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    FindClose(h);
    return 1;
}

/* ESP 根目录下第一个（按字节序）32 位十六进制的目录名 */
int gk3w_esp_pick_mid(const gk3w_esp *e, char out[33])
{
    wchar_t pat[MAX_PATH * 2];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int found = 0;
    _snwprintf(pat, MAX_PATH * 2, L"%ls*", e->root);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    do {
        char n[MAX_PATH * 3];
        gk3w_w2u(fd.cFileName, n, sizeof n);
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && gk3w_is_machine_id(n) && (!found || strcmp(n, out) < 0)) {
            memcpy(out, n, 33);
            found = 1;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

int gk3w_esp_write(const gk3w_esp *e, const char *rel, const uint8_t *data, size_t len)
{
    wchar_t p[MAX_PATH * 2], dir[MAX_PATH * 2], *slash;
    int w;
    gk3w_esp_path(e, rel, p, MAX_PATH * 2);
    wcscpy(dir, p);
    slash = wcsrchr(dir, L'\\');
    if (slash && (size_t)(slash - dir) > e->root_len) {
        *slash = 0;
        if (gk3w_mkdirs(dir, e->root_len))
            return gk3w_err_kv(GK3W_TOUCHED_YES, "esp-write", "file", rel, "cannot create a directory on the EFI system partition", GetLastError());
    }
    w = gk3w_write_file_verify(p, data, len);
    if (w)
        return gk3w_err_kv(GK3W_TOUCHED_YES, w == -1 ? "esp-write" : "esp-verify", "file", rel,
                           w == -1 ? "writing the EFI system partition failed: out of space, or a media error" : "a file on the EFI system partition did not read back identically", GetLastError());
    return 0;
}

/* 写成 <rel>.tmp、读回核对，再改名覆盖 <rel>（gk3w_replace_file_verify，保留 <rel> 原来的属性）：中途失败或掉电时
 * <rel> 要么是原来的、要么是完整的新文件。 */
int gk3w_esp_write_renamed(const gk3w_esp *e, const char *rel, const uint8_t *data, size_t len)
{
    wchar_t pf[MAX_PATH * 2], dir[MAX_PATH * 2], *slash;
    gk3w_esp_path(e, rel, pf, MAX_PATH * 2);
    wcscpy(dir, pf);
    slash = wcsrchr(dir, L'\\');
    if (slash && (size_t)(slash - dir) > e->root_len) {
        *slash = 0;
        if (gk3w_mkdirs(dir, e->root_len))
            return gk3w_err_kv(GK3W_TOUCHED_YES, "esp-write", "file", rel, "cannot create a directory on the EFI system partition", GetLastError());
    }
    if (gk3w_replace_file_verify(pf, data, len))
        return gk3w_err_kv(GK3W_TOUCHED_YES, "esp-write", "file", rel, "writing the EFI system partition failed: out of space, a media error, or the file did not read back identically", GetLastError());
    return 0;
}

int gk3w_esp_open(gk3w_disk *d, gk3w_esp *e, const PARTITION_INFORMATION_EX **part, gk3w_touched T)
{
    const PARTITION_INFORMATION_EX *esp_part = NULL;
    int n_esp = 0;
    memset(e, 0, sizeof *e);
    for (DWORD i = 0; i < d->lay->PartitionCount; i++) {
        const PARTITION_INFORMATION_EX *p = &d->lay->PartitionEntry[i];
        if (p->PartitionStyle == PARTITION_STYLE_GPT && p->PartitionLength.QuadPart &&
            !memcmp(&p->Gpt.PartitionType, &GK3W_ESP_TYPE, sizeof(GUID))) {
            esp_part = p;
            n_esp++;
        }
    }
    if (n_esp != 1)
        return gk3w_err(T, n_esp ? "esp-ambiguous" : "esp-missing", n_esp ? "this disk has more than one EFI system partition" : "this disk has no EFI system partition", 0), -1;
    {
        /* Android 侧的 sepolicy 只给名叫 esp 或 "EFI system partition" 的分区打 ESP 的标签 */
        wchar_t wn[GK3_GPT_NAME_CHARS + 1];
        char en[GK3_GPT_NAME_CHARS * 3 + 1];
        memcpy(wn, esp_part->Gpt.Name, sizeof esp_part->Gpt.Name);
        wn[GK3_GPT_NAME_CHARS] = 0;
        gk3w_w2u(wn, en, sizeof en);
        if (strcmp(en, "esp") && strcmp(en, "EFI system partition"))
            gk3w_log("warning: the EFI system partition is named '%s', not 'esp' or 'EFI system partition'; slot switching and "
                     "OTA will fail on future enforcing builds. Renaming the partition is harmless (UEFI only looks at the type GUID).", en);
    }
    if (gk3w_find_volume(d->num, (uint64_t)esp_part->StartingOffset.QuadPart, e->root, MAX_PATH))
        return gk3w_err(T, "esp-mount", "cannot find the volume of the EFI system partition; refusing to install a boot chain on an unreadable ESP (disk not touched)", 0), -1;
    e->root_len = wcslen(e->root);
    {
        wchar_t fs[32] = L"", p[MAX_PATH * 2];
        if (!GetVolumeInformationW(e->root, NULL, 0, NULL, NULL, NULL, fs, 32) || _wcsicmp(fs, L"FAT32"))
            return gk3w_err(T, "esp-not-fat", "the EFI system partition is not FAT32 (disk not touched)", GetLastError()), -1;
        _snwprintf(p, MAX_PATH * 2, L"%lsEFI\\Microsoft\\Boot\\bootmgfw.efi", e->root);
        e->windows = gk3w_exists(p);
    }
    *part = esp_part;
    return 0;
}

int gk3w_esp_plan_backups(const gk3w_esp *e, const uint8_t *sd, size_t sd_len, uint8_t *orig[2], size_t orig_len[2], gk3w_touched T)
{
    /* 原件留一份（gk3_apply 同一条规矩）：只在第一次装时留，且与这份 systemd-boot 不同时才留。
     * 文件在而读不了（共享冲突、读错误）就在这里拒绝 —— 不能跳过备份再去覆盖它。
     * 回落路径另有一条（gk3w_install_backup_decide）：已有备份、而现在回落路径上是别的系统的引导程序（上次卸载
     * 之后别的系统放的）时，备份删了不丢就用现在的换掉它，两份都是别人的原件就拒绝 —— 不能覆盖任何一份 */
    static const char *const names[2] = {"EFI/BOOT/BOOTAA64.EFI", "loader/loader.conf"};
    orig[0] = orig[1] = NULL;
    orig_len[0] = orig_len[1] = 0;
    for (int k = 0; k < 2; k++) {
        char relb[96];
        wchar_t pth[MAX_PATH * 2];
        snprintf(relb, sizeof relb, "%s.before-gaokun3", names[k]);
        gk3w_esp_path(e, names[k], pth, MAX_PATH * 2);
        if (k == 0) {
            static const char *const rels[2] = {"EFI/Microsoft/Boot/bootmgfw.efi", "EFI/BOOT/BOOTAA64.EFI.before-gaokun3"};
            uint8_t *cur = NULL, *wb[2] = {NULL, NULL};
            size_t cur_len = 0, wb_len[2] = {0, 0};
            const char *bad = NULL;
            DWORD bad_err = 0;
            if (gk3w_exists(pth) && gk3w_read_file(pth, &cur, &cur_len, 64u << 20))
                bad = names[k], bad_err = GetLastError();
            for (int j = 0; j < 2 && !bad; j++) {
                wchar_t pj[MAX_PATH * 2];
                gk3w_esp_path(e, rels[j], pj, MAX_PATH * 2);
                if (gk3w_exists(pj) && gk3w_read_file(pj, &wb[j], &wb_len[j], 64u << 20))
                    bad = rels[j], bad_err = GetLastError();
            }
            if (bad) {
                free(cur), free(wb[0]), free(wb[1]);
                gk3w_err_kv(T, "esp-unreadable", "file", bad, "cannot read a file on the EFI system partition that must be checked or backed up first (disk not touched)", bad_err);
                return -1;
            }
            switch (gk3w_install_backup_decide(cur, cur_len, wb[0], wb_len[0], wb[1], wb_len[1], sd, sd_len)) {
            case GK3W_BK_SAVE:
                orig[0] = cur, orig_len[0] = cur_len, cur = NULL;
                break;
            case GK3W_BK_REPLACE:
                gk3w_log("EFI/BOOT/BOOTAA64.EFI now holds another system's boot loader, and its .before-gaokun3 can go without loss: keeping the current file as .before-gaokun3 instead");
                orig[0] = cur, orig_len[0] = cur_len, cur = NULL;
                break;
            case GK3W_BK_CONFLICT:
                free(cur), free(wb[0]), free(wb[1]);
                gk3w_err(T, "fallback-conflict", "EFI/BOOT/BOOTAA64.EFI holds another system's boot loader, and EFI/BOOT/BOOTAA64.EFI.before-gaokun3 holds a different one that is not Windows: installing would lose one of them (disk not touched)", 0);
                return -1;
            case GK3W_BK_NONE:
                break;
            }
            free(cur), free(wb[0]), free(wb[1]);
            continue;
        }
        if (gk3w_esp_size_of(e, relb) >= 0 || !gk3w_exists(pth))
            continue;
        if (gk3w_read_file(pth, &orig[k], &orig_len[k], 64u << 20)) {
            free(orig[0]);
            orig[0] = NULL;
            gk3w_err_kv(T, "esp-unreadable", "file", names[k], "cannot read a file on the EFI system partition that must be backed up first (disk not touched)", GetLastError());
            return -1;
        }
        /* 我们自己写的 loader.conf（例如写入本机磁盘的 LiveCD 留下的）不是原件 */
        if (gk3w_is_our_loader_conf(orig[k], orig_len[k])) {
            free(orig[k]);
            orig[k] = NULL;
        }
    }
    return 0;
}

int gk3w_esp_write_sdboot(const gk3w_esp *e, const uint8_t *sd, size_t sd_len, uint8_t *const orig[2], const size_t orig_len[2],
                          const char *conf)
{
    static const char *const backups[2] = {"EFI/BOOT/BOOTAA64.EFI", "loader/loader.conf"};
    char relp[160];
    for (int k = 0; k < 2; k++) {
        if (!orig[k])
            continue;
        snprintf(relp, sizeof relp, "%s.before-gaokun3", backups[k]);
        if (gk3w_esp_write_renamed(e, relp, orig[k], orig_len[k]))
            return -1;
        gk3w_log("backed up the original %s as %s", backups[k], relp);
    }
    if (conf && gk3w_esp_write(e, "loader/loader.conf", (const uint8_t *)conf, strlen(conf)))
        return -1;
    /* 只在字节不同时写（BitLocker 量的是固件每次加载的这个文件）。回落路径写成 .tmp 再改名：中途断电或写错时
     * 它要么还是原来的文件、要么是完整的 systemd-boot，不会是半个文件（卸载会把半个文件当成别的系统的引导程序） */
    for (int k = 0; k < 2; k++) {
        const char *t = k ? "EFI/systemd/systemd-bootaa64.efi" : "EFI/BOOT/BOOTAA64.EFI";
        if (gk3w_esp_same(e, t, sd, sd_len))
            gk3w_log("%s is already this systemd-boot, not rewritten", t);
        else if (k ? gk3w_esp_write(e, t, sd, sd_len) : gk3w_esp_write_renamed(e, t, sd, sd_len))
            return -1;
    }
    return 0;
}
