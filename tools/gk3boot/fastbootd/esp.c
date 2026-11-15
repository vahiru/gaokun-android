/* gk3-fastbootd：ESP（刷 boot_x 之后的同步、set_active 的 default 镜像）。
 *
 * 规则逐条照现有实现，改一边要改另一边：
 *   · 找 ESP：只在目标盘上找（不碰别的盘）。cmdline 有 gk3.esp=<PARTUUID>（gk3boot 自己是从哪个 ESP 起的）
 *     就只认它；否则候选 = 目标盘上类型为 EFI System 或开头是 FAT 引导扇区的分区，只读挂上看有没有
 *     loader/entries/ 下的 *-android-*.conf（下同）—— 与 boot_control/EspSlot.cpp:LooksLikeOurEsp、
 *     gaokun3-ota-postinstall.sh:find_esp 的"按内容认 ESP"同一判据。恰好一个才用，多个就拒绝（有歧义不猜）。
 *   · 挂载点私有（rundir/esp），不叫 /mnt/esp。
 *   · 选目录：loader/entries 里【直连条目】<32 位小写十六进制>-android-<x>.conf 必须恰好一个，它的 linux 行必须是
 *     /<mid>/android/slot_<x>/Image，内核就写进 /<mid>/android/slot_<x>/ —— 与 gaokun3-ota-postinstall.sh 的
 *     is_direct_entry / KPATH 两段、安装器选 machine-id 目录的规则（installer/backend/src/linux/apply.c 的 esp_pick_mid）、gk3boot.c 的 direct_cb 同一规则。
 *   · 写文件：.<名字>.new → fsync → 丢缓存读回逐字节比对 → rename → fsync 目录；写前按"新文件合计 − 将被覆盖的旧文件"
 *     核对空闲空间。
 *   · 条目 options = boot.img 的 cmdline（头 cmdline + extra 直接相接）+ " androidboot.slot_suffix=_<x>"（已带则不加），
 *     与 postinstall 的 cmdline 同步一节逐字节相同；没有 options 行就不改（同 postinstall 的 grep 守卫）。
 *   · loader.conf 的 default：第一行 default 换成 "default *-android-<x>.conf"，其余 default 行删掉，没有就追加
 *     —— EspSlot.cpp:SetEspDefaultSlot。
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/mount.h>
#endif

#include "fbd.h"

/* EFI System Partition 的类型 GUID C12A7328-F81F-11D2-BA4B-00A0C93EC93B，盘上（mixed-endian）字节序 */
__attribute__((unused)) static const uint8_t ESP_TYPE[16] = {0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
                                     0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b};

static void seterr(char *err, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void seterr(char *err, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

static bool has_android_entry(const char *root)
{
    char p[512];
    DIR *d;
    struct dirent *e;
    bool found = false;
    snprintf(p, sizeof(p), "%s/loader/entries", root);
    d = opendir(p);
    if (!d)
        return false;
    while ((e = readdir(d)))
        if (!fnmatch("*-android-*.conf", e->d_name, 0)) {
            found = true;
            break;
        }
    closedir(d);
    return found;
}

__attribute__((unused)) static bool looks_fat(fb_disk *d, uint32_t idx)
{
    uint8_t s[512];
    for (uint32_t i = 0; i < d->n_all; i++) {
        if (d->all[i].index != idx)
            continue;
        if (pread(d->fd, s, sizeof(s), (off_t)(d->all[i].first_lba * d->bs)) != (ssize_t)sizeof(s))
            return false;
        return s[510] == 0x55 && s[511] == 0xaa && (!memcmp(s + 0x36, "FAT", 3) || !memcmp(s + 0x52, "FAT", 3));
    }
    return false;
}

#ifdef __linux__
static int try_mount(const char *dev, const char *mnt, bool ro)
{
    mkdir(mnt, 0700);
    return mount(dev, mnt, "vfat", MS_NOATIME | MS_NOSUID | MS_NODEV | (ro ? MS_RDONLY : 0), "shortname=mixed");
}
#endif

int fb_esp_open(fb_esp *e, fb_disk *d, const fb_esp_opts *o, char *err, size_t err_len)
{
    memset(e, 0, sizeof(*e));
    if (o->esp_dir_override) {
        snprintf(e->root, sizeof(e->root), "%s", o->esp_dir_override);
        if (!has_android_entry(e->root)) {
            seterr(err, err_len, "--esp-dir %s has no loader/entries/*-android-*.conf", e->root);
            return -1;
        }
        return 0;
    }
#ifdef __linux__
    {
        char mnt[300], node[300], cand_dev[300] = "", cand_uuid[37] = "";
        int ncand = 0;
        if (!d->ok) {
            seterr(err, err_len, "%s", d->err);
            return -1;
        }
        snprintf(mnt, sizeof(mnt), "%s/esp", o->rundir);
        mkdir(o->rundir, 0700);
        for (uint32_t i = 0; i < d->n_all; i++) {
            bool want;
            if (o->want_esp_uuid && o->want_esp_uuid[0])
                want = !strcasecmp(d->all[i].partuuid, o->want_esp_uuid);
            else
                want = !memcmp(d->all[i].type, ESP_TYPE, 16) || looks_fat(d, d->all[i].index);
            if (!want)
                continue;
            /* 我们自己的六个分区绝不当 ESP 挂 */
            bool ours = false;
            for (int k = 0; k < FB_P_N; k++)
                if (d->p[k].index == d->all[i].index)
                    ours = true;
            if (ours)
                continue;
            if (!fb_disk_part_node(d, d->all[i].index, o->rundir, node, sizeof(node))) {
                fb_log("esp: no device node for partition %u (%s)", d->all[i].index, d->all[i].name);
                continue;
            }
            if (try_mount(node, mnt, true)) {
                fb_log("esp: partition %u (%s) does not mount as vfat: %s", d->all[i].index, d->all[i].name, strerror(errno));
                continue;
            }
            if (has_android_entry(mnt)) {
                ncand++;
                snprintf(cand_dev, sizeof(cand_dev), "%s", node);
                snprintf(cand_uuid, sizeof(cand_uuid), "%s", d->all[i].partuuid);
            }
            umount(mnt);
        }
        if (ncand == 0) {
            seterr(err, err_len, "no ESP with loader/entries/*-android-*.conf on the target disk%s",
                   o->want_esp_uuid ? " (gk3.esp given)" : "");
            return -1;
        }
        if (ncand > 1) {
            seterr(err, err_len, "%d ESPs on the target disk carry Android entries — ambiguous, not writing", ncand);
            return -1;
        }
        if (try_mount(cand_dev, mnt, false)) {
            seterr(err, err_len, "mount %s rw: %s", cand_dev, strerror(errno));
            return -1;
        }
        e->mounted = true;
        snprintf(e->root, sizeof(e->root), "%s", mnt);
        snprintf(e->dev, sizeof(e->dev), "%s", cand_dev);
        snprintf(e->partuuid, sizeof(e->partuuid), "%s", cand_uuid);
        return 0;
    }
#else
    (void)d;
    seterr(err, err_len, "ESP mounting needs Linux (use --esp-dir on this host)");
    return -1;
#endif
}

void fb_esp_close(fb_esp *e)
{
#ifdef __linux__
    if (e->mounted) {
        sync();
        if (umount(e->root))
            fb_log("esp: umount %s: %s", e->root, strerror(errno));
    }
#endif
    e->mounted = false;
}

/* ---------------------------------------------------------------- 文件写入（临时文件 + 读回 + rename） */

static int write_all(int fd, const void *buf, size_t n)
{
    const uint8_t *b = buf;
    while (n) {
        ssize_t r = write(fd, b, n);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return -1;
        b += r;
        n -= (size_t)r;
    }
    return 0;
}

static int fsync_dir(const char *dir)
{
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int rc;
    if (fd < 0)
        return -1;
    rc = fsync(fd);
    close(fd);
    /* vfat 上目录 fsync 可能报 EINVAL，不当错 */
    return rc && errno != EINVAL ? -1 : 0;
}

/* dir/name ← data；先写 dir/.name.new 并读回比对，再 rename。 */
static int put_file(const char *dir, const char *name, const void *data, size_t len, char *err, size_t err_len)
{
    char tmp[600], dst[600];
    int fd;
    uint8_t *rb;
    snprintf(tmp, sizeof(tmp), "%s/.%s.new", dir, name);
    snprintf(dst, sizeof(dst), "%s/%s", dir, name);
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        seterr(err, err_len, "create %s: %s", tmp, strerror(errno));
        return -1;
    }
    if (write_all(fd, data, len) || fsync(fd)) {
        seterr(err, err_len, "write %s: %s", tmp, strerror(errno));
        close(fd);
        unlink(tmp);
        return -1;
    }
    close(fd);
    /* 读回：丢掉页缓存再读（vfat 上 DONTNEED 只作废干净页；上面已 fsync，页都是干净的） */
    fd = open(tmp, O_RDONLY | O_CLOEXEC);
    rb = malloc(len ? len : 1);
    if (fd < 0 || !rb) {
        seterr(err, err_len, "re-open %s: %s", tmp, strerror(errno));
        if (fd >= 0)
            close(fd);
        free(rb);
        unlink(tmp);
        return -1;
    }
#ifdef __linux__
    posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
#endif
    {
        size_t got = 0;
        while (got < len) {
            ssize_t r = read(fd, rb + got, len - got);
            if (r <= 0)
                break;
            got += (size_t)r;
        }
        uint8_t extra;
        if (got != len || memcmp(rb, data, len) || read(fd, &extra, 1) != 0) {
            seterr(err, err_len, "read-back of %s does not match what was written", tmp);
            close(fd);
            free(rb);
            unlink(tmp);
            return -1;
        }
    }
    close(fd);
    free(rb);
    if (rename(tmp, dst)) {
        seterr(err, err_len, "rename %s -> %s: %s", tmp, dst, strerror(errno));
        unlink(tmp);
        return -1;
    }
    fsync_dir(dir);
    return 0;
}

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    char *b;
    long l;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    l = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc((size_t)l + 1);
    if (b && fread(b, 1, (size_t)l, f) != (size_t)l) {
        free(b);
        b = NULL;
    }
    fclose(f);
    if (b) {
        b[l] = 0;
        *len = (size_t)l;
    }
    return b;
}

/* ---------------------------------------------------------------- 直连条目 */

static bool is_direct_entry(const char *fn, unsigned slot)
{
    char suf[16];
    size_t n = strlen(fn);
    snprintf(suf, sizeof(suf), "-android-%c.conf", 'a' + slot);
    if (n != 32 + strlen(suf) || strcmp(fn + 32, suf))
        return false;
    for (int i = 0; i < 32; i++)
        if (!((fn[i] >= '0' && fn[i] <= '9') || (fn[i] >= 'a' && fn[i] <= 'f')))
            return false;
    return true;
}

/* 找该槽的直连条目与目录：ent = 条目全路径，slotdir = /<mid>/android/slot_<x> 的全路径 */
static int pick_slot_dir(fb_esp *e, unsigned slot, bool create, char *ent, size_t ent_len, char *slotdir, size_t sd_len,
                         char *err, size_t err_len)
{
    char p[600], kpath[600], want_tail[64], mid[300];
    DIR *d;
    struct dirent *de;
    int n = 0;
    char *buf, *line;
    size_t len;
    struct stat st;

    snprintf(p, sizeof(p), "%s/loader/entries", e->root);
    d = opendir(p);
    if (!d) {
        seterr(err, err_len, "no loader/entries on the ESP");
        return -1;
    }
    while ((de = readdir(d))) {
        if (!is_direct_entry(de->d_name, slot))
            continue;
        snprintf(p, sizeof(p), "%s/loader/entries/%s", e->root, de->d_name);
        if (stat(p, &st) || !S_ISREG(st.st_mode))
            continue;
        n++;
        snprintf(ent, ent_len, "%s", p);
    }
    closedir(d);
    if (n != 1) {
        seterr(err, err_len, "ESP has %d direct entries <machine-id>-android-%c.conf (need exactly one to know where the kernel goes)",
               n, 'a' + slot);
        return -1;
    }
    buf = slurp(ent, &len);
    if (!buf) {
        seterr(err, err_len, "read %s: %s", ent, strerror(errno));
        return -1;
    }
    kpath[0] = 0;
    for (line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        if (strncmp(line, "linux", 5) || !(line[5] == ' ' || line[5] == '\t'))
            continue;
        char *v = line + 5;
        while (*v == ' ' || *v == '\t')
            v++;
        size_t l = strlen(v);
        while (l && (v[l - 1] == ' ' || v[l - 1] == '\t' || v[l - 1] == '\r'))
            v[--l] = 0;
        snprintf(kpath, sizeof(kpath), "%s", v);
        break;
    }
    free(buf);
    snprintf(want_tail, sizeof(want_tail), "/android/slot_%c/Image", 'a' + slot);
    {
        size_t kl = strlen(kpath), tl = strlen(want_tail);
        const char *m;
        if (kpath[0] != '/' || kl <= tl + 1 || strcmp(kpath + kl - tl, want_tail)) {
            seterr(err, err_len, "entry linux line '%s' is not /<dir>/android/slot_%c/Image", kpath, 'a' + slot);
            return -1;
        }
        m = kpath + 1;
        snprintf(mid, sizeof(mid), "%.*s", (int)(kl - tl - 1), m);
        if (!mid[0] || strchr(mid, '/') || !strcmp(mid, ".") || !strcmp(mid, "..")) {
            seterr(err, err_len, "entry linux line '%s' is not one directory level deep", kpath);
            return -1;
        }
    }
    snprintf(p, sizeof(p), "%s/%s", e->root, mid);
    if (stat(p, &st) || !S_ISDIR(st.st_mode)) {
        seterr(err, err_len, "directory /%s named by the entry does not exist on the ESP", mid);
        return -1;
    }
    if (create) {
        snprintf(slotdir, sd_len, "%s/%s/android", e->root, mid);
        mkdir(slotdir, 0755);
    }
    snprintf(slotdir, sd_len, "%s/%s/android/slot_%c", e->root, mid, 'a' + slot);
    if (create)
        mkdir(slotdir, 0755);
    return 0;
}

static uint64_t fsize(const char *dir, const char *name)
{
    char p[600];
    struct stat st;
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    return stat(p, &st) ? 0 : (uint64_t)st.st_size;
}

/* 条目 options 行改写：postinstall 的 awk 把每一行 ^options[[:space:]] 都换成 "options    <cmd>" */
static int rewrite_options(const char *ent, const char *cmd, char *err, size_t err_len)
{
    size_t len, cap, o = 0;
    char *buf = slurp(ent, &len), *out, *p, *dir, *slash;
    bool any = false;
    int rc;
    if (!buf) {
        seterr(err, err_len, "read %s", ent);
        return -1;
    }
    cap = len + strlen(cmd) * 4 + 64;
    out = malloc(cap);
    if (!out) {
        free(buf);
        return -1;
    }
    for (p = buf; *p;) {
        char *nl = strchr(p, '\n');
        size_t ll = nl ? (size_t)(nl - p) : strlen(p);
        if (ll > 7 && !strncmp(p, "options", 7) && isspace((unsigned char)p[7])) {
            o += (size_t)snprintf(out + o, cap - o, "options    %s\n", cmd);
            any = true;
        } else {
            memcpy(out + o, p, ll);
            o += ll;
            if (nl)
                out[o++] = '\n';
        }
        p += ll + (nl ? 1 : 0);
    }
    free(buf);
    if (!any) {
        free(out);
        seterr(err, err_len, "entry has no options line; left unchanged");
        return 1;
    }
    dir = strdup(ent);
    slash = strrchr(dir, '/');
    *slash = 0;
    rc = put_file(dir, slash + 1, out, o, err, err_len);
    free(dir);
    free(out);
    return rc;
}

int fb_esp_sync_slot(fb_esp *e, unsigned slot, const uint8_t *img, size_t img_len, const gk3_bootimg *b,
                     void (*info)(void *ctx, const char *msg), void *ctx, char *err, size_t err_len)
{
    char ent[600], dir[600], msg[300], cmd[GK3_BOOT_ARGS_SIZE + GK3_BOOT_EXTRA_ARGS_SIZE + 64];
    struct statvfs vf;
    uint64_t need, avail, old = 0;
    long cl;
    static const char *const names[] = {"Image", "ramdisk.img", "gaokun3.dtb", "cmdline.txt"};

    if (pick_slot_dir(e, slot, true, ent, sizeof(ent), dir, sizeof(dir), err, err_len))
        return -1;
    if (b->kernel_off + b->kernel_size > img_len || b->ramdisk_off + b->ramdisk_size > img_len ||
        b->dtb_off + b->dtb_size > img_len) {
        seterr(err, err_len, "boot image segments out of range");
        return -1;
    }
    cl = gk3_bootimg_cmdline(b, cmd, sizeof(cmd) - 40);
    if (cl < 0) {
        seterr(err, err_len, "boot image cmdline too long");
        return -1;
    }
    /* 空间：新文件合计（+ 1 MiB 余量）≤ 空闲 + 将被覆盖的旧文件 */
    need = (uint64_t)b->kernel_size + b->ramdisk_size + b->dtb_size + (uint64_t)cl + 1 + (1u << 20);
    for (unsigned i = 0; i < 4; i++)
        old += fsize(dir, names[i]);
    if (statvfs(dir, &vf)) {
        seterr(err, err_len, "statvfs: %s", strerror(errno));
        return -1;
    }
    avail = (uint64_t)vf.f_bavail * vf.f_frsize;
    if (need > avail + old) {
        seterr(err, err_len, "ESP too full: need %llu KiB, have %llu KiB free + %llu KiB to be replaced",
               (unsigned long long)(need >> 10), (unsigned long long)(avail >> 10), (unsigned long long)(old >> 10));
        return -1;
    }
    snprintf(msg, sizeof(msg), "ESP: %s -> %s", strrchr(ent, '/') + 1, dir + strlen(e->root));
    info(ctx, msg);
    if (put_file(dir, "Image", img + b->kernel_off, b->kernel_size, err, err_len) ||
        put_file(dir, "ramdisk.img", img + b->ramdisk_off, b->ramdisk_size, err, err_len) ||
        put_file(dir, "gaokun3.dtb", img + b->dtb_off, b->dtb_size, err, err_len))
        return -1;
    {
        char line[sizeof(cmd) + 2];
        int n = snprintf(line, sizeof(line), "%s\n", cmd);
        if (put_file(dir, "cmdline.txt", line, (size_t)n, err, err_len))
            return -1;
    }
    /* options：cmdline 去掉 \r\n 后 + slot_suffix（postinstall 的 case 判重） */
    {
        char opt[sizeof(cmd)], probe[sizeof(cmd) + 2];
        size_t o = 0;
        for (long i = 0; i < cl; i++)
            if (cmd[i] != '\r' && cmd[i] != '\n')
                opt[o++] = cmd[i];
        opt[o] = 0;
        snprintf(probe, sizeof(probe), " %s ", opt);
        if (!strstr(probe, " androidboot.slot_suffix="))
            snprintf(opt + o, sizeof(opt) - o, " androidboot.slot_suffix=_%c", 'a' + slot);
        int r = rewrite_options(ent, opt, err, err_len);
        if (r < 0)
            return -1;
        if (r > 0)
            info(ctx, "ESP: entry has no options line, left unchanged");
    }
    sync();
    snprintf(msg, sizeof(msg), "ESP: slot_%c updated (Image %u, ramdisk %u, dtb %u bytes, read back OK)", 'a' + slot,
             b->kernel_size, b->ramdisk_size, b->dtb_size);
    info(ctx, msg);
    return 0;
}

int fb_esp_set_default(fb_esp *e, unsigned slot, char *err, size_t err_len)
{
    char path[600], dir[600], want[64];
    size_t len, cap, o = 0;
    char *buf, *out, *p;
    bool replaced = false;
    int rc;
    snprintf(dir, sizeof(dir), "%s/loader", e->root);
    snprintf(path, sizeof(path), "%s/loader.conf", dir);
    buf = slurp(path, &len);
    if (!buf) {
        seterr(err, err_len, "read loader/loader.conf: %s", strerror(errno));
        return -1;
    }
    snprintf(want, sizeof(want), "default *-android-%c.conf", 'a' + slot);
    cap = len + sizeof(want) + 4;
    out = malloc(cap);
    if (!out) {
        free(buf);
        return -1;
    }
    /* 逐字照 android::base::Split(contents, "\n") → 替换 / 丢弃 default 行 → 没有就 push_back → Join("\n") */
    {
        bool first = true;
        for (p = buf;;) {
            char *nl = strchr(p, '\n');
            size_t ll = nl ? (size_t)(nl - p) : strlen(p);
            const char *t = p;
            bool is_def;
            while (t < p + ll && isspace((unsigned char)*t))
                t++;
            is_def = (size_t)(p + ll - t) >= 7 && !strncmp(t, "default", 7);
            if (!is_def || !replaced) {
                if (!first)
                    out[o++] = '\n';
                first = false;
                if (is_def) {
                    o += (size_t)snprintf(out + o, cap - o, "%s", want);
                    replaced = true;
                } else {
                    memcpy(out + o, p, ll);
                    o += ll;
                }
            }
            if (!nl)
                break;
            p = nl + 1;
        }
        if (!replaced) {
            if (!first)
                out[o++] = '\n';
            o += (size_t)snprintf(out + o, cap - o, "%s", want);
        }
    }
    free(buf);
    rc = put_file(dir, "loader.conf", out, o, err, err_len);
    free(out);
    return rc;
}

int fb_esp_get_default(fb_esp *e, char *out, size_t out_len)
{
    char path[600];
    size_t len;
    char *buf, *line;
    snprintf(path, sizeof(path), "%s/loader/loader.conf", e->root);
    buf = slurp(path, &len);
    out[0] = 0;
    if (!buf)
        return -1;
    for (line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        char *t = line;
        while (isspace((unsigned char)*t))
            t++;
        if (strncmp(t, "default", 7) || !isspace((unsigned char)t[7]))
            continue;
        t += 7;
        while (isspace((unsigned char)*t))
            t++;
        snprintf(out, out_len, "%s", t);
        out[strcspn(out, "\r")] = 0;
        break;
    }
    free(buf);
    return out[0] ? 0 : -1;
}

int fb_esp_slot_present(fb_esp *e, unsigned slot, char *err, size_t err_len)
{
    char ent[600], dir[600];
    static const char *const need[] = {"Image", "gaokun3.dtb", "ramdisk.img"};
    if (pick_slot_dir(e, slot, false, ent, sizeof(ent), dir, sizeof(dir), err, err_len))
        return -1;
    for (unsigned i = 0; i < 3; i++)
        if (fsize(dir, need[i]) == 0) {
            seterr(err, err_len, "ESP %s/%s is missing or empty", dir + strlen(e->root), need[i]);
            return -1;
        }
    return 0;
}
