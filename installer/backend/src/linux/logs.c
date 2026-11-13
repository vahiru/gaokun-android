/* Linux：把日志另存到用户拿得到的地方。
 *
 * 问题：安装 U 盘只有一个分区，类型是 EFI System，日志写在它的 gaokun3/diag/ 下；Windows 与 macOS 默认都不给 ESP
 *   分配盘符，装失败的人把 U 盘插回电脑看不到日志。免 U 盘安装时介质是内置盘上的 GK3LIVE（有盘符），回 Windows 就能看到。
 *
 *   gk3l log-targets              能存日志的地方，一行一个（第一个是默认）：
 *     LOGTARGET part=/dev/sda1 fs=vfat size_mib=… label=… removable=yes|no medium=yes|no esp=yes|no
 *     只列 FAT / exFAT：不往 NTFS 写（Windows 休眠或快速启动时写它会损坏数据）。内置盘上的分区不列，除非它就是
 *     启动介质。顺序：另插的 U 盘 → 启动介质本身（esp=yes 的话电脑上不好读）。
 *   gk3l save-logs [--to 分区|目录]  不给就用 log-targets 的第一个；给目录就直接写进去（测试用）。
 *     写一个 gaokun3-logs-<时间>/：界面日志、各 job 的输出、分区表备份、dmesg、本次启动的 journal、gk3l probe、
 *     /etc/gaokun3-release、最近的开机诊断报告。stdout：LOGSAVED dir=… part=…|- files=N esp=yes|no
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

/* blkid -p 的 PART_ENTRY_TYPE（小写） */
static void part_type(const char *dev, char *out, size_t cap)
{
    char buf[2048], *save = NULL;
    char *av[] = {"blkid", "-p", "-o", "udev", (char *)dev, NULL};
    out[0] = 0;
    if (gk3l_run(av, buf, sizeof buf) != 0)
        return;
    for (char *l = strtok_r(buf, "\n", &save); l; l = strtok_r(NULL, "\n", &save))
        if (!strncmp(l, "ID_PART_ENTRY_TYPE=", 19)) {
            snprintf(out, cap, "%s", l + 19);
            for (char *c = out; *c; c++)
                if (*c >= 'A' && *c <= 'Z')
                    *c = (char)(*c + 32);
        }
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
        nk = gk3l_kernel_parts(e->d_name, kp, 64);
        for (size_t i = 0; i < nk && n < cap; i++) {
            target *x = &t[n];
            int med = medium[0] && !strcmp(kp[i].devno, medium);
            char type[64];
            /* 内置盘上的分区（ESP、Windows 的分区）不当存日志的地方 —— 除非它就是启动介质（免 U 盘时的 GK3LIVE） */
            if (!rem && !med)
                continue;
            memset(x, 0, sizeof *x);
            snprintf(x->part, sizeof x->part, "/dev/%.63s", kp[i].name);
            gk3l_blkid(x->part, x->fs, sizeof x->fs, x->label, sizeof x->label);
            if (strcmp(x->fs, "vfat") && strcmp(x->fs, "exfat"))
                continue;
            part_type(x->part, type, sizeof type);
            x->esp = !strcmp(type, ESP_TYPE);
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

static int copy_file(const char *src, const char *dst)
{
    uint8_t *b;
    size_t n;
    int err = gk3l_read_file(src, &b, &n, 256u << 20);
    if (err)
        return err;
    err = gk3l_write_file(dst, b, n);
    free(b);
    return err;
}

/* 跑一条命令，stdout 与 stderr 都进 dst */
static int run_to(char *const av[], const char *dst)
{
    int fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644), saved, r;
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
    close(fd);
    return r < 0 ? -1 : 0;
}

int gk3l_cmd_save_logs(int argc, char **argv)
{
    const char *to = gk3l_arg(argc, argv, "--to"), *run = getenv("GK3_RUNDIR");
    static target t[64];
    char part[96] = "-", m[256] = "", dir[4096], p[4400], q[4500], ts[32], self[4096];
    int own = 0, remount_ro = 0, esp = 0, files = 0, err;
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
        /* 界面日志（介质上的与内存里的）、发行信息 */
        static const char *const src[][2] = {
            {"/media/gk3/gaokun3/diag/installer.log", "diag-installer.log"},
            {"/run/gk3-installer/installer.log", "gk3-installer-installer.log"},
            {"/etc/gaokun3-release", "etc-gaokun3-release"},
            {"/media/gk3/gaokun3/release.txt", "gaokun3-release.txt"},
        };
        for (size_t i = 0; i < sizeof src / sizeof src[0]; i++) {
            snprintf(p, sizeof p, "%s/%s", dir, src[i][1]);
            if (!copy_file(src[i][0], p))
                files++;
        }
    }
    {
        /* 最近的开机诊断报告、分区表备份（介质上的与内存里的）、各 job 的输出 */
        const char *bdirs[] = {"/media/gk3/gaokun3", run};
        char newest[512] = "";
        time_t nt = 0;
        DIR *dd = opendir("/media/gk3/gaokun3/diag");
        struct dirent *e;
        if (dd) {
            while ((e = readdir(dd))) {
                struct stat s2;
                if (strncmp(e->d_name, "boot-", 5) || !strstr(e->d_name, ".log"))
                    continue;
                snprintf(p, sizeof p, "/media/gk3/gaokun3/diag/%s", e->d_name);
                if (!stat(p, &s2) && s2.st_mtime >= nt) {
                    nt = s2.st_mtime;
                    snprintf(newest, sizeof newest, "%s", e->d_name);
                }
            }
            closedir(dd);
        }
        if (newest[0]) {
            snprintf(p, sizeof p, "/media/gk3/gaokun3/diag/%s", newest);
            snprintf(q, sizeof q, "%.4000s/%.400s", dir, newest);
            if (!copy_file(p, q))
                files++;
        }
        for (int k = 0; k < 2; k++) {
            dd = opendir(bdirs[k]);
            if (!dd)
                continue;
            while ((e = readdir(dd))) {
                if (strncmp(e->d_name, "gpt-before-", 11))
                    continue;
                snprintf(p, sizeof p, "%s/%s", bdirs[k], e->d_name);
                snprintf(q, sizeof q, "%s/%s", dir, e->d_name);
                if (!copy_file(p, q))
                    files++;
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
                if (!f)
                    continue;
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
                fclose(f);
                files++;
            }
            closedir(dd);
        }
    }
    {
        char *dm[] = {"dmesg", NULL};
        char *jr[] = {"journalctl", "-b", "--no-pager", "-n", "5000", NULL};
        ssize_t sn = readlink("/proc/self/exe", self, sizeof self - 1);
        snprintf(p, sizeof p, "%s/dmesg.txt", dir);
        if (!run_to(dm, p))
            files++;
        snprintf(p, sizeof p, "%s/journal.txt", dir);
        if (!run_to(jr, p))
            files++;
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
                    if (write(fd, buf, strlen(buf)) >= 0 && r >= 0)
                        files++;
                    close(fd);
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
    gk3w_line_begin(&l, "LOGSAVED");
    gk3w_line_kv(&l, "dir", dir + strlen(m) + 1);
    gk3w_line_kv(&l, "part", part);
    gk3w_line_u64(&l, "files", (uint64_t)files);
    gk3w_line_kv(&l, "esp", esp ? "yes" : "no");
    err = gk3w_line_emit(&l, stdout);
    gk3w_line_free(&l);
    return err ? 1 : 0;
}
