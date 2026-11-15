/* gk3l：同一块盘上 Windows 卷的状态（休眠、BitLocker）与 systemd-boot 的 EFI 变量。
 *
 * 休眠：Windows 休眠（快速启动的"关机"也是休眠）时它缓存着 ESP 的 FAT，这边写了、Windows 醒来再写就可能把 FAT
 *   写坏。判据：任一 NTFS 卷的 hiberfil.sys 以 hibr / HIBR 开头（与 ntfs-3g 自己的休眠检查相同）。读法是内核的 ntfs3
 *   只读挂载：只读时 ntfs3 不重放、不清空 $LogFile（fs/ntfs3/fsntfs.c 的 ntfs_loadlog_and_replay），不往卷上写任何东西，
 *   也不因为卷在休眠而拒绝只读挂载。测试容器的内核没有 ntfs3：GK3_NTFS_MOUNT 换成一条挂载命令（如 "ntfs-3g -o ro"）。
 * EFI 变量：efivarfs 里的文件是 4 字节属性 + 值（systemd-boot 的变量是 UTF-16LE 字符串）；删之前先去掉不可变标记。
 *   GK3_EFIVARS=<目录>：测试时当作 efivarfs。 */
#define _GNU_SOURCE
#include "sysstate.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include "disk.h"
#include "files.h"
#include "lx.h"

#define LOADER_GUID "4a67b082-0a4c-41cf-b6c7-440b29bb8c4f"

/* 0 = 在休眠；1 = 没在休眠；2 = 挂不上，判断不了 */
static int ntfs_hibernated(const char *dev)
{
    const char *cmd = getenv("GK3_NTFS_MOUNT"), *run = getenv("GK3_RUNDIR");
    char dir[256], hib[300];
    int rc, mounted = 0;
    if (!run || !*run)
        run = "/run/gaokun3";
    if (cmd && *cmd) {
        char buf[512], *argv[16], *save = NULL;
        int argc = 0;
        snprintf(buf, sizeof buf, "%s", cmd);
        snprintf(dir, sizeof dir, "%s/ntfs.XXXXXX", run);
        if (gk3l_mkdirs(run) || !mkdtemp(dir))
            return 2;
        for (char *t = strtok_r(buf, " ", &save); t && argc < 13; t = strtok_r(NULL, " ", &save))
            argv[argc++] = t;
        argv[argc++] = (char *)dev;
        argv[argc++] = dir;
        argv[argc] = NULL;
        if (gk3l_run(argv, NULL, 0) == 0)
            mounted = 1;
        else
            rmdir(dir);
    } else if (!gk3l_mount_tmp(dev, "ntfs3", 1, dir, sizeof dir)) {
        mounted = 1;
    }
    if (!mounted)
        return 2;
    snprintf(hib, sizeof hib, "%.250s/hiberfil.sys", dir);
    /* 只读开头 4 字节：hiberfil.sys 可能有几个 GiB */
    {
        int fd = open(hib, O_RDONLY | O_CLOEXEC);
        uint8_t b[4];
        rc = 1;
        if (fd >= 0) {
            if (read(fd, b, 4) == 4 && (!memcmp(b, "hibr", 4) || !memcmp(b, "HIBR", 4)))
                rc = 0;
            close(fd);
        }
    }
    gk3l_umount_tmp(dir);
    return rc;
}

void gk3l_win_state(const char *disk, gk3l_win *w)
{
    gk3l_kpart kp[256];
    size_t nk = gk3l_kernel_parts(disk, kp, 256);
    int unk = 0;
    memset(w, 0, sizeof *w);
    for (size_t i = 0; i < nk; i++) {
        char dev[96], fs[64], label[256];
        snprintf(dev, sizeof dev, "/dev/%.63s", kp[i].name);
        gk3l_blkid(dev, fs, sizeof fs, label, sizeof label);
        if (!strcmp(fs, "BitLocker")) {
            w->bitlocker = 1;
        } else if (!strcmp(fs, "ntfs") && w->hibernated != 1) {
            switch (ntfs_hibernated(dev)) {
            case 0:
                w->hibernated = 1;
                snprintf(w->hib_part, sizeof w->hib_part, "%s", dev);
                break;
            case 2:
                unk = 1;
                break;
            }
        }
    }
    /* 有 BitLocker 卷（里面的 NTFS 这边读不到）或有 NTFS 卷挂不上，而别的都没在休眠：判断不了 */
    if (w->hibernated != 1 && (w->bitlocker || unk))
        w->hibernated = -1;
}

/* ── EFI 变量 ── */

static const char *efivars_dir(void)
{
    static char d[256];
    const char *t = getenv("GK3_EFIVARS");
    if (t && *t)
        return gk3l_is_dir(t) ? t : NULL;
    snprintf(d, sizeof d, "/sys/firmware/efi/efivars");
    {
        char probe[320];
        snprintf(probe, sizeof probe, "%s/SecureBoot-8be4df61-93ca-11d2-aa0d-00e098032b8c", d);
        /* 还没挂就挂一次（live 的 systemd 一般已经挂了） */
        if (!gk3l_exists(probe) && mount("efivarfs", d, "efivarfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) && errno != EBUSY)
            return NULL;
    }
    return gk3l_is_dir(d) ? d : NULL;
}

int gk3l_loader_var(const char *name, char *out, size_t cap)
{
    const char *d = efivars_dir();
    char p[512];
    uint8_t *b;
    size_t n, k = 0;
    int e;
    if (!d)
        return 2;
    snprintf(p, sizeof p, "%s/%s-" LOADER_GUID, d, name);
    e = gk3l_read_file(p, &b, &n, 4096);
    if (e == ENOENT)
        return 1;
    if (e)
        return 2;
    /* 4 字节属性之后是 UTF-16LE；systemd-boot 的条目名是 ASCII，非 ASCII 的码元记成 '?' */
    for (size_t i = 4; i + 1 < n && k + 1 < cap; i += 2) {
        unsigned c = b[i] | (unsigned)b[i + 1] << 8;
        if (!c)
            break;
        out[k++] = c < 0x80 && c >= 0x20 ? (char)c : '?';
    }
    out[k] = 0;
    free(b);
    return 0;
}

int gk3l_loader_var_delete(const char *name)
{
    const char *d = efivars_dir();
    char p[512];
    int fd;
    if (!d)
        return -1;
    snprintf(p, sizeof p, "%s/%s-" LOADER_GUID, d, name);
    if (!gk3l_exists(p))
        return 0;
    /* efivarfs 的文件默认带不可变标记（防止误删），chattr -i 的同一个 ioctl */
    fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        int fl = 0;
        if (ioctl(fd, FS_IOC_GETFLAGS, &fl) == 0 && (fl & FS_IMMUTABLE_FL)) {
            fl &= ~FS_IMMUTABLE_FL;
            (void)ioctl(fd, FS_IOC_SETFLAGS, &fl);
        }
        close(fd);
    }
    if (unlink(p) && errno != ENOENT)
        return -1;
    return gk3l_exists(p) ? -1 : 0;
}
