/* Linux：安装之前给界面看的两样东西（只读）。
 *
 *   gk3l esp-info --disk /dev/X [--sdboot FILE]
 *       这块盘上 ESP 的状况，双系统 / 重新安装之前界面据此先写明"装不装得下、要用户确认什么"，而不是等 apply
 *       在动盘之前一刻才拒绝。一行：
 *       ESP part= size_mib= free_mib= need_mib=150 windows=yes|no gaokun3=yes|no mountable=yes|no small=yes|no
 *           bootaa64=none|sdboot|other bitlocker=yes|no hibernated=yes|no|unknown loader_default=<值>|none|unknown
 *       盘上没有 ESP 或不止一个：ERR esp-missing / esp-ambiguous。
 *       windows：EFI/Microsoft/Boot/bootmgfw.efi 在；gaokun3：已经有我们的启动项；small：分区小于 200 MiB（重装 Windows
 *       建的 100 MiB 那种，双系统不装）；bootaa64：回落路径与 systemd-boot 比（other + bitlocker ⇒ 要用户确认拿到恢复密钥）；
 *       hibernated=yes ⇒ apply 不写 ESP；loader_default：EFI 变量 LoaderEntryDefault（unknown = efivarfs 读不到）。
 *
 *   gk3l release-info [--dir DIR]
 *       安装介质上的系统文件（默认 /media/gk3/gaokun3/payload）。一行：
 *       RELEASE dir= boot=yes|no super=zst|img|no sha256=yes|no version=<版本>|? super_mib=
 *       version 取自 install-artifacts.sha256 里第一个 .zip 的名字。 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>

#include "../esp.h"
#include "../proto.h"
#include "disk.h"
#include "files.h"
#include "lx.h"
#include "sysstate.h"

#define ESP_TYPE "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"

static const char *sdboot_path(const char *arg)
{
    static const char *const cand[] = {"/usr/share/gaokun3/systemd-bootaa64.efi", "/usr/lib/systemd/boot/efi/systemd-bootaa64.efi"};
    const char *env = getenv("GK3_SDBOOT");
    if (arg && *arg)
        return arg;
    if (env && *env)
        return env;
    for (size_t i = 0; i < sizeof cand / sizeof cand[0]; i++)
        if (gk3l_file_size(cand[i]) > 0)
            return cand[i];
    return NULL;
}

static int has_android_entries(const char *root)
{
    char d[4200];
    DIR *dir;
    struct dirent *e;
    int found = 0;
    snprintf(d, sizeof d, "%s/loader/entries", root);
    dir = opendir(d);
    if (!dir)
        return 0;
    while ((e = readdir(dir)) && !found) {
        size_t n = strlen(e->d_name);
        found = n > 5 && strstr(e->d_name, "-android-") && !strcmp(e->d_name + n - 5, ".conf");
    }
    closedir(dir);
    return found;
}

int gk3l_cmd_esp_info(int argc, char **argv)
{
    const char *disk = gk3l_arg(argc, argv, "--disk");
    static gk3l_disk d;
    char name[64], dev[96] = "", root[4096] = "", tmp[256], ld[256] = "none", fs[64], label[256];
    const gk3l_kpart *k = NULL;
    uint64_t size = 0, free_mib = 0;
    int n_esp = 0, mounted_tmp = 0, mountable = 0, windows = 0, ours = 0, err;
    const char *aa = "none";
    gk3l_win w;
    gk3w_line l = {0};

    if (!disk)
        return gk3l_err(GK3W_TOUCHED_NA, "usage", "usage: esp-info --disk /dev/X [--sdboot FILE]", 0);
    if (gk3l_whole_disk_name(disk, name, sizeof name))
        return gk3l_err_kv(GK3W_TOUCHED_NA, "disk-open", "disk", disk, "--disk is not a whole disk", 0);
    err = gk3l_disk_open(&d, name, 0);
    if (err)
        return gk3l_err_kv(GK3W_TOUCHED_NA, err == EACCES || err == EPERM ? "not-root" : "disk-open", "disk", disk, "cannot open the disk", err);
    if (gk3l_disk_read_table(&d) || strcmp(d.table, "gpt")) {
        gk3l_disk_close(&d);
        return gk3l_err(GK3W_TOUCHED_NA, "esp-missing", "the disk has no GPT partition table, so no EFI system partition", 0);
    }
    for (uint32_t i = 0; i < d.g.num_entries; i++) {
        gk3_gpt_part q;
        char type[37];
        if (gk3_gpt_get(&d.g, i, &q) != GK3_OK)
            continue;
        gk3l_guid_upper(q.type_guid, type);
        if (strcmp(type, ESP_TYPE))
            continue;
        n_esp++;
        size = (q.last_lba - q.first_lba + 1) * d.bs >> 20;
        k = gk3l_kernel_find(d.kp, d.nk, q.index);
    }
    gk3l_disk_close(&d);
    if (n_esp != 1)
        return gk3l_err(GK3W_TOUCHED_NA, n_esp ? "esp-ambiguous" : "esp-missing", n_esp ? "this disk has more than one EFI system partition" : "this disk has no EFI system partition", 0);
    if (k) {
        snprintf(dev, sizeof dev, "/dev/%s", k->name);
        gk3l_blkid(dev, fs, sizeof fs, label, sizeof label);
        /* 别处已经挂着就直接读那里（同一个超级块不能再以只读挂一次） */
        if (!strcmp(fs, "vfat") && gk3l_mount_of(k->devno, root, sizeof root)) {
            if (!gk3l_mount_tmp(dev, "vfat", 1, tmp, sizeof tmp)) {
                snprintf(root, sizeof root, "%s", tmp);
                mounted_tmp = 1;
            } else {
                root[0] = 0;
            }
        }
    }
    if (root[0]) {
        struct statvfs sv;
        char p[4300];
        const char *sd = sdboot_path(gk3l_arg(argc, argv, "--sdboot"));
        uint8_t *b = NULL;
        size_t bl = 0;
        mountable = 1;
        if (!statvfs(root, &sv))
            free_mib = (uint64_t)sv.f_bavail * sv.f_frsize >> 20;
        snprintf(p, sizeof p, "%s/EFI/Microsoft/Boot/bootmgfw.efi", root);
        windows = gk3l_file_size(p) >= 0;   /* vfat 查找不分大小写 */
        ours = has_android_entries(root);
        snprintf(p, sizeof p, "%s/EFI/BOOT/BOOTAA64.EFI", root);
        if (gk3l_file_size(p) >= 0) {
            aa = "other";
            if (sd && !gk3l_read_file(sd, &b, &bl, 16u << 20) && gk3l_file_same(p, b, bl))
                aa = "sdboot";
            free(b);
        }
        if (mounted_tmp)
            gk3l_umount_tmp(root);
    }
    gk3l_win_state(name, &w);
    switch (gk3l_loader_var("LoaderEntryDefault", ld, sizeof ld)) {
    case 1:
        snprintf(ld, sizeof ld, "none");
        break;
    case 2:
        snprintf(ld, sizeof ld, "unknown");
        break;
    }
    gk3w_line_begin(&l, "ESP");
    gk3w_line_kv(&l, "part", dev);
    gk3w_line_u64(&l, "size_mib", size);
    gk3w_line_u64(&l, "free_mib", free_mib);
    gk3w_line_u64(&l, "need_mib", GK3W_ESP_NEED_MIB);
    gk3w_line_kv(&l, "windows", windows ? "yes" : "no");
    gk3w_line_kv(&l, "gaokun3", ours ? "yes" : "no");
    gk3w_line_kv(&l, "mountable", mountable ? "yes" : "no");
    gk3w_line_kv(&l, "small", size < GK3W_ESP_MIN_SIZE_MIB ? "yes" : "no");
    gk3w_line_kv(&l, "bootaa64", aa);
    gk3w_line_kv(&l, "bitlocker", w.bitlocker ? "yes" : "no");
    gk3w_line_kv(&l, "hibernated", w.hibernated == 1 ? "yes" : w.hibernated ? "unknown" : "no");
    gk3w_line_kv(&l, "loader_default", ld);
    err = gk3w_line_emit(&l, stdout);
    gk3w_line_free(&l);
    return err ? 1 : 0;
}

int gk3l_cmd_release_info(int argc, char **argv)
{
    const char *dir = gk3l_arg(argc, argv, "--dir");
    char p[4200], ver[256] = "?";
    const char *super = "no";
    int64_t smib = 0, sz;
    uint8_t *m = NULL;
    size_t ml = 0;
    int sha = 0, rc;
    gk3w_line l = {0};
    if (!dir)
        dir = "/media/gk3/gaokun3/payload";
    snprintf(p, sizeof p, "%s/super.img.zst", dir);
    if ((sz = gk3l_file_size(p)) >= 0) {
        super = "zst";
        smib = sz >> 20;
    } else {
        snprintf(p, sizeof p, "%s/super.img", dir);
        if ((sz = gk3l_file_size(p)) >= 0) {
            super = "img";
            smib = sz >> 20;
        }
    }
    snprintf(p, sizeof p, "%s/install-artifacts.sha256", dir);
    if (!gk3l_read_file(p, &m, &ml, 1u << 20)) {
        /* "<sha256>  [*]<名字>.zip"：第一个 .zip 的名字就是版本 */
        size_t i = 0;
        sha = 1;
        while (i < ml) {
            size_t e = i, s;
            while (e < ml && m[e] != '\n')
                e++;
            if (e - i < 66) {
                i = e + 1;
                continue;
            }
            s = i + 64;
            while (s < e && (m[s] == ' ' || m[s] == '*'))
                s++;
            {
                size_t end = e;
                if (end > s && m[end - 1] == '\r')
                    end--;
                if (end - s > 4 && end - s - 4 < sizeof ver && !memcmp(m + end - 4, ".zip", 4)) {
                    memcpy(ver, m + s, end - s - 4);
                    ver[end - s - 4] = 0;
                    break;
                }
            }
            i = e + 1;
        }
        free(m);
    }
    snprintf(p, sizeof p, "%s/boot.img", dir);
    gk3w_line_begin(&l, "RELEASE");
    gk3w_line_kv(&l, "dir", dir);
    gk3w_line_kv(&l, "boot", gk3l_file_size(p) >= 0 ? "yes" : "no");
    gk3w_line_kv(&l, "super", super);
    gk3w_line_kv(&l, "sha256", sha ? "yes" : "no");
    gk3w_line_kv(&l, "version", ver);
    gk3w_line_u64(&l, "super_mib", (uint64_t)smib);
    rc = gk3w_line_emit(&l, stdout) ? 1 : 0;
    gk3w_line_free(&l);
    return rc;
}
