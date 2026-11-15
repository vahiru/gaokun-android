/* Linux：把日志另存到用户拿得到的地方。
 *
 * LiveCD 不自动往介质写任何东西（见 lx.h），日志、任务输出与分区表备份都只在内存里，关机即丢失。装失败时由用户在界面上
 *   另存一份，带到电脑上附在问题报告里。
 *
 *   gk3l log-targets              能存日志的地方，一行一个（第一个是默认）：
 *     LOGTARGET part=/dev/sda1 fs=vfat size_mib=… label=… removable=yes|no medium=yes|no esp=no
 *     只列可移动盘上、Windows 会分配盘符的 FAT / exFAT 分区（GPT 的 Microsoft 基本数据分区，或 MBR 上不是 0xEF 的分区）：
 *     日志要能在电脑上直接打开。不列 NTFS（Windows 休眠或快速启动时写它会损坏数据），不列 EFI 系统分区（dd 写入的
 *     混合 ISO 上那一块：没有盘符，空间也只够放启动文件）与内置盘上的分区（GK3LIVE 是 XBOOTLDR 类型，同样没有盘符）。
 *     顺序：另插的 U 盘 → 启动介质本身（文件拖进 FAT32 U 盘做成的介质）。
 *   gk3l save-logs [--to 分区|目录]  不给就用 log-targets 的第一个；给目录就直接写进去（测试用）。
 *     写一个 gaokun3-logs-<时间>/：界面日志、各 job 的输出、分区表备份、dmesg、本次启动的 journal、gk3l probe、
 *     /etc/gaokun3-release。stdout：LOGSAVED dir=… part=…|- files=N esp=yes|no；有文件没写成（多半是空间不够）时
 *     报 ERR code=logs-incomplete dir= failed=N，退出码 1。
 * 测试用：GK3_ALLOW_LOOP=1 时 loop 设备算可移动盘。 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#include "../proto.h"
#include "disk.h"
#include "files.h"
#include "lx.h"

#define ESP_TYPE "c12a7328-f81f-11d2-ba4b-00a0c93ec93b"
#define BASIC_DATA_TYPE "ebd0a0a2-b9e5-4433-87c0-68b6b72699c7"

/* blkid -p 的 PART_ENTRY_TYPE（小写；GPT 是 GUID，MBR 是 0x0c 这样的类型号）；scheme 可为 NULL */
static void part_type2(const char *dev, char *out, size_t cap, char *scheme, size_t scap)
{
    char buf[2048], *save = NULL;
    char *av[] = {"blkid", "-p", "-o", "udev", (char *)dev, NULL};
    out[0] = 0;
    if (scheme)
        scheme[0] = 0;
    if (gk3l_run(av, buf, sizeof buf) != 0)
        return;
    for (char *l = strtok_r(buf, "\n", &save); l; l = strtok_r(NULL, "\n", &save))
        if (!strncmp(l, "ID_PART_ENTRY_TYPE=", 19)) {
            snprintf(out, cap, "%s", l + 19);
            for (char *c = out; *c; c++)
                if (*c >= 'A' && *c <= 'Z')
                    *c = (char)(*c + 32);
        } else if (scheme && !strncmp(l, "ID_PART_ENTRY_SCHEME=", 21)) {
            snprintf(scheme, scap, "%s", l + 21);
        }
}

static void part_type(const char *dev, char *out, size_t cap)
{
    part_type2(dev, out, cap, NULL, 0);
}

/* Windows 会给它分配盘符：GPT 上只有 Microsoft 基本数据分区，MBR 上除了 EFI 系统分区（0xEF）都会 */
static int gets_drive_letter(const char *scheme, const char *type)
{
    if (!strcmp(scheme, "gpt"))
        return !strcmp(type, BASIC_DATA_TYPE);
    if (!strcmp(scheme, "dos"))
        return strcmp(type, "0xef") != 0;
    return 0;
}

static int removable(const char *disk)
{
    char p[256], v[16] = "", real[4096];
    const char *loop = getenv("GK3_ALLOW_LOOP");
    snprintf(p, sizeof p, "/sys/block/%.200s/removable", disk);
    if (!gk3l_read_line(p, v, sizeof v) && !strcmp(v, "1"))
        return 1;
    snprintf(p, sizeof p, "/sys/block/%.200s", disk);
    if (realpath(p, real) && strstr(real, "/usb"))
        return 1;
    return !strncmp(disk, "loop", 4) && loop && !strcmp(loop, "1");
}

typedef struct {
    char part[96], fs[16], label[256];
    uint64_t size_mib;
    int removable, medium, esp;
} target;

static size_t list_targets(target *t, size_t cap)
{
    char medium[32] = "";
    DIR *sb = opendir("/sys/block");
    struct dirent *e;
    size_t n = 0;
    gk3l_medium_devno(medium, sizeof medium);
    if (!sb)
        return 0;
    while ((e = readdir(sb))) {
        gk3l_kpart kp[64];
        size_t nk;
        int rem;
        if (e->d_name[0] == '.' || !strncmp(e->d_name, "ram", 3) || !strncmp(e->d_name, "zram", 4) || !strncmp(e->d_name, "dm-", 3))
            continue;
        rem = removable(e->d_name);
        /* 内置盘上的分区不当存日志的地方（见文件头） */
        if (!rem)
            continue;
        nk = gk3l_kernel_parts(e->d_name, kp, 64);
        for (size_t i = 0; i < nk && n < cap; i++) {
            target *x = &t[n];
            int med = medium[0] && !strcmp(kp[i].devno, medium);
            char type[64], scheme[16];
            memset(x, 0, sizeof *x);
            snprintf(x->part, sizeof x->part, "/dev/%.63s", kp[i].name);
            gk3l_blkid(x->part, x->fs, sizeof x->fs, x->label, sizeof x->label);
            if (strcmp(x->fs, "vfat") && strcmp(x->fs, "exfat"))
                continue;
            part_type2(x->part, type, sizeof type, scheme, sizeof scheme);
            if (!gets_drive_letter(scheme, type))
                continue;
            x->size_mib = kp[i].size512 / 2048;
            x->removable = rem;
            x->medium = med;
            n++;
        }
    }
    closedir(sb);
    /* 另插的 U 盘排在启动介质前面 */
    for (size_t i = 0; i < n; i++)
        for (size_t j = i + 1; j < n; j++)
            if (t[i].medium && !t[j].medium) {
                target tmp = t[i];
                t[i] = t[j];
                t[j] = tmp;
            }
    return n;
}

int gk3l_cmd_log_targets(int argc, char **argv)
{
    static target t[64];
    size_t n = list_targets(t, 64);
    (void)argc;
    (void)argv;
    for (size_t i = 0; i < n; i++) {
        gk3w_line l = {0};
        gk3w_line_begin(&l, "LOGTARGET");
        gk3w_line_kv(&l, "part", t[i].part);
        gk3w_line_kv(&l, "fs", t[i].fs);
        gk3w_line_u64(&l, "size_mib", t[i].size_mib);
        gk3w_line_kv(&l, "label", t[i].label);
        gk3w_line_kv(&l, "removable", t[i].removable ? "yes" : "no");
        gk3w_line_kv(&l, "medium", t[i].medium ? "yes" : "no");
        gk3w_line_kv(&l, "esp", t[i].esp ? "yes" : "no");
        (void)gk3w_line_emit(&l, stdout);
        gk3w_line_free(&l);
    }
    return 0;
}

/* 0：抄好了；1：源文件读不到（没有就跳过）；-1：写不进去（多半是空间不够，写了一半的删掉） */
static int copy_file(const char *src, const char *dst)
{
    uint8_t *b;
    size_t n;
    int err;
    if (gk3l_read_file(src, &b, &n, 256u << 20))
        return 1;
    err = gk3l_write_file(dst, b, n);
    free(b);
    if (err) {
        unlink(dst);
        return -1;
    }
    return 0;
}

static void tally(int r, int *files, int *failed)
{
    if (!r)
        (*files)++;
    else if (r < 0)
        (*failed)++;
}

/* 跑一条命令，stdout 与 stderr 都进 dst。0：写好了；1：命令起不来；-1：文件写不进去 */
static int run_to(char *const av[], const char *dst)
{
    int fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644), saved, r, synced;
    if (fd < 0)
        return -1;
    fflush(stdout);
    fflush(stderr);
    saved = dup(2);
    dup2(fd, 2);
    r = gk3l_run(av, NULL, 0);   /* 子进程的 stdout 并进 stderr，也就是 dst */
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    synced = fsync(fd);
    close(fd);
    if (synced)
        return -1;
    return r < 0 ? 1 : 0;
}

int gk3l_cmd_save_logs(int argc, char **argv)
{
    const char *to = gk3l_arg(argc, argv, "--to"), *run = getenv("GK3_RUNDIR");
    static target t[64];
    char part[96] = "-", m[256] = "", dir[4096], p[4400], q[4500], ts[32], self[4096];
    int own = 0, remount_ro = 0, esp = 0, files = 0, failed = 0, err;
    time_t now = time(NULL);
    struct tm tmv;
    struct stat st;
    gk3w_line l = {0};

    if (!run || !*run)
        run = "/run/gaokun3";
    if (!to) {
        size_t n = list_targets(t, 64);
        if (!n)
            return gk3l_err(GK3W_TOUCHED_NA, "logs-no-target", "no place to save the logs: plug in a FAT or exFAT USB drive and try again", 0);
        to = t[0].part;
    }
    if (stat(to, &st))
        return gk3l_err_kv(GK3W_TOUCHED_NA, "usage", "to", to, "neither a partition nor a directory", errno);
    if (S_ISDIR(st.st_mode)) {
        snprintf(m, sizeof m, "%s", to);
    } else if (S_ISBLK(st.st_mode)) {
        char fs[16], label[256], type[64], devno[32];
        snprintf(part, sizeof part, "%s", to);
        gk3l_blkid(part, fs, sizeof fs, label, sizeof label);
        if (strcmp(fs, "vfat") && strcmp(fs, "exfat"))
            return gk3l_err_kv(GK3W_TOUCHED_NA, "logs-not-fat", "part", part, "the partition is not FAT or exFAT; logs are not written to other file systems", 0);
        part_type(part, type, sizeof type);
        esp = !strcmp(type, ESP_TYPE);
        snprintf(devno, sizeof devno, "%u:%u", major(st.st_rdev), minor(st.st_rdev));
        if (!gk3l_mount_of(devno, m, sizeof m)) {
            struct statvfs sv;
            if (!statvfs(m, &sv) && (sv.f_flag & ST_RDONLY)) {
                if (mount(NULL, m, NULL, MS_REMOUNT | (sv.f_flag & ST_NOSUID ? MS_NOSUID : 0) | (sv.f_flag & ST_NODEV ? MS_NODEV : 0) |
                                             (sv.f_flag & ST_NOEXEC ? MS_NOEXEC : 0), NULL))
                    return gk3l_err_kv(GK3W_TOUCHED_NA, "logs-readonly", "part", part, "the partition cannot be made writable (write-protected?)", errno);
                remount_ro = 1;
            }
        } else if ((err = gk3l_mount_tmp(part, fs, 0, m, sizeof m))) {
            return gk3l_err_kv(GK3W_TOUCHED_NA, "logs-mount", "part", part, "cannot mount the partition", err);
        } else {
            own = 1;
        }
    } else {
        return gk3l_err_kv(GK3W_TOUCHED_NA, "usage", "to", to, "neither a partition nor a directory", 0);
    }
    localtime_r(&now, &tmv);
    strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", &tmv);
    snprintf(dir, sizeof dir, "%s/gaokun3-logs-%s", m, ts);
    snprintf(p, sizeof p, "%s/jobs", dir);
    if (gk3l_mkdirs(p)) {
        files = -1;
        goto done;
    }
    {
        /* 界面日志、发行信息 */
        static const char *const src[][2] = {
            {"/run/gk3-installer/installer.log", "gk3-installer-installer.log"},
            {"/etc/gaokun3-release", "etc-gaokun3-release"},
            {"/media/gk3/gaokun3/release.txt", "gaokun3-release.txt"},
        };
        for (size_t i = 0; i < sizeof src / sizeof src[0]; i++) {
            snprintf(p, sizeof p, "%s/%s", dir, src[i][1]);
            tally(copy_file(src[i][0], p), &files, &failed);
        }
    }
    {
        /* 分区表备份、各 job 的输出 */
        DIR *dd;
        struct dirent *e;
        dd = opendir(run);
        if (dd) {
            while ((e = readdir(dd))) {
                if (strncmp(e->d_name, "gpt-before-", 11))
                    continue;
                snprintf(p, sizeof p, "%s/%s", run, e->d_name);
                snprintf(q, sizeof q, "%s/%s", dir, e->d_name);
                tally(copy_file(p, q), &files, &failed);
            }
            closedir(dd);
        }
        snprintf(p, sizeof p, "%s/jobs", run);
        dd = opendir(p);
        if (dd) {
            while ((e = readdir(dd))) {
                char jd[4200], out[4600];
                FILE *f;
                if (e->d_name[0] == '.')
                    continue;
                snprintf(jd, sizeof jd, "%s/jobs/%s", run, e->d_name);
                snprintf(out, sizeof out, "%s/jobs/%s.log", dir, e->d_name);
                f = fopen(out, "we");
                if (!f) {
                    failed++;
                    continue;
                }
                for (int s = 0; s < 4; s++) {
                    static const char *const nm[] = {"cmd", "rc", "out", "err"};
                    uint8_t *b;
                    size_t n;
                    snprintf(q, sizeof q, "%s/%s", jd, nm[s]);
                    fprintf(f, "--- %s\n", nm[s]);
                    if (!gk3l_read_file(q, &b, &n, 64u << 20)) {
                        fwrite(b, 1, n, f);
                        free(b);
                    }
                }
                int bad = ferror(f);
                if (fclose(f) || bad) {
                    unlink(out);
                    failed++;
                } else {
                    files++;
                }
            }
            closedir(dd);
        }
    }
    {
        char *dm[] = {"dmesg", NULL};
        char *jr[] = {"journalctl", "-b", "--no-pager", "-n", "5000", NULL};
        ssize_t sn = readlink("/proc/self/exe", self, sizeof self - 1);
        snprintf(p, sizeof p, "%s/dmesg.txt", dir);
        tally(run_to(dm, p), &files, &failed);
        snprintf(p, sizeof p, "%s/journal.txt", dir);
        tally(run_to(jr, p), &files, &failed);
        if (sn > 0) {
            char *pr[] = {self, "probe", NULL};
            self[sn] = 0;
            snprintf(p, sizeof p, "%s/probe.txt", dir);
            /* probe 的记录在 stdout，收进来再写进文件（错误行进日志） */
            {
                int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
                if (fd >= 0) {
                    char buf[1 << 16];
                    int r = gk3l_run(pr, buf, sizeof buf);
                    size_t len = strlen(buf);
                    if (write(fd, buf, len) != (ssize_t)len || fsync(fd))
                        failed++;
                    else if (r >= 0)
                        files++;
                    close(fd);
                } else {
                    failed++;
                }
            }
        }
    }
    sync();
done:
    if (own)
        gk3l_umount_tmp(m);
    else if (remount_ro)
        (void)mount(NULL, m, NULL, MS_REMOUNT | MS_RDONLY, NULL);
    if (files < 0)
        return gk3l_err_kv(GK3W_TOUCHED_NA, "logs-mkdir", "to", to, "cannot create a directory there (full?)", 0);
    if (failed) {
        char nf[16];
        snprintf(nf, sizeof nf, "%d", failed);
        return gk3l_err_kv2(GK3W_TOUCHED_NA, "logs-incomplete", "dir", dir + strlen(m) + 1, "failed", nf,
                            "some files could not be written (out of space?); the logs are incomplete", 0);
    }
    gk3w_line_begin(&l, "LOGSAVED");
    gk3w_line_kv(&l, "dir", dir + strlen(m) + 1);
    gk3w_line_kv(&l, "part", part);
    gk3w_line_u64(&l, "files", (uint64_t)files);
    gk3w_line_kv(&l, "esp", esp ? "yes" : "no");
    err = gk3w_line_emit(&l, stdout);
    gk3w_line_free(&l);
    return err ? 1 : 0;
}
