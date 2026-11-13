/* gk3l：块设备、sysfs 的分区视图、主 GPT（说明见 disk.h）。 */
#define _GNU_SOURCE
#include "disk.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/blkpg.h>
#include <linux/fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#include "../gptw.h"
#include "../layout.h"
#include "../release.h"
#include "files.h"
#include "lx.h"

/* ── 读写 ── */

static int io_all(int fd, void *buf, size_t len, uint64_t off, int wr)
{
    uint8_t *p = buf;
    while (len) {
        ssize_t n = wr ? pwrite(fd, p, len, (off_t)off) : pread(fd, p, len, (off_t)off);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            if (n == 0)
                errno = EIO;
            return -1;
        }
        p += n;
        len -= (size_t)n;
        off += (uint64_t)n;
    }
    return 0;
}

int gk3l_pread(const gk3l_disk *d, void *buf, size_t len, uint64_t off)
{
    return io_all(d->fd, buf, len, off, 0);
}

int gk3l_pwrite(const gk3l_disk *d, const void *buf, size_t len, uint64_t off)
{
    return io_all(d->fd, (void *)buf, len, off, 1);
}

int gk3l_write_verify(const gk3l_disk *d, const void *buf, size_t len, uint64_t off, void *chk)
{
    if (gk3l_pwrite(d, buf, len, off))
        return -1;
    /* O_DIRECT 绕过页缓存：读回的是设备上的内容。再 fdatasync 一次，让设备自己的写缓存也落到介质上 */
    if (fdatasync(d->fd))
        return -1;
    if (gk3l_pread(d, chk, len, off))
        return -1;
    return memcmp(buf, chk, len) ? -2 : 0;
}

static int blk_read(void *ctx, uint64_t lba, uint32_t count, void *buf)
{
    gk3l_disk *d = ctx;
    if (lba > UINT64_MAX / d->bs || (uint64_t)count * d->bs > SIZE_MAX)
        return -1;
    return gk3l_pread(d, buf, (size_t)count * d->bs, lba * d->bs);
}

/* ── 打开 ── */

int gk3l_disk_open(gk3l_disk *d, const char *name, int rw)
{
    char p[256];
    int ss = 0, ps = 0, err;
    memset(d, 0, sizeof *d);
    d->fd = -1;
    snprintf(d->name, sizeof d->name, "%s", name);
    snprintf(d->path, sizeof d->path, "/dev/%s", name);
    snprintf(p, sizeof p, "/sys/block/%s/dev", name);
    gk3l_read_line(p, d->devno, sizeof d->devno);
    d->direct = rw;
    d->fd = open(d->path, (rw ? O_RDWR | O_DIRECT : O_RDONLY) | O_CLOEXEC);
    if (d->fd < 0)
        return errno;
    if (ioctl(d->fd, BLKSSZGET, &ss) == 0)
        d->bs = (uint32_t)ss;
    if (ioctl(d->fd, BLKPBSZGET, &ps) == 0)
        d->pbs = (uint32_t)ps;
    if (ioctl(d->fd, BLKGETSIZE64, &d->bytes)) {
        err = errno;
        close(d->fd);
        d->fd = -1;
        return err;
    }
    d->blocks = d->bs ? d->bytes / d->bs : 0;
    return 0;
}

void gk3l_disk_close(gk3l_disk *d)
{
    if (d->fd >= 0)
        close(d->fd);
    d->fd = -1;
    gk3w_aligned_free(d->hdr);
    gk3w_aligned_free(d->ent);
    d->hdr = d->ent = NULL;
}

int gk3l_whole_disk_name(const char *path, char *out, size_t cap)
{
    struct stat st;
    char p[128], *base;
    char real[4096];
    if (stat(path, &st) || !S_ISBLK(st.st_mode))
        return -1;
    snprintf(p, sizeof p, "/sys/dev/block/%u:%u", major(st.st_rdev), minor(st.st_rdev));
    if (!realpath(p, real))
        return -1;
    /* 整块盘没有 partition 文件 */
    snprintf(p, sizeof p, "/sys/dev/block/%u:%u/partition", major(st.st_rdev), minor(st.st_rdev));
    if (!access(p, F_OK))
        return -1;
    base = strrchr(real, '/');
    if (!base || strlen(base + 1) >= cap)
        return -1;
    strcpy(out, base + 1);
    return 0;
}

/* ── sysfs ── */

size_t gk3l_kernel_parts(const char *disk, gk3l_kpart *out, size_t cap)
{
    char p[512];
    DIR *dir;
    struct dirent *e;
    size_t n = 0;
    snprintf(p, sizeof p, "/sys/block/%s", disk);
    dir = opendir(p);
    if (!dir)
        return 0;
    while ((e = readdir(dir)) && n < cap) {
        gk3l_kpart *k = &out[n];
        if (e->d_name[0] == '.' || strlen(e->d_name) >= sizeof k->name)
            continue;
        snprintf(p, sizeof p, "/sys/block/%s/%s/partition", disk, e->d_name);
        if (gk3l_read_u64(p, &k->num))
            continue;
        memcpy(k->name, e->d_name, strlen(e->d_name) + 1);   /* 长度上面已查过 */
        snprintf(p, sizeof p, "/sys/block/%s/%s/start", disk, e->d_name);
        if (gk3l_read_u64(p, &k->start512))
            continue;
        snprintf(p, sizeof p, "/sys/block/%s/%s/size", disk, e->d_name);
        if (gk3l_read_u64(p, &k->size512))
            continue;
        snprintf(p, sizeof p, "/sys/block/%s/%s/dev", disk, e->d_name);
        gk3l_read_line(p, k->devno, sizeof k->devno);
        n++;
    }
    closedir(dir);
    return n;
}

const gk3l_kpart *gk3l_kernel_find(const gk3l_kpart *k, size_t n, uint64_t num)
{
    for (size_t i = 0; i < n; i++)
        if (k[i].num == num)
            return &k[i];
    return NULL;
}

void gk3l_part_devname(const char *disk, uint32_t idx, char *out, size_t cap)
{
    size_t n = strlen(disk);
    snprintf(out, cap, "%s%s%u", disk, n && disk[n - 1] >= '0' && disk[n - 1] <= '9' ? "p" : "", idx);
}

/* ── 分区表 ── */

/* 没有 GPT 表头时看 LBA 0：保护性 MBR（有 0xEE 分区）⇒ damaged（是 GPT 盘，表头没了）；0x55AA 结尾、
 * 又不是整盘文件系统的引导扇区 ⇒ mbr；否则 raw。读不出返回 NULL */
static const char *classify_no_gpt(gk3l_disk *d)
{
    uint8_t *b = gk3w_aligned_alloc(d->bs);
    const char *r = "raw";
    gk3w_content_kind k;
    int pmbr = 0;
    if (!b)
        return "raw";
    if (blk_read(d, 0, 1, b)) {
        gk3w_aligned_free(b);
        return NULL;
    }
    k = gk3w_content_kind_of(b, d->bs);
    for (int i = 0; i < 4; i++)
        if (b[446 + 16 * i + 4] == 0xEE)
            pmbr = 1;
    if (b[510] == 0x55 && b[511] == 0xAA && pmbr)
        r = "damaged";
    else if (b[510] == 0x55 && b[511] == 0xAA && k != GK3W_KIND_NTFS && k != GK3W_KIND_FAT && k != GK3W_KIND_EXFAT &&
             k != GK3W_KIND_BITLOCKER)
        r = "mbr";
    gk3w_aligned_free(b);
    return r;
}

int gk3l_disk_read_table(gk3l_disk *d)
{
    gk3_blk dev = {d, d->bs, d->blocks, blk_read, NULL, NULL};
    if (!d->hdr)
        d->hdr = gk3w_aligned_alloc(d->bs < 4096 ? 4096 : d->bs);
    if (!d->ent)
        d->ent = gk3w_aligned_alloc(GK3L_ENTRIES_MAX);
    if (!d->hdr || !d->ent)
        return -1;
    d->nk = gk3l_kernel_parts(d->name, d->kp, sizeof d->kp / sizeof d->kp[0]);
    d->table = "gpt";
    d->gerr = gk3_gpt_read(&dev, &d->g, d->hdr, d->ent, GK3L_ENTRIES_MAX);
    if (d->gerr == GK3_EIO) {
        d->table = "unreadable";
    } else if (d->gerr == GK3_EMAGIC) {
        d->table = classify_no_gpt(d);
        if (!d->table)
            d->table = "unreadable", d->gerr = GK3_EIO;
    } else if (d->gerr != GK3_OK) {
        d->table = "damaged";
    }
    return 0;
}

int gk3l_disk_kernel_ok(const gk3l_disk *d)
{
    uint32_t f = d->bs / 512, used = 0;
    if (strcmp(d->table, "gpt"))
        return 0;
    for (uint32_t i = 0; i < d->g.num_entries; i++) {
        gk3_gpt_part p;
        const gk3l_kpart *k;
        if (gk3_gpt_get(&d->g, i, &p) != GK3_OK)
            continue;
        used++;
        k = gk3l_kernel_find(d->kp, d->nk, p.index);
        if (!k || k->start512 != p.first_lba * f || k->size512 != (p.last_lba - p.first_lba + 1) * f)
            return 0;
    }
    return used == d->nk;
}

/* ── 文本 ── */

void gk3l_guid_upper(const uint8_t g[16], char out[37])
{
    gk3_guid_str(g, out);
    for (char *c = out; *c; c++)
        if (*c >= 'a' && *c <= 'f')
            *c = (char)(*c - 32);
}

void gk3l_gpt_name_utf8(const gk3_gpt *g, uint32_t idx0, char *out, size_t cap)
{
    const uint8_t *e = g->entries + (size_t)idx0 * g->entry_size + 56;
    size_t n = 0;
    for (int k = 0; k < GK3_GPT_NAME_CHARS; k++) {
        uint32_t c = gk3_le16(e + 2 * k);
        uint8_t u[4];
        size_t m;
        if (!c)
            break;
        /* 代理对拼成一个码点；落单的代理换成 U+FFFD */
        if (c >= 0xD800 && c <= 0xDBFF && k + 1 < GK3_GPT_NAME_CHARS) {
            uint32_t lo = gk3_le16(e + 2 * (k + 1));
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                k++;
            } else {
                c = 0xFFFD;
            }
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            c = 0xFFFD;
        }
        if (c < 0x80) {
            u[0] = (uint8_t)c;
            m = 1;
        } else if (c < 0x800) {
            u[0] = (uint8_t)(0xC0 | (c >> 6));
            u[1] = (uint8_t)(0x80 | (c & 0x3F));
            m = 2;
        } else if (c < 0x10000) {
            u[0] = (uint8_t)(0xE0 | (c >> 12));
            u[1] = (uint8_t)(0x80 | ((c >> 6) & 0x3F));
            u[2] = (uint8_t)(0x80 | (c & 0x3F));
            m = 3;
        } else {
            u[0] = (uint8_t)(0xF0 | (c >> 18));
            u[1] = (uint8_t)(0x80 | ((c >> 12) & 0x3F));
            u[2] = (uint8_t)(0x80 | ((c >> 6) & 0x3F));
            u[3] = (uint8_t)(0x80 | (c & 0x3F));
            m = 4;
        }
        if (n + m >= cap)
            break;
        memcpy(out + n, u, m);
        n += m;
    }
    out[n] = 0;
}

const char *gk3l_gpt_why(gk3_err e)
{
    switch (e) {
    case GK3_EMAGIC: return "magic";
    case GK3_ECRC: return "crc";
    case GK3_EVERSION: return "version";
    case GK3_ERANGE: return "range";
    case GK3_ENOSPC: return "too-many-entries";
    default: return "invalid";
    }
}

static int hexval(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* blkid 的 udev 格式把不安全的字符写成 \xNN */
static void unescape_hex(char *s)
{
    char *w = s;
    for (char *r = s; *r; r++) {
        if (r[0] == '\\' && r[1] == 'x' && hexval(r[2]) >= 0 && hexval(r[3]) >= 0) {
            *w++ = (char)(hexval(r[2]) * 16 + hexval(r[3]));
            r += 3;
        } else {
            *w++ = *r;
        }
    }
    *w = 0;
}

void gk3l_blkid(const char *dev, char *fs, size_t fscap, char *label, size_t lcap)
{
    char out[4096], *save = NULL;
    char *argv[] = {"blkid", "-p", "-o", "udev", (char *)dev, NULL};
    fs[0] = label[0] = 0;
    /* 退出码 2 = 认不出文件系统（空分区）：照常报空 */
    if (gk3l_run(argv, out, sizeof out) != 0)
        return;
    for (char *line = strtok_r(out, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (!strncmp(line, "ID_FS_TYPE=", 11))
            snprintf(fs, fscap, "%s", line + 11);
        else if (!strncmp(line, "ID_FS_LABEL_ENC=", 16)) {
            snprintf(label, lcap, "%s", line + 16);
            unescape_hex(label);
        }
    }
}

/* ── 内核的分区、锁、备份（写盘的命令共用） ── */

int gk3l_disk_lock(gk3l_disk *d)
{
    /* 排他锁：udev 见到它就不去探测这块盘（systemd 的块设备锁约定）；另一个安装器进程也拿不到。等最多 5 秒 */
    for (int i = 0; i < 50; i++) {
        if (!flock(d->fd, LOCK_EX | LOCK_NB))
            return 0;
        usleep(100000);
    }
    return -1;
}

int gk3l_kernel_add(const gk3l_disk *d, uint32_t idx, uint64_t first, uint64_t last)
{
    struct blkpg_partition p;
    struct blkpg_ioctl_arg a;
    memset(&p, 0, sizeof p);
    p.start = (long long)(first * d->bs);
    p.length = (long long)((last - first + 1) * d->bs);
    p.pno = (int)idx;
    memset(&a, 0, sizeof a);
    a.op = BLKPG_ADD_PARTITION;
    a.datalen = sizeof p;
    a.data = &p;
    return ioctl(d->fd, BLKPG, &a) ? errno : 0;
}

int gk3l_kernel_resize(const gk3l_disk *d, uint32_t idx, uint64_t first, uint64_t last)
{
    struct blkpg_partition p;
    struct blkpg_ioctl_arg a;
    memset(&p, 0, sizeof p);
    p.start = (long long)(first * d->bs);
    p.length = (long long)((last - first + 1) * d->bs);
    p.pno = (int)idx;
    memset(&a, 0, sizeof a);
    a.op = BLKPG_RESIZE_PARTITION;
    a.datalen = sizeof p;
    a.data = &p;
    return ioctl(d->fd, BLKPG, &a) ? errno : 0;
}

/* 等内核建好 idx 号分区的节点：sysfs 里有它、/dev 下同名节点是块设备且设备号一致。最多等 10 秒 */
int gk3l_wait_node(const gk3l_disk *d, uint32_t idx, char *out, size_t cap)
{
    for (int i = 0; i < 100; i++) {
        gk3l_kpart kp[256];
        size_t nk = gk3l_kernel_parts(d->name, kp, 256);
        const gk3l_kpart *k = gk3l_kernel_find(kp, nk, idx);
        struct stat st;
        if (k) {
            unsigned ma, mi;
            snprintf(out, cap, "/dev/%s", k->name);
            if (sscanf(k->devno, "%u:%u", &ma, &mi) == 2 && !stat(out, &st) && S_ISBLK(st.st_mode) &&
                major(st.st_rdev) == ma && minor(st.st_rdev) == mi)
                return 0;
        }
        usleep(100000);
    }
    return -1;
}

/* 丢掉分区设备的页缓存：新分区是经整块盘写的，谁之前经分区节点读过，缓存里就是旧内容 */
void gk3l_flush_node(const char *dev)
{
    int fd = open(dev, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        (void)ioctl(fd, BLKFLSBUF, 0);
        close(fd);
    }
}

/* ── 分区表备份 ── */

/* 盘头 head 个扇区与盘尾 tail 个扇区原样存进 <dir>/gpt-before-<tag>-<盘>-<时间>-head<N>-tail<M>.bin（与 Windows 版同一格式）。
 * GPT 盘上 head / tail 就是主表区（LBA 0 到 usable 起点）与备份表区（usable 终点之后到盘尾）。还原：两段各自 dd 回原位 */
/* 写一份备份文件并读回核对；返回 0 或 errno */
static int backup_to(const char *dir, const char *name, const uint8_t *data, size_t len, char *path, size_t cap)
{
    int err = gk3l_mkdirs(dir);
    snprintf(path, cap, "%s/%s", dir, name);
    if (!err)
        err = gk3l_write_file(path, data, len);
    if (!err && !gk3l_file_same(path, data, len))
        err = EIO;
    /* 写了一半的备份不能留下：以后会被当成一份完整的 */
    if (err)
        unlink(path);
    return err;
}

int gk3l_gpt_backup(const gk3l_disk *d, const char *dir_arg, const char *tag, uint64_t head, uint64_t tail)
{
    const char *run = getenv("GK3_RUNDIR");
    char name[256], path[4400], ts[32];
    uint8_t *gb;
    size_t len = (size_t)((head + tail) * d->bs);
    time_t now = time(NULL);
    struct tm tmv;
    int err, in_ram = 0;
    if ((head + tail) * d->bs > (4u << 20))
        return gk3l_err(GK3W_TOUCHED_NO, "gpt-backup", "the partition table area is unusually large; refusing to continue (disk not touched)", 0), -1;
    gb = gk3w_aligned_alloc(len);
    if (!gb)
        return gk3l_err(GK3W_TOUCHED_NO, "nomem", "out of memory", ENOMEM), -1;
    if (gk3l_pread(d, gb, (size_t)(head * d->bs), 0) ||
        gk3l_pread(d, gb + head * d->bs, (size_t)(tail * d->bs), (d->blocks - tail) * d->bs)) {
        err = errno;
        gk3w_aligned_free(gb);
        return gk3l_err(GK3W_TOUCHED_NO, "gpt-backup", "reading the partition table area failed (disk not touched)", err), -1;
    }
    localtime_r(&now, &tmv);
    strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", &tmv);
    snprintf(name, sizeof name, "gpt-before-%s-%s-%s-head%llu-tail%llu.bin", tag, d->name, ts, (unsigned long long)head,
             (unsigned long long)tail);
    if (dir_arg && *dir_arg) {
        /* 调用方指定的地方写不进去就不装 */
        err = backup_to(dir_arg, name, gb, len, path, sizeof path);
    } else {
        char pd[4096];
        err = ENOENT;
        /* 写到安装介质的 gaokun3/（U 盘：/media/gk3 改成可写；读进了内存：临时挂上介质分区本身） */
        if (!gk3l_persist_open(pd, sizeof pd)) {
            err = backup_to(pd, name, gb, len, path, sizeof path);
            gk3l_persist_close();
            /* 持久的那份在临时挂上的分区里：内存副本里也放一份 —— 界面、restore-table、另存日志都按 /media/gk3 的路径找，
             * 重启后 initramfs 读进内存的也正是这个路径 */
            if (!err && strcmp(pd, "/media/gk3/gaokun3") && gk3l_is_dir("/media/gk3/gaokun3")) {
                char p2[4400];
                if (!backup_to("/media/gk3/gaokun3", name, gb, len, p2, sizeof p2))
                    snprintf(path, sizeof path, "%s", p2);
            }
        }
        /* 介质写不进去（写保护、只读介质）就留在内存里，照旧装 */
        if (err) {
            in_ram = 1;
            err = backup_to(run && *run ? run : "/run/gaokun3", name, gb, len, path, sizeof path);
        }
    }
    gk3w_aligned_free(gb);
    if (err)
        return gk3l_err(GK3W_TOUCHED_NO, "gpt-backup", "writing the partition table backup failed (disk not touched)", err), -1;
    gk3l_log("partition table backup: %s (restore: dd the first %llu sectors to the start of %s and the last %llu to its end, bs=%u)",
             path, (unsigned long long)head, d->path, (unsigned long long)tail, d->bs);
    if (in_ram)
        gk3l_log("warning: the installation medium is not writable, so the backup is only in RAM (%s) and is lost on reboot", path);
    {
        /* 界面的失败页据此告诉用户有备份、在哪里（不去日志里找字样） */
        gk3w_line l = {0};
        gk3w_line_begin(&l, "BACKUP");
        gk3w_line_kv(&l, "path", path);
        gk3w_line_kv(&l, "disk", d->path);
        gk3w_line_u64(&l, "sector", d->bs);
        gk3w_line_u64(&l, "head", head);
        gk3w_line_u64(&l, "tail", tail);
        gk3w_line_kv(&l, "ram", in_ram ? "yes" : "no");
        (void)gk3w_line_emit(&l, stdout);
        gk3w_line_free(&l);
    }
    return 0;
}

/* 这个分区（"主:次"）在不在用：挂着、当交换分区、或者上面叠着别的设备（dm / md）。是的话 what 写明是哪种 */
int gk3l_part_in_use(const char *disk, const gk3l_kpart *k, char *what, size_t cap)
{
    char p[512], where[4096];
    DIR *h;
    struct dirent *e;
    if (!gk3l_mount_of(k->devno, where, sizeof where)) {
        snprintf(what, cap, "/dev/%.63s@%.200s", k->name, where);
        return 1;
    }
    {
        FILE *f = fopen("/proc/swaps", "re");
        char line[512], dev[300];
        if (f) {
            while (fgets(line, sizeof line, f))
                if (sscanf(line, "%299s", dev) == 1 && !strncmp(dev, "/dev/", 5) && !strcmp(dev + 5, k->name)) {
                    fclose(f);
                    snprintf(what, cap, "/dev/%.63s@swap", k->name);
                    return 1;
                }
            fclose(f);
        }
    }
    if (!strcmp(disk, k->name))
        snprintf(p, sizeof p, "/sys/block/%s/holders", disk);
    else
        snprintf(p, sizeof p, "/sys/block/%s/%s/holders", disk, k->name);
    h = opendir(p);
    if (h) {
        while ((e = readdir(h)))
            if (e->d_name[0] != '.') {
                closedir(h);
                snprintf(what, cap, "/dev/%.63s@%.63s", k->name, e->d_name);
                return 1;
            }
        closedir(h);
    }
    /* 内核自己的判据：独占打开（O_EXCL）。挂载（FUSE、btrfs 在 mountinfo 里是匿名设备号，上面认不出）、交换分区、
     * dm / md 都独占着它；整块盘上任何一个分区被独占，整块盘的独占打开也会失败 */
    {
        unsigned ma, mi;
        char node[96];
        int fd;
        snprintf(node, sizeof node, "/dev/%.63s", k->name);
        if (sscanf(k->devno, "%u:%u", &ma, &mi) == 2) {
            struct stat st;
            if (!stat(node, &st) && S_ISBLK(st.st_mode) && major(st.st_rdev) == ma && minor(st.st_rdev) == mi) {
                fd = open(node, O_RDONLY | O_EXCL | O_CLOEXEC);
                if (fd < 0 && errno == EBUSY) {
                    snprintf(what, cap, "/dev/%.63s@claimed", k->name);
                    return 1;
                }
                if (fd >= 0)
                    close(fd);
            }
        }
    }
    return 0;
}

int gk3l_kernel_del(const gk3l_disk *d, uint64_t pno)
{
    struct blkpg_partition p;
    struct blkpg_ioctl_arg a;
    memset(&p, 0, sizeof p);
    p.pno = (int)pno;
    memset(&a, 0, sizeof a);
    a.op = BLKPG_DEL_PARTITION;
    a.datalen = sizeof p;
    a.data = &p;
    return ioctl(d->fd, BLKPG, &a) ? errno : 0;
}


int gk3l_gpt_write(const gk3l_disk *d, const uint8_t *hdr, const uint8_t *ent, void *chk)
{
    gk3_gpt g;
    uint8_t *bak;
    uint64_t bel = 0, tb;
    size_t elen;
    int w;
    if (gk3_gpt_parse_header(hdr, d->bs, &g) != GK3_OK)
        return errno = EINVAL, -1;
    tb = ((uint64_t)g.num_entries * g.entry_size + d->bs - 1) / d->bs;
    elen = (size_t)(tb * d->bs);
    bak = gk3w_aligned_alloc(d->bs < 4096 ? 4096 : d->bs);
    if (!bak)
        return errno = ENOMEM, -1;
    if (gk3w_gpt_make_backup(hdr, d->bs, bak, &bel) != GK3W_GPTW_OK) {
        gk3w_aligned_free(bak);
        return errno = EINVAL, -1;
    }
    /* 主表项 → 主表头 → 备份表项 → 备份表头：每一份表头只在它的表项写完之后才写，任何时刻断电，
     * 两份表要么是旧的、要么是新的、要么表头与表项的 CRC 对不上（固件与内核会改用另一份） */
    w = gk3l_write_verify(d, ent, elen, g.entries_lba * d->bs, chk);
    if (!w)
        w = gk3l_write_verify(d, hdr, d->bs, 1 * (uint64_t)d->bs, chk);
    if (!w)
        w = gk3l_write_verify(d, ent, elen, bel * d->bs, chk);
    if (!w)
        w = gk3l_write_verify(d, bak, d->bs, g.alt_lba * d->bs, chk);
    {
        int e = errno;
        gk3w_aligned_free(bak);
        errno = e;
    }
    return w;
}
