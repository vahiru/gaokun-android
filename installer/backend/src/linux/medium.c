/* gk3l：安装介质，以及诊断文件（分区表备份、任务日志）的持久保存（说明见 lx.h）。 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "disk.h"
#include "files.h"
#include "lx.h"

#define MEDIA "/media/gk3"

typedef struct {
    char disk[64];
    uint64_t start, size;       /* sysfs 的 512 字节单位 */
    uint64_t esp_start, esp_size;   /* 删掉安装器分区之后：持久保存改去的 ESP（gk3l_medium_note_esp） */
    int ram, ok;
} medium_rec;

static const char *rec_path(void)
{
    const char *p = getenv("GK3_MEDIUM_FILE");
    return p && *p ? p : "/run/gaokun3/medium";
}

static void read_rec(medium_rec *m)
{
    FILE *f;
    char line[256];
    int have = 0;
    memset(m, 0, sizeof *m);
    f = fopen(rec_path(), "re");
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        char *v = strchr(line, '='), *end = NULL;
        if (!v)
            continue;
        *v++ = 0;
        v[strcspn(v, "\r\n")] = 0;
        if (!strcmp(line, "disk") && *v && strlen(v) < sizeof m->disk && !strchr(v, '/')) {
            strcpy(m->disk, v);
            have |= 1;
        } else if (!strcmp(line, "start") && *v >= '0' && *v <= '9') {
            m->start = strtoull(v, &end, 10);
            have |= *end ? 0 : 2;
        } else if (!strcmp(line, "size") && *v >= '0' && *v <= '9') {
            m->size = strtoull(v, &end, 10);
            have |= *end ? 0 : 4;
        } else if (!strcmp(line, "ram")) {
            m->ram = !strcmp(v, "yes");
        } else if (!strcmp(line, "esp_start") && *v >= '0' && *v <= '9') {
            m->esp_start = strtoull(v, NULL, 10);
        } else if (!strcmp(line, "esp_size") && *v >= '0' && *v <= '9') {
            m->esp_size = strtoull(v, NULL, 10);
        }
    }
    fclose(f);
    m->ok = have == 7 && m->size;
}

/* 盘上起点、大小是这样的分区现在是哪个（sysfs）；整盘清空时分区被重新加进内核，分区号可能变 */
static int find_kpart(const char *disk, uint64_t start, uint64_t size, gk3l_kpart *out)
{
    static gk3l_kpart kp[256];
    size_t n = gk3l_kernel_parts(disk, kp, 256);
    for (size_t i = 0; i < n; i++)
        if (kp[i].start512 == start && kp[i].size512 == size) {
            *out = kp[i];
            return 0;
        }
    return -1;
}

static int rec_part(const medium_rec *m, gk3l_kpart *out)
{
    return find_kpart(m->disk, m->start, m->size, out);
}

void gk3l_medium_note_esp(uint64_t start512, uint64_t size512)
{
    FILE *f = fopen(rec_path(), "ae");
    if (!f || fprintf(f, "esp_start=%llu\nesp_size=%llu\n", (unsigned long long)start512, (unsigned long long)size512) < 0)
        gk3l_log("warning: cannot note the EFI system partition in the medium record; later logs stay in memory");
    if (f)
        fclose(f);
}

int gk3l_medium_devno(char *out, size_t cap)
{
    medium_rec m;
    const char *dir = getenv("GK3_MEDIUM_DIR");
    read_rec(&m);
    if (m.ok && m.ram) {
        gk3l_kpart k;
        if (rec_part(&m, &k) || strlen(k.devno) >= cap)
            return -1;
        strcpy(out, k.devno);
        return 0;
    }
    return gk3l_devno_at(dir && *dir ? dir : MEDIA, out, cap);
}

int gk3l_medium_ram(void)
{
    medium_rec m;
    read_rec(&m);
    return m.ok && m.ram;
}

/* ── 持久保存 ── */

static struct {
    int open;
    int how;            /* 0 = /media/gk3 本来就可写；1 = 从只读改成了可写（关时改回）；2 = 临时挂载（关时卸下） */
    unsigned long fl;   /* how == 1：原来的挂载选项 */
    char mnt[4096];
} P;
static char esp_node[96];

static unsigned long keep_flags(const struct statvfs *sv)
{
    return (sv->f_flag & ST_NOSUID ? MS_NOSUID : 0) | (sv->f_flag & ST_NODEV ? MS_NODEV : 0) | (sv->f_flag & ST_NOEXEC ? MS_NOEXEC : 0) |
           (sv->f_flag & ST_NOATIME ? MS_NOATIME : 0) | (sv->f_flag & ST_RELATIME ? MS_RELATIME : 0);
}

/* 临时把一个 FAT / exFAT 分区挂成可写，目录是 <挂载点>/gaokun3（没有就建） */
static int open_tmp(const char *node, char *dir, size_t cap)
{
    char fs[16], label[256];
    gk3l_blkid(node, fs, sizeof fs, label, sizeof label);
    if (strcmp(fs, "vfat") && strcmp(fs, "exfat"))
        return -1;
    if (gk3l_mount_tmp(node, fs, 0, P.mnt, sizeof P.mnt))
        return -1;
    if ((size_t)snprintf(dir, cap, "%s/gaokun3", P.mnt) >= cap || gk3l_mkdirs(dir)) {
        gk3l_umount_tmp(P.mnt);
        return -1;
    }
    P.how = 2;
    P.open = 1;
    return 0;
}

int gk3l_persist_open(char *dir, size_t cap)
{
    medium_rec m;
    struct statvfs sv;
    if (P.open)
        return -1;
    if (esp_node[0])
        return open_tmp(esp_node, dir, cap);
    read_rec(&m);
    if (m.ok && m.ram) {
        gk3l_kpart k;
        char node[96];
        /* 介质分区已经删掉了（remove-installer）：改去当时记下的 ESP */
        if (rec_part(&m, &k) && (!m.esp_size || find_kpart(m.disk, m.esp_start, m.esp_size, &k)))
            return -1;
        snprintf(node, sizeof node, "/dev/%s", k.name);
        return open_tmp(node, dir, cap);
    }
    /* 介质挂在 /media/gk3：平时只读，安装器会话通常已经把它改成了可写（gk3-installer-session）。改过的不改回去 ——
     * 会话还开着 installer.log 在写 */
    if (!gk3l_is_dir(MEDIA "/gaokun3") || statvfs(MEDIA, &sv) || cap <= strlen(MEDIA "/gaokun3"))
        return -1;
    P.how = 0;
    if (sv.f_flag & ST_RDONLY) {
        P.fl = keep_flags(&sv);
        if (mount(NULL, MEDIA, NULL, MS_REMOUNT | P.fl, NULL))
            return -1;
        P.how = 1;
    }
    strcpy(dir, MEDIA "/gaokun3");
    P.open = 1;
    return 0;
}

void gk3l_persist_close(void)
{
    if (!P.open)
        return;
    sync();
    if (P.how == 2)
        (void)gk3l_umount_tmp(P.mnt);
    else if (P.how == 1)
        (void)mount(NULL, MEDIA, NULL, MS_REMOUNT | MS_RDONLY | P.fl, NULL);
    P.open = 0;
}

void gk3l_persist_to_esp(const char *node)
{
    snprintf(esp_node, sizeof esp_node, "%s", node ? node : "");
}

static int copy_to(const char *src, const char *dst)
{
    uint8_t *b;
    size_t n;
    int err = gk3l_read_file(src, &b, &n, 256u << 20);
    if (!err) {
        err = gk3l_write_file(dst, b, n);
        free(b);
    }
    if (err)
        gk3l_log("warning: could not keep %s on persistent storage (errno %d)", src, err);
    return err ? -1 : 0;
}

/* 内存副本里要写回的文件：diag/ 下的普通文件、installer-lang、分区表备份。对每一个调 f（rel 相对 gaokun3/）；
 * f 返回非 0 时记一次失败。返回失败次数 */
static int each_file(int (*f)(const char *src, const char *rel, void *ctx), void *ctx)
{
    char src[4400], rel[4400];
    DIR *dd;
    struct dirent *e;
    struct stat st;
    int bad = 0;
    if ((dd = opendir(MEDIA "/gaokun3/diag"))) {
        while ((e = readdir(dd))) {
            snprintf(src, sizeof src, "%s/%s", MEDIA "/gaokun3/diag", e->d_name);
            if (e->d_name[0] == '.' || stat(src, &st) || !S_ISREG(st.st_mode))
                continue;
            snprintf(rel, sizeof rel, "diag/%s", e->d_name);
            bad += f(src, rel, ctx) != 0;
        }
        closedir(dd);
    }
    if (!stat(MEDIA "/gaokun3/installer-lang", &st) && S_ISREG(st.st_mode))
        bad += f(MEDIA "/gaokun3/installer-lang", "installer-lang", ctx) != 0;
    if ((dd = opendir(MEDIA "/gaokun3"))) {
        while ((e = readdir(dd))) {
            snprintf(src, sizeof src, "%s/%s", MEDIA "/gaokun3", e->d_name);
            if (strncmp(e->d_name, "gpt-before-", 11) || stat(src, &st) || !S_ISREG(st.st_mode))
                continue;
            bad += f(src, e->d_name, ctx) != 0;
        }
        closedir(dd);
    }
    return bad;
}

static int add_size(const char *src, const char *rel, void *ctx)
{
    struct stat st;
    (void)rel;
    if (!stat(src, &st))
        *(uint64_t *)ctx += (uint64_t)st.st_size;
    return 0;
}

uint64_t gk3l_persist_bytes(void)
{
    uint64_t n = 0;
    (void)each_file(add_size, &n);
    return n;
}

static int sync_one(const char *src, const char *rel, void *ctx)
{
    char dst[8600];
    snprintf(dst, sizeof dst, "%s/%s", (const char *)ctx, rel);
    /* 分区表备份写了就不再变：已经有的不重写 */
    if (!strncmp(rel, "gpt-before-", 11) && gk3l_exists(dst))
        return 0;
    return copy_to(src, dst);
}

int gk3l_persist_sync(void)
{
    char pd[4096], dd[4200];
    int bad;
    if (!gk3l_medium_ram())
        return 0;
    if (gk3l_persist_open(pd, sizeof pd))
        return -1;
    snprintf(dd, sizeof dd, "%s/diag", pd);
    bad = gk3l_mkdirs(dd) ? 1 : each_file(sync_one, pd);
    gk3l_persist_close();
    return bad ? -1 : 0;
}

int gk3l_cmd_persist_sync(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return gk3l_persist_sync() ? 1 : 0;
}
