/* Linux：列出磁盘、分区、空闲区（只读）。
 *
 * 记录与 Windows 版（win/probe.c）同为 DISK / PART / FREE，字段同名；不同之处：
 *   - path= 是 /dev 节点（/dev/nvme0n1、/dev/nvme0n1p5），PART 的 num= 与 idx= 都是 GPT 表项序号（Linux 的 pN）。
 *   - DISK 没有 num= / system=；多报 medium=（安装器就是从这块盘、这个分区启动的）。介质分区的 PART 在介质读进了
 *     内存、而且像安装器分区（gk3l_live_like）时多一个 ram=yes（没有挂载，整盘清空时保留，装完可以删掉，见 live.h）。
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../layout.h"
#include "../proto.h"
#include "disk.h"
#include "gk3core.h"
#include "live.h"
#include "lx.h"

#define FREE_MIN_BYTES (16ull << 20)    /* 小于 16 MiB 的缝隙不报（GPT 对齐留下的） */
#define MIN_DISK_BYTES (1ull << 30)     /* 小于 1 GiB 的盘不当安装目标 */
#define MAX_DISKS 128

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
    snprintf(p, sizeof p, "/sys/block/%s/device/vendor", name);
    gk3l_read_line(p, vendor, sizeof vendor);
    snprintf(p, sizeof p, "/sys/block/%s/device/model", name);
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

typedef struct {
    gk3_gpt_part p;
    const gk3l_kpart *k;
} entry_t;

static int cmp_entry(const void *a, const void *b)
{
    const entry_t *x = a, *y = b;
    return x->p.first_lba < y->p.first_lba ? -1 : x->p.first_lba > y->p.first_lba;
}

static void fail_disk(gk3w_line *l, const char *code, const char *disk, const char *k, uint64_t v, const char *human)
{
    gk3w_line_begin(l, "ERR");
    gk3w_line_kv(l, "code", code);
    gk3w_line_kv(l, "disk", disk);
    if (k)
        gk3w_line_u64(l, k, v);
    gk3w_fail(l, GK3W_TOUCHED_NA, human);
}

/* 返回 0 成功；-1 有错（已报 ERR）；-2 权限不够（已报，别的盘也不用试了） */
static int probe_disk(const char *name, const char *medium, gk3w_line *l)
{
    static gk3l_disk d;
    static entry_t ents[GK3W_MAX_PARTS];
    char model[256], path[96];
    size_t ne = 0;
    int rc = 0, is_medium = 0, err;

    err = gk3l_disk_open(&d, name, 0);
    if (err) {
        snprintf(path, sizeof path, "/dev/%s", name);
        if (err == ENOENT || err == ENXIO || err == ENOMEDIUM)
            return 0;   /* 没有节点、没插介质的读卡器：不算错 */
        if (err == EACCES || err == EPERM) {
            fail_disk(l, "not-root", path, "errno", (uint64_t)err, "must run as root");
            return -2;
        }
        fail_disk(l, "disk-open", path, "errno", (uint64_t)err, "cannot open the disk");
        return -1;
    }
    if (d.bytes < MIN_DISK_BYTES) {
        gk3l_disk_close(&d);
        return 0;
    }
    if (d.bs != 512 && d.bs != 4096) {
        fail_disk(l, "disk-sector", d.path, "sector", d.bs, "the disk sector size is neither 512 nor 4096 bytes");
        gk3l_disk_close(&d);
        return -1;
    }
    if (gk3l_disk_read_table(&d)) {
        gk3l_err(GK3W_TOUCHED_NA, "nomem", "out of memory", ENOMEM);
        gk3l_disk_close(&d);
        return -1;
    }
    if (medium[0]) {
        if (!strcmp(d.devno, medium))
            is_medium = 1;
        for (size_t i = 0; i < d.nk; i++)
            if (!strcmp(d.kp[i].devno, medium))
                is_medium = 1;
    }
    if (!strcmp(d.table, "gpt")) {
        for (uint32_t i = 0; i < d.g.num_entries && ne < GK3W_MAX_PARTS; i++) {
            if (gk3_gpt_get(&d.g, i, &ents[ne].p) != GK3_OK)
                continue;
            ents[ne].k = gk3l_kernel_find(d.kp, d.nk, ents[ne].p.index);
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
    {
        char p[256], rem[8] = "0";
        snprintf(p, sizeof p, "/sys/block/%s/removable", name);
        gk3l_read_line(p, rem, sizeof rem);
        gk3w_line_kv(l, "removable", rem[0] == '1' ? "1" : "0");
    }
    gk3w_line_kv(l, "medium", is_medium ? "yes" : "no");
    gk3w_line_kv(l, "table", d.table);
    if (!strcmp(d.table, "damaged"))
        gk3w_line_kv(l, "why", gk3l_gpt_why(d.gerr));
    if (!strcmp(d.table, "gpt"))
        gk3w_line_kv(l, "kernel", gk3l_disk_kernel_ok(&d) ? "ok" : "differs");
    if (gk3w_line_emit(l, stdout))
        rc = -1;

    if (!strcmp(d.table, "unreadable")) {
        fail_disk(l, "disk-unreadable", d.path, NULL, 0, "cannot read the partition table (I/O error); its partitions and free space are not listed");
        rc = -1;
        goto out;
    }
    if (strcmp(d.table, "gpt"))
        goto out;

    {
        gk3w_span spans[GK3W_MAX_PARTS], free_spans[GK3W_MAX_PARTS + 1];
        size_t nf = 0;
        gk3w_layout_err le;
        for (size_t i = 0; i < ne; i++) {
            const gk3_gpt_part *q = &ents[i].p;
            char dn[64], type[37], uuid[37], fs[64], label[1024], pname[GK3_GPT_NAME_CHARS * 4 + 1];
            int pm = 0;
            spans[i] = (gk3w_span){q->first_lba, q->last_lba};
            /* 分区节点：内核列出的就用它的名字；没列出的按内核的命名规则拼 */
            if (ents[i].k)
                snprintf(dn, sizeof dn, "%s", ents[i].k->name);
            else
                gk3l_part_devname(name, q->index, dn, sizeof dn);
            snprintf(path, sizeof path, "/dev/%s", dn);
            gk3l_guid_upper(q->type_guid, type);
            gk3l_guid_upper(q->part_guid, uuid);
            gk3l_gpt_name_utf8(&d.g, q->index - 1, pname, sizeof pname);
            fs[0] = label[0] = 0;
            if (ents[i].k)
                gk3l_blkid(path, fs, sizeof fs, label, sizeof label);
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
            gk3w_line_kv(l, "os", gk3w_guess_os(pname, type, fs));
            gk3w_line_kv(l, "medium", pm ? "yes" : "no");
            if (pm && gk3l_medium_ram() && gk3l_live_like(type, fs, label, pname))
                gk3w_line_kv(l, "ram", "yes");
            if (gk3w_line_emit(l, stdout))
                rc = -1;
        }
        le = gk3w_free_spans(d.g.first_usable, d.g.last_usable, spans, ne, d.bs, FREE_MIN_BYTES, free_spans,
                             sizeof free_spans / sizeof free_spans[0], &nf);
        if (le != GK3W_LAYOUT_OK) {
            fail_disk(l, "disk-layout", d.path, "why", le, "the partition table has out-of-range or overlapping partitions");
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
    gk3l_disk_close(&d);
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
