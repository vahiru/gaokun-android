/* Linux：安装 Android，LiveCD 用。三种方式：
 *
 *   gk3l apply --disk /dev/X --release DIR [--mode alongside] --region-start LBA --region-end LBA   双系统（默认）
 *   gk3l apply --disk /dev/X --release DIR --mode wipe                                             整盘清空
 *   gk3l apply --disk /dev/X --release DIR --mode reinstall [--keep-data yes|no]                   重新安装
 *   共同的可选参数：[--sdboot FILE] [--backup-dir DIR] [--machine-id HEX32] [--default-os android|windows]
 *                   [--userdata-mib N] [--bitlocker-confirmed]
 *
 * 步骤与进度代码同 Windows 版 gk3w apply：1 check / verify-sha / test-zst → 2 plan → 5 write-gpt → 15 format →
 * 30 write-super → 70 write-boot → 80 bootloader → 100 done（重新安装没有 write-gpt）。★ 所有输入在第一次写盘之前验完
 * （boot.img 拆得开、super 完整试解一遍、ESP 可读且空间够、Windows 没在休眠、区间确实空闲、分区名不重、要写的分区
 * 不在用），之后 ERR 一律带 touched=yes。
 *
 * 发布目录：boot.img、super.img.zst（或 sparse 格式的 super.img），可选 install-artifacts.sha256（有就逐个核对）。
 * systemd-boot：--sdboot，否则 GK3_SDBOOT，否则依次找 /usr/share/gaokun3/、发布目录、/usr/lib/systemd/boot/efi/ 下的
 * systemd-bootaa64.efi。
 *
 * 三种方式的差别（规则同 installer-lib.sh 的 gk3_apply）：
 *   - 双系统：只往用户选的空闲区里加分区，用盘上现有的 ESP；分区名不能与盘上已有的重。
 *   - 整盘清空：换一张新的 GPT（最前面 300 MiB 的 ESP，格式化为 FAT32），安装介质在这块盘上、或者盘上有分区在用
 *     （挂着、交换分区、上面叠着别的设备）时拒绝。内核里的旧分区先逐个去掉（去不掉就加回来、不动盘）。
 *     EFI 变量 LoaderEntryOneShot 也一并删掉。
 *   - 重新安装：分区表不动，按名字认出现有的那一套（每个名字恰好一个、大小够这一版），super / boot / misc 重写，
 *     metadata / userdata 格式化（--keep-data yes 时保留）；要写的分区在用时拒绝。
 *
 * 与 Windows 版的差别：
 *   - 分区表自己写（../gptw.c：主表、备份表各自 CRC），再用 BLKPG 逐个告诉内核新分区 —— 不重读整张表，ESP 等分区挂着
 *     也不受影响。双系统要求动手之前内核的分区视图与盘上的 GPT 一致（gk3l probe 的 kernel=ok）。
 *   - 写盘期间对整块盘持 BSD 排他锁：systemd-udevd 见到锁就不去探测这块盘（systemd 的块设备锁约定），
 *     新分区在写完之前不会被 udev 读进缓存。
 *   - metadata / userdata 用 Debian 的 mkfs.ext4 直接格式化（参数同 installer-lib.sh），写完核对超级块的魔数与卷标。
 *   - ESP 挂载后写文件，写完卸下、丢掉这块设备的缓存再只读挂回去逐个比对：读回的是介质上的内容，不是页缓存。
 *     ESP 在别处挂着时拒绝（esp-busy）。
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


enum { M_ALONGSIDE, M_WIPE, M_REINSTALL };

/* 要写的分区：misc metadata boot_a boot_b super userdata esp（esp 只在整盘清空时由我们建） */
enum { T_MISC, T_META, T_BOOTA, T_BOOTB, T_SUPER, T_DATA, T_ESP, T_N };
static const char *const T_NAMES[T_N] = {"misc", "metadata", "boot_a", "boot_b", "super", "userdata", "esp"};

typedef struct {
    int mode, keep, defos_windows, blconfirmed;
    const char *rel, *backup_dir;
    uint64_t rstart, rend, ud_mib;
    char mid_fb[33];
    /* 输入 */
    char p_boot[4096], p_super[4096];
    int zst;
    uint8_t *sdboot, *man;
    size_t sdboot_len, man_len;
    gk3w_boot bi;
    /* 盘 */
    gk3l_disk d;
    int opened;
    uint8_t *snap;
    size_t snap_len;
    /* ESP */
    char esp_dev[96];
    uint64_t esp_bytes;
    esp_ctx esp;
    int esp_mounted, aa_change;
    uint8_t *orig[2];
    size_t orig_len[2];
    /* 方案与目标 */
    gk3w_plan plan;
    gk3w_reinstall ri;
    uint8_t *nhdr, *nent, *npmbr;   /* 整盘清空：新表（动盘之前在内存里建好） */
    struct {
        char node[96];
        uint64_t off, bytes;
        uint32_t idx;
    } t[T_N];
    uint8_t *zero, *chk, *bootbuf;
    gk3w_touched T;
} apply_ctx;

/* ── 输入：全部在动盘之前验完 ── */
static int check_inputs(apply_ctx *c, const char *sdboot_arg)
{
    char p_sha[4096], sdboot_path[4096];
    const char *why = NULL;
    int err;
    gk3w_progress(1, "check");
    snprintf(c->p_boot, sizeof c->p_boot, "%s/boot.img", c->rel);
    snprintf(c->p_super, sizeof c->p_super, "%s/super.img.zst", c->rel);
    snprintf(p_sha, sizeof p_sha, "%s/install-artifacts.sha256", c->rel);
    c->zst = 1;
    if (gk3l_file_size(c->p_super) < 0) {
        snprintf(c->p_super, sizeof c->p_super, "%s/super.img", c->rel);
        c->zst = 0;
        if (gk3l_file_size(c->p_super) < 0)
            return gk3l_err(c->T, "release-no-super", "the release directory has neither super.img.zst nor super.img", 0), -1;
    }
    if (find_sdboot(sdboot_arg, c->rel, sdboot_path, sizeof sdboot_path) ||
        gk3l_read_file(sdboot_path, &c->sdboot, &c->sdboot_len, 16u << 20) || c->sdboot_len < 2 || memcmp(c->sdboot, "MZ", 2))
        return gk3l_err(c->T, "sdboot-missing", "systemd-boot (systemd-bootaa64.efi) is missing, unreadable or not a PE file", 0), -1;
    if (!have_tool("mkfs.ext4"))
        return gk3l_err_kv(c->T, "tool-missing", "tool", "mkfs.ext4", "mkfs.ext4 is missing", 0), -1;
    if (c->mode == M_WIPE && !have_tool("mkfs.vfat"))
        return gk3l_err_kv(c->T, "tool-missing", "tool", "mkfs.vfat", "mkfs.vfat is missing", 0), -1;
    if (gk3l_read_file(p_sha, &c->man, &c->man_len, 1u << 20) == 0) {
        const char *names[2] = {"boot.img", c->zst ? "super.img.zst" : "super.img"};
        const char *paths[2] = {c->p_boot, c->p_super};
        gk3w_progress(1, "verify-sha");
        for (int i = 0; i < 2; i++) {
            char want[65], got[65];
            if (gk3w_manifest_hash(c->man, c->man_len, names[i], want))
                continue;
            if (sha256_file(paths[i], got) || strcmp(want, got))
                return gk3l_err_kv(c->T, "sha-mismatch", "file", names[i], "a file does not match the sha256 in the release manifest (disk not touched)", 0), -1;
            gk3l_log("%s: sha256 matches the release manifest", names[i]);
        }
    } else {
        gk3l_log("no install-artifacts.sha256 in the release directory, skipping the sha256 check");
    }
    if ((err = gk3l_read_file(c->p_boot, &c->bi.img, &c->bi.len, GK3W_BOOT_MAX)))
        return gk3l_err(c->T, "release-no-boot", "the release directory has no readable boot.img (or it is larger than 64 MiB)", err), -1;
    {
        gk3w_boot_err be = gk3w_boot_check(&c->bi);
        if (be != GK3W_BOOT_OK)
            return gk3l_err(c->T, "bootimg-unpack", gk3w_boot_strerror(be), 0), -1;
        gk3l_log("  Image       %10u bytes", c->bi.b.kernel_size);
        gk3l_log("  ramdisk.img %10u bytes", c->bi.b.ramdisk_size);
        gk3l_log("  gaokun3.dtb %10u bytes", c->bi.b.dtb_size);
    }
    gk3w_progress(1, "test-zst");
    if (run_image(c->p_super, c->zst, (uint64_t)GK3W_SUPER_MIB << 20, NULL, 0, &why))
        return gk3l_err_kv(c->T, "zst-corrupt", "why", why, "the super image is incomplete or not a valid sparse image (disk not touched)", 0), -1;
    gk3l_log("%s: full test decode passed", c->zst ? "super.img.zst" : "super.img");
    return 0;
}

/* ── 盘：打开、加锁、读分区表 ── */
static int open_disk(apply_ctx *c, const char *disk_arg)
{
    char dname[64];
    int err;
    if (gk3l_whole_disk_name(disk_arg, dname, sizeof dname))
        return gk3l_err_kv(c->T, "disk-open", "disk", disk_arg, "--disk is not a whole disk", 0), -1;
    err = gk3l_disk_open(&c->d, dname, 1);
    if (err)
        return gk3l_err_kv(c->T, err == EACCES || err == EPERM ? "not-root" : "disk-open", "disk", disk_arg, "cannot open the disk for writing", err), -1;
    c->opened = 1;
    if (c->d.bs != 512 && c->d.bs != 4096)
        return gk3l_err(c->T, "disk-sector", "the disk sector size is neither 512 nor 4096 bytes", 0), -1;
    if (gk3l_disk_lock(&c->d))
        return gk3l_err(c->T, "disk-busy", "another program holds the disk (udev or another installer); try again (disk not touched)", errno), -1;
    if (gk3l_disk_read_table(&c->d))
        return gk3l_err(c->T, "nomem", "out of memory", ENOMEM), -1;
    if (!strcmp(c->d.table, "unreadable"))
        return gk3l_err_kv(c->T, "disk-unreadable", "disk", c->d.path, "cannot read the partition table (disk not touched)", 0), -1;
    if (c->mode == M_WIPE)
        return 0;   /* 整盘清空：MBR、没有分区表、表坏了都行，本来就要换掉 */
    if (!strcmp(c->d.table, "mbr"))
        return gk3l_err_kv(c->T, "mbr-disk", "disk", c->d.path, "the disk has an MBR partition table; only a whole-disk wipe can use it (disk not touched)", 0), -1;
    if (strcmp(c->d.table, "gpt"))
        return gk3l_err_kv(c->T, "disk-not-gpt", "table", c->d.table, "the disk has no usable GPT partition table; only a whole-disk wipe can use it (disk not touched)", 0), -1;
    if (!gk3l_disk_kernel_ok(&c->d))
        return gk3l_err(c->T, "kernel-stale", "the kernel's view of the partitions differs from the partition table on disk; reboot and try again (disk not touched)", 0), -1;
    c->snap_len = (size_t)c->d.g.num_entries * c->d.g.entry_size;
    c->snap = malloc(c->d.bs + c->snap_len);
    if (!c->snap)
        return gk3l_err(c->T, "nomem", "out of memory", ENOMEM), -1;
    memcpy(c->snap, c->d.hdr, c->d.bs);
    memcpy(c->snap + c->d.bs, c->d.ent, c->snap_len);
    return 0;
}

/* ── ESP：恰好一个、FAT、没在别处挂着；只读挂上做全部检查（双系统与重新安装） ── */
static int esp_check(apply_ctx *c)
{
    gk3l_disk *d = &c->d;
    int n_esp = 0, err;
    uint32_t esp_idx = 0;
    char en[GK3_GPT_NAME_CHARS * 4 + 1] = "", fs[64], label[256];
    for (uint32_t i = 0; i < d->g.num_entries; i++) {
        gk3_gpt_part q;
        char type[37];
        if (gk3_gpt_get(&d->g, i, &q) != GK3_OK)
            continue;
        gk3l_guid_upper(q.type_guid, type);
        if (!strcmp(type, ESP_TYPE)) {
            n_esp++;
            esp_idx = q.index;
            c->esp_bytes = (q.last_lba - q.first_lba + 1) * d->bs;
            gk3l_gpt_name_utf8(&d->g, i, en, sizeof en);
        }
    }
    if (n_esp != 1)
        return gk3l_err(c->T, n_esp ? "esp-ambiguous" : "esp-missing", n_esp ? "this disk has more than one EFI system partition (disk not touched)" : "this disk has no EFI system partition (disk not touched)", 0), -1;
    /* Android 侧的 sepolicy 只给名叫 esp 或 "EFI system partition" 的分区打 ESP 的标签 */
    if (strcmp(en, "esp") && strcmp(en, "EFI system partition"))
        gk3l_log("warning: the EFI system partition is named '%s', not 'esp' or 'EFI system partition'; slot switching and "
                 "OTA will fail on future enforcing builds. Renaming the partition is harmless (UEFI only looks at the type GUID).", en);
    {
        const gk3l_kpart *k = gk3l_kernel_find(d->kp, d->nk, esp_idx);
        char where[4096];
        snprintf(c->esp_dev, sizeof c->esp_dev, "/dev/%s", k ? k->name : "");
        /* 别处挂着的 ESP：只读挂不上（同一个超级块不能一处读写一处只读），别人还可能正在往里写 —— 先卸下再装 */
        if (k && !gk3l_mount_of(k->devno, where, sizeof where))
            return gk3l_err_kv(c->T, "esp-busy", "mnt", where, "the EFI system partition is mounted; unmount it first (disk not touched)", 0), -1;
    }
    gk3l_blkid(c->esp_dev, fs, sizeof fs, label, sizeof label);
    if (strcmp(fs, "vfat"))
        return gk3l_err_kv(c->T, "esp-not-fat", "fs", fs, "the EFI system partition is not FAT (disk not touched)", 0), -1;
    if ((err = gk3l_mount_tmp(c->esp_dev, "vfat", 1, c->esp.dir, sizeof c->esp.dir)))
        return gk3l_err(c->T, "esp-mount", "cannot mount the EFI system partition; refusing to install a boot chain on an unreadable ESP (disk not touched)", err), -1;
    c->esp_mounted = 1;
    {
        char pb[4096];
        esp_path(&c->esp, "EFI/Microsoft/Boot/bootmgfw.efi", pb, sizeof pb);
        c->esp.windows = gk3l_file_size(pb) >= 0;   /* vfat 查找不分大小写 */
    }
    if (c->defos_windows && !c->esp.windows)
        return gk3l_err(c->T, "default-os-no-windows", "there is no Windows boot manager on the EFI system partition, so Windows cannot be the default (disk not touched)", 0), -1;
    if (!esp_pick_mid(&c->esp, c->esp.mid))
        memcpy(c->esp.mid, c->mid_fb, 33);
    {
        /* 原件留一份：回落路径按 gk3w_install_backup_decide；loader.conf 只在第一次装时留。
         * 文件在而读不了就在这里拒绝 —— 不能跳过备份再去覆盖它 */
        static const char *const rels[3] = {"EFI/BOOT/BOOTAA64.EFI", "EFI/Microsoft/Boot/bootmgfw.efi", "EFI/BOOT/BOOTAA64.EFI.before-gaokun3"};
        uint8_t *b[3] = {NULL, NULL, NULL};
        size_t bl[3] = {0, 0, 0};
        char pth[4096];
        for (int j = 0; j < 3; j++) {
            esp_path(&c->esp, rels[j], pth, sizeof pth);
            if (gk3l_exists(pth) && (err = gk3l_read_file(pth, &b[j], &bl[j], 64u << 20))) {
                free(b[0]), free(b[1]), free(b[2]);
                return gk3l_err_kv(c->T, "esp-unreadable", "file", rels[j], "cannot read a file on the EFI system partition that must be checked or backed up first (disk not touched)", err), -1;
            }
        }
        c->aa_change = !(b[0] && bl[0] == c->sdboot_len && !memcmp(b[0], c->sdboot, c->sdboot_len));
        switch (gk3w_install_backup_decide(b[0], bl[0], b[1], bl[1], b[2], bl[2], c->sdboot, c->sdboot_len)) {
        case GK3W_BK_REPLACE:
            gk3l_log("EFI/BOOT/BOOTAA64.EFI now holds another system's boot loader, and its .before-gaokun3 can go without loss: keeping the current file as .before-gaokun3 instead");
            /* fall through */
        case GK3W_BK_SAVE:
            c->orig[0] = b[0], c->orig_len[0] = bl[0], b[0] = NULL;
            break;
        case GK3W_BK_CONFLICT:
            free(b[0]), free(b[1]), free(b[2]);
            return gk3l_err(c->T, "fallback-conflict", "EFI/BOOT/BOOTAA64.EFI holds another system's boot loader, and EFI/BOOT/BOOTAA64.EFI.before-gaokun3 holds a different one that is not Windows: installing would lose one of them (disk not touched)", 0), -1;
        case GK3W_BK_NONE:
            break;
        }
        free(b[0]), free(b[1]), free(b[2]);
        esp_path(&c->esp, "loader/loader.conf", pth, sizeof pth);
        if (esp_size_of(&c->esp, "loader/loader.conf.before-gaokun3") < 0 && gk3l_exists(pth)) {
            if ((err = gk3l_read_file(pth, &c->orig[1], &c->orig_len[1], 64u << 20)))
                return gk3l_err_kv(c->T, "esp-unreadable", "file", "loader/loader.conf", "cannot read a file on the EFI system partition that must be backed up first (disk not touched)", err), -1;
            if (c->orig_len[1] == c->sdboot_len && !memcmp(c->orig[1], c->sdboot, c->sdboot_len)) {
                free(c->orig[1]);
                c->orig[1] = NULL;
            }
        }
    }
    {
        struct statvfs sv;
        gk3w_esp_space sp;
        gk3w_esp_check ck;
        int64_t need = 256, aa = esp_size_of(&c->esp, "EFI/BOOT/BOOTAA64.EFI");
        char relp[96];
        uint64_t sizes[3] = {c->bi.b.kernel_size, c->bi.b.dtb_size, c->bi.b.ramdisk_size};
        static const char *const fn[3] = {"Image", "gaokun3.dtb", "ramdisk.img"};
        need += esp_delta(&c->esp, "EFI/BOOT/BOOTAA64.EFI", c->sdboot_len);
        need += esp_delta(&c->esp, "EFI/systemd/systemd-bootaa64.efi", c->sdboot_len);
        for (char sl = 'a'; sl <= 'b'; sl++)
            for (int k = 0; k < 3; k++) {
                snprintf(relp, sizeof relp, "%s/android/slot_%c/%s", c->esp.mid, sl, fn[k]);
                need += esp_delta(&c->esp, relp, sizes[k]);
            }
        /* 要新写的备份；回落路径写成 .tmp 再改名，改名之前旧文件还占着空间 */
        if (c->orig[0])
            need += (int64_t)c->orig_len[0] / 1024 + 4;
        if (aa >= 0 && c->aa_change)
            need += aa / 1024 + 4;
        if (statvfs(c->esp.dir, &sv))
            return gk3l_err(c->T, "esp-mount", "cannot read the free space of the EFI system partition (disk not touched)", errno), -1;
        sp.esp_size_mib = c->esp_bytes >> 20;
        sp.free_kib = (int64_t)((uint64_t)sv.f_bavail * sv.f_frsize / 1024);
        sp.need_kib = need;
        sp.slot_kib = (int64_t)((sizes[0] + sizes[1] + sizes[2]) / 1024);
        sp.windows = c->esp.windows;
        sp.reinstall = c->mode == M_REINSTALL;
        gk3l_log("ESP: using directory %s; %lld MiB to write (net of files overwritten), %lld MiB free; Windows boot manager present: %s",
                 c->esp.mid, (long long)(need / 1024), (long long)(sp.free_kib / 1024), c->esp.windows ? "yes" : "no");
        ck = gk3w_esp_space_check(&sp);
        if (ck != GK3W_ESP_OK) {
            /* 字段与 installer-lib.sh 相同：界面按它们写出差多少 */
            gk3w_line l = {0};
            gk3w_line_begin(&l, "ERR");
            gk3w_line_kv(&l, "code", gk3w_esp_check_code(ck));
            switch (ck) {
            case GK3W_ESP_TOO_SMALL:
                gk3w_line_u64(&l, "size_mib", sp.esp_size_mib);
                gk3w_line_u64(&l, "min_mib", GK3W_ESP_MIN_SIZE_MIB);
                break;
            case GK3W_ESP_FULL:
                gk3w_line_u64(&l, "need_mib", (uint64_t)need / 1024);
                gk3w_line_u64(&l, "free_mib", (uint64_t)sp.free_kib / 1024);
                break;
            case GK3W_ESP_FULL_MIN:
                gk3w_line_u64(&l, "need_mib", sp.reinstall ? GK3W_ESP_REINSTALL_NEED_MIB : GK3W_ESP_NEED_MIB);
                gk3w_line_u64(&l, "free_mib", (uint64_t)sp.free_kib / 1024);
                break;
            case GK3W_ESP_OTA_ROOM:
                gk3w_line_u64(&l, "left_mib", (uint64_t)((sp.free_kib - need) > 0 ? (sp.free_kib - need) / 1024 : 0));
                break;
            case GK3W_ESP_WIN_RESERVE: {
                int64_t left = sp.free_kib - need - sp.slot_kib - (int64_t)GK3W_ESP_GK3BOOT_KIB;
                gk3w_line_u64(&l, "left_mib", (uint64_t)(left > 0 ? left / 1024 : 0));
                gk3w_line_u64(&l, "need_mib", GK3W_ESP_WIN_RESERVE_KIB / 1024);
                break;
            }
            default:
                break;
            }
            gk3w_fail(&l, c->T, "not enough space on the EFI system partition (see the code; disk not touched)");
            gk3w_line_free(&l);
            return -1;
        }
    }
    /* 卸不下来的话，后面写 ESP 时就挂不上可写的（那时盘已经写过了）：在这里就停 */
    if ((err = gk3l_umount_tmp(c->esp.dir)))
        return gk3l_err(c->T, "esp-umount", "cannot unmount the EFI system partition after checking it (disk not touched)", err), -1;
    c->esp_mounted = 0;
    if (c->esp.windows) {
        gk3l_win w;
        gk3l_win_state(d->name, &w);
        gk3l_log("Windows volumes: bitlocker=%s hibernated=%s%s%s", w.bitlocker ? "yes" : "no",
                 w.hibernated == 1 ? "yes" : w.hibernated ? "unknown" : "no", w.hib_part[0] ? " part=" : "", w.hib_part);
        if (w.hibernated == 1)
            return gk3l_err_kv(c->T, "esp-windows-hibernated", "part", w.hib_part,
                               "Windows is hibernated or in Fast Startup: writing the shared EFI system partition now could let Windows corrupt it when it resumes. Shut Windows down with Fast Startup off, then try again (disk not touched)", 0), -1;
        if (w.hibernated < 0)
            gk3l_log("warning: cannot tell whether Windows is hibernated (a BitLocker volume, or an NTFS volume that does not mount); make sure Windows was shut down with Fast Startup off");
        if (w.bitlocker && c->aa_change && !c->blconfirmed)
            return gk3l_err(c->T, "bitlocker-key-unconfirmed",
                            "a Windows volume is BitLocker-encrypted and this install replaces EFI/BOOT/BOOTAA64.EFI: Windows may ask for the 48-digit recovery key next time. Confirm that you have the recovery key (--bitlocker-confirmed) (disk not touched)", 0), -1;
    }
    return 0;
}

/* ── 方案 ── */

static int plan_alongside(apply_ctx *c)
{
    gk3l_disk *d = &c->d;
    char dup[128] = "";
    gk3w_span parts[GK3W_MAX_PARTS], spans[GK3W_MAX_PARTS + 1];
    size_t np = 0, nf = 0;
    int inside = 0;
    gk3w_plan_req req;
    gk3w_plan_err pe;
    for (uint32_t i = 0; i < d->g.num_entries; i++) {
        gk3_gpt_part q;
        char n[GK3_GPT_NAME_CHARS * 4 + 1];
        if (gk3_gpt_get(&d->g, i, &q) != GK3_OK)
            continue;
        if (np < GK3W_MAX_PARTS)
            parts[np++] = (gk3w_span){q.first_lba, q.last_lba};
        gk3l_gpt_name_utf8(&d->g, i, n, sizeof n);
        for (size_t k = 0; k < sizeof OUR_NAMES / sizeof OUR_NAMES[0]; k++)
            if (!strcmp(n, OUR_NAMES[k]) && strlen(dup) + strlen(n) + 2 < sizeof dup) {
                if (dup[0])
                    strcat(dup, ",");
                strcat(dup, n);
            }
    }
    if (dup[0])
        return gk3l_err_kv(c->T, "partlabel-conflict", "names", dup, "the disk already has partitions with these names; Android would mount one of them at random (disk not touched)", 0), -1;
    if (gk3w_free_spans(d->g.first_usable, d->g.last_usable, parts, np, d->bs, 0, spans, GK3W_MAX_PARTS + 1, &nf) != GK3W_LAYOUT_OK)
        return gk3l_err(c->T, "disk-layout", "the existing partition table has out-of-range or overlapping entries (disk not touched)", 0), -1;
    for (size_t i = 0; i < nf && i < GK3W_MAX_PARTS + 1; i++)
        if (c->rstart >= spans[i].first && c->rend <= spans[i].last && c->rstart <= c->rend)
            inside = 1;
    if (!inside)
        return gk3l_err(c->T, "bad-region", "the selected region is not free space (disk not touched)", 0), -1;
    memset(&req, 0, sizeof req);
    req.sector = d->bs;
    req.region = (gk3w_span){c->rstart, c->rend};
    req.userdata_mib = c->ud_mib;
    pe = gk3w_plan_print(&req, d->path, &c->plan, stdout);
    if (pe != GK3W_PLAN_OK)
        return gk3l_err_kv(c->T, "plan", "why", gk3w_plan_strerror(pe), "the selected free space is too small (disk not touched)", 0), -1;
    return 0;
}

static int plan_wipe(apply_ctx *c)
{
    gk3l_disk *d = &c->d;
    uint8_t dg[16];
    gk3_gpt g;
    gk3w_plan_req req;
    gk3w_plan_err pe;
    char medium[32] = "", what[400] = "";
    /* 安装介质就在这块盘上：整盘清空等于锯掉自己脚下的地板（而且跑到一半才死） */
    if (!gk3l_medium_devno(medium, sizeof medium)) {
        int on = !strcmp(medium, d->devno);
        for (size_t i = 0; i < d->nk; i++)
            on |= !strcmp(d->kp[i].devno, medium);
        if (on)
            return gk3l_err_kv(c->T, "wipe-medium", "disk", d->path, "the installation medium is on this disk; it cannot wipe the disk it runs from (disk not touched)", 0), -1;
    }
    {
        /* 整块盘本身也算（没有分区表、整盘格式化的 U 盘可能就挂着） */
        gk3l_kpart whole;
        memset(&whole, 0, sizeof whole);
        snprintf(whole.name, sizeof whole.name, "%s", d->name);
        snprintf(whole.devno, sizeof whole.devno, "%s", d->devno);
        if (gk3l_part_in_use(d->name, &whole, what, sizeof what))
            return gk3l_err_kv(c->T, "wipe-mounted", "mounts", what, "the disk is in use (mounted, swap, or under another device) (disk not touched)", 0), -1;
    }
    for (size_t i = 0; i < d->nk; i++)
        if (gk3l_part_in_use(d->name, &d->kp[i], what, sizeof what))
            return gk3l_err_kv(c->T, "wipe-mounted", "mounts", what, "a partition of this disk is in use (mounted, swap, or under another device) (disk not touched)", 0), -1;
    /* 新表先在内存里建好：方案要的就是它的 usable 区 */
    c->npmbr = gk3w_aligned_alloc(4096);
    c->nhdr = gk3w_aligned_alloc(4096);
    c->nent = gk3w_aligned_alloc(GK3W_GPT_ENTRIES * GK3W_GPT_ENTRY_SIZE);
    if (!c->npmbr || !c->nhdr || !c->nent)
        return gk3l_err(c->T, "nomem", "out of memory", ENOMEM), -1;
    if (getrandom(dg, 16, 0) != 16)
        return gk3l_err(c->T, "uuid", "cannot generate a disk GUID (disk not touched)", errno), -1;
    gk3w_guid_v4(dg);
    if (gk3w_gpt_init(d->bs, d->blocks, dg, c->npmbr, c->nhdr, c->nent) != GK3W_GPTW_OK ||
        gk3_gpt_parse_header(c->nhdr, d->bs, &g) != GK3_OK)
        return gk3l_err(c->T, "plan", "the disk is too small for a partition table (disk not touched)", 0), -1;
    memset(&req, 0, sizeof req);
    req.sector = d->bs;
    req.region = (gk3w_span){g.first_usable, g.last_usable};
    req.userdata_mib = c->ud_mib;
    req.whole = 1;
    pe = gk3w_plan_print(&req, d->path, &c->plan, stdout);
    if (pe != GK3W_PLAN_OK)
        return gk3l_err_kv(c->T, "plan", "why", gk3w_plan_strerror(pe), "the disk is too small for Android (disk not touched)", 0), -1;
    return 0;
}

static int plan_reinstall(apply_ctx *c)
{
    gk3l_disk *d = &c->d;
    static gk3w_part_ref parts[GK3W_MAX_PARTS];
    static char pathbuf[GK3W_MAX_PARTS][96];
    const char *paths[GK3W_MAX_PARTS];
    size_t n = 0;
    char medium[32] = "", busy[400] = "";
    gk3w_plan_err pe;
    for (uint32_t i = 0; i < d->g.num_entries && n < GK3W_MAX_PARTS; i++) {
        gk3_gpt_part q;
        const gk3l_kpart *k;
        if (gk3_gpt_get(&d->g, i, &q) != GK3_OK)
            continue;
        gk3l_gpt_name_utf8(&d->g, i, parts[n].name, sizeof parts[n].name);
        parts[n].idx = q.index;
        parts[n].first = q.first_lba;
        parts[n].last = q.last_lba;
        k = gk3l_kernel_find(d->kp, d->nk, q.index);
        snprintf(pathbuf[n], sizeof pathbuf[n], "/dev/%s", k ? k->name : "");
        paths[n] = pathbuf[n];
        n++;
    }
    /* 分区表本身要合乎常理：名字对得上、却与别的分区重叠的表（坏的、手改的）不往上写 */
    {
        gk3w_span sp[GK3W_MAX_PARTS], fr[GK3W_MAX_PARTS + 1];
        size_t nf = 0;
        for (size_t i = 0; i < n; i++)
            sp[i] = (gk3w_span){parts[i].first, parts[i].last};
        if (gk3w_free_spans(d->g.first_usable, d->g.last_usable, sp, n, d->bs, 0, fr, GK3W_MAX_PARTS + 1, &nf) != GK3W_LAYOUT_OK)
            return gk3l_err(c->T, "disk-layout", "the existing partition table has out-of-range or overlapping entries (disk not touched)", 0), -1;
    }
    pe = gk3w_reinstall_print(d->bs, parts, paths, n, c->keep, &c->ri, stdout);
    if (pe != GK3W_PLAN_OK)
        return gk3l_err_kv(c->T, "plan", "why", gk3w_plan_strerror(pe), "the Android partitions on this disk do not form one complete installation of this version (disk not touched)", 0), -1;
    /* 要写的分区一个都不能在用 —— 首先是安装器自己所在的那个（免 U 盘时它就在这块盘上） */
    gk3l_medium_devno(medium, sizeof medium);
    for (int k = 0; k < GK3W_REUSE_N; k++) {
        const gk3l_kpart *kp = gk3l_kernel_find(d->kp, d->nk, c->ri.p[k].idx);
        char what[400];
        if (!kp)
            continue;
        if ((medium[0] && !strcmp(kp->devno, medium)) || gk3l_part_in_use(d->name, kp, what, sizeof what)) {
            if (strlen(busy) + strlen(kp->name) + 7 < sizeof busy)
                snprintf(busy + strlen(busy), sizeof busy - strlen(busy), "%s/dev/%s", busy[0] ? "," : "", kp->name);
        }
    }
    if (busy[0])
        return gk3l_err_kv(c->T, "reinstall-busy", "parts", busy, "a partition to be rewritten is in use (is the installer running from it?) (disk not touched)", 0), -1;
    return 0;
}

/* ── 写分区表（双系统：加分区；整盘清空：换一张新表）、告诉内核、等节点 ── */

/* 整盘清空：内核里的旧分区先全部去掉（都不在用，见 plan_wipe）。有一个去不掉就把去掉的加回来、不动盘 */
static int wipe_kernel_parts(apply_ctx *c)
{
    gk3l_disk *d = &c->d;
    for (size_t i = 0; i < d->nk; i++) {
        int err = gk3l_kernel_del(d, d->kp[i].num);
        if (err) {
            for (size_t j = 0; j < i; j++) {
                uint64_t f = d->bs / 512;
                if (d->kp[j].size512 < f ||
                    gk3l_kernel_add(d, (uint32_t)d->kp[j].num, d->kp[j].start512 / f, (d->kp[j].start512 + d->kp[j].size512) / f - 1))
                    gk3l_log("warning: could not give partition %s back to the kernel; reboot before using this disk", d->kp[j].name);
            }
            return gk3l_err_kv(GK3W_TOUCHED_NO, "wipe-mounted", "mounts", d->kp[i].name, "the kernel would not release a partition of this disk (disk not touched)", err), -1;
        }
    }
    return 0;
}

static int write_table(apply_ctx *c)
{
    gk3l_disk *d = &c->d;
    gk3w_gpt_new np[GK3W_PLAN_MAX];
    uint32_t idx[GK3W_PLAN_MAX];
    uint8_t *hdr = c->mode == M_WIPE ? c->nhdr : d->hdr, *ent = c->mode == M_WIPE ? c->nent : d->ent;
    uint8_t bak[4096];
    uint64_t bel = 0;
    size_t elen;
    gk3w_gptw_err ge;
    gk3_gpt g;
    int w, err;
    if (gk3_gpt_parse_header(hdr, d->bs, &g) != GK3_OK)
        return gk3l_err(GK3W_TOUCHED_NO, "gpt-layout", "internal error: the partition table header does not parse (disk not touched)", 0), -1;
    elen = (size_t)(((uint64_t)g.num_entries * g.entry_size + d->bs - 1) / d->bs * d->bs);
    /* 备份表头要在盘内：从更大的盘上抄来的表，写备份表时才发现越界就晚了 */
    if (g.alt_lba >= d->blocks)
        return gk3l_err(GK3W_TOUCHED_NO, "gpt-layout", "the backup partition table header lies beyond the end of the disk (disk not touched)", 0), -1;
    for (size_t i = 0; i < c->plan.n; i++) {
        memset(&np[i], 0, sizeof np[i]);
        np[i].name = c->plan.parts[i].name;
        gk3w_guid_parse(strcmp(np[i].name, "esp") ? LINUX_DATA : ESP_TYPE, np[i].type);
        if (getrandom(np[i].uuid, 16, 0) != 16)
            return gk3l_err(GK3W_TOUCHED_NO, "uuid", "cannot generate a partition GUID (disk not touched)", errno), -1;
        gk3w_guid_v4(np[i].uuid);
        np[i].first = c->plan.parts[i].first;
        np[i].last = c->plan.parts[i].last;
    }
    ge = gk3w_gpt_add(hdr, d->bs, ent, elen, np, c->plan.n, idx);
    /* 备份表放得下（gk3l_gpt_write 里还会再生成一次） */
    if (ge == GK3W_GPTW_OK && d->bs <= sizeof bak)
        ge = gk3w_gpt_make_backup(hdr, d->bs, bak, &bel);
    if (ge != GK3W_GPTW_OK)
        return gk3l_err_kv(GK3W_TOUCHED_NO, ge == GK3W_GPTW_FULL ? "gpt-full" : "gpt-layout", "why", gk3w_gptw_strerror(ge),
                           ge == GK3W_GPTW_FULL ? "the partition table is full (disk not touched)" : "cannot add the partitions to the partition table (disk not touched)", 0), -1;
    if (c->mode == M_WIPE && wipe_kernel_parts(c))
        return -1;

    /* ══ 从这里起开始动盘 ══ */
    c->T = GK3W_TOUCHED_YES;
    gk3w_progress(5, "write-gpt");
    /* 整盘清空先写保护性 MBR，并清掉表项之后到第一个分区之前的旧数据（旧的引导扇区、文件系统签名） */
    w = 0;
    if (c->mode == M_WIPE) {
        uint64_t gap = c->plan.parts[0].first - g.first_usable;
        memset(c->zero, 0, IO_CHUNK);
        memcpy(c->zero, c->npmbr, d->bs);
        w = gk3l_write_verify(d, c->zero, d->bs, 0, c->chk);
        memset(c->zero, 0, d->bs);
        if (!w && gap && gap * d->bs <= IO_CHUNK)
            w = gk3l_write_verify(d, c->zero, (size_t)(gap * d->bs), g.first_usable * d->bs, c->chk);
    }
    if (!w)
        w = gk3l_gpt_write(d, hdr, ent, c->chk);
    err = errno;
    if (w)
        return gk3l_err_kv(c->T, w == -1 ? "disk-write" : "disk-verify", "part", "gpt", w == -1 ? "writing the partition table failed" : "the partition table did not read back identically", err), -1;
    if (gk3l_disk_read_table(d) || strcmp(d->table, "gpt"))
        return gk3l_err(c->T, "verify-layout", "the partition table does not parse after writing it", 0), -1;
    for (size_t i = 0; i < c->plan.n; i++) {
        gk3_gpt_part q;
        int t;
        if (gk3_gpt_get(&d->g, idx[i] - 1, &q) != GK3_OK || q.first_lba != c->plan.parts[i].first || q.last_lba != c->plan.parts[i].last)
            return gk3l_err_kv(c->T, "verify-layout", "name", c->plan.parts[i].name, "after writing the partition table, a new partition does not match the plan", 0), -1;
        if ((err = gk3l_kernel_add(d, idx[i], q.first_lba, q.last_lba)))
            return gk3l_err_kv(c->T, "kernel-add", "name", c->plan.parts[i].name, "the kernel refused the new partition (BLKPG)", err), -1;
        for (t = 0; t < T_N; t++)
            if (!strcmp(T_NAMES[t], c->plan.parts[i].name))
                break;
        if (t < T_N) {
            c->t[t].idx = idx[i];
            c->t[t].off = q.first_lba * d->bs;
            c->t[t].bytes = (q.last_lba - q.first_lba + 1) * d->bs;
        }
    }
    for (int t = 0; t < T_N; t++)
        if (c->t[t].idx && gk3l_wait_node(d, c->t[t].idx, c->t[t].node, sizeof c->t[t].node))
            return gk3l_err_kv(c->T, "node-missing", "name", T_NAMES[t], "the device node of a new partition did not appear", 0), -1;
    gk3l_log("partition table written (primary and backup), the kernel knows the %zu new partitions", c->plan.n);
    return 0;
}

/* 重新安装：目标就是盘上现有的分区 */
static int reuse_targets(apply_ctx *c)
{
    for (int k = 0; k < GK3W_REUSE_N; k++) {
        const gk3w_part_ref *p = &c->ri.p[k];
        const gk3l_kpart *kp = gk3l_kernel_find(c->d.kp, c->d.nk, p->idx);
        c->t[k].idx = p->idx;
        c->t[k].off = p->first * c->d.bs;
        c->t[k].bytes = (p->last - p->first + 1) * c->d.bs;
        if (!kp)
            return gk3l_err_kv(c->T, "kernel-stale", "name", T_NAMES[k], "the kernel does not know a partition to be rewritten (disk not touched)", 0), -1;
        snprintf(c->t[k].node, sizeof c->t[k].node, "/dev/%s", kp->name);
    }
    return 0;
}

/* ── 15 format / 30 write-super / 70 write-boot ── */

static int mkfs_ext4(apply_ctx *c, int t, const char *label, int no_reserve)
{
    char *av[10];
    int a = 0, r;
    av[a++] = "mkfs.ext4";
    av[a++] = "-q";
    av[a++] = "-F";
    if (no_reserve) {
        av[a++] = "-m";
        av[a++] = "0";
    }
    av[a++] = "-L";
    av[a++] = (char *)label;
    av[a++] = c->t[t].node;
    av[a] = NULL;
    r = gk3l_run(av, NULL, 0);
    if (r || check_ext4(&c->d, c->t[t].off, label, c->zero))
        return gk3l_err_kv(c->T, "mkfs", "part", label, r ? "mkfs.ext4 failed" : "after formatting, the partition does not start with the expected ext4 superblock", 0), -1;
    memset(c->zero, 0, IO_CHUNK);
    return 0;
}

static int write_parts(apply_ctx *c)
{
    gk3l_disk *d = &c->d;
    const char *why = NULL;
    int w;
    gk3w_progress(15, "format");
    /* 重新安装：从这里起动盘（分区表不动） */
    c->T = GK3W_TOUCHED_YES;
    /* misc：按分区的实际大小清零（重新安装复用的 misc 可能只有 1007 KiB），再写一份合法的初始状态 */
    {
        uint64_t mb = c->t[T_MISC].bytes;
        for (uint64_t off = 0; off < mb; off += IO_CHUNK) {
            size_t n = mb - off < IO_CHUNK ? (size_t)(mb - off) : IO_CHUNK;
            w = gk3l_write_verify(d, c->zero, n, c->t[T_MISC].off + off, c->chk);
            if (w)
                return gk3l_err_kv(c->T, w == -1 ? "disk-write" : "disk-verify", "part", "misc", w == -1 ? "zeroing failed" : "zeroed area did not read back as zero", errno), -1;
        }
    }
    {
        uint8_t head[GK3_MISC_INIT_HEAD_SIZE], recb[GK3_REC_SIZE];
        gk3_setdef def = c->esp.windows ? (c->defos_windows ? GK3_SETDEF_WINDOWS : GK3_SETDEF_ANDROID) : GK3_SETDEF_NONE;
        char hex[GK3_MISC_BCAB_SIZE * 2 + 1];
        if (gk3_misc_build_install(head, recb, 0, def))
            return gk3l_err(c->T, "misc-init", "internal error: the misc initial state failed its self-check", 0), -1;
        /* 64 KiB：BCB + BCAB 在 0，GK3 记录在 8 KiB，其余是零 */
        memcpy(c->zero, head, sizeof head);
        memcpy(c->zero + GK3_MISC_GK3_OFF, recb, sizeof recb);
        w = gk3l_write_verify(d, c->zero, GK3_MISC_READ_SIZE, c->t[T_MISC].off, c->chk);
        if (w)
            return gk3l_err(c->T, "misc-init", "writing the misc initial state failed", errno), -1;
        for (unsigned k = 0; k < GK3_MISC_BCAB_SIZE; k++)
            snprintf(hex + 2 * k, 3, "%02x", head[GK3_MISC_BCAB_OFF + k]);
        gk3l_log("misc: MISCINIT slot=a default=%s bcab=%s", def == GK3_SETDEF_WINDOWS ? "windows" : def == GK3_SETDEF_ANDROID ? "android" : "none", hex);
        memset(c->zero, 0, IO_CHUNK);
    }
    if (c->mode == M_WIPE) {
        char *av[] = {"mkfs.vfat", "-F", "32", "-n", "ESP", c->t[T_ESP].node, NULL};
        if (gk3l_run(av, NULL, 0))
            return gk3l_err_kv(c->T, "mkfs", "part", "esp", "mkfs.vfat failed", 0), -1;
    }
    if (c->mode == M_REINSTALL && c->keep) {
        gk3l_log("keeping user data: userdata and metadata are not formatted");
    } else {
        /* 参数同 installer-lib.sh：userdata 加 -m 0（Android 的应用都不是 uid 0，5% 的保留块谁也用不上；
         * 系统的保留空间由 fstab 的 reservedsize= 交给 fs_mgr 管） */
        if (mkfs_ext4(c, T_META, "metadata", 0) || mkfs_ext4(c, T_DATA, "userdata", 1))
            return -1;
        gk3l_log("metadata, userdata: ext4 created (superblock magic and label read back from the disk)");
    }

    gk3w_progress(30, "write-super");
    {
        disk_sink ss = {d, c->t[T_SUPER].off, c->chk, gk3w_aligned_alloc(IO_CHUNK), 0};
        gk3w_sparse_sink sink = {&ss, ds_write, ds_fill};
        if (!ss.pat)
            return gk3l_err(c->T, "nomem", "out of memory", ENOMEM), -1;
        if (run_image(c->p_super, c->zst, (uint64_t)GK3W_SUPER_MIB << 20, &sink, 1, &why)) {
            gk3w_aligned_free(ss.pat);
            return gk3l_err_kv(c->T, "super-write", "why", why, ss.failed == -2 ? "super did not read back identically" : "writing super failed", errno), -1;
        }
        gk3w_aligned_free(ss.pat);
        /* 偏移 4096 处必须是 LP geometry 魔数（installer-lib.sh gk3__write_super 的同一道检查） */
        if (gk3l_pread(d, c->zero, 4096, c->t[T_SUPER].off + 4096) || memcmp(c->zero, "gDla", 4))
            return gk3l_err(c->T, "super-bad-lp", "no LP geometry magic at offset 4096 of super: what was written is not a usable super", 0), -1;
        gk3l_log("super: LP geometry magic correct");
        memset(c->zero, 0, IO_CHUNK);
    }

    gk3w_progress(70, "write-boot");
    {
        size_t padded = (c->bi.len + d->bs - 1) / d->bs * d->bs;
        uint8_t *c2 = gk3w_aligned_alloc(padded);
        int targets[2] = {T_BOOTA, T_BOOTB};
        c->bootbuf = gk3w_aligned_alloc(padded);
        if (!c->bootbuf || !c2) {
            gk3w_aligned_free(c2);
            return gk3l_err(c->T, "nomem", "out of memory", ENOMEM), -1;
        }
        memset(c->bootbuf, 0, padded);
        memcpy(c->bootbuf, c->bi.img, c->bi.len);
        for (int k = 0; k < 2; k++) {
            w = gk3l_write_verify(d, c->bootbuf, padded, c->t[targets[k]].off, c2);
            if (w) {
                gk3w_aligned_free(c2);
                return gk3l_err_kv(c->T, w == -1 ? "disk-write" : "disk-verify", "part", T_NAMES[targets[k]],
                                   w == -1 ? "writing boot failed" : "boot did not read back identically", errno), -1;
            }
        }
        gk3w_aligned_free(c2);
    }
    for (int t = 0; t < T_N; t++)
        if (c->t[t].node[0])
            gk3l_flush_node(c->t[t].node);
    return 0;
}

/* ── 80 bootloader：ESP ── */
static int write_esp(apply_ctx *c)
{
    static const char *const fn[3] = {"Image", "gaokun3.dtb", "ramdisk.img"};
    const uint8_t *src[3] = {c->bi.img + c->bi.b.kernel_off, c->bi.img + c->bi.b.dtb_off, c->bi.img + c->bi.b.ramdisk_off};
    size_t sz[3] = {c->bi.b.kernel_size, c->bi.b.dtb_size, c->bi.b.ramdisk_size};
    static const char *const backups[2] = {"EFI/BOOT/BOOTAA64.EFI", "loader/loader.conf"};
    static char entry[2][GK3_BOOT_ARGS_SIZE + GK3_BOOT_EXTRA_ARGS_SIZE + 512];
    static esp_log elog;
    char relp[160];
    int err;
    gk3w_progress(80, "bootloader");
    memset(&elog, 0, sizeof elog);
    elog.e = &c->esp;
    if (c->mode == M_WIPE) {
        snprintf(c->esp_dev, sizeof c->esp_dev, "%s", c->t[T_ESP].node);
        memcpy(c->esp.mid, c->mid_fb, 33);   /* 新格式化的 ESP：没有现成目录 */
    }
    if ((err = gk3l_mount_tmp(c->esp_dev, "vfat", 0, c->esp.dir, sizeof c->esp.dir)))
        return gk3l_err(c->T, "esp-mount", "cannot mount the EFI system partition for writing", err), -1;
    c->esp_mounted = 1;
    if (esp_cleanup(&c->esp))
        return -1;
    for (int k = 0; k < 2; k++) {
        if (!c->orig[k])
            continue;
        snprintf(relp, sizeof relp, "%s.before-gaokun3", backups[k]);
        if (esp_put(&elog, relp, c->orig[k], c->orig_len[k], 1))
            return -1;
        gk3l_log("backed up the original %s as %s", backups[k], relp);
    }
    /* 只在字节不同时写（BitLocker 量的是固件每次加载的这个文件）。回落路径写成 .tmp 再改名：中途断电时它要么还是
     * 原来的文件、要么是完整的 systemd-boot */
    for (int k = 0; k < 2; k++) {
        const char *t = k ? "EFI/systemd/systemd-bootaa64.efi" : "EFI/BOOT/BOOTAA64.EFI";
        if (esp_same(&c->esp, t, c->sdboot, c->sdboot_len))
            gk3l_log("%s is already this systemd-boot, not rewritten", t);
        else if (esp_put(&elog, t, c->sdboot, c->sdboot_len, !k))
            return -1;
    }
    for (char sl = 'a'; sl <= 'b'; sl++) {
        int n;
        for (int k = 0; k < 3; k++) {
            snprintf(relp, sizeof relp, "%s/android/slot_%c/%s", c->esp.mid, sl, fn[k]);
            if (esp_put(&elog, relp, src[k], sz[k], 0))
                return -1;
        }
        snprintf(relp, sizeof relp, "loader/entries/%s-android-%c.conf", c->esp.mid, sl);
        n = gk3w_esp_android_entry(entry[sl - 'a'], sizeof entry[0], c->esp.mid, sl, c->bi.cmdline);
        if (n < 0)
            return gk3l_err(c->T, "esp-write", "internal error: cannot generate the boot entry", 0), -1;
        if (esp_put(&elog, relp, (const uint8_t *)entry[sl - 'a'], (size_t)n, 0))
            return -1;
    }
    if (esp_put(&elog, "loader/loader.conf", (const uint8_t *)gk3w_loader_conf, strlen(gk3w_loader_conf), 0))
        return -1;
    sync();
    if ((err = gk3l_umount_tmp(c->esp.dir)))
        return gk3l_err(c->T, "esp-umount", "cannot unmount the EFI system partition; what was written may not be on the disk", err), -1;
    c->esp_mounted = 0;
    /* 从介质读回核对：丢掉这块设备的缓存，只读挂回去逐个比 */
    gk3l_flush_node(c->esp_dev);
    if ((err = gk3l_mount_tmp(c->esp_dev, "vfat", 1, c->esp.dir, sizeof c->esp.dir)))
        return gk3l_err(c->T, "verify-remount", "cannot mount the EFI system partition again to verify it", err), -1;
    c->esp_mounted = 1;
    for (size_t i = 0; i < elog.n; i++)
        if (!esp_same(&c->esp, elog.w[i].rel, elog.w[i].data, elog.w[i].len))
            return gk3l_err_kv(c->T, "esp-verify", "file", elog.w[i].rel, "a file on the EFI system partition did not read back identically", 0), -1;
    gk3l_umount_tmp(c->esp.dir);
    c->esp_mounted = 0;
    gk3l_log("ESP: systemd-boot, kernel / dtb / ramdisk for both slots, both entries and loader.conf written; %zu files read back from the disk and verified", elog.n);
    return 0;
}

/* LoaderEntryDefault 优先于 loader.conf 的 default：装完还在的话，第一次重启进的就不是新装的 Android。
 * 不管值是什么都删（默认系统由统一启动入口按 misc 里的请求去写）；整盘清空时 LoaderEntryOneShot 一并删（盘上已经没有它
 * 能指的东西）。删不掉只提醒：盘上的东西都已写好 */
static void clear_loader_vars(apply_ctx *c)
{
    const char *names[2] = {"LoaderEntryDefault", "LoaderEntryOneShot"};
    for (int i = 0; i < (c->mode == M_WIPE ? 2 : 1); i++) {
        char val[256];
        int r = gk3l_loader_var(names[i], val, sizeof val);
        if (r == 2) {
            gk3l_log("cannot read EFI variables (efivarfs unavailable); %s not checked", names[i]);
            return;
        }
        if (r == 1)
            continue;
        if (gk3l_loader_var_delete(names[i]) == 0) {
            gk3l_log("deleted the EFI variable %s (was '%s')", names[i], val);
        } else {
            gk3w_line l = {0};
            gk3l_log("warning: the EFI variable %s='%s' cannot be deleted", names[i], val);
            gk3w_line_begin(&l, "NOTE");
            gk3w_line_kv(&l, "code", "loadervar-stuck");
            gk3w_line_kv(&l, "name", names[i]);
            gk3w_line_kv(&l, "value", val);
            (void)gk3w_line_emit(&l, stdout);
            gk3w_line_free(&l);
        }
    }
}

int gk3l_cmd_apply(int argc, char **argv)
{
    static apply_ctx c;
    const char *disk_arg = gk3l_arg(argc, argv, "--disk"), *mode = gk3l_arg(argc, argv, "--mode"),
               *mid_arg = gk3l_arg(argc, argv, "--machine-id"), *defos_arg = gk3l_arg(argc, argv, "--default-os"),
               *keep = gk3l_arg(argc, argv, "--keep-data");
    int rc = 1, bad = 0;

    memset(&c, 0, sizeof c);
    c.T = GK3W_TOUCHED_NO;
    c.rel = gk3l_arg(argc, argv, "--release");
    c.backup_dir = gk3l_arg(argc, argv, "--backup-dir");
    c.blconfirmed = gk3l_flag(argc, argv, "--bitlocker-confirmed");
    c.mode = !mode || !strcmp(mode, "alongside") ? M_ALONGSIDE : !strcmp(mode, "wipe") ? M_WIPE : !strcmp(mode, "reinstall") ? M_REINSTALL : -1;
    if (c.mode < 0 || !disk_arg || !c.rel || (gk3l_arg(argc, argv, "--userdata-mib") && gk3l_arg_u64(argc, argv, "--userdata-mib", &c.ud_mib)) ||
        (keep && strcmp(keep, "yes") && strcmp(keep, "no")))
        bad = 1;
    if (c.mode == M_ALONGSIDE && (gk3l_arg_u64(argc, argv, "--region-start", &c.rstart) || gk3l_arg_u64(argc, argv, "--region-end", &c.rend)))
        bad = 1;
    if (bad)
        return gk3l_err(c.T, "usage",
                        "usage: apply --disk /dev/X --release DIR [--mode alongside|wipe|reinstall] [--region-start LBA --region-end LBA] "
                        "[--keep-data yes|no] [--sdboot FILE] [--backup-dir DIR] [--machine-id HEX32] [--default-os android|windows] "
                        "[--userdata-mib N] [--bitlocker-confirmed]",
                        0);
    c.keep = keep && !strcmp(keep, "yes");
    if (defos_arg && strcmp(defos_arg, "android") && strcmp(defos_arg, "windows"))
        return gk3l_err(c.T, "usage", "--default-os must be android or windows", 0);
    c.defos_windows = defos_arg && !strcmp(defos_arg, "windows");
    if (c.defos_windows && c.mode == M_WIPE)
        return gk3l_err(c.T, "default-os-no-windows", "after wiping the whole disk there is no Windows, so it cannot be the default", 0);
    if (!mid_arg)
        mid_arg = getenv("GK3_MACHINE_ID");
    if (mid_arg && *mid_arg) {
        if (!gk3w_is_machine_id(mid_arg))
            return gk3l_err(c.T, "usage", "--machine-id must be 32 lowercase hex digits", 0);
        memcpy(c.mid_fb, mid_arg, 33);
    } else {
        uint8_t r[16];
        if (getrandom(r, sizeof r, 0) != (ssize_t)sizeof r)
            return gk3l_err(c.T, "random", "cannot generate a machine-id", errno);
        for (int i = 0; i < 16; i++)
            snprintf(c.mid_fb + 2 * i, 3, "%02x", r[i]);
    }
    if (gk3l_sys_gate())
        return 1;

    c.zero = gk3w_aligned_alloc(IO_CHUNK);
    c.chk = gk3w_aligned_alloc(IO_CHUNK);
    if (!c.zero || !c.chk) {
        gk3l_err(c.T, "nomem", "out of memory", ENOMEM);
        goto out;
    }
    memset(c.zero, 0, IO_CHUNK);
    if (check_inputs(&c, gk3l_arg(argc, argv, "--sdboot")) || open_disk(&c, disk_arg))
        goto out;
    if (c.mode != M_WIPE && esp_check(&c))
        goto out;
    gk3w_progress(2, "plan");
    if (c.mode == M_ALONGSIDE ? plan_alongside(&c) : c.mode == M_WIPE ? plan_wipe(&c) : plan_reinstall(&c))
        goto out;

    /* 最后一次核对：准备期间分区表被别的工具改过就不写 */
    if (c.mode != M_WIPE &&
        (gk3l_disk_read_table(&c.d) || strcmp(c.d.table, "gpt") || memcmp(c.snap, c.d.hdr, c.d.bs) ||
         memcmp(c.snap + c.d.bs, c.d.ent, c.snap_len) || !gk3l_disk_kernel_ok(&c.d))) {
        gk3l_err(c.T, "layout-changed", "the partition table changed while preparing the install; run the installer again (disk not touched)", 0);
        goto out;
    }
    if (c.mode == M_REINSTALL) {
        /* 分区表不动：不备份；目标就是现有分区 */
        gk3l_log("reinstall: the partition table is not changed; reusing the existing %d partitions", GK3W_REUSE_N);
        if (reuse_targets(&c))
            goto out;
    } else {
        if (c.mode == M_ALONGSIDE ? gk3l_gpt_backup(&c.d, c.backup_dir, "apply", c.d.g.first_usable, c.d.blocks - (c.d.g.last_usable + 1))
                                  : gk3l_gpt_backup(&c.d, c.backup_dir, "wipe", (1u << 20) / c.d.bs, (1u << 20) / c.d.bs))
            goto out;
        if (write_table(&c))
            goto out;
    }
    if (write_parts(&c) || write_esp(&c))
        goto out;
    clear_loader_vars(&c);
    gk3w_progress(100, "done");
    rc = 0;
out:
    if (c.esp_mounted)
        gk3l_umount_tmp(c.esp.dir);
    if (c.opened)
        gk3l_disk_close(&c.d);
    free(c.orig[0]);
    free(c.orig[1]);
    free(c.snap);
    free(c.bi.img);
    free(c.sdboot);
    free(c.man);
    gk3w_aligned_free(c.zero);
    gk3w_aligned_free(c.chk);
    gk3w_aligned_free(c.bootbuf);
    gk3w_aligned_free(c.nhdr);
    gk3w_aligned_free(c.nent);
    gk3w_aligned_free(c.npmbr);
    return rc;
}
