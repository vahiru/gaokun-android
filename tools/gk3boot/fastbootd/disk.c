/* gk3-fastbootd：目标盘与分区白名单（fastboot-design §4.4、§4.10；boot-entry-design §4.4.1）。
 *
 * 规则：
 *   1. 目标盘：cmdline 有 gk3.disk=<misc 的 PARTUUID>（gk3boot 传）就只认 misc 是这个 PARTUUID 的那块盘；
 *      没有就扫全部盘，要求恰好一块满足第 2 条。0 块或多块（含两块盘 misc PARTUUID 相同的克隆盘）⇒ 拒绝一切写入。
 *   2. 这块盘的主 GPT 里 misc / boot_a / boot_b / super / userdata / metadata 各恰好出现一次
 *      （PARTLABEL 精确匹配、大小写敏感；与安装器认分区的规则相同，
 *       libgk3core 的 gk3_gpt_require_unique 实现）。
 *   3. 对外只暴露前 5 个名字；misc 只能按偏移写 BCB / BCAB / VAB 三段；esp、Windows 分区、救援分区、整盘
 *      在协议里根本不存在 —— 所有写都经 fb_part_write / fb_misc_write，它们只接受表里的下标并查越界。
 *   4. 所有 I/O 走整盘节点 + 绝对偏移（不依赖 /dev/block/by-name，那是 Android ueventd 的产物；也不按分区号）。
 *   5. 每次写之前重读主 GPT，表头 / 表 CRC 必须与打开时一致（fb_disk_recheck）。
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/sysmacros.h>
#endif

#include "fbd.h"

const char *const fb_part_names[FB_P_N] = {"boot_a", "boot_b", "super", "userdata", "metadata", "misc"};

/* ---------------------------------------------------------------- gk3_blk 包装 */

typedef struct { int fd; uint32_t bs; } blkctx;

static int blk_read(void *ctx, uint64_t lba, uint32_t count, void *buf)
{
    blkctx *c = ctx;
    size_t len = (size_t)count * c->bs;
    ssize_t r = pread(c->fd, buf, len, (off_t)(lba * c->bs));
    return r == (ssize_t)len ? 0 : -1;
}

static void guid_str(const uint8_t g[16], char out[37]) { gk3_guid_str(g, out); }

/* 读主 GPT 并填 d->p / d->all。返回 0 成功；失败把原因写进 err。 */
static int read_gpt(int fd, uint32_t bs, uint64_t nblocks, fb_disk *d, char *err, size_t err_len,
                    uint32_t *hdr_crc, uint32_t *tab_crc)
{
    blkctx c = {fd, bs};
    gk3_blk dev = {&c, bs, nblocks, blk_read, NULL, NULL};
    uint8_t *hdr = malloc(bs), *ent = NULL;
    gk3_gpt g;
    gk3_err e;
    const char *bad = NULL;
    size_t elen;
    int rc = -1;

    if (!hdr) {
        snprintf(err, err_len, "out of memory");
        return -1;
    }
    if (blk_read(&c, 1, 1, hdr)) {
        snprintf(err, err_len, "cannot read LBA 1");
        goto out;
    }
    e = gk3_gpt_parse_header(hdr, bs, &g);
    if (e) {
        snprintf(err, err_len, "no valid primary GPT (%s)", gk3_strerror(e));
        goto out;
    }
    elen = ((size_t)g.num_entries * g.entry_size + bs - 1) / bs * bs;
    ent = malloc(elen);
    if (!ent) {
        snprintf(err, err_len, "out of memory");
        goto out;
    }
    e = gk3_gpt_read(&dev, &g, hdr, ent, elen);
    if (e) {
        snprintf(err, err_len, "primary GPT rejected (%s)", gk3_strerror(e));
        goto out;
    }
    *hdr_crc = gk3_le32(hdr + 16);
    *tab_crc = gk3_le32(hdr + 88);
    if (d) {
        guid_str(g.disk_guid, d->disk_guid);
        d->n_all = 0;
        for (uint32_t i = 0; i < g.num_entries && d->n_all < 128; i++) {
            gk3_gpt_part p;
            if (gk3_gpt_get(&g, i, &p))
                continue;
            d->all[d->n_all].index = p.index;
            d->all[d->n_all].first_lba = p.first_lba;
            d->all[d->n_all].last_lba = p.last_lba;
            memcpy(d->all[d->n_all].name, p.name, sizeof(d->all[0].name));
            memcpy(d->all[d->n_all].type, p.type_guid, 16);
            guid_str(p.part_guid, d->all[d->n_all].partuuid);
            d->n_all++;
        }
    }
    e = gk3_gpt_require_unique(&g, (const char *const *)fb_part_names, FB_P_N, &bad);
    if (e) {
        snprintf(err, err_len, "partition '%s' is %s on this disk (each of misc/boot_a/boot_b/super/userdata/metadata must appear exactly once)",
                 bad ? bad : "?", e == GK3_EDUP ? "duplicated" : e == GK3_ENOENT ? "missing" : gk3_strerror(e));
        goto out;
    }
    if (d) {
        for (int i = 0; i < FB_P_N; i++) {
            gk3_gpt_part p;
            gk3_gpt_find(&g, fb_part_names[i], &p);
            d->p[i].name = fb_part_names[i];
            d->p[i].index = p.index;
            d->p[i].first_lba = p.first_lba;
            d->p[i].last_lba = p.last_lba;
            d->p[i].off = p.first_lba * bs;
            d->p[i].size = (p.last_lba - p.first_lba + 1) * bs;
            guid_str(p.part_guid, d->p[i].partuuid);
        }
    }
    rc = 0;
out:
    free(hdr);
    free(ent);
    return rc;
}

static int open_dev(const char *path, int *fd, bool *is_blk, uint32_t *bs, uint64_t *nblocks, bool rw)
{
    struct stat st;
    int f = open(path, (rw ? O_RDWR : O_RDONLY) | O_CLOEXEC);
    if (f < 0)
        return -1;
    if (fstat(f, &st)) {
        close(f);
        return -1;
    }
    *is_blk = S_ISBLK(st.st_mode);
    *bs = 512;
    if (*is_blk) {
#ifdef __linux__
        int ss = 0;
        uint64_t sz = 0;
        if (ioctl(f, BLKSSZGET, &ss) == 0 && ss >= 512)
            *bs = (uint32_t)ss;
        if (ioctl(f, BLKGETSIZE64, &sz)) {
            close(f);
            return -1;
        }
        *nblocks = sz / *bs;
#else
        close(f);
        errno = ENOTSUP;
        return -1;
#endif
    } else if (S_ISREG(st.st_mode)) {
        *nblocks = (uint64_t)st.st_size / *bs;
    } else {
        close(f);
        errno = EINVAL;
        return -1;
    }
    *fd = f;
    return 0;
}

static void read_sysfs(const char *path, char *out, size_t n)
{
    FILE *f = fopen(path, "r");
    out[0] = 0;
    if (!f)
        return;
    if (!fgets(out, (int)n, f))
        out[0] = 0;
    fclose(f);
    out[strcspn(out, "\n")] = 0;
    for (size_t l = strlen(out); l && out[l - 1] == ' '; l--)
        out[l - 1] = 0;
}

/* /sys/block/<name> 的整盘节点：/dev/<name> 存在且 rdev 对得上就用它，否则在 rundir 里 mknod 一个。 */
__attribute__((unused)) static int disk_node(const char *name, const char *rundir, char *out, size_t n)
{
#ifdef __linux__
    char p[300], mm[32];
    unsigned ma, mi;
    struct stat st;
    snprintf(p, sizeof(p), "/sys/block/%s/dev", name);
    read_sysfs(p, mm, sizeof(mm));
    if (sscanf(mm, "%u:%u", &ma, &mi) != 2)
        return -1;
    snprintf(out, n, "/dev/%s", name);
    if (stat(out, &st) == 0 && S_ISBLK(st.st_mode) && st.st_rdev == makedev(ma, mi))
        return 0;
    snprintf(p, sizeof(p), "%s/dev", rundir);
    mkdir(rundir, 0700);
    mkdir(p, 0700);
    snprintf(out, n, "%s/dev/%s", rundir, name);
    unlink(out);
    if (mknod(out, S_IFBLK | 0600, makedev(ma, mi)))
        return -1;
    return 0;
#else
    (void)name; (void)rundir; (void)out; (void)n;
    return -1;
#endif
}

const char *fb_disk_part_node(fb_disk *d, uint32_t index, const char *rundir, char *out, size_t out_len)
{
#ifdef __linux__
    char base[300], p[600], v[32];
    DIR *dir;
    struct dirent *e;
    unsigned ma = 0, mi = 0;
    bool found = false;
    struct stat st;
    if (!d->sysname[0])
        return NULL;
    snprintf(base, sizeof(base), "/sys/block/%s", d->sysname);
    dir = opendir(base);
    if (!dir)
        return NULL;
    while ((e = readdir(dir))) {
        if (strncmp(e->d_name, d->sysname, strlen(d->sysname)))
            continue;
        snprintf(p, sizeof(p), "%s/%s/partition", base, e->d_name);
        read_sysfs(p, v, sizeof(v));
        if (!v[0] || (uint32_t)strtoul(v, NULL, 10) != index)
            continue;
        snprintf(p, sizeof(p), "%s/%s/dev", base, e->d_name);
        read_sysfs(p, v, sizeof(v));
        if (sscanf(v, "%u:%u", &ma, &mi) == 2)
            found = true;
        break;
    }
    closedir(dir);
    if (!found)
        return NULL;
    mkdir(rundir, 0700);
    snprintf(p, sizeof(p), "%s/dev", rundir);
    mkdir(p, 0700);
    snprintf(out, out_len, "%s/dev/%sp%u", rundir, d->sysname, index);
    if (stat(out, &st) == 0 && S_ISBLK(st.st_mode) && st.st_rdev == makedev(ma, mi))
        return out;
    unlink(out);
    if (mknod(out, S_IFBLK | 0600, makedev(ma, mi)))
        return NULL;
    return out;
#else
    (void)d; (void)index; (void)rundir; (void)out; (void)out_len;
    return NULL;
#endif
}

__attribute__((unused)) static bool in_filter(const char *filter, const char *path, const char *sysname)
{
    char buf[1024], *tok, *save = NULL;
    if (!filter)
        return true;
    snprintf(buf, sizeof(buf), "%s", filter);
    for (tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        const char *b = strrchr(tok, '/');
        b = b ? b + 1 : tok;
        if (!strcmp(tok, path) || !strcmp(b, sysname))
            return true;
    }
    return false;
}

/* 把一块候选盘（已打开）试一遍；符合就把它装进 d（只在 d->fd < 0 时装）。返回 1 = 符合。 */
/* 拒绝原因：有合法 GPT 的盘的原因比"根本没有 GPT"的盘更有用（扫描时会看到虚拟机 / U 盘之类的无关盘），
 * 所以前者一旦出现就不再被后者覆盖。 */
static bool why_has_gpt;

static int try_disk(fb_disk *d, const char *path, const char *sysname, const fb_disk_opts *o, char *why, size_t why_len,
                    int *n_match)
{
    fb_disk tmp;
    int fd;
    bool is_blk;
    uint32_t bs, hc, tc;
    uint64_t nb;
    char err[200];

    memset(&tmp, 0, sizeof(tmp));
    if (open_dev(path, &fd, &is_blk, &bs, &nb, false)) {
        if (!why_has_gpt)
            snprintf(why, why_len, "%s: cannot open (%s)", path, strerror(errno));
        return 0;
    }
    if (read_gpt(fd, bs, nb, &tmp, err, sizeof(err), &hc, &tc)) {
        bool has_gpt = strncmp(err, "no valid primary GPT", 20) != 0 && strncmp(err, "cannot read", 11) != 0;
        if (has_gpt || !why_has_gpt)
            snprintf(why, why_len, "%s: %s", path, err);
        why_has_gpt |= has_gpt;
        close(fd);
        return 0;
    }
    close(fd);
    if (o->want_misc_uuid && o->want_misc_uuid[0] && strcasecmp(tmp.p[FB_P_MISC].partuuid, o->want_misc_uuid)) {
        snprintf(why, why_len, "%s: misc PARTUUID %s != gk3.disk", path, tmp.p[FB_P_MISC].partuuid);
        why_has_gpt = true;
        return 0;
    }
    (*n_match)++;
    if (*n_match == 1) {
        memcpy(d->p, tmp.p, sizeof(d->p));
        memcpy(d->all, tmp.all, sizeof(d->all));
        d->n_all = tmp.n_all;
        memcpy(d->disk_guid, tmp.disk_guid, sizeof(d->disk_guid));
        snprintf(d->path, sizeof(d->path), "%s", path);
        snprintf(d->sysname, sizeof(d->sysname), "%s", sysname);
        d->bs = bs;
        d->nblocks = nb;
        d->is_blk = is_blk;
        d->gpt_hdr_crc = hc;
        d->gpt_tab_crc = tc;
    }
    return 1;
}

void fb_disk_open(fb_disk *d, const fb_disk_opts *o)
{
    char why[300] = "";
    int n_match = 0;

    why_has_gpt = false;
    memset(d, 0, sizeof(*d));
    d->fd = -1;
    if (o->disk_override) {
        const char *b = strrchr(o->disk_override, '/');
        char sys[64] = "";
        struct stat st;
        /* 整盘节点就从 /sys/dev/block/<maj:min> 反查 sysname（找 ESP 分区节点要用）；镜像文件没有 sysname */
#ifdef __linux__
        if (stat(o->disk_override, &st) == 0 && S_ISBLK(st.st_mode)) {
            char lp[128], tgt[512];
            ssize_t l;
            snprintf(lp, sizeof(lp), "/sys/dev/block/%u:%u", major(st.st_rdev), minor(st.st_rdev));
            l = readlink(lp, tgt, sizeof(tgt) - 1);
            if (l > 0) {
                tgt[l] = 0;
                b = strrchr(tgt, '/');
                snprintf(sys, sizeof(sys), "%s", b ? b + 1 : tgt);
            }
        }
#else
        (void)st;
#endif
        (void)b;
        try_disk(d, o->disk_override, sys, o, why, sizeof(why), &n_match);
        if (!n_match) {
            snprintf(d->err, sizeof(d->err), "target disk rejected: %s", why);
            return;
        }
    } else {
#ifdef __linux__
        DIR *dir = opendir("/sys/block");
        struct dirent *e;
        char path[300];
        if (!dir) {
            snprintf(d->err, sizeof(d->err), "cannot list /sys/block");
            return;
        }
        while ((e = readdir(dir))) {
            if (e->d_name[0] == '.' || !strncmp(e->d_name, "ram", 3) || !strncmp(e->d_name, "zram", 4) ||
                !strncmp(e->d_name, "dm-", 3))
                continue;
            if (disk_node(e->d_name, o->rundir, path, sizeof(path)))
                continue;
            if (!in_filter(o->disks_filter, path, e->d_name))
                continue;
            try_disk(d, path, e->d_name, o, why, sizeof(why), &n_match);
        }
        closedir(dir);
#endif
        if (n_match == 0) {
            snprintf(d->err, sizeof(d->err), "no target disk%s%s: %s",
                     o->want_misc_uuid ? " with misc PARTUUID " : "", o->want_misc_uuid ? o->want_misc_uuid : "",
                     why[0] ? why : "no candidate disks");
            return;
        }
        if (n_match > 1) {
            snprintf(d->err, sizeof(d->err), "%d disks qualify as the target%s — refusing all writes (pass gk3.disk=<misc PARTUUID>)",
                     n_match, o->want_misc_uuid ? " with the same misc PARTUUID (cloned disk?)" : "");
            return;
        }
    }
    {
        bool is_blk;
        uint32_t bs;
        uint64_t nb;
        if (open_dev(d->path, &d->fd, &is_blk, &bs, &nb, true)) {
            snprintf(d->err, sizeof(d->err), "%s: cannot open read-write (%s)", d->path, strerror(errno));
            d->fd = -1;
            return;
        }
        if (bs != d->bs || nb != d->nblocks) {
            snprintf(d->err, sizeof(d->err), "%s: geometry changed while opening", d->path);
            close(d->fd);
            d->fd = -1;
            return;
        }
    }
    if (d->sysname[0]) {
        char p[300];
        snprintf(p, sizeof(p), "/sys/block/%s/device/model", d->sysname);
        read_sysfs(p, d->model, sizeof(d->model));
    }
    if (!d->model[0])
        snprintf(d->model, sizeof(d->model), "%s", d->is_blk ? "unknown" : "image file");
    d->ok = true;
    fb_log("target disk %s (%s, %s, %u-byte blocks, %llu blocks, disk GUID %s)", d->path, d->sysname[0] ? d->sysname : "-",
           d->model, d->bs, (unsigned long long)d->nblocks, d->disk_guid);
    for (int i = 0; i < FB_P_N; i++)
        fb_log("  %-8s p%-2u LBA %llu-%llu  %llu bytes  PARTUUID %s", d->p[i].name, d->p[i].index,
               (unsigned long long)d->p[i].first_lba, (unsigned long long)d->p[i].last_lba,
               (unsigned long long)d->p[i].size, d->p[i].partuuid);
}

int fb_disk_recheck(fb_disk *d, char *why, size_t why_len)
{
    uint32_t hc, tc;
    char err[200];
    if (!d->ok) {
        snprintf(why, why_len, "%s", d->err);
        return -1;
    }
    /* 先丢掉缓存：要比对的是介质上此刻的表，不是我们启动时读进页缓存的那份 */
    fb_disk_sync_drop(d, -1, 0, 0);
    if (read_gpt(d->fd, d->bs, d->nblocks, NULL, err, sizeof(err), &hc, &tc)) {
        snprintf(why, why_len, "GPT re-check failed: %s", err);
        return -1;
    }
    if (hc != d->gpt_hdr_crc || tc != d->gpt_tab_crc) {
        snprintf(why, why_len, "GPT changed since start-up — refusing to write (restart fastboot)");
        return -1;
    }
    return 0;
}

static int span_ok(fb_disk *d, int pi, uint64_t off, uint64_t len)
{
    if (!d->ok || d->fd < 0 || pi < 0 || pi >= FB_P_N) {
        errno = EPERM;
        return 0;
    }
    if (off > d->p[pi].size || len > d->p[pi].size - off) {
        errno = ERANGE;
        return 0;
    }
    return 1;
}

int fb_part_read(fb_disk *d, int pi, uint64_t off, void *buf, size_t len)
{
    uint8_t *b = buf;
    if (!span_ok(d, pi, off, len))
        return -1;
    while (len) {
        ssize_t r = pread(d->fd, b, len, (off_t)(d->p[pi].off + off));
        if (r <= 0) {
            if (r < 0 && errno == EINTR)
                continue;
            if (r == 0)
                errno = EIO;
            return -1;
        }
        b += r;
        off += (uint64_t)r;
        len -= (size_t)r;
    }
    return 0;
}

int fb_part_write(fb_disk *d, int pi, uint64_t off, const void *buf, size_t len)
{
    const uint8_t *b = buf;
    /* misc 不走这里：它只能经 fb_misc_write 写三段 */
    if (pi == FB_P_MISC) {
        errno = EPERM;
        return -1;
    }
    if (!span_ok(d, pi, off, len))
        return -1;
    while (len) {
        ssize_t r = pwrite(d->fd, b, len, (off_t)(d->p[pi].off + off));
        if (r <= 0) {
            if (r < 0 && errno == EINTR)
                continue;
            if (r == 0)
                errno = EIO;
            return -1;
        }
        b += r;
        off += (uint64_t)r;
        len -= (size_t)r;
    }
    return 0;
}

int fb_disk_sync_drop(fb_disk *d, int pi, uint64_t off, uint64_t len)
{
    if (fsync(d->fd))
        return -1;
#ifdef __linux__
    if (d->is_blk) {
        /* BLKFLSBUF：写回并作废这块盘的缓冲，之后的读从介质来 */
        if (ioctl(d->fd, BLKFLSBUF, 0))
            return -1;
    } else {
        /* pi < 0：整个文件 */
        posix_fadvise(d->fd, pi < 0 ? 0 : (off_t)(d->p[pi].off + off), pi < 0 ? 0 : (off_t)len, POSIX_FADV_DONTNEED);
    }
#else
    (void)pi; (void)off; (void)len;
#endif
    return 0;
}

int fb_part_discard(fb_disk *d, int pi, uint64_t off, uint64_t len)
{
    if (!span_ok(d, pi, off, len) || pi == FB_P_MISC)
        return -1;
#ifdef __linux__
    if (d->is_blk) {
        uint64_t r[2] = {d->p[pi].off + off, len};
        if (ioctl(d->fd, BLKDISCARD, r))
            return errno == EOPNOTSUPP || errno == ENOTTY ? 1 : -1;
        return 0;
    }
    if (fallocate(d->fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, (off_t)(d->p[pi].off + off), (off_t)len))
        return errno == EOPNOTSUPP ? 1 : -1;
    return 0;
#else
    return 1;
#endif
}

#define ZCHUNK (4u << 20)

int fb_part_zero_verify(fb_disk *d, int pi, uint64_t off, uint64_t len)
{
    static uint8_t *z, *rb;
    uint64_t done;
    if (!z) {
        z = calloc(1, ZCHUNK);
        rb = malloc(ZCHUNK);
        if (!z || !rb)
            return -1;
    }
    for (done = 0; done < len;) {
        size_t n = len - done > ZCHUNK ? ZCHUNK : (size_t)(len - done);
        if (fb_part_write(d, pi, off + done, z, n))
            return -1;
        done += n;
    }
    if (fb_disk_sync_drop(d, pi, off, len))
        return -1;
    for (done = 0; done < len;) {
        size_t n = len - done > ZCHUNK ? ZCHUNK : (size_t)(len - done);
        if (fb_part_read(d, pi, off + done, rb, n))
            return -1;
        if (!gk3_is_zero(rb, n) && memcmp(rb, z, n)) {
            errno = EIO;
            return -2;
        }
        done += n;
    }
    return 0;
}

int fb_part_lookup(const char *name, int cur_slot)
{
    for (int i = 0; i < FB_P_EXPOSED; i++)
        if (!strcmp(name, fb_part_names[i]))
            return i;
    if (!strcmp(name, "boot") && (cur_slot == 0 || cur_slot == 1))
        return cur_slot == 0 ? FB_P_BOOT_A : FB_P_BOOT_B;
    return -1;
}

/* ---------------------------------------------------------------- misc */

int fb_misc_read(fb_disk *d, void *buf64k)
{
    if (!d->ok || d->p[FB_P_MISC].size < GK3_MISC_READ_SIZE) {
        errno = EPERM;
        return -1;
    }
    if (fb_disk_sync_drop(d, FB_P_MISC, 0, GK3_MISC_READ_SIZE))
        return -1;
    return fb_part_read(d, FB_P_MISC, 0, buf64k, GK3_MISC_READ_SIZE);
}

int fb_misc_write(fb_disk *d, uint32_t off, const void *data, size_t len)
{
    uint8_t *rb;
    ssize_t r;
    bool ok = (off == GK3_MISC_BCB_OFF && len <= GK3_MISC_BCB_SIZE) ||
              (off == GK3_MISC_BCAB_OFF && len == GK3_MISC_BCAB_SIZE) ||
              (off == GK3_MISC_GK3_OFF && len == GK3_MISC_GK3_SIZE) ||
              (off == GK3_MISC_SYSTEM_OFF && len <= 64);
    if (!ok || !d->ok) {
        fb_log("BUG: refused misc write off=%u len=%zu", off, len);
        errno = EPERM;
        return -1;
    }
    r = pwrite(d->fd, data, len, (off_t)(d->p[FB_P_MISC].off + off));
    if (r != (ssize_t)len)
        return -1;
    if (fb_disk_sync_drop(d, FB_P_MISC, off, len))
        return -1;
    rb = malloc(len);
    if (!rb)
        return -1;
    r = pread(d->fd, rb, len, (off_t)(d->p[FB_P_MISC].off + off));
    if (r != (ssize_t)len || memcmp(rb, data, len)) {
        free(rb);
        errno = EIO;
        return -1;
    }
    free(rb);
    return 0;
}
