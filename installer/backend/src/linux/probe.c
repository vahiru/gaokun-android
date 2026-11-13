/* Linux：列出磁盘、分区、空闲区（只读）。
 *
 * 记录与 Windows 版（win/probe.c）同为 DISK / PART / FREE，字段同名；不同之处：
 *   - path= 是 /dev 节点（/dev/nvme0n1、/dev/nvme0n1p5），PART 的 num= 与 idx= 都是 GPT 表项序号（Linux 的 pN）。
 *   - DISK 没有 num= / system=；多报 medium=（安装器就是从这块盘、这个分区启动的）。
 *   - DISK 的 kernel=：内核眼里的分区（sysfs）与盘上主 GPT 逐项核对的结果（ok / differs），对应 Windows 的 rawgpt=。
 *     differs 说明内核还没重读分区表，写盘入口要拒绝。
 * 分区表以盘上的主 GPT 为准（libgk3core 直接读，只信主表）。table= 的取值：
 *   gpt        主 GPT 有效
 *   damaged    主 GPT 不能用（why=：magic 有保护性 MBR 却没有 GPT 表头，crc / version / range / invalid 表头或表项
 *              校验不过）：不列分区，只能整盘清空。备份 GPT 不读不修（内核与 sgdisk 会退而用备份表，我们不）
 *   mbr        没有 GPT，LBA 0 是 MBR 分区表（含空的 MBR 表）：只能整盘清空
 *   raw        既没有 GPT 也没有 MBR（全新的盘、整盘格式化的 U 盘）：只能整盘清空
 *   unreadable 读盘出错：后面不跟 PART / FREE，另报 ERR disk-unreadable，这块盘不给选
 * 小于 1 GiB 的盘不列；loop 设备只在 GK3_ALLOW_LOOP=1 时列（测试用）。 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "../layout.h"
#include "../proto.h"
#include "gk3core.h"
#include "lx.h"

#define FREE_MIN_BYTES (16ull << 20)    /* 小于 16 MiB 的缝隙不报（GPT 对齐留下的） */
#define MIN_DISK_BYTES (1ull << 30)     /* 小于 1 GiB 的盘不当安装目标 */
#define ENTRIES_MAX (1u << 20)
#define MAX_DISKS 128

/* ── 盘的基本信息 ── */

typedef struct {
    char name[64];              /* nvme0n1 */
    char path[80];              /* /dev/nvme0n1 */
    int fd;
    uint32_t bs, pbs;
    uint64_t bytes;
} disk_t;

/* sysfs 里的一个分区 */
typedef struct {
    char name[64];
    char devno[32];
    uint64_t num, start512, size512;
} kpart_t;

static int pread_all(int fd, void *buf, size_t len, uint64_t off)
{
    uint8_t *p = buf;
    while (len) {
        ssize_t n = pread(fd, p, len, (off_t)off);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        p += n;
        len -= (size_t)n;
        off += (uint64_t)n;
    }
    return 0;
}

static int blk_read(void *ctx, uint64_t lba, uint32_t count, void *buf)
{
    disk_t *d = ctx;
    if (lba > UINT64_MAX / d->bs || (uint64_t)count * d->bs > SIZE_MAX)
        return -1;
    return pread_all(d->fd, buf, (size_t)count * d->bs, lba * d->bs);
}

static void sysfs_path(char *out, size_t cap, const char *disk, const char *child, const char *attr)
{
    if (child)
        snprintf(out, cap, "/sys/block/%s/%s/%s", disk, child, attr);
    else
        snprintf(out, cap, "/sys/block/%s/%s", disk, attr);
}

static void trim(char *s)
{
    size_t n = strlen(s), i = 0;
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = 0;
    while (s[i] == ' ' || s[i] == '\t')
        i++;
    memmove(s, s + i, n - i + 1);
}

/* 型号：sysfs 的 device/vendor + device/model（NVMe 只有 model）。libata 的 vendor 一律是 "ATA"，不要 */
static void disk_model(const char *name, char *out, size_t cap)
{
    char p[256], vendor[128] = "", model[128] = "";
    sysfs_path(p, sizeof p, name, NULL, "device/vendor");
    gk3l_read_line(p, vendor, sizeof vendor);
    sysfs_path(p, sizeof p, name, NULL, "device/model");
    gk3l_read_line(p, model, sizeof model);
    trim(vendor);
    trim(model);
    if (!strcmp(vendor, "ATA"))
        vendor[0] = 0;
    snprintf(out, cap, "%s%s%s", vendor, vendor[0] && model[0] ? " " : "", model);
}

/* 总线：看设备名与 sysfs 里设备的真实路径（与 lsblk 的 TRAN 判据相近） */
static const char *disk_bus(const char *name)
{
    char p[256], real[4096];
    if (!strncmp(name, "nvme", 4))
        return "nvme";
    if (!strncmp(name, "mmcblk", 6))
        return "mmc";
    if (!strncmp(name, "loop", 4))
        return "loop";
    if (!strncmp(name, "vd", 2))
        return "virtio";
    snprintf(p, sizeof p, "/sys/block/%s", name);
    if (!realpath(p, real))
        return "other";
    if (strstr(real, "/usb"))
        return "usb";
    if (strstr(real, "/ata"))
        return "sata";
    return "other";
}

/* 列出 sysfs 里这块盘的分区（子目录里有 partition 文件的） */
static size_t kernel_parts(const char *disk, kpart_t *out, size_t cap)
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
        kpart_t *k = &out[n];
        if (e->d_name[0] == '.' || strlen(e->d_name) >= sizeof k->name)
            continue;
        sysfs_path(p, sizeof p, disk, e->d_name, "partition");
        if (gk3l_read_u64(p, &k->num))
            continue;
        memcpy(k->name, e->d_name, strlen(e->d_name) + 1);   /* 长度上面已查过 */
        sysfs_path(p, sizeof p, disk, e->d_name, "start");
        if (gk3l_read_u64(p, &k->start512))
            continue;
        sysfs_path(p, sizeof p, disk, e->d_name, "size");
        if (gk3l_read_u64(p, &k->size512))
            continue;
        sysfs_path(p, sizeof p, disk, e->d_name, "dev");
        gk3l_read_line(p, k->devno, sizeof k->devno);
        n++;
    }
    closedir(dir);
    return n;
}

static const kpart_t *kernel_find(const kpart_t *k, size_t n, uint64_t num)
{
    for (size_t i = 0; i < n; i++)
        if (k[i].num == num)
            return &k[i];
    return NULL;
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

/* 分区上的文件系统与卷标：blkid -p 直接读分区（不走 blkid 的缓存） */
static void part_fs(const char *dev, char *fs, size_t fscap, char *label, size_t lcap)
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

/* GPT 分区名（UTF-16LE，最多 36 个码元）→ UTF-8。libgk3core 只收 ASCII 名字（按名查找用），
 * 这里给界面显示用，中文名照样报出。坏的代理对换成 U+FFFD */
static void gpt_name_utf8(const gk3_gpt *g, uint32_t idx0, char *out, size_t cap)
{
    const uint8_t *e = g->entries + (size_t)idx0 * g->entry_size + 56;
    size_t n = 0;
    for (int k = 0; k < GK3_GPT_NAME_CHARS; k++) {
        uint32_t c = gk3_le16(e + 2 * k);
        uint8_t u[4];
        size_t m;
        if (!c)
            break;
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

/* 主 GPT 不能用的原因（DISK 的 why=） */
static const char *gpt_why(gk3_err e)
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

/* GUID → 大写文本（与 sgdisk、Windows 版的写法一致） */
static void guid_upper(const uint8_t g[16], char out[37])
{
    gk3_guid_str(g, out);
    for (char *c = out; *c; c++)
        if (*c >= 'a' && *c <= 'f')
            *c = (char)(*c - 32);
}

/* 分区节点：内核列出的就用它的名字；没列出的按内核的命名规则拼（盘名以数字结尾加 p） */
static void part_path(const disk_t *d, const kpart_t *k, uint32_t idx, char *out, size_t cap)
{
    size_t n = strlen(d->name);
    if (k)
        snprintf(out, cap, "/dev/%s", k->name);
    else
        snprintf(out, cap, "/dev/%s%s%u", d->name, n && d->name[n - 1] >= '0' && d->name[n - 1] <= '9' ? "p" : "", idx);
}

/* 没有 GPT 表头时看 LBA 0：保护性 MBR（有 0xEE 分区）⇒ damaged（是 GPT 盘，表头没了）；0x55AA 结尾、
 * 又不是整盘文件系统的引导扇区 ⇒ mbr；否则 raw。读不出返回 NULL */
static const char *classify_no_gpt(disk_t *d)
{
    uint8_t *b = malloc(d->bs);
    const char *r = "raw";
    gk3w_content_kind k;
    int pmbr = 0;
    if (!b)
        return "raw";
    if (blk_read(d, 0, 1, b)) {
        free(b);
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
    free(b);
    return r;
}

typedef struct {
    gk3_gpt_part p;
    const kpart_t *k;
} entry_t;

static int cmp_entry(const void *a, const void *b)
{
    const entry_t *x = a, *y = b;
    return x->p.first_lba < y->p.first_lba ? -1 : x->p.first_lba > y->p.first_lba;
}

/* 内核的分区（sysfs）与盘上的 GPT 是否一致：个数相同，每个都有同号、同起止的表项 */
static int kernel_matches(const disk_t *d, const entry_t *e, size_t ne, const kpart_t *k, size_t nk)
{
    uint32_t f = d->bs / 512;
    if (ne != nk)
        return 0;
    for (size_t i = 0; i < ne; i++)
        if (!e[i].k || e[i].k->start512 != e[i].p.first_lba * f ||
            e[i].k->size512 != (e[i].p.last_lba - e[i].p.first_lba + 1) * f)
            return 0;
    return 1;
}

/* 返回 0 成功；-1 有错（已报 ERR）；-2 权限不够（已报，别的盘也不用试了） */
static int probe_disk(const char *name, const char *medium, gk3w_line *l)
{
    disk_t d = {.fd = -1};
    char model[256], p[256], devno[32] = "";
    uint8_t *hdr = NULL, *ent = NULL;
    entry_t *ents = NULL;
    kpart_t *kp = NULL;
    size_t nk = 0, ne = 0;
    gk3_gpt g;
    gk3_err ge = GK3_OK;
    const char *table = "gpt";
    int rc = 0, is_medium = 0, err;

    snprintf(d.name, sizeof d.name, "%s", name);
    snprintf(d.path, sizeof d.path, "/dev/%s", name);
    d.fd = open(d.path, O_RDONLY | O_CLOEXEC);
    if (d.fd < 0) {
        err = errno;
        if (err == ENOENT || err == ENXIO || err == ENOMEDIUM)
            return 0;   /* 没有节点、没插介质的读卡器：不算错 */
        gk3w_line_begin(l, "ERR");
        gk3w_line_kv(l, "code", err == EACCES || err == EPERM ? "not-root" : "disk-open");
        gk3w_line_kv(l, "disk", d.path);
        gk3w_line_u64(l, "errno", (uint64_t)err);
        gk3w_fail(l, GK3W_TOUCHED_NA, err == EACCES || err == EPERM ? "must run as root" : "cannot open the disk");
        return err == EACCES || err == EPERM ? -2 : -1;
    }
    {
        int ss = 0, ps = 0;
        if (ioctl(d.fd, BLKSSZGET, &ss) == 0)
            d.bs = (uint32_t)ss;
        if (ioctl(d.fd, BLKPBSZGET, &ps) == 0)
            d.pbs = (uint32_t)ps;
    }
    if (ioctl(d.fd, BLKGETSIZE64, &d.bytes)) {
        err = errno;
        close(d.fd);
        gk3w_line_begin(l, "ERR");
        gk3w_line_kv(l, "code", "disk-size");
        gk3w_line_kv(l, "disk", d.path);
        gk3w_line_u64(l, "errno", (uint64_t)err);
        gk3w_fail(l, GK3W_TOUCHED_NA, "cannot read the disk size");
        return -1;
    }
    if (d.bytes < MIN_DISK_BYTES) {
        close(d.fd);
        return 0;
    }
    if (d.bs != 512 && d.bs != 4096) {
        close(d.fd);
        gk3w_line_begin(l, "ERR");
        gk3w_line_kv(l, "code", "disk-sector");
        gk3w_line_kv(l, "disk", d.path);
        gk3w_line_u64(l, "sector", d.bs);
        gk3w_fail(l, GK3W_TOUCHED_NA, "the disk sector size is neither 512 nor 4096 bytes");
        return -1;
    }

    kp = calloc(GK3W_MAX_PARTS, sizeof *kp);
    ents = calloc(GK3W_MAX_PARTS, sizeof *ents);
    hdr = malloc(d.bs);
    ent = malloc(ENTRIES_MAX);
    if (!kp || !ents || !hdr || !ent) {
        gk3l_err(GK3W_TOUCHED_NA, "nomem", "out of memory", ENOMEM);
        rc = -1;
        goto out;
    }
    nk = kernel_parts(name, kp, GK3W_MAX_PARTS);
    sysfs_path(p, sizeof p, name, NULL, "dev");
    gk3l_read_line(p, devno, sizeof devno);
    if (medium[0]) {
        if (!strcmp(devno, medium))
            is_medium = 1;
        for (size_t i = 0; i < nk; i++)
            if (!strcmp(kp[i].devno, medium))
                is_medium = 1;
    }

    {
        gk3_blk dev = {&d, d.bs, d.bytes / d.bs, blk_read, NULL, NULL};
        ge = gk3_gpt_read(&dev, &g, hdr, ent, ENTRIES_MAX);
    }
    if (ge == GK3_EIO) {
        table = "unreadable";
    } else if (ge == GK3_EMAGIC) {
        table = classify_no_gpt(&d);
        if (!table)
            table = "unreadable", ge = GK3_EIO;
    } else if (ge != GK3_OK) {
        table = "damaged";
    }

    if (!strcmp(table, "gpt")) {
        for (uint32_t i = 0; i < g.num_entries && ne < GK3W_MAX_PARTS; i++) {
            if (gk3_gpt_get(&g, i, &ents[ne].p) != GK3_OK)
                continue;
            ents[ne].k = kernel_find(kp, nk, ents[ne].p.index);
            ne++;
        }
        qsort(ents, ne, sizeof *ents, cmp_entry);
    }

    disk_model(name, model, sizeof model);
    gk3w_line_begin(l, "DISK");
    gk3w_line_kv(l, "path", d.path);
    gk3w_line_u64(l, "size_mib", d.bytes >> 20);
    gk3w_line_u64(l, "sector", d.bs);
    gk3w_line_u64(l, "physsector", d.pbs);
    gk3w_line_kv(l, "model", model[0] ? model : "?");
    gk3w_line_kv(l, "bus", disk_bus(name));
    sysfs_path(p, sizeof p, name, NULL, "removable");
    {
        char rem[8] = "0";
        gk3l_read_line(p, rem, sizeof rem);
        gk3w_line_kv(l, "removable", rem[0] == '1' ? "1" : "0");
    }
    gk3w_line_kv(l, "medium", is_medium ? "yes" : "no");
    gk3w_line_kv(l, "table", table);
    if (!strcmp(table, "damaged"))
        gk3w_line_kv(l, "why", gpt_why(ge));
    if (!strcmp(table, "gpt"))
        gk3w_line_kv(l, "kernel", kernel_matches(&d, ents, ne, kp, nk) ? "ok" : "differs");
    if (gk3w_line_emit(l, stdout))
        rc = -1;

    if (!strcmp(table, "unreadable")) {
        gk3w_line_begin(l, "ERR");
        gk3w_line_kv(l, "code", "disk-unreadable");
        gk3w_line_kv(l, "disk", d.path);
        gk3w_fail(l, GK3W_TOUCHED_NA, "cannot read the partition table (I/O error); its partitions and free space are not listed");
        rc = -1;
        goto out;
    }
    if (strcmp(table, "gpt"))
        goto out;

    {
        gk3w_span spans[GK3W_MAX_PARTS], free_spans[GK3W_MAX_PARTS + 1];
        size_t nf = 0;
        gk3w_layout_err le;
        for (size_t i = 0; i < ne; i++) {
            const gk3_gpt_part *q = &ents[i].p;
            char path[128], type[37], uuid[37], fs[64], label[1024], pname[GK3_GPT_NAME_CHARS * 4 + 1];
            int pm = 0;
            spans[i] = (gk3w_span){q->first_lba, q->last_lba};
            part_path(&d, ents[i].k, q->index, path, sizeof path);
            guid_upper(q->type_guid, type);
            guid_upper(q->part_guid, uuid);
            gpt_name_utf8(&g, q->index - 1, pname, sizeof pname);
            fs[0] = label[0] = 0;
            if (ents[i].k)
                part_fs(path, fs, sizeof fs, label, sizeof label);
            if (medium[0] && ents[i].k && !strcmp(ents[i].k->devno, medium))
                pm = 1;
            gk3w_line_begin(l, "PART");
            gk3w_line_kv(l, "path", path);
            gk3w_line_kv(l, "disk", d.path);
            gk3w_line_u64(l, "num", q->index);
            gk3w_line_u64(l, "idx", q->index);
            gk3w_line_u64(l, "start", q->first_lba);
            gk3w_line_u64(l, "end", q->last_lba);
            gk3w_line_u64(l, "size_mib", gk3w_span_bytes(spans[i], d.bs) >> 20);
            gk3w_line_u64(l, "size_kib", gk3w_span_bytes(spans[i], d.bs) >> 10);
            gk3w_line_kv(l, "type", type);
            gk3w_line_kv(l, "uuid", uuid);
            gk3w_line_hex64(l, "attrs", q->attrs);
            gk3w_line_kv(l, "name", pname);
            gk3w_line_kv(l, "fs", fs);
            gk3w_line_kv(l, "fslabel", label);
            gk3w_line_kv(l, "os", gk3w_guess_os(q->name, type, fs));
            gk3w_line_kv(l, "medium", pm ? "yes" : "no");
            if (gk3w_line_emit(l, stdout))
                rc = -1;
        }
        le = gk3w_free_spans(g.first_usable, g.last_usable, spans, ne, d.bs, FREE_MIN_BYTES, free_spans,
                             sizeof free_spans / sizeof free_spans[0], &nf);
        if (le != GK3W_LAYOUT_OK) {
            gk3w_line_begin(l, "ERR");
            gk3w_line_kv(l, "code", "disk-layout");
            gk3w_line_kv(l, "disk", d.path);
            gk3w_line_u64(l, "why", le);
            gk3w_fail(l, GK3W_TOUCHED_NA, "the partition table has out-of-range or overlapping partitions");
            rc = -1;
            goto out;
        }
        for (size_t i = 0; i < nf && i < sizeof free_spans / sizeof free_spans[0]; i++) {
            gk3w_line_begin(l, "FREE");
            gk3w_line_kv(l, "disk", d.path);
            gk3w_line_u64(l, "start", free_spans[i].first);
            gk3w_line_u64(l, "end", free_spans[i].last);
            gk3w_line_u64(l, "size_mib", gk3w_span_bytes(free_spans[i], d.bs) >> 20);
            if (gk3w_line_emit(l, stdout))
                rc = -1;
        }
    }
out:
    free(kp);
    free(ents);
    free(hdr);
    free(ent);
    close(d.fd);
    return rc;
}

static int skip_disk(const char *name)
{
    static const char *const never[] = {"ram", "zram", "dm-", "sr", "md", "nbd", "mtdblock", "fd"};
    const char *loop = getenv("GK3_ALLOW_LOOP");
    for (size_t i = 0; i < sizeof never / sizeof never[0]; i++)
        if (!strncmp(name, never[i], strlen(never[i])))
            return 1;
    /* 没人往 loop 设备上装系统；测试用 GK3_ALLOW_LOOP=1 放开 */
    if (!strncmp(name, "loop", 4))
        return !(loop && !strcmp(loop, "1"));
    return 0;
}

static int cmp_name(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

int gk3l_cmd_probe(int argc, char **argv)
{
    gk3w_line l = {0};
    char medium[32] = "";
    char *names[MAX_DISKS];
    size_t n = 0;
    int rc = 0;
    DIR *dir;
    struct dirent *e;
    (void)argc;
    (void)argv;

    gk3l_medium_devno(medium, sizeof medium);
    dir = opendir("/sys/block");
    if (!dir)
        return gk3l_err(GK3W_TOUCHED_NA, "sysfs", "cannot list /sys/block", errno);
    while ((e = readdir(dir)) && n < MAX_DISKS) {
        if (e->d_name[0] == '.' || skip_disk(e->d_name) || strlen(e->d_name) >= 64)
            continue;
        names[n] = strdup(e->d_name);
        if (names[n])
            n++;
    }
    closedir(dir);
    qsort(names, n, sizeof names[0], cmp_name);
    for (size_t i = 0; i < n; i++) {
        /* 权限不够时每块盘都会失败，报一次就够了 */
        if (rc != -2) {
            int r = probe_disk(names[i], medium, &l);
            if (r == -2 || (r < 0 && !rc))
                rc = r;
        }
        free(names[i]);
    }
    gk3w_line_free(&l);
    return rc ? 1 : 0;
}
