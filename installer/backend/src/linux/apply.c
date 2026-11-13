/* Linux：装 Android 到一段空闲空间（双系统），LiveCD 用。
 *
 *   gk3l apply --disk /dev/X --region-start LBA --region-end LBA --release DIR
 *              [--sdboot FILE] [--backup-dir DIR] [--machine-id HEX32] [--default-os android|windows]
 *              [--userdata-mib N] [--bitlocker-confirmed]
 *
 * 步骤与进度代码同 Windows 版 gk3w apply：1 check / verify-sha / test-zst → 2 plan → 5 write-gpt → 15 format →
 * 30 write-super → 70 write-boot → 80 bootloader → 100 done。★ 所有输入在第一次写盘之前验完（boot.img 拆得开、super 完整
 * 试解一遍、ESP 可读且空间够、Windows 没在休眠、区间确实空闲、分区名不重），之后 ERR 一律带 touched=yes。
 *
 * 发布目录：boot.img、super.img.zst（或 sparse 格式的 super.img），可选 install-artifacts.sha256（有就逐个核对）。
 * systemd-boot：--sdboot，否则 GK3_SDBOOT，否则依次找 /usr/share/gaokun3/、发布目录、/usr/lib/systemd/boot/efi/ 下的
 * systemd-bootaa64.efi。
 *
 * 与 Windows 版的差别：
 *   - 分区表自己写（../gptw.c：主表、备份表各自 CRC），再用 BLKPG 逐个告诉内核新分区 —— 不重读整张表，ESP 等分区挂着
 *     也不受影响。动手之前内核的分区视图必须与盘上的 GPT 一致（gk3l probe 的 kernel=ok）。
 *   - 写盘期间对整块盘持 BSD 排他锁：systemd-udevd 见到锁就不去探测这块盘（systemd 的块设备锁约定），
 *     新分区在写完之前不会被 udev 读进缓存。
 *   - metadata / userdata 用 Debian 的 mkfs.ext4 直接格式化（参数同 installer-lib.sh），写完核对超级块的魔数与卷标。
 *   - ESP 挂载后写文件，写完卸下、丢掉这块设备的缓存再只读挂回去逐个比对：读回的是介质上的内容，不是页缓存。
 *   - Windows 卷在休眠时不写共用的 ESP（ERR esp-windows-hibernated）；Windows 卷是 BitLocker、而这次要换掉
 *     EFI/BOOT/BOOTAA64.EFI 时，要 --bitlocker-confirmed（用户在界面上确认拿到了恢复密钥），Linux 这边暂停不了 BitLocker。
 *   - ESP 上已有的启动项按 installer-lib.sh 的规则清理（别的目录的直连条目改名停用、旧的救援启动项与统一启动入口的
 *     条目删掉），不像 Windows 版那样拒绝：LiveCD 没有卸载功能，删过 Android 分区的盘上留着这些条目是常见情形。
 *   - 写完删 EFI 变量 LoaderEntryDefault（删不掉报 NOTE loadervar-stuck）。 */
#define _GNU_SOURCE
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
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#include "../esp.h"
#include "../gptw.h"
#include "../layout.h"
#include "../plan.h"
#include "../proto.h"
#include "../release.h"
#include "../sparse.h"
#include "disk.h"
#include "files.h"
#include "gk3core.h"
#include "lx.h"
#include "sysstate.h"

#define IO_CHUNK GK3W_IO_CHUNK
#define ESP_TYPE "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"
#define LINUX_DATA "0FC63DAF-8483-4772-8E79-3D69D8477DE4"
#define MEDIUM "/media/gk3"

static const char *const OUR_NAMES[] = {"misc", "metadata", "boot_a", "boot_b", "super", "userdata", "gk3rescue"};

/* ── 输入 ─────────────────────────────────────────────────────────── */

static int sha256_file(const char *path, char out[65])
{
    char buf[256];
    char *argv[] = {"sha256sum", "--", (char *)path, NULL};
    if (gk3l_run(argv, buf, sizeof buf) != 0 || strlen(buf) < 64)
        return -1;
    for (int i = 0; i < 64; i++) {
        char c = buf[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return -1;
        out[i] = c;
    }
    out[64] = 0;
    return 0;
}

static long fd_read(void *ctx, uint8_t *buf, size_t cap)
{
    int fd = *(int *)ctx;
    for (;;) {
        ssize_t n = read(fd, buf, cap);
        if (n < 0 && errno == EINTR)
            continue;
        return (long)n;
    }
}

/* 打开 path 交给 gk3w_image_run（sink 为 NULL 时只试解） */
static int run_image(const char *path, int zst, uint64_t limit, const gk3w_sparse_sink *sink, int prog, const char **why)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC), rc;
    struct stat st;
    gk3w_reader rd;
    *why = "open";
    if (fd < 0)
        return -1;
    if (fstat(fd, &st) || !st.st_size) {
        close(fd);
        return -1;
    }
    posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
    rd.ctx = &fd;
    rd.read = fd_read;
    rd.size = (uint64_t)st.st_size;
    rc = gk3w_image_run(&rd, zst, limit, sink, prog, why, NULL);
    close(fd);
    return rc;
}

/* systemd-boot 从哪来 */
static int find_sdboot(const char *arg, const char *rel, char *out, size_t cap)
{
    const char *env = getenv("GK3_SDBOOT");
    char cand[3][4096];
    if (arg && *arg) {
        snprintf(out, cap, "%s", arg);
        return 0;
    }
    if (env && *env) {
        snprintf(out, cap, "%s", env);
        return 0;
    }
    snprintf(cand[0], sizeof cand[0], "/usr/share/gaokun3/systemd-bootaa64.efi");
    snprintf(cand[1], sizeof cand[1], "%s/systemd-bootaa64.efi", rel);
    snprintf(cand[2], sizeof cand[2], "/usr/lib/systemd/boot/efi/systemd-bootaa64.efi");
    for (int i = 0; i < 3; i++)
        if (gk3l_file_size(cand[i]) > 0) {
            snprintf(out, cap, "%.4095s", cand[i]);
            return 0;
        }
    return -1;
}

static int have_tool(const char *t)
{
    const char *path = getenv("PATH");
    char buf[4096], p[4200], *save = NULL;
    snprintf(buf, sizeof buf, "%s", path && *path ? path : "/usr/sbin:/usr/bin:/sbin:/bin");
    for (char *d = strtok_r(buf, ":", &save); d; d = strtok_r(NULL, ":", &save)) {
        snprintf(p, sizeof p, "%s/%s", d, t);
        if (!access(p, X_OK))
            return 1;
    }
    return 0;
}

/* ── super / boot / misc：经整块盘的 O_DIRECT 句柄写，写一段读回一段 ── */

typedef struct {
    const gk3l_disk *d;
    uint64_t base;
    uint8_t *chk, *pat;
    int failed;
} disk_sink;

static int ds_write(void *ctx, uint64_t off, const uint8_t *data, size_t len)
{
    disk_sink *s = ctx;
    int w = gk3l_write_verify(s->d, data, len, s->base + off, s->chk);
    if (w)
        s->failed = w;
    return w;
}

static int ds_fill(void *ctx, uint64_t off, uint64_t len, uint32_t pattern)
{
    disk_sink *s = ctx;
    for (size_t i = 0; i < IO_CHUNK; i += 4)
        memcpy(s->pat + i, &pattern, 4);
    while (len) {
        size_t n = len < IO_CHUNK ? (size_t)len : IO_CHUNK;
        if (ds_write(ctx, off, s->pat, n))
            return -1;
        off += n;
        len -= n;
    }
    return 0;
}

/* 分区开头是不是一个卷标为 label 的 ext4（超级块在 1024：魔数 0xEF53 @0x38、卷标 @0x78） */
static int check_ext4(const gk3l_disk *d, uint64_t off, const char *label, uint8_t *buf4k)
{
    if (gk3l_pread(d, buf4k, 4096, off))
        return -1;
    return buf4k[1024 + 0x38] == 0x53 && buf4k[1024 + 0x39] == 0xEF && !strncmp((char *)buf4k + 1024 + 0x78, label, 16) ? 0 : -1;
}

/* ── 新分区：告诉内核、等节点 ── */

static int kernel_add(const gk3l_disk *d, uint32_t idx, uint64_t first, uint64_t last)
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

/* 等内核建好 idx 号分区的节点：sysfs 里有它、/dev 下同名节点是块设备且设备号一致。最多等 10 秒 */
static int wait_node(const gk3l_disk *d, uint32_t idx, char *out, size_t cap)
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
static void flush_node(const char *dev)
{
    int fd = open(dev, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        (void)ioctl(fd, BLKFLSBUF, 0);
        close(fd);
    }
}

/* ── 分区表备份 ── */

/* GPT 主表区（LBA 0 到 usable 起点）与备份表区（usable 终点之后到盘尾）原样存进
 * <dir>/gpt-before-apply-<盘>-<时间>-head<扇区数>-tail<扇区数>.bin（与 Windows 版同一格式）。还原：两段各自 dd 回原位 */
static int gpt_backup(const gk3l_disk *d, const char *dir_arg)
{
    char dir[4096], path[4400], ts[32];
    uint64_t head = d->g.first_usable, tail = d->blocks - (d->g.last_usable + 1);
    uint8_t *gb;
    time_t now = time(NULL);
    struct tm tmv;
    int err, in_ram = 0;
    if (dir_arg && *dir_arg) {
        snprintf(dir, sizeof dir, "%s", dir_arg);
    } else if (gk3l_is_dir(MEDIUM "/gaokun3")) {
        /* 安装介质平时只读挂着：备份要留在拔得走的地方 */
        (void)mount(NULL, MEDIUM, NULL, MS_REMOUNT | MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
        snprintf(dir, sizeof dir, MEDIUM "/gaokun3");
    } else {
        const char *run = getenv("GK3_RUNDIR");
        snprintf(dir, sizeof dir, "%s", run && *run ? run : "/run/gaokun3");
        in_ram = 1;
    }
    if ((head + tail) * d->bs > (4u << 20))
        return gk3l_err(GK3W_TOUCHED_NO, "gpt-backup", "the partition table area is unusually large; refusing to continue (disk not touched)", 0), -1;
    gb = gk3w_aligned_alloc((size_t)((head + tail) * d->bs));
    if (!gb)
        return gk3l_err(GK3W_TOUCHED_NO, "nomem", "out of memory", ENOMEM), -1;
    if (gk3l_pread(d, gb, (size_t)(head * d->bs), 0) ||
        gk3l_pread(d, gb + head * d->bs, (size_t)(tail * d->bs), (d->g.last_usable + 1) * d->bs)) {
        err = errno;
        gk3w_aligned_free(gb);
        return gk3l_err(GK3W_TOUCHED_NO, "gpt-backup", "reading the partition table area failed (disk not touched)", err), -1;
    }
    localtime_r(&now, &tmv);
    strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", &tmv);
    snprintf(path, sizeof path, "%s/gpt-before-apply-%s-%s-head%llu-tail%llu.bin", dir, d->name, ts,
             (unsigned long long)head, (unsigned long long)tail);
    err = gk3l_mkdirs(dir);
    if (!err)
        err = gk3l_write_file(path, gb, (size_t)((head + tail) * d->bs));
    if (!err && !gk3l_file_same(path, gb, (size_t)((head + tail) * d->bs)))
        err = EIO;
    gk3w_aligned_free(gb);
    if (err)
        return gk3l_err(GK3W_TOUCHED_NO, "gpt-backup", "writing the partition table backup failed (disk not touched)", err), -1;
    gk3l_log("partition table backup: %s (restore: dd the first %llu sectors to the start of %s and the last %llu to its end, bs=%u)",
             path, (unsigned long long)head, d->path, (unsigned long long)tail, d->bs);
    if (in_ram)
        gk3l_log("warning: the installation medium is not writable, so the backup is only in RAM (%s) and is lost on reboot", path);
    return 0;
}

/* ── ESP ──────────────────────────────────────────────────────────── */

typedef struct {
    char dir[256];              /* 挂载点 */
    char mid[33];
    int windows;                /* EFI/Microsoft/Boot/bootmgfw.efi 在 */
} esp_ctx;

static void esp_path(const esp_ctx *e, const char *rel, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s", e->dir, rel);
}

static int64_t esp_size_of(const esp_ctx *e, const char *rel)
{
    char p[4096];
    esp_path(e, rel, p, sizeof p);
    return gk3l_file_size(p);
}

/* 往 ESP 上写这一组文件要多占多少 KiB（同名旧文件只算变大的部分，按 4 KiB 簇向上取整）—— gk3__esp_delta_kib */
static int64_t esp_delta(const esp_ctx *e, const char *rel, uint64_t newsz)
{
    int64_t old = esp_size_of(e, rel);
    if (old < 0)
        old = 0;
    return (int64_t)newsz > old ? ((int64_t)newsz - old + 4095) / 4096 * 4 : 0;
}

static int esp_same(const esp_ctx *e, const char *rel, const uint8_t *data, size_t len)
{
    char p[4096];
    esp_path(e, rel, p, sizeof p);
    return gk3l_file_same(p, data, len);
}

/* ESP 根目录下第一个（按字节序）32 位十六进制的目录名 —— gk3__esp_pick_mid */
static int esp_pick_mid(const esp_ctx *e, char out[33])
{
    DIR *d = opendir(e->dir);
    struct dirent *de;
    int found = 0;
    if (!d)
        return 0;
    while ((de = readdir(d))) {
        char p[4096];
        if (!gk3w_is_machine_id(de->d_name))
            continue;
        esp_path(e, de->d_name, p, sizeof p);
        if (gk3l_is_dir(p) && (!found || strcmp(de->d_name, out) < 0)) {
            memcpy(out, de->d_name, 33);
            found = 1;
        }
    }
    closedir(d);
    return found;
}

/* 写过的文件：卸下后从介质读回比对 */
typedef struct {
    char rel[160];
    const uint8_t *data;
    size_t len;
} written;

#define MAX_WRITTEN 24

typedef struct {
    esp_ctx *e;
    written w[MAX_WRITTEN];
    size_t n;
} esp_log;

static int esp_put(esp_log *lg, const char *rel, const uint8_t *data, size_t len, int renamed)
{
    char p[4096], dir[4096], *slash;
    int err;
    esp_path(lg->e, rel, p, sizeof p);
    snprintf(dir, sizeof dir, "%s", p);
    slash = strrchr(dir, '/');
    if (slash) {
        *slash = 0;
        if ((err = gk3l_mkdirs(dir)))
            return gk3l_err_kv(GK3W_TOUCHED_YES, "esp-write", "file", rel, "cannot create a directory on the EFI system partition", err);
    }
    err = renamed ? gk3l_replace_file(p, data, len) : gk3l_write_file(p, data, len);
    if (err)
        return gk3l_err_kv(GK3W_TOUCHED_YES, "esp-write", "file", rel, "writing the EFI system partition failed: out of space, or a media error", err);
    if (lg->n < MAX_WRITTEN) {
        snprintf(lg->w[lg->n].rel, sizeof lg->w[lg->n].rel, "%s", rel);
        lg->w[lg->n].data = data;
        lg->w[lg->n].len = len;
        lg->n++;
    }
    return 0;
}

/* 别的 <machine-id> 目录下我们的直连条目：default 的通配 *-android-<槽>.conf 会同时匹配它们，改名停用（不删）。
 * 统一启动入口的条目（gk3boot-* / gk3prev-* / gk3boot-tools.conf）与没人引用的 EFI/gk3boot/<版本>/ 删掉（log/ 留着）：
 * 装完的机器回到"还没有入口"，第一次开机完成时由装上的系统部署它自己那一版。旧的救援启动项与 <mid>/rescue/ 删掉
 * （不再装救援系统）。规则与 installer-lib.sh 的 gk3_apply 相同。 */
static int is_direct_entry(const char *n)
{
    size_t l = strlen(n);
    if (l != 32 + 15 || (strcmp(n + 32, "-android-a.conf") && strcmp(n + 32, "-android-b.conf")))
        return 0;
    for (int i = 0; i < 32; i++)
        if (!((n[i] >= '0' && n[i] <= '9') || (n[i] >= 'a' && n[i] <= 'f')))
            return 0;
    return 1;
}

static int ci_contains(const uint8_t *hay, size_t n, const char *needle)
{
    size_t m = strlen(needle);
    for (size_t i = 0; i + m <= n; i++) {
        size_t k = 0;
        while (k < m) {
            char a = (char)hay[i + k], b = needle[k];
            if (a >= 'A' && a <= 'Z')
                a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z')
                b = (char)(b + 32);
            if (a != b)
                break;
            k++;
        }
        if (k == m)
            return 1;
    }
    return 0;
}

static int esp_cleanup(const esp_ctx *e)
{
    char ed[4096], p[4400], q[4500];
    DIR *d;
    struct dirent *de;
    int gn = 0, err;
    esp_path(e, "loader/entries", ed, sizeof ed);
    d = opendir(ed);
    if (d) {
        while ((de = readdir(d))) {
            const char *n = de->d_name;
            snprintf(p, sizeof p, "%s/%s", ed, n);
            if (is_direct_entry(n) && strncmp(n, e->mid, 32)) {
                snprintf(q, sizeof q, "%s.disabled", p);
                if (rename(p, q)) {
                    err = errno;
                    closedir(d);
                    return gk3l_err_kv(GK3W_TOUCHED_YES, "esp-write", "file", n, "cannot disable a boot entry of another directory", err);
                }
                gk3l_log("disabled the boot entry of another directory %s -> %s.disabled (the default wildcard would match it too)", n, n);
            } else if ((!strncmp(n, "gk3boot-android-", 16) || !strncmp(n, "gk3prev-android-", 16) || !strcmp(n, "gk3boot-tools.conf")) &&
                       gk3l_file_size(p) >= 0) {
                if (unlink(p)) {
                    err = errno;
                    closedir(d);
                    return gk3l_err_kv(GK3W_TOUCHED_YES, "esp-write", "file", n, "cannot delete an old boot entry", err);
                }
                gn++;
            } else if (!strncmp(n, e->mid, 32) && (!strcmp(n + 32, "-rescue.conf") || !strcmp(n + 32, "-rescue-b.conf"))) {
                if (unlink(p)) {
                    err = errno;
                    closedir(d);
                    return gk3l_err_kv(GK3W_TOUCHED_YES, "esp-write", "file", n, "cannot delete an old rescue boot entry", err);
                }
                gk3l_log("deleted the old rescue boot entry %s", n);
            }
        }
        closedir(d);
    }
    snprintf(p, sizeof p, "%s/%s/rescue", e->dir, e->mid);
    if (gk3l_is_dir(p) && (err = gk3l_rm_rf(p)))
        return gk3l_err_kv(GK3W_TOUCHED_YES, "esp-write", "file", "rescue", "cannot delete the old rescue directory", err);
    esp_path(e, "EFI/gk3boot", p, sizeof p);
    d = opendir(p);
    if (d) {
        while ((de = readdir(d))) {
            char vd[4700], needle[300];
            int used = 0;
            if (de->d_name[0] == '.' || !strcmp(de->d_name, "log"))
                continue;
            snprintf(vd, sizeof vd, "%s/%s", p, de->d_name);
            if (!gk3l_is_dir(vd))
                continue;
            /* 还有别的条目（手放的实验条目）指着它就留着 —— 同 HAL 回收目录的规则 */
            snprintf(needle, sizeof needle, "/EFI/gk3boot/%s/", de->d_name);
            {
                DIR *d2 = opendir(ed);
                struct dirent *f;
                while (d2 && (f = readdir(d2)) && !used) {
                    size_t fl = strlen(f->d_name);
                    uint8_t *b;
                    size_t bl;
                    char fp[4400];
                    if (fl < 5 || strcmp(f->d_name + fl - 5, ".conf"))
                        continue;
                    snprintf(fp, sizeof fp, "%s/%s", ed, f->d_name);
                    if (!gk3l_read_file(fp, &b, &bl, 1u << 20)) {
                        used = ci_contains(b, bl, needle);
                        free(b);
                    }
                }
                if (d2)
                    closedir(d2);
            }
            if (used)
                continue;
            if ((err = gk3l_rm_rf(vd))) {
                closedir(d);
                return gk3l_err_kv(GK3W_TOUCHED_YES, "esp-write", "file", de->d_name, "cannot delete an old boot manager directory", err);
            }
            gn++;
        }
        closedir(d);
    }
    if (gn)
        gk3l_log("unified boot entry: deleted %d old entries / version directories (the installed system deploys its own on first boot)", gn);
    return 0;
}

/* ── apply ────────────────────────────────────────────────────────── */

static int part_index_by_name(const gk3w_plan *plan, const char *name)
{
    for (size_t i = 0; i < plan->n; i++)
        if (!strcmp(plan->parts[i].name, name))
            return (int)i;
    return -1;
}

int gk3l_cmd_apply(int argc, char **argv)
{
    uint64_t rstart, rend, ud_mib = 0;
    const char *disk_arg = gk3l_arg(argc, argv, "--disk"), *rel = gk3l_arg(argc, argv, "--release"),
               *sdboot_arg = gk3l_arg(argc, argv, "--sdboot"), *backup_dir = gk3l_arg(argc, argv, "--backup-dir"),
               *mid_arg = gk3l_arg(argc, argv, "--machine-id"), *defos_arg = gk3l_arg(argc, argv, "--default-os");
    int blconfirmed = gk3l_flag(argc, argv, "--bitlocker-confirmed");
    char p_boot[4096], p_super[4096], p_sha[4096], sdboot_path[4096], dname[64], mid_fb[33];
    char esp_dev[96] = "", nodes[GK3W_PLAN_MAX][96];
    static gk3l_disk d;
    gk3w_boot bi;
    gk3w_plan plan;
    esp_ctx esp;
    esp_log elog;
    uint8_t *sdboot = NULL, *man = NULL, *zero = NULL, *chk = NULL, *bootbuf = NULL, *snap = NULL;
    uint8_t *orig[2] = {NULL, NULL};
    size_t orig_len[2] = {0, 0}, sdboot_len = 0, man_len = 0, snap_len = 0;
    uint32_t idx[GK3W_PLAN_MAX];
    uint64_t poff[GK3W_PLAN_MAX];
    const char *why = NULL;
    int zst = 1, defos_windows = 0, rc = 1, opened = 0, esp_mounted = 0, err, aa_change = 0;
    uint64_t esp_bytes = 0;
    gk3w_touched T = GK3W_TOUCHED_NO;

    memset(&bi, 0, sizeof bi);
    memset(&esp, 0, sizeof esp);
    memset(&elog, 0, sizeof elog);
    elog.e = &esp;
    if (!disk_arg || !rel || gk3l_arg_u64(argc, argv, "--region-start", &rstart) || gk3l_arg_u64(argc, argv, "--region-end", &rend) ||
        (gk3l_arg(argc, argv, "--userdata-mib") && gk3l_arg_u64(argc, argv, "--userdata-mib", &ud_mib)))
        return gk3l_err(T, "usage",
                        "usage: apply --disk /dev/X --region-start LBA --region-end LBA --release DIR [--sdboot FILE] [--backup-dir DIR] "
                        "[--machine-id HEX32] [--default-os android|windows] [--userdata-mib N] [--bitlocker-confirmed]",
                        0);
    if (defos_arg && strcmp(defos_arg, "android") && strcmp(defos_arg, "windows"))
        return gk3l_err(T, "usage", "--default-os must be android or windows", 0);
    defos_windows = defos_arg && !strcmp(defos_arg, "windows");
    if (!mid_arg)
        mid_arg = getenv("GK3_MACHINE_ID");
    if (mid_arg && *mid_arg) {
        if (!gk3w_is_machine_id(mid_arg))
            return gk3l_err(T, "usage", "--machine-id must be 32 lowercase hex digits", 0);
        memcpy(mid_fb, mid_arg, 33);
    } else {
        uint8_t r[16];
        if (getrandom(r, sizeof r, 0) != (ssize_t)sizeof r)
            return gk3l_err(T, "random", "cannot generate a machine-id", errno);
        for (int i = 0; i < 16; i++)
            snprintf(mid_fb + 2 * i, 3, "%02x", r[i]);
    }
    if (gk3l_sys_gate())
        return 1;

    /* ── 输入：全部在动盘之前验完 ── */
    gk3w_progress(1, "check");
    snprintf(p_boot, sizeof p_boot, "%s/boot.img", rel);
    snprintf(p_super, sizeof p_super, "%s/super.img.zst", rel);
    snprintf(p_sha, sizeof p_sha, "%s/install-artifacts.sha256", rel);
    if (gk3l_file_size(p_super) < 0) {
        snprintf(p_super, sizeof p_super, "%s/super.img", rel);
        zst = 0;
        if (gk3l_file_size(p_super) < 0) {
            gk3l_err(T, "release-no-super", "the release directory has neither super.img.zst nor super.img", 0);
            goto out;
        }
    }
    if (find_sdboot(sdboot_arg, rel, sdboot_path, sizeof sdboot_path) ||
        gk3l_read_file(sdboot_path, &sdboot, &sdboot_len, 16u << 20) || sdboot_len < 2 || memcmp(sdboot, "MZ", 2)) {
        gk3l_err(T, "sdboot-missing", "systemd-boot (systemd-bootaa64.efi) is missing, unreadable or not a PE file", 0);
        goto out;
    }
    if (!have_tool("mkfs.ext4")) {
        gk3l_err_kv(T, "tool-missing", "tool", "mkfs.ext4", "mkfs.ext4 is missing", 0);
        goto out;
    }
    if (gk3l_read_file(p_sha, &man, &man_len, 1u << 20) == 0) {
        const char *names[2] = {"boot.img", zst ? "super.img.zst" : "super.img"};
        const char *paths[2] = {p_boot, p_super};
        gk3w_progress(1, "verify-sha");
        for (int i = 0; i < 2; i++) {
            char want[65], got[65];
            if (gk3w_manifest_hash(man, man_len, names[i], want))
                continue;
            if (sha256_file(paths[i], got) || strcmp(want, got)) {
                gk3l_err_kv(T, "sha-mismatch", "file", names[i], "a file does not match the sha256 in the release manifest (disk not touched)", 0);
                goto out;
            }
            gk3l_log("%s: sha256 matches the release manifest", names[i]);
        }
    } else {
        gk3l_log("no install-artifacts.sha256 in the release directory, skipping the sha256 check");
    }
    if ((err = gk3l_read_file(p_boot, &bi.img, &bi.len, GK3W_BOOT_MAX))) {
        gk3l_err(T, "release-no-boot", "the release directory has no readable boot.img (or it is larger than 64 MiB)", err);
        goto out;
    }
    {
        gk3w_boot_err be = gk3w_boot_check(&bi);
        if (be != GK3W_BOOT_OK) {
            gk3l_err(T, "bootimg-unpack", gk3w_boot_strerror(be), 0);
            goto out;
        }
        gk3l_log("  Image       %10u bytes", bi.b.kernel_size);
        gk3l_log("  ramdisk.img %10u bytes", bi.b.ramdisk_size);
        gk3l_log("  gaokun3.dtb %10u bytes", bi.b.dtb_size);
    }
    gk3w_progress(1, "test-zst");
    if (run_image(p_super, zst, (uint64_t)GK3W_SUPER_MIB << 20, NULL, 0, &why)) {
        gk3l_err_kv(T, "zst-corrupt", "why", why, "the super image is incomplete or not a valid sparse image (disk not touched)", 0);
        goto out;
    }
    gk3l_log("%s: full test decode passed", zst ? "super.img.zst" : "super.img");

    /* ── 盘 ── */
    if (gk3l_whole_disk_name(disk_arg, dname, sizeof dname)) {
        gk3l_err_kv(T, "disk-open", "disk", disk_arg, "--disk is not a whole disk", 0);
        goto out;
    }
    err = gk3l_disk_open(&d, dname, 1);
    if (err) {
        gk3l_err_kv(T, err == EACCES || err == EPERM ? "not-root" : "disk-open", "disk", disk_arg, "cannot open the disk for writing", err);
        goto out;
    }
    opened = 1;
    if (d.bs != 512 && d.bs != 4096) {
        gk3l_err(T, "disk-sector", "the disk sector size is neither 512 nor 4096 bytes", 0);
        goto out;
    }
    /* 排他锁：udev 见到它就不去探测这块盘（systemd 的块设备锁约定）；另一个安装器进程也拿不到 */
    {
        int locked = 0;
        for (int i = 0; i < 50 && !locked; i++) {
            if (!flock(d.fd, LOCK_EX | LOCK_NB))
                locked = 1;
            else
                usleep(100000);
        }
        if (!locked) {
            gk3l_err(T, "disk-busy", "another program holds the disk (udev or another installer); try again (disk not touched)", errno);
            goto out;
        }
    }
    if (gk3l_disk_read_table(&d)) {
        gk3l_err(T, "nomem", "out of memory", ENOMEM);
        goto out;
    }
    if (!strcmp(d.table, "unreadable")) {
        gk3l_err_kv(T, "disk-unreadable", "disk", d.path, "cannot read the partition table (disk not touched)", 0);
        goto out;
    }
    if (!strcmp(d.table, "mbr")) {
        gk3l_err_kv(T, "mbr-disk", "disk", d.path, "the disk has an MBR partition table; only a whole-disk wipe can use it (disk not touched)", 0);
        goto out;
    }
    if (strcmp(d.table, "gpt")) {
        gk3l_err_kv(T, "disk-not-gpt", "table", d.table, "the disk has no usable GPT partition table; only a whole-disk wipe can use it (disk not touched)", 0);
        goto out;
    }
    if (!gk3l_disk_kernel_ok(&d)) {
        gk3l_err(T, "kernel-stale", "the kernel's view of the partitions differs from the partition table on disk; reboot and try again (disk not touched)", 0);
        goto out;
    }
    snap_len = (size_t)d.g.num_entries * d.g.entry_size;
    snap = malloc(d.bs + snap_len);
    if (!snap) {
        gk3l_err(T, "nomem", "out of memory", ENOMEM);
        goto out;
    }
    memcpy(snap, d.hdr, d.bs);
    memcpy(snap + d.bs, d.ent, snap_len);

    /* ── ESP：恰好一个，FAT，挂得上 ── */
    {
        int n_esp = 0;
        uint32_t esp_idx = 0;
        char en[GK3_GPT_NAME_CHARS * 4 + 1] = "", fs[64], label[256];
        for (uint32_t i = 0; i < d.g.num_entries; i++) {
            gk3_gpt_part q;
            char type[37];
            if (gk3_gpt_get(&d.g, i, &q) != GK3_OK)
                continue;
            gk3l_guid_upper(q.type_guid, type);
            if (!strcmp(type, ESP_TYPE)) {
                n_esp++;
                esp_idx = q.index;
                esp_bytes = (q.last_lba - q.first_lba + 1) * d.bs;
                gk3l_gpt_name_utf8(&d.g, i, en, sizeof en);
            }
        }
        if (n_esp != 1) {
            gk3l_err(T, n_esp ? "esp-ambiguous" : "esp-missing", n_esp ? "this disk has more than one EFI system partition (disk not touched)" : "this disk has no EFI system partition (disk not touched)", 0);
            goto out;
        }
        /* Android 侧的 sepolicy 只给名叫 esp 或 "EFI system partition" 的分区打 ESP 的标签 */
        if (strcmp(en, "esp") && strcmp(en, "EFI system partition"))
            gk3l_log("warning: the EFI system partition is named '%s', not 'esp' or 'EFI system partition'; slot switching and "
                     "OTA will fail on future enforcing builds. Renaming the partition is harmless (UEFI only looks at the type GUID).", en);
        {
            const gk3l_kpart *k = gk3l_kernel_find(d.kp, d.nk, esp_idx);
            char where[4096];
            snprintf(esp_dev, sizeof esp_dev, "/dev/%s", k ? k->name : "");
            /* 别处挂着的 ESP：只读挂不上（同一个超级块不能一处读写一处只读），别人还可能正在往里写 —— 先卸下再装 */
            if (k && !gk3l_mount_of(k->devno, where, sizeof where)) {
                gk3l_err_kv(T, "esp-busy", "mnt", where, "the EFI system partition is mounted; unmount it first (disk not touched)", 0);
                goto out;
            }
        }
        gk3l_blkid(esp_dev, fs, sizeof fs, label, sizeof label);
        if (strcmp(fs, "vfat")) {
            gk3l_err_kv(T, "esp-not-fat", "fs", fs, "the EFI system partition is not FAT (disk not touched)", 0);
            goto out;
        }
    }
    if ((err = gk3l_mount_tmp(esp_dev, "vfat", 1, esp.dir, sizeof esp.dir))) {
        gk3l_err(T, "esp-mount", "cannot mount the EFI system partition; refusing to install a boot chain on an unreadable ESP (disk not touched)", err);
        goto out;
    }
    esp_mounted = 1;
    {
        char pb[4096];
        esp_path(&esp, "EFI/Microsoft/Boot/bootmgfw.efi", pb, sizeof pb);
        esp.windows = gk3l_file_size(pb) >= 0;   /* vfat 查找不分大小写 */
    }
    if (defos_windows && !esp.windows) {
        gk3l_err(T, "default-os-no-windows", "there is no Windows boot manager on the EFI system partition, so Windows cannot be the default (disk not touched)", 0);
        goto out;
    }
    if (!esp_pick_mid(&esp, esp.mid))
        memcpy(esp.mid, mid_fb, 33);
    {
        /* 原件留一份：回落路径按 gk3w_install_backup_decide；loader.conf 只在第一次装时留。
         * 文件在而读不了就在这里拒绝 —— 不能跳过备份再去覆盖它 */
        static const char *const rels[3] = {"EFI/BOOT/BOOTAA64.EFI", "EFI/Microsoft/Boot/bootmgfw.efi", "EFI/BOOT/BOOTAA64.EFI.before-gaokun3"};
        uint8_t *b[3] = {NULL, NULL, NULL};
        size_t bl[3] = {0, 0, 0};
        char pth[4096];
        for (int j = 0; j < 3; j++) {
            esp_path(&esp, rels[j], pth, sizeof pth);
            if (gk3l_exists(pth) && (err = gk3l_read_file(pth, &b[j], &bl[j], 64u << 20))) {
                free(b[0]), free(b[1]), free(b[2]);
                gk3l_err_kv(T, "esp-unreadable", "file", rels[j], "cannot read a file on the EFI system partition that must be checked or backed up first (disk not touched)", err);
                goto out;
            }
        }
        aa_change = !(b[0] && bl[0] == sdboot_len && !memcmp(b[0], sdboot, sdboot_len));
        switch (gk3w_install_backup_decide(b[0], bl[0], b[1], bl[1], b[2], bl[2], sdboot, sdboot_len)) {
        case GK3W_BK_REPLACE:
            gk3l_log("EFI/BOOT/BOOTAA64.EFI now holds another system's boot loader, and its .before-gaokun3 can go without loss: keeping the current file as .before-gaokun3 instead");
            /* fall through */
        case GK3W_BK_SAVE:
            orig[0] = b[0], orig_len[0] = bl[0], b[0] = NULL;
            break;
        case GK3W_BK_CONFLICT:
            free(b[0]), free(b[1]), free(b[2]);
            gk3l_err(T, "fallback-conflict", "EFI/BOOT/BOOTAA64.EFI holds another system's boot loader, and EFI/BOOT/BOOTAA64.EFI.before-gaokun3 holds a different one that is not Windows: installing would lose one of them (disk not touched)", 0);
            goto out;
        case GK3W_BK_NONE:
            break;
        }
        free(b[0]), free(b[1]), free(b[2]);
        esp_path(&esp, "loader/loader.conf", pth, sizeof pth);
        if (esp_size_of(&esp, "loader/loader.conf.before-gaokun3") < 0 && gk3l_exists(pth)) {
            if ((err = gk3l_read_file(pth, &orig[1], &orig_len[1], 64u << 20))) {
                gk3l_err_kv(T, "esp-unreadable", "file", "loader/loader.conf", "cannot read a file on the EFI system partition that must be backed up first (disk not touched)", err);
                goto out;
            }
            if (orig_len[1] == sdboot_len && !memcmp(orig[1], sdboot, sdboot_len)) {
                free(orig[1]);
                orig[1] = NULL;
            }
        }
    }
    {
        struct statvfs sv;
        gk3w_esp_space sp;
        gk3w_esp_check ck;
        int64_t need = 256, aa = esp_size_of(&esp, "EFI/BOOT/BOOTAA64.EFI");
        char relp[96];
        uint64_t sizes[3] = {bi.b.kernel_size, bi.b.dtb_size, bi.b.ramdisk_size};
        static const char *const fn[3] = {"Image", "gaokun3.dtb", "ramdisk.img"};
        need += esp_delta(&esp, "EFI/BOOT/BOOTAA64.EFI", sdboot_len);
        need += esp_delta(&esp, "EFI/systemd/systemd-bootaa64.efi", sdboot_len);
        for (char sl = 'a'; sl <= 'b'; sl++)
            for (int k = 0; k < 3; k++) {
                snprintf(relp, sizeof relp, "%s/android/slot_%c/%s", esp.mid, sl, fn[k]);
                need += esp_delta(&esp, relp, sizes[k]);
            }
        /* 要新写的备份；回落路径写成 .tmp 再改名，改名之前旧文件还占着空间 */
        if (orig[0])
            need += (int64_t)orig_len[0] / 1024 + 4;
        if (aa >= 0 && aa_change)
            need += aa / 1024 + 4;
        if (statvfs(esp.dir, &sv)) {
            gk3l_err(T, "esp-mount", "cannot read the free space of the EFI system partition (disk not touched)", errno);
            goto out;
        }
        sp.esp_size_mib = esp_bytes >> 20;
        sp.free_kib = (int64_t)((uint64_t)sv.f_bavail * sv.f_frsize / 1024);
        sp.need_kib = need;
        sp.slot_kib = (int64_t)((sizes[0] + sizes[1] + sizes[2]) / 1024);
        sp.windows = esp.windows;
        gk3l_log("ESP: using directory %s; %lld MiB to write (net of files overwritten), %lld MiB free; Windows boot manager present: %s",
                 esp.mid, (long long)(need / 1024), (long long)(sp.free_kib / 1024), esp.windows ? "yes" : "no");
        ck = gk3w_esp_space_check(&sp);
        if (ck != GK3W_ESP_OK) {
            gk3l_err(T, gk3w_esp_check_code(ck), "not enough space on the EFI system partition (see the code; disk not touched)", 0);
            goto out;
        }
    }
    gk3l_umount_tmp(esp.dir);
    esp_mounted = 0;
    if (esp.windows) {
        gk3l_win w;
        gk3l_win_state(d.name, &w);
        gk3l_log("Windows volumes: bitlocker=%s hibernated=%s%s%s", w.bitlocker ? "yes" : "no",
                 w.hibernated == 1 ? "yes" : w.hibernated ? "unknown" : "no", w.hib_part[0] ? " part=" : "", w.hib_part);
        if (w.hibernated == 1) {
            gk3l_err_kv(T, "esp-windows-hibernated", "part", w.hib_part,
                        "Windows is hibernated or in Fast Startup: writing the shared EFI system partition now could let Windows corrupt it when it resumes. Shut Windows down with Fast Startup off, then try again (disk not touched)", 0);
            goto out;
        }
        if (w.hibernated < 0)
            gk3l_log("warning: cannot tell whether Windows is hibernated (a BitLocker volume, or an NTFS volume that does not mount); make sure Windows was shut down with Fast Startup off");
        if (w.bitlocker && aa_change && !blconfirmed) {
            gk3l_err(T, "bitlocker-key-unconfirmed",
                     "a Windows volume is BitLocker-encrypted and this install replaces EFI/BOOT/BOOTAA64.EFI: Windows may ask for the 48-digit recovery key next time. Confirm that you have the recovery key (--bitlocker-confirmed) (disk not touched)", 0);
            goto out;
        }
    }

    /* ── 分区名查重、区间核对、方案 ── */
    gk3w_progress(2, "plan");
    {
        char dup[128] = "";
        gk3w_span parts[GK3W_MAX_PARTS], spans[GK3W_MAX_PARTS + 1];
        size_t np = 0, nf = 0;
        int inside = 0;
        gk3w_plan_req req;
        gk3w_plan_err pe;
        for (uint32_t i = 0; i < d.g.num_entries; i++) {
            gk3_gpt_part q;
            char n[GK3_GPT_NAME_CHARS * 4 + 1];
            if (gk3_gpt_get(&d.g, i, &q) != GK3_OK)
                continue;
            if (np < GK3W_MAX_PARTS)
                parts[np++] = (gk3w_span){q.first_lba, q.last_lba};
            gk3l_gpt_name_utf8(&d.g, i, n, sizeof n);
            for (size_t k = 0; k < sizeof OUR_NAMES / sizeof OUR_NAMES[0]; k++)
                if (!strcmp(n, OUR_NAMES[k]) && strlen(dup) + strlen(n) + 2 < sizeof dup) {
                    if (dup[0])
                        strcat(dup, ",");
                    strcat(dup, n);
                }
        }
        if (dup[0]) {
            gk3l_err_kv(T, "partlabel-conflict", "names", dup, "the disk already has partitions with these names; Android would mount one of them at random (disk not touched)", 0);
            goto out;
        }
        if (gk3w_free_spans(d.g.first_usable, d.g.last_usable, parts, np, d.bs, 0, spans, GK3W_MAX_PARTS + 1, &nf) != GK3W_LAYOUT_OK) {
            gk3l_err(T, "disk-layout", "the existing partition table has out-of-range or overlapping entries (disk not touched)", 0);
            goto out;
        }
        for (size_t i = 0; i < nf && i < GK3W_MAX_PARTS + 1; i++)
            if (rstart >= spans[i].first && rend <= spans[i].last && rstart <= rend)
                inside = 1;
        if (!inside) {
            gk3l_err(T, "bad-region", "the selected region is not free space (disk not touched)", 0);
            goto out;
        }
        req.sector = d.bs;
        req.region = (gk3w_span){rstart, rend};
        req.userdata_mib = ud_mib;
        req.rescue = 0;
        pe = gk3w_plan_print(&req, d.path, &plan, stdout);
        if (pe != GK3W_PLAN_OK) {
            gk3l_err_kv(T, "plan", "why", gk3w_plan_strerror(pe), "the selected free space is too small (disk not touched)", 0);
            goto out;
        }
    }

    /* ── 分区表备份、最后一次核对 ── */
    zero = gk3w_aligned_alloc(IO_CHUNK);
    chk = gk3w_aligned_alloc(IO_CHUNK);
    if (!zero || !chk) {
        gk3l_err(T, "nomem", "out of memory", ENOMEM);
        goto out;
    }
    memset(zero, 0, IO_CHUNK);
    if (gk3l_disk_read_table(&d) || strcmp(d.table, "gpt") || memcmp(snap, d.hdr, d.bs) || memcmp(snap + d.bs, d.ent, snap_len) ||
        !gk3l_disk_kernel_ok(&d)) {
        gk3l_err(T, "layout-changed", "the partition table changed while preparing the install; run the installer again (disk not touched)", 0);
        goto out;
    }
    if (gpt_backup(&d, backup_dir))
        goto out;

    /* ══ 从这里起开始动盘 ══ */
    T = GK3W_TOUCHED_YES;
    gk3w_progress(5, "write-gpt");
    {
        gk3w_gpt_new np[GK3W_PLAN_MAX];
        uint8_t *bak = gk3w_aligned_alloc(d.bs < 4096 ? 4096 : d.bs);
        uint64_t bel = 0, tb = (snap_len + d.bs - 1) / d.bs;
        gk3w_gptw_err ge;
        int w;
        if (!bak) {
            gk3l_err(GK3W_TOUCHED_NO, "nomem", "out of memory (disk not touched)", ENOMEM);
            goto out;
        }
        for (size_t i = 0; i < plan.n; i++) {
            memset(&np[i], 0, sizeof np[i]);
            np[i].name = plan.parts[i].name;
            gk3w_guid_parse(LINUX_DATA, np[i].type);
            if (getrandom(np[i].uuid, 16, 0) != 16) {
                gk3w_aligned_free(bak);
                gk3l_err(GK3W_TOUCHED_NO, "uuid", "cannot generate a partition GUID (disk not touched)", errno);
                goto out;
            }
            gk3w_guid_v4(np[i].uuid);
            np[i].first = plan.parts[i].first;
            np[i].last = plan.parts[i].last;
        }
        ge = gk3w_gpt_add(d.hdr, d.bs, d.ent, (size_t)(tb * d.bs), np, plan.n, idx);
        if (ge == GK3W_GPTW_OK)
            ge = gk3w_gpt_make_backup(d.hdr, d.bs, bak, &bel);
        if (ge != GK3W_GPTW_OK) {
            gk3w_aligned_free(bak);
            gk3l_err_kv(GK3W_TOUCHED_NO, ge == GK3W_GPTW_FULL ? "gpt-full" : "gpt-layout", "why", gk3w_gptw_strerror(ge),
                        ge == GK3W_GPTW_FULL ? "the partition table is full (disk not touched)" : "cannot add the partitions to the partition table (disk not touched)", 0);
            goto out;
        }
        /* 主表项 → 主表头 → 备份表项 → 备份表头：每一份表头只在它的表项写完之后才写，任何时刻断电，
         * 两份表要么是旧的、要么是新的、要么表头与表项的 CRC 对不上（固件与内核会改用另一份） */
        w = gk3l_write_verify(&d, d.ent, (size_t)(tb * d.bs), d.g.entries_lba * d.bs, chk);
        if (!w)
            w = gk3l_write_verify(&d, d.hdr, d.bs, 1 * (uint64_t)d.bs, chk);
        if (!w)
            w = gk3l_write_verify(&d, d.ent, (size_t)(tb * d.bs), bel * d.bs, chk);
        if (!w)
            w = gk3l_write_verify(&d, bak, d.bs, d.g.alt_lba * d.bs, chk);
        err = errno;
        gk3w_aligned_free(bak);
        if (w) {
            gk3l_err_kv(T, w == -1 ? "disk-write" : "disk-verify", "part", "gpt", w == -1 ? "writing the partition table failed" : "the partition table did not read back identically", err);
            goto out;
        }
        if (gk3l_disk_read_table(&d) || strcmp(d.table, "gpt")) {
            gk3l_err(T, "verify-layout", "the partition table does not parse after writing it", 0);
            goto out;
        }
        for (size_t i = 0; i < plan.n; i++) {
            gk3_gpt_part q;
            if (gk3_gpt_get(&d.g, idx[i] - 1, &q) != GK3_OK || q.first_lba != plan.parts[i].first || q.last_lba != plan.parts[i].last) {
                gk3l_err_kv(T, "verify-layout", "name", plan.parts[i].name, "after writing the partition table, a new partition does not match the plan", 0);
                goto out;
            }
            poff[i] = q.first_lba * d.bs;
            if ((err = kernel_add(&d, idx[i], q.first_lba, q.last_lba))) {
                gk3l_err_kv(T, "kernel-add", "name", plan.parts[i].name, "the kernel refused the new partition (BLKPG)", err);
                goto out;
            }
        }
        for (size_t i = 0; i < plan.n; i++)
            if (wait_node(&d, idx[i], nodes[i], sizeof nodes[i])) {
                gk3l_err_kv(T, "node-missing", "name", plan.parts[i].name, "the device node of a new partition did not appear", 0);
                goto out;
            }
        gk3l_log("partition table written (primary and backup), the kernel knows the %zu new partitions", plan.n);
    }

    {
        int i_misc = part_index_by_name(&plan, "misc"), i_meta = part_index_by_name(&plan, "metadata"),
            i_boota = part_index_by_name(&plan, "boot_a"), i_bootb = part_index_by_name(&plan, "boot_b"),
            i_super = part_index_by_name(&plan, "super"), i_data = part_index_by_name(&plan, "userdata");
        if (i_misc < 0 || i_meta < 0 || i_boota < 0 || i_bootb < 0 || i_super < 0 || i_data < 0) {
            gk3l_err(T, "plan", "internal error: the plan lacks a partition", 0);
            goto out;
        }

        /* ── 15 format：misc 清零 + 初始状态；metadata / userdata 格式化 ── */
        gk3w_progress(15, "format");
        for (uint64_t off = 0; off < (uint64_t)GK3W_MISC_MIB << 20; off += IO_CHUNK) {
            int w = gk3l_write_verify(&d, zero, IO_CHUNK, poff[i_misc] + off, chk);
            if (w) {
                gk3l_err_kv(T, w == -1 ? "disk-write" : "disk-verify", "part", "misc", w == -1 ? "zeroing failed" : "zeroed area did not read back as zero", errno);
                goto out;
            }
        }
        {
            uint8_t head[GK3_MISC_INIT_HEAD_SIZE], recb[GK3_REC_SIZE];
            gk3_setdef def = esp.windows ? (defos_windows ? GK3_SETDEF_WINDOWS : GK3_SETDEF_ANDROID) : GK3_SETDEF_NONE;
            uint8_t *m = zero;   /* 64 KiB：BCB + BCAB 在 0，GK3 记录在 8 KiB，其余是零 */
            char hex[GK3_MISC_BCAB_SIZE * 2 + 1];
            int w;
            if (gk3_misc_build_install(head, recb, 0, def)) {
                gk3l_err(T, "misc-init", "internal error: the misc initial state failed its self-check", 0);
                goto out;
            }
            memcpy(m, head, sizeof head);
            memcpy(m + GK3_MISC_GK3_OFF, recb, sizeof recb);
            w = gk3l_write_verify(&d, m, GK3_MISC_READ_SIZE, poff[i_misc], chk);
            if (w) {
                gk3l_err(T, "misc-init", "writing the misc initial state failed", errno);
                goto out;
            }
            for (unsigned k = 0; k < GK3_MISC_BCAB_SIZE; k++)
                snprintf(hex + 2 * k, 3, "%02x", head[GK3_MISC_BCAB_OFF + k]);
            gk3l_log("misc: MISCINIT slot=a default=%s bcab=%s", def == GK3_SETDEF_WINDOWS ? "windows" : def == GK3_SETDEF_ANDROID ? "android" : "none", hex);
            memset(zero, 0, IO_CHUNK);
        }
        {
            /* 参数同 installer-lib.sh：userdata 加 -m 0（Android 的应用都不是 uid 0，5% 的保留块谁也用不上；
             * 系统的保留空间由 fstab 的 reservedsize= 交给 fs_mgr 管） */
            struct {
                int i;
                const char *label;
                int no_reserve;
            } fs[2] = {{i_meta, "metadata", 0}, {i_data, "userdata", 1}};
            for (int k = 0; k < 2; k++) {
                char *av[10];
                int a = 0, r;
                av[a++] = "mkfs.ext4";
                av[a++] = "-q";
                av[a++] = "-F";
                if (fs[k].no_reserve) {
                    av[a++] = "-m";
                    av[a++] = "0";
                }
                av[a++] = "-L";
                av[a++] = (char *)fs[k].label;
                av[a++] = nodes[fs[k].i];
                av[a] = NULL;
                r = gk3l_run(av, NULL, 0);
                if (r || check_ext4(&d, poff[fs[k].i], fs[k].label, zero)) {
                    gk3l_err_kv(T, "mkfs", "part", fs[k].label, r ? "mkfs.ext4 failed" : "after formatting, the partition does not start with the expected ext4 superblock", 0);
                    goto out;
                }
            }
            memset(zero, 0, IO_CHUNK);
            gk3l_log("metadata, userdata: ext4 created (superblock magic and label read back from the disk)");
        }

        /* ── 30 write-super ── */
        gk3w_progress(30, "write-super");
        {
            disk_sink ss = {&d, poff[i_super], chk, gk3w_aligned_alloc(IO_CHUNK), 0};
            gk3w_sparse_sink sink = {&ss, ds_write, ds_fill};
            if (!ss.pat) {
                gk3l_err(T, "nomem", "out of memory", ENOMEM);
                goto out;
            }
            if (run_image(p_super, zst, (uint64_t)GK3W_SUPER_MIB << 20, &sink, 1, &why)) {
                gk3w_aligned_free(ss.pat);
                gk3l_err_kv(T, "super-write", "why", why, ss.failed == -2 ? "super did not read back identically" : "writing super failed", errno);
                goto out;
            }
            gk3w_aligned_free(ss.pat);
            /* 偏移 4096 处必须是 LP geometry 魔数（installer-lib.sh gk3__write_super 的同一道检查） */
            if (gk3l_pread(&d, zero, 4096, poff[i_super] + 4096) || memcmp(zero, "gDla", 4)) {
                gk3l_err(T, "super-bad-lp", "no LP geometry magic at offset 4096 of super: what was written is not a usable super", 0);
                goto out;
            }
            gk3l_log("super: LP geometry magic correct");
            memset(zero, 0, IO_CHUNK);
        }

        /* ── 70 write-boot ── */
        gk3w_progress(70, "write-boot");
        {
            size_t padded = (bi.len + d.bs - 1) / d.bs * d.bs;
            uint8_t *c2 = gk3w_aligned_alloc(padded);
            int targets[2] = {i_boota, i_bootb};
            bootbuf = gk3w_aligned_alloc(padded);
            if (!bootbuf || !c2) {
                gk3w_aligned_free(c2);
                gk3l_err(T, "nomem", "out of memory", ENOMEM);
                goto out;
            }
            memset(bootbuf, 0, padded);
            memcpy(bootbuf, bi.img, bi.len);
            for (int k = 0; k < 2; k++) {
                int w = gk3l_write_verify(&d, bootbuf, padded, poff[targets[k]], c2);
                if (w) {
                    gk3w_aligned_free(c2);
                    gk3l_err_kv(T, w == -1 ? "disk-write" : "disk-verify", "part", plan.parts[targets[k]].name,
                                w == -1 ? "writing boot failed" : "boot did not read back identically", errno);
                    goto out;
                }
            }
            gk3w_aligned_free(c2);
        }
        for (size_t i = 0; i < plan.n; i++)
            flush_node(nodes[i]);
    }

    /* ── 80 bootloader：ESP ── */
    gk3w_progress(80, "bootloader");
    if ((err = gk3l_mount_tmp(esp_dev, "vfat", 0, esp.dir, sizeof esp.dir))) {
        gk3l_err(T, "esp-mount", "cannot mount the EFI system partition for writing", err);
        goto out;
    }
    esp_mounted = 1;
    {
        static const char *const fn[3] = {"Image", "gaokun3.dtb", "ramdisk.img"};
        const uint8_t *src[3] = {bi.img + bi.b.kernel_off, bi.img + bi.b.dtb_off, bi.img + bi.b.ramdisk_off};
        size_t sz[3] = {bi.b.kernel_size, bi.b.dtb_size, bi.b.ramdisk_size};
        static const char *const backups[2] = {"EFI/BOOT/BOOTAA64.EFI", "loader/loader.conf"};
        static char entry[2][GK3_BOOT_ARGS_SIZE + GK3_BOOT_EXTRA_ARGS_SIZE + 512];
        char relp[160];
        if (esp_cleanup(&esp))
            goto out;
        for (int k = 0; k < 2; k++) {
            if (!orig[k])
                continue;
            snprintf(relp, sizeof relp, "%s.before-gaokun3", backups[k]);
            if (esp_put(&elog, relp, orig[k], orig_len[k], 1))
                goto out;
            gk3l_log("backed up the original %s as %s", backups[k], relp);
        }
        /* 只在字节不同时写（BitLocker 量的是固件每次加载的这个文件）。回落路径写成 .tmp 再改名：中途断电时它要么还是
         * 原来的文件、要么是完整的 systemd-boot */
        for (int k = 0; k < 2; k++) {
            const char *t = k ? "EFI/systemd/systemd-bootaa64.efi" : "EFI/BOOT/BOOTAA64.EFI";
            if (esp_same(&esp, t, sdboot, sdboot_len))
                gk3l_log("%s is already this systemd-boot, not rewritten", t);
            else if (esp_put(&elog, t, sdboot, sdboot_len, !k))
                goto out;
        }
        for (char sl = 'a'; sl <= 'b'; sl++) {
            int n;
            for (int k = 0; k < 3; k++) {
                snprintf(relp, sizeof relp, "%s/android/slot_%c/%s", esp.mid, sl, fn[k]);
                if (esp_put(&elog, relp, src[k], sz[k], 0))
                    goto out;
            }
            snprintf(relp, sizeof relp, "loader/entries/%s-android-%c.conf", esp.mid, sl);
            n = gk3w_esp_android_entry(entry[sl - 'a'], sizeof entry[0], esp.mid, sl, bi.cmdline);
            if (n < 0) {
                gk3l_err(T, "esp-write", "internal error: cannot generate the boot entry", 0);
                goto out;
            }
            if (esp_put(&elog, relp, (const uint8_t *)entry[sl - 'a'], (size_t)n, 0))
                goto out;
        }
        if (esp_put(&elog, "loader/loader.conf", (const uint8_t *)gk3w_loader_conf, strlen(gk3w_loader_conf), 0))
            goto out;
        sync();
        if ((err = gk3l_umount_tmp(esp.dir))) {
            gk3l_err(T, "esp-umount", "cannot unmount the EFI system partition; what was written may not be on the disk", err);
            goto out;
        }
        esp_mounted = 0;
        /* 从介质读回核对：丢掉这块设备的缓存，只读挂回去逐个比 */
        flush_node(esp_dev);
        if ((err = gk3l_mount_tmp(esp_dev, "vfat", 1, esp.dir, sizeof esp.dir))) {
            gk3l_err(T, "verify-remount", "cannot mount the EFI system partition again to verify it", err);
            goto out;
        }
        esp_mounted = 1;
        for (size_t i = 0; i < elog.n; i++)
            if (!esp_same(&esp, elog.w[i].rel, elog.w[i].data, elog.w[i].len)) {
                gk3l_err_kv(T, "esp-verify", "file", elog.w[i].rel, "a file on the EFI system partition did not read back identically", 0);
                goto out;
            }
        gk3l_umount_tmp(esp.dir);
        esp_mounted = 0;
        gk3l_log("ESP: systemd-boot, kernel / dtb / ramdisk for both slots, both entries and loader.conf written; %zu files read back from the disk and verified", elog.n);
    }

    /* LoaderEntryDefault 优先于 loader.conf 的 default：装完还在的话，第一次重启进的就不是新装的 Android。
     * 不管值是什么都删（默认系统由统一启动入口按 misc 里的请求去写）；删不掉只提醒：盘上的东西都已写好 */
    {
        char val[256];
        int r = gk3l_loader_var("LoaderEntryDefault", val, sizeof val);
        if (r == 2) {
            gk3l_log("cannot read EFI variables (efivarfs unavailable); LoaderEntryDefault not checked");
        } else if (r == 0) {
            if (gk3l_loader_var_delete("LoaderEntryDefault") == 0) {
                gk3l_log("deleted the EFI variable LoaderEntryDefault (was '%s')", val);
            } else {
                gk3w_line l = {0};
                gk3l_log("warning: the EFI variable LoaderEntryDefault='%s' cannot be deleted", val);
                gk3w_line_begin(&l, "NOTE");
                gk3w_line_kv(&l, "code", "loadervar-stuck");
                gk3w_line_kv(&l, "name", "LoaderEntryDefault");
                gk3w_line_kv(&l, "value", val);
                (void)gk3w_line_emit(&l, stdout);
                gk3w_line_free(&l);
            }
        }
    }
    gk3w_progress(100, "done");
    rc = 0;
out:
    if (esp_mounted)
        gk3l_umount_tmp(esp.dir);
    if (opened)
        gk3l_disk_close(&d);
    free(orig[0]);
    free(orig[1]);
    free(snap);
    free(bi.img);
    free(sdboot);
    free(man);
    gk3w_aligned_free(zero);
    gk3w_aligned_free(chk);
    gk3w_aligned_free(bootbuf);
    return rc;
}
