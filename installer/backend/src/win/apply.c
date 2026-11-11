/* Windows：装 Android 到一段空闲空间（双系统），对应 installer-lib.sh 的 gk3_apply --mode alongside。
 *
 *   gk3w apply --disk N --region-start LBA --region-end LBA --release DIR --sdboot FILE --backup-dir DIR
 *              [--mke2fs PATH] [--machine-id HEX32] [--default-os android|windows] [--userdata-mib N]
 *
 * 发布目录：boot.img、super.img.zst，可选 install-artifacts.sha256（有就逐个核对）。
 * 步骤与进度代码同 gk3_apply：1 check / verify-sha / test-zst → 2 plan → 5 write-gpt → 15 format → 30 write-super
 * → 70 write-boot → 80 bootloader → 100 done。★ 所有输入在第一次写盘之前验完（boot.img 拆得开、super 完整试解一遍、
 * ESP 空间够、区间确实空闲、分区名不重），之后 ERR 一律带 touched=yes。
 *
 * metadata / userdata 的格式化：给了 --mke2fs（Android platform-tools 里的 mke2fs.exe，同目录要有 mke2fs.conf）时，
 * 与 fastboot -w 的做法相同：动盘之前用 mke2fs -E android_sparse 按分区大小各生成一个 sparse 镜像（参数照
 * fastboot/fs.cpp generate_ext4_image，另加 LiveCD 的卷标与 userdata 的 -m 0、两者都 -I 256），格式化那一步用与 super
 * 同一套 sparse 写入写进去，写完核对 ext4 魔数与卷标。没给时退回清零（metadata 整个、userdata 开头 16 MiB），
 * 由 fstab 的 formattable 在首次开机格式化（尚未上机验证）。
 *
 * 与 LiveCD 的差别（原型阶段）：
 *   - 只对 VHD 动手；EFI 变量（LoaderEntryDefault）不碰 —— VHD 测试时那是这台 PC 自己的固件变量。
 *   - ESP 上已经有我们的启动项时拒绝（重新安装不在原型范围内）；不带救援分区。 */
#include <windows.h>
#include <winioctl.h>
#include <rpc.h>

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../esp.h"
#include "../layout.h"
#include "../plan.h"
#include "../proto.h"
#include "../sparse.h"
#include "disk.h"
#include "gk3core.h"
#include "win.h"
#include "zstd.h"

#define IO_CHUNK (4u << 20)
#define BOOT_MAX (64u << 20)
#define ZERO_HEAD (16u << 20)

typedef struct {
    uint8_t *img;               /* 整个 boot.img */
    size_t len;
    gk3_bootimg b;
    char cmdline[GK3_BOOT_ARGS_SIZE + GK3_BOOT_EXTRA_ARGS_SIZE + 1];
} bootimg;

static const char *const OUR_NAMES[] = {"misc", "metadata", "boot_a", "boot_b", "super", "userdata", "gk3rescue"};

/* ── 输入 ─────────────────────────────────────────────────────────── */

static int load_boot(const wchar_t *path, bootimg *bi)
{
    long n;
    memset(bi, 0, sizeof *bi);
    if (gk3w_read_file(path, &bi->img, &bi->len, BOOT_MAX))
        return gk3w_err(GK3W_TOUCHED_NA, "release-no-boot", "发布目录里没有可读的 boot.img（或大于 64 MiB）", GetLastError());
    if (gk3_bootimg_parse(bi->img, bi->len, BOOT_MAX, &bi->b) || bi->b.total_size > bi->len)
        return gk3w_err(GK3W_TOUCHED_NA, "bootimg-unpack", "boot.img 拆不开 —— 盘还没动过", 0);
    /* 与 gk3-bootimg.py 相同的体检：只认 v2，三段都不空，内核是 PE，dtb 是 FDT */
    if (bi->b.version != 2 || !bi->b.kernel_size || !bi->b.ramdisk_size || !bi->b.dtb_size ||
        memcmp(bi->img + bi->b.kernel_off, "MZ", 2) || memcmp(bi->img + bi->b.dtb_off, "\xd0\x0d\xfe\xed", 4))
        return gk3w_err(GK3W_TOUCHED_NA, "bootimg-unpack", "boot.img 不是带 dtb 的 v2 镜像，或内核 / dtb 格式不对 —— 盘还没动过", 0);
    n = gk3_bootimg_cmdline(&bi->b, bi->cmdline, sizeof bi->cmdline);
    if (n <= 0)
        return gk3w_err(GK3W_TOUCHED_NA, "bootimg-unpack", "boot.img 里没有内核命令行 —— 盘还没动过", 0);
    for (long i = 0; i < n; i++)
        if ((unsigned char)bi->cmdline[i] < 0x20 || (unsigned char)bi->cmdline[i] > 0x7e)
            return gk3w_err(GK3W_TOUCHED_NA, "bootimg-unpack", "boot.img 的命令行里有非 ASCII 或控制字符 —— 盘还没动过", 0);
    gk3w_log("  Image      %10u 字节", bi->b.kernel_size);
    gk3w_log("  ramdisk.img%10u 字节", bi->b.ramdisk_size);
    gk3w_log("  gaokun3.dtb%10u 字节", bi->b.dtb_size);
    return 0;
}

/* install-artifacts.sha256 里 name 那一行的 sha256（小写 hex）；没有返回 -1 */
static int manifest_hash(const uint8_t *m, size_t len, const char *name, char out[65])
{
    size_t i = 0, nl = strlen(name);
    while (i < len) {
        size_t e = i;
        while (e < len && m[e] != '\n')
            e++;
        /* "<64 hex>  <name>" */
        if (e - i >= 66 + nl && m[i + 64] == ' ' && m[i + 65] == ' ' && !memcmp(m + i + 66, name, nl) &&
            (e - i == 66 + nl || (e - i == 67 + nl && m[e - 1] == '\r'))) {
            for (int k = 0; k < 64; k++)
                out[k] = (char)(m[i + k] >= 'A' && m[i + k] <= 'F' ? m[i + k] + 32 : m[i + k]);
            out[64] = 0;
            return 0;
        }
        i = e + 1;
    }
    return -1;
}

static int file_sha256(const wchar_t *path, char out[65])
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    gk3w_sha256_ctx *c;
    uint8_t *buf, d[32];
    DWORD got;
    int rc = 0;
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    buf = malloc(IO_CHUNK);
    c = gk3w_sha256_begin();
    if (!buf || !c) {
        free(buf);
        if (c)
            gk3w_sha256_end(c, d);
        CloseHandle(h);
        return -1;
    }
    while (ReadFile(h, buf, IO_CHUNK, &got, NULL) && got)
        if (gk3w_sha256_update(c, buf, got))
            rc = -1;
    if (gk3w_sha256_end(c, d))
        rc = -1;
    gk3w_hex(d, 32, out);
    free(buf);
    CloseHandle(h);
    return rc;
}

/* ── super：zstd → sparse → 盘 ─────────────────────────────────────── */

typedef struct {
    HANDLE h;
    uint64_t base;              /* super 分区在盘上的字节偏移 */
    uint8_t *chk;               /* 读回比对用（IO_CHUNK，对齐） */
    uint8_t *pat;               /* FILL 用（IO_CHUNK，对齐） */
    int failed;                 /* -1 写失败；-2 读回不一致 */
} super_sink;

static int ss_write(void *ctx, uint64_t off, const uint8_t *data, size_t len)
{
    super_sink *s = ctx;
    int w = gk3w_write_verify(s->h, data, (DWORD)len, s->base + off, s->chk);
    if (w)
        s->failed = w;
    return w;
}

static int ss_fill(void *ctx, uint64_t off, uint64_t len, uint32_t pattern)
{
    super_sink *s = ctx;
    for (size_t i = 0; i < IO_CHUNK; i += 4)
        memcpy(s->pat + i, &pattern, 4);
    while (len) {
        size_t n = len < IO_CHUNK ? (size_t)len : IO_CHUNK;
        if (ss_write(ctx, off, s->pat, n))
            return -1;
        off += n;
        len -= n;
    }
    return 0;
}

/* 把 sparse 镜像（zst = 1 时先 zstd 解码）展开写出去。sink 为 NULL 时只校验（动盘之前的完整试解）。
 * prog：0 = 不报进度；否则按读入的压缩字节报 30→70。返回 0 成功，失败时 *why 是原因。 */
static int run_image(const wchar_t *path, int zst, uint64_t limit, const gk3w_sparse_sink *sink, int prog, const char **why)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    LARGE_INTEGER fsz;
    ZSTD_DCtx *dc = NULL;
    uint8_t *in = NULL, *out = NULL, *sbuf = NULL;
    gk3w_sparse s;
    uint64_t read_total = 0;
    size_t r = 1;
    unsigned last_pct = 0;
    int rc = -1;
    DWORD got;

    *why = "open";
    if (f == INVALID_HANDLE_VALUE || !GetFileSizeEx(f, &fsz) || !fsz.QuadPart)
        goto out;
    *why = "nomem";
    in = malloc(IO_CHUNK);
    out = malloc(IO_CHUNK);
    sbuf = _aligned_malloc(IO_CHUNK, 4096);
    dc = zst ? ZSTD_createDCtx() : NULL;
    if (!in || !out || !sbuf || (zst && !dc))
        goto out;
    /* release.sh 用 -19 --long 压：解码时放宽窗口上限（同 installer-lib.sh 的 zstd -dc --long=31） */
    if (dc)
        ZSTD_DCtx_setParameter(dc, ZSTD_d_windowLogMax, 31);
    if (gk3w_sparse_init(&s, limit, sbuf, IO_CHUNK))
        goto out;
    if (!zst)
        r = 0;
    while (ReadFile(f, in, IO_CHUNK, &got, NULL) && got) {
        ZSTD_inBuffer ib = {in, got, 0};
        read_total += got;
        if (!zst && gk3w_sparse_feed(&s, in, got, sink)) {
            *why = s.err;
            goto out;
        }
        while (zst && ib.pos < ib.size) {
            ZSTD_outBuffer ob = {out, IO_CHUNK, 0};
            r = ZSTD_decompressStream(dc, &ob, &ib);
            if (ZSTD_isError(r)) {
                *why = "zstd-corrupt";
                goto out;
            }
            if (gk3w_sparse_feed(&s, out, ob.pos, sink)) {
                *why = s.err;
                goto out;
            }
        }
        if (prog) {
            unsigned pct = 30 + (unsigned)(read_total * 40 / (uint64_t)fsz.QuadPart);
            if (pct != last_pct) {
                fprintf(stderr, "PROGRESS %u write-super done_mib=%llu total_mib=%llu\n", pct,
                        (unsigned long long)(s.out_off >> 20), (unsigned long long)(gk3w_sparse_total(&s) >> 20));
                fflush(stderr);
                last_pct = pct;
            }
        }
    }
    /* 读到文件尾时帧必须正好结束（r == 0）；否则把解码器里剩下的吐完，还不结束就是下载不完整 */
    while (r) {
        ZSTD_inBuffer ib = {in, 0, 0};
        ZSTD_outBuffer ob = {out, IO_CHUNK, 0};
        r = ZSTD_decompressStream(dc, &ob, &ib);
        if (ZSTD_isError(r) || !ob.pos) {
            *why = "zst-truncated";
            goto out;
        }
        if (gk3w_sparse_feed(&s, out, ob.pos, sink)) {
            *why = s.err;
            goto out;
        }
    }
    if (gk3w_sparse_finish(&s, sink)) {
        *why = s.err;
        goto out;
    }
    rc = 0;
out:
    if (dc)
        ZSTD_freeDCtx(dc);
    free(in);
    free(out);
    _aligned_free(sbuf);
    if (f != INVALID_HANDLE_VALUE)
        CloseHandle(f);
    return rc;
}

/* ── ext4（mke2fs.exe） ──────────────────────────────────────────── */

/* 用 platform-tools 的 mke2fs.exe 生成一个 android_sparse 格式的 ext4 镜像（fastboot -w 的同一种做法）。
 * mke2fs 的输出转到我们的 stderr（stdout 只留给协议记录）。 */
static int make_ext4(const wchar_t *exe, const wchar_t *out, uint64_t blocks, const wchar_t *label, int no_reserve)
{
    wchar_t conf[MAX_PATH], cmd[MAX_PATH * 3], *slash;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE herr = NULL, hnul;
    DWORD code = 1;
    if (wcslen(exe) >= MAX_PATH)
        return -1;
    wcscpy(conf, exe);
    slash = wcsrchr(conf, L'\\');
    if (!slash)
        return -1;
    wcscpy(slash + 1, L"mke2fs.conf");
    if (!gk3w_exists(conf) || !SetEnvironmentVariableW(L"MKE2FS_CONFIG", conf))
        return -1;
    DeleteFileW(out);
    _snwprintf(cmd, MAX_PATH * 3, L"\"%ls\" -q -t ext4 -b 4096 -I 256 -E android_sparse -O uninit_bg %ls-L %ls \"%ls\" %llu", exe,
               no_reserve ? L"-m 0 " : L"", label, out, (unsigned long long)blocks);
    hnul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    if (!DuplicateHandle(GetCurrentProcess(), GetStdHandle(STD_ERROR_HANDLE), GetCurrentProcess(), &herr, 0, TRUE,
                         DUPLICATE_SAME_ACCESS))
        herr = NULL;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = hnul;
    si.hStdOutput = herr ? herr : hnul;
    si.hStdError = herr ? herr : hnul;
    if (CreateProcessW(exe, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        if (WaitForSingleObject(pi.hProcess, 300000) != WAIT_OBJECT_0)
            TerminateProcess(pi.hProcess, 1);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    if (herr)
        CloseHandle(herr);
    if (hnul != INVALID_HANDLE_VALUE)
        CloseHandle(hnul);
    return code == 0 && gk3w_exists(out) ? 0 : -1;
}

/* 分区开头是不是一个卷标为 label 的 ext4（超级块在 1024：魔数 0xEF53 @0x38、卷标 @0x78） */
static int check_ext4(HANDLE h, uint64_t off, const char *label, uint8_t *buf4k)
{
    if (gk3w_pread(h, buf4k, 4096, off))
        return -1;
    return buf4k[1024 + 0x38] == 0x53 && buf4k[1024 + 0x39] == 0xEF && !strncmp((char *)buf4k + 1024 + 0x78, label, 16) ? 0 : -1;
}

/* ── ESP ──────────────────────────────────────────────────────────── */

typedef struct {
    wchar_t root[MAX_PATH];     /* \\?\Volume{…}\ */
    size_t root_len;
    char mid[33];
    int windows;                /* EFI\Microsoft\Boot\bootmgfw.efi 在 */
} esp_ctx;

static void esp_path(const esp_ctx *e, const char *rel, wchar_t *out, size_t cap)
{
    wchar_t w[MAX_PATH];
    gk3w_u2w(rel, w, MAX_PATH);
    for (wchar_t *p = w; *p; p++)
        if (*p == L'/')
            *p = L'\\';
    _snwprintf(out, cap, L"%ls%ls", e->root, w);
}

/* ESP 上 rel 这个文件与 data 是否逐字节相同；不存在算不同 */
static int esp_same(const esp_ctx *e, const char *rel, const uint8_t *data, size_t len)
{
    wchar_t p[MAX_PATH * 2];
    uint8_t *old;
    size_t olen;
    int same;
    esp_path(e, rel, p, MAX_PATH * 2);
    if (gk3w_read_file(p, &old, &olen, 256u << 20))
        return 0;
    same = olen == len && !memcmp(old, data, len);
    free(old);
    return same;
}

static int64_t esp_size_of(const esp_ctx *e, const char *rel)
{
    wchar_t p[MAX_PATH * 2];
    WIN32_FILE_ATTRIBUTE_DATA a;
    esp_path(e, rel, p, MAX_PATH * 2);
    if (!GetFileAttributesExW(p, GetFileExInfoStandard, &a))
        return -1;
    return (int64_t)(((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow);
}

/* 往 ESP 上写这一组文件要多占多少 KiB（同名旧文件只算变大的部分，按 4 KiB 簇向上取整）—— gk3__esp_delta_kib */
static int64_t esp_delta(const esp_ctx *e, const char *rel, uint64_t newsz)
{
    int64_t old = esp_size_of(e, rel);
    if (old < 0)
        old = 0;
    return (int64_t)newsz > old ? ((int64_t)newsz - old + 4095) / 4096 * 4 : 0;
}

static int esp_has_android_entries(const esp_ctx *e)
{
    wchar_t pat[MAX_PATH * 2];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    _snwprintf(pat, MAX_PATH * 2, L"%lsloader\\entries\\*-android-*.conf", e->root);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    FindClose(h);
    return 1;
}

/* ESP 根目录下第一个（按字节序）32 位十六进制的目录名 —— gk3__esp_pick_mid */
static int esp_pick_mid(const esp_ctx *e, char out[33])
{
    wchar_t pat[MAX_PATH * 2];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int found = 0;
    _snwprintf(pat, MAX_PATH * 2, L"%ls*", e->root);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    do {
        char n[MAX_PATH * 3];
        gk3w_w2u(fd.cFileName, n, sizeof n);
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && gk3w_is_machine_id(n) && (!found || strcmp(n, out) < 0)) {
            memcpy(out, n, 33);
            found = 1;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

static int esp_write(const esp_ctx *e, const char *rel, const uint8_t *data, size_t len)
{
    wchar_t p[MAX_PATH * 2], dir[MAX_PATH * 2], *slash;
    int w;
    esp_path(e, rel, p, MAX_PATH * 2);
    wcscpy(dir, p);
    slash = wcsrchr(dir, L'\\');
    if (slash && (size_t)(slash - dir) > e->root_len) {
        *slash = 0;
        if (gk3w_mkdirs(dir, e->root_len))
            return gk3w_err_kv(GK3W_TOUCHED_YES, "esp-write", "file", rel, "在 ESP 上建目录失败", GetLastError());
    }
    w = gk3w_write_file_verify(p, data, len);
    if (w)
        return gk3w_err_kv(GK3W_TOUCHED_YES, w == -1 ? "esp-write" : "esp-verify", "file", rel,
                           w == -1 ? "写 ESP 失败：ESP 空间不够，或者介质出了错" : "ESP 上的文件读回不一致", GetLastError());
    return 0;
}

/* ── apply ────────────────────────────────────────────────────────── */

int gk3w_cmd_apply(int argc, wchar_t **argv)
{
    uint64_t num, rstart, rend, ud_mib = 0;
    const wchar_t *rel = gk3w_arg(argc, argv, L"--release"), *sdboot_path = gk3w_arg(argc, argv, L"--sdboot"),
                  *backup_dir = gk3w_arg(argc, argv, L"--backup-dir"), *mid_arg = gk3w_arg(argc, argv, L"--machine-id"),
                  *defos_arg = gk3w_arg(argc, argv, L"--default-os"), *mke2fs = gk3w_arg(argc, argv, L"--mke2fs");
    wchar_t img_meta[MAX_PATH] = L"", img_data[MAX_PATH] = L"";
    wchar_t p_boot[MAX_PATH], p_super[MAX_PATH], p_sha[MAX_PATH];
    bootimg bi;
    uint8_t *sdboot = NULL, *man = NULL, *zero = NULL, *chk = NULL, *bootbuf = NULL;
    size_t sdboot_len = 0, man_len = 0;
    gk3w_disk d;
    esp_ctx esp;
    gk3w_plan plan;
    GUID ids[GK3W_PLAN_MAX];
    uint64_t poff[GK3W_PLAN_MAX];
    const char *why = NULL;
    int defos_windows = 0, rc = 1, opened = 0;
    char mid_fb[33];
    gk3w_touched T = GK3W_TOUCHED_NO;
    const PARTITION_INFORMATION_EX *esp_part = NULL;

    memset(&bi, 0, sizeof bi);
    memset(&esp, 0, sizeof esp);
    if (gk3w_arg_u64(argc, argv, L"--disk", &num) || num > 255 || gk3w_arg_u64(argc, argv, L"--region-start", &rstart) ||
        gk3w_arg_u64(argc, argv, L"--region-end", &rend) || !rel || !sdboot_path || !backup_dir ||
        (gk3w_arg(argc, argv, L"--userdata-mib") && gk3w_arg_u64(argc, argv, L"--userdata-mib", &ud_mib)))
        return gk3w_err(T, "usage",
                        "用法：apply --disk N --region-start LBA --region-end LBA --release DIR --sdboot FILE --backup-dir DIR "
                        "[--mke2fs PATH] [--machine-id HEX32] [--default-os android|windows] [--userdata-mib N]",
                        0);
    if (defos_arg && wcscmp(defos_arg, L"android") && wcscmp(defos_arg, L"windows"))
        return gk3w_err(T, "usage", "--default-os 只能是 android 或 windows", 0);
    defos_windows = defos_arg && !wcscmp(defos_arg, L"windows");
    if (mid_arg) {
        gk3w_w2u(mid_arg, mid_fb, sizeof mid_fb);
        if (!gk3w_is_machine_id(mid_fb))
            return gk3w_err(T, "usage", "--machine-id 要 32 位小写十六进制", 0);
    } else {
        uint8_t r[16];
        if (gk3w_random(r, sizeof r))
            return gk3w_err(T, "random", "生成 machine-id 失败", 0);
        gk3w_hex(r, 16, mid_fb);
    }

    /* ── 输入：全部在动盘之前验完 ── */
    gk3w_progress(1, "check");
    _snwprintf(p_boot, MAX_PATH, L"%ls\\boot.img", rel);
    _snwprintf(p_super, MAX_PATH, L"%ls\\super.img.zst", rel);
    _snwprintf(p_sha, MAX_PATH, L"%ls\\install-artifacts.sha256", rel);
    if (!gk3w_exists(p_super)) {
        gk3w_err(T, "release-no-super", "发布目录里没有 super.img.zst", 0);
        goto out;
    }
    if (gk3w_read_file(sdboot_path, &sdboot, &sdboot_len, 16u << 20) || sdboot_len < 2 || memcmp(sdboot, "MZ", 2)) {
        gk3w_err(T, "tool-missing", "systemd-boot（--sdboot）读不到或不是 PE 文件", 0);
        goto out;
    }
    if (gk3w_read_file(p_sha, &man, &man_len, 1u << 20) == 0) {
        static const char *const names[] = {"boot.img", "super.img.zst"};
        const wchar_t *paths[] = {p_boot, p_super};
        gk3w_progress(1, "verify-sha");
        for (int i = 0; i < 2; i++) {
            char want[65], got[65];
            if (manifest_hash(man, man_len, names[i], want))
                continue;
            if (file_sha256(paths[i], got) || strcmp(want, got)) {
                gk3w_err_kv(T, "sha-mismatch", "file", names[i], "下载的文件与发版清单的 sha256 不一致 —— 重新下载（盘还没动过）", 0);
                goto out;
            }
            gk3w_log("%s：sha256 与发版清单一致", names[i]);
        }
    } else {
        gk3w_log("发布目录里没有 install-artifacts.sha256，跳过 sha256 核对");
    }
    if (load_boot(p_boot, &bi))
        goto out;
    gk3w_progress(1, "test-zst");
    if (run_image(p_super, 1, (uint64_t)GK3W_SUPER_MIB << 20, NULL, 0, &why)) {
        gk3w_err_kv(T, "zst-corrupt", "why", why, "super.img.zst 不完整或不是一份合法的 sparse 镜像 —— 重新下载（盘还没动过）", 0);
        goto out;
    }
    gk3w_log("super.img.zst：完整试解通过");

    /* ── 盘与 ESP ── */
    if (gk3w_disk_open_vhd((DWORD)num, &d, T))
        goto out;
    opened = 1;
    {
        int n_esp = 0;
        for (DWORD i = 0; i < d.lay->PartitionCount; i++) {
            const PARTITION_INFORMATION_EX *p = &d.lay->PartitionEntry[i];
            if (p->PartitionStyle == PARTITION_STYLE_GPT && p->PartitionLength.QuadPart &&
                !memcmp(&p->Gpt.PartitionType, &GK3W_ESP_TYPE, sizeof(GUID))) {
                esp_part = p;
                n_esp++;
            }
        }
        if (n_esp != 1) {
            gk3w_err(T, n_esp ? "esp-ambiguous" : "esp-missing", n_esp ? "这块盘上有不止一个 EFI 分区" : "这块盘上没有 EFI 分区", 0);
            goto out;
        }
    }
    if (gk3w_find_volume(d.num, (uint64_t)esp_part->StartingOffset.QuadPart, esp.root, MAX_PATH)) {
        gk3w_err(T, "esp-mount", "找不到 EFI 分区对应的卷 —— 不敢往一个读不了的 ESP 上装引导链（盘还没动过）", 0);
        goto out;
    }
    esp.root_len = wcslen(esp.root);
    {
        wchar_t fs[32] = L"", p[MAX_PATH * 2];
        if (!GetVolumeInformationW(esp.root, NULL, 0, NULL, NULL, NULL, fs, 32) || _wcsicmp(fs, L"FAT32")) {
            gk3w_err(T, "esp-not-fat", "EFI 分区不是 FAT32（盘还没动过）", GetLastError());
            goto out;
        }
        _snwprintf(p, MAX_PATH * 2, L"%lsEFI\\Microsoft\\Boot\\bootmgfw.efi", esp.root);
        esp.windows = gk3w_exists(p);
    }
    if (defos_windows && !esp.windows) {
        gk3w_err(T, "default-os-no-windows", "EFI 分区上没有 Windows 的启动管理器，不能把 Windows 设成默认系统（盘还没动过）", 0);
        goto out;
    }
    if (esp_has_android_entries(&esp)) {
        gk3w_err(T, "esp-has-android", "EFI 分区上已经有 Android 的启动项（重新安装不在原型范围内）（盘还没动过）", 0);
        goto out;
    }
    if (!esp_pick_mid(&esp, esp.mid))
        memcpy(esp.mid, mid_fb, 33);
    {
        ULARGE_INTEGER avail;
        gk3w_esp_space sp;
        gk3w_esp_check ck;
        int64_t need = 256;
        char rel_path[96];
        uint64_t sizes[3] = {bi.b.kernel_size, bi.b.dtb_size, bi.b.ramdisk_size};
        static const char *const fn[3] = {"Image", "gaokun3.dtb", "ramdisk.img"};
        int64_t aa = esp_size_of(&esp, "EFI/BOOT/BOOTAA64.EFI");
        need += esp_delta(&esp, "EFI/BOOT/BOOTAA64.EFI", sdboot_len);
        need += esp_delta(&esp, "EFI/systemd/systemd-bootaa64.efi", sdboot_len);
        for (char sl = 'a'; sl <= 'b'; sl++)
            for (int k = 0; k < 3; k++) {
                snprintf(rel_path, sizeof rel_path, "%s/android/slot_%c/%s", esp.mid, sl, fn[k]);
                need += esp_delta(&esp, rel_path, sizes[k]);
            }
        if (aa >= 0 && esp_size_of(&esp, "EFI/BOOT/BOOTAA64.EFI.before-gaokun3") < 0 &&
            !esp_same(&esp, "EFI/BOOT/BOOTAA64.EFI", sdboot, sdboot_len))
            need += aa / 1024 + 4;
        if (!GetDiskFreeSpaceExW(esp.root, &avail, NULL, NULL)) {
            gk3w_err(T, "esp-mount", "读不到 EFI 分区的空闲空间（盘还没动过）", GetLastError());
            goto out;
        }
        sp.esp_size_mib = (uint64_t)esp_part->PartitionLength.QuadPart >> 20;
        sp.free_kib = (int64_t)(avail.QuadPart / 1024);
        sp.need_kib = need;
        sp.slot_kib = (int64_t)((sizes[0] + sizes[1] + sizes[2]) / 1024);
        sp.windows = esp.windows;
        gk3w_log("ESP 上用目录 %s；要写 %lld MiB（已扣掉会被覆盖的同名文件），空闲 %lld MiB；Windows 的启动管理器：%s",
                 esp.mid, (long long)(need / 1024), (long long)(sp.free_kib / 1024), esp.windows ? "yes" : "no");
        ck = gk3w_esp_space_check(&sp);
        if (ck != GK3W_ESP_OK) {
            gk3w_err(T, gk3w_esp_check_code(ck), "EFI 分区空间不够（详见代码；盘还没动过）", 0);
            goto out;
        }
    }

    /* ── 分区名查重、区间核对、方案 ── */
    gk3w_progress(2, "plan");
    {
        char dup[128] = "";
        gk3w_span parts[GK3W_MAX_PARTS], spans[GK3W_MAX_PARTS + 1];
        size_t np = 0, nf = 0;
        uint64_t uf, ul;
        int inside = 0;
        gk3w_plan_req req;
        gk3w_plan_err pe;
        for (DWORD i = 0; i < d.lay->PartitionCount; i++) {
            const PARTITION_INFORMATION_EX *p = &d.lay->PartitionEntry[i];
            wchar_t wn[GK3_GPT_NAME_CHARS + 1];
            char n[GK3_GPT_NAME_CHARS * 3 + 1];
            if (p->PartitionStyle != PARTITION_STYLE_GPT || !p->PartitionLength.QuadPart)
                continue;
            if (np < GK3W_MAX_PARTS)
                parts[np++] = (gk3w_span){(uint64_t)p->StartingOffset.QuadPart / d.bs,
                                          ((uint64_t)p->StartingOffset.QuadPart + (uint64_t)p->PartitionLength.QuadPart) / d.bs - 1};
            memcpy(wn, p->Gpt.Name, sizeof p->Gpt.Name);
            wn[GK3_GPT_NAME_CHARS] = 0;
            gk3w_w2u(wn, n, sizeof n);
            for (size_t k = 0; k < sizeof OUR_NAMES / sizeof OUR_NAMES[0]; k++)
                if (!strcmp(n, OUR_NAMES[k]) && strlen(dup) + strlen(n) + 2 < sizeof dup) {
                    if (dup[0])
                        strcat(dup, ",");
                    strcat(dup, n);
                }
        }
        if (dup[0]) {
            gk3w_err_kv(T, "partlabel-conflict", "names", dup, "盘上已经有同名的分区（Android 开机会随机挂到其中一个）（盘还没动过）", 0);
            goto out;
        }
        gk3w_disk_usable(&d, &uf, &ul);
        if (gk3w_free_spans(uf, ul, parts, np, d.bs, 0, spans, GK3W_MAX_PARTS + 1, &nf) != GK3W_LAYOUT_OK) {
            gk3w_err(T, "disk-layout", "现有分区表有越界或重叠（盘还没动过）", 0);
            goto out;
        }
        for (size_t i = 0; i < nf && i < GK3W_MAX_PARTS + 1; i++)
            if (rstart >= spans[i].first && rend <= spans[i].last && rstart <= rend)
                inside = 1;
        if (!inside) {
            gk3w_err(T, "bad-region", "选的区间不是一段空闲空间（盘还没动过）", 0);
            goto out;
        }
        req.sector = d.bs;
        req.region = (gk3w_span){rstart, rend};
        req.userdata_mib = ud_mib;
        req.rescue = 0;
        pe = gk3w_plan_alongside(&req, &plan);
        if (pe != GK3W_PLAN_OK) {
            gk3w_line l = {0};
            gk3w_line_begin(&l, "PLANERR");
            gk3w_line_kv(&l, "msg", gk3w_plan_strerror(pe));
            gk3w_line_u64(&l, "avail_mib", plan.avail_mib);
            gk3w_line_u64(&l, "need_mib", plan.need_mib);
            gk3w_line_u64(&l, "fixed_mib", plan.fixed_mib);
            (void)gk3w_line_emit(&l, stdout);
            gk3w_line_free(&l);
            gk3w_err_kv(T, "plan", "why", gk3w_plan_strerror(pe), "这段空闲空间放不下（盘还没动过）", 0);
            goto out;
        }
        for (size_t i = 0; i < plan.n; i++) {
            gk3w_line l = {0};
            gk3w_line_begin(&l, "PLAN");
            gk3w_line_kv(&l, "op", "mkpart");
            gk3w_line_u64(&l, "disk", d.num);
            gk3w_line_kv(&l, "name", plan.parts[i].name);
            gk3w_line_u64(&l, "start", plan.parts[i].first);
            gk3w_line_u64(&l, "end", plan.parts[i].last);
            gk3w_line_u64(&l, "size_mib", plan.parts[i].mib);
            gk3w_line_kv(&l, "type", "8300");
            (void)gk3w_line_emit(&l, stdout);
            gk3w_line_free(&l);
        }
    }

    /* ── metadata / userdata 的 ext4 镜像：也在动盘之前生成并完整试解 ── */
    if (mke2fs) {
        wchar_t tmp[MAX_PATH];
        struct {
            const char *name;
            wchar_t *img;
            const wchar_t *label;
            int no_reserve;
        } fs[2] = {{"metadata", img_meta, L"metadata", 0}, {"userdata", img_data, L"userdata", 1}};
        if (!GetTempPathW(MAX_PATH, tmp)) {
            gk3w_err(T, "mkfs", "取不到临时目录（盘还没动过）", GetLastError());
            goto out;
        }
        for (int k = 0; k < 2; k++) {
            uint64_t bytes = 0;
            for (size_t i = 0; i < plan.n; i++)
                if (!strcmp(plan.parts[i].name, fs[k].name))
                    bytes = (plan.parts[i].last - plan.parts[i].first + 1) * d.bs;
            _snwprintf(fs[k].img, MAX_PATH, L"%lsgk3w-%lu-%ls.simg", tmp, (unsigned long)GetCurrentProcessId(), fs[k].label);
            if (!bytes || make_ext4(mke2fs, fs[k].img, bytes / 4096, fs[k].label, fs[k].no_reserve) ||
                run_image(fs[k].img, 0, bytes, NULL, 0, &why)) {
                gk3w_err_kv(T, "mkfs", "part", fs[k].name, "用 mke2fs 生成 ext4 镜像失败（盘还没动过）", GetLastError());
                goto out;
            }
        }
        gk3w_log("mke2fs：metadata 与 userdata 的 ext4 镜像已生成并试解通过");
    }

    /* ── 分区表备份（GPT 主表区 + 备份表区，原样） ── */
    zero = _aligned_malloc(IO_CHUNK, 4096);
    chk = _aligned_malloc(IO_CHUNK, 4096);
    if (!zero || !chk) {
        gk3w_err(T, "nomem", "内存不足", 0);
        goto out;
    }
    {
        uint64_t uf, ul, total = d.size / d.bs, head, tail;
        wchar_t bpath[MAX_PATH * 2];
        SYSTEMTIME st;
        uint8_t *gb;
        int w;
        gk3w_disk_usable(&d, &uf, &ul);
        head = uf;
        tail = total - (ul + 1);
        if ((head + tail) * d.bs > IO_CHUNK) {
            gk3w_err(T, "gpt-backup", "分区表区大得反常，不敢继续（盘还没动过）", 0);
            goto out;
        }
        gb = zero;   /* 借用：此时还没用到 */
        if (gk3w_pread(d.h, gb, (DWORD)(head * d.bs), 0) ||
            gk3w_pread(d.h, gb + head * d.bs, (DWORD)(tail * d.bs), (ul + 1) * d.bs)) {
            gk3w_err(T, "gpt-backup", "读分区表区失败（盘还没动过）", GetLastError());
            goto out;
        }
        GetLocalTime(&st);
        _snwprintf(bpath, MAX_PATH * 2, L"%ls\\gpt-before-apply-disk%lu-%04u%02u%02u-%02u%02u%02u-head%llu-tail%llu.bin",
                   backup_dir, (unsigned long)d.num, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                   (unsigned long long)head, (unsigned long long)tail);
        gk3w_mkdirs(backup_dir, (backup_dir[0] && backup_dir[1] == L':') ? 2 : 0);
        w = gk3w_write_file_verify(bpath, gb, (size_t)((head + tail) * d.bs));
        if (w) {
            gk3w_err(T, "gpt-backup", "写分区表备份失败（盘还没动过）", GetLastError());
            goto out;
        }
        {
            char b8[MAX_PATH * 3];
            gk3w_w2u(bpath, b8, sizeof b8);
            gk3w_log("分区表备份：%s", b8);
        }
        memset(zero, 0, IO_CHUNK);
    }

    /* ══ 从这里起开始动盘 ══ */
    T = GK3W_TOUCHED_YES;
    gk3w_progress(5, "write-gpt");
    {
        DWORD count = 0;
        DRIVE_LAYOUT_INFORMATION_EX *lay =
            calloc(1, offsetof(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry) +
                          (d.lay->PartitionCount + plan.n) * sizeof(PARTITION_INFORMATION_EX));
        if (!lay) {
            gk3w_err(T, "nomem", "内存不足", 0);
            goto out;
        }
        lay->PartitionStyle = PARTITION_STYLE_GPT;
        lay->Gpt = d.lay->Gpt;
        for (DWORD i = 0; i < d.lay->PartitionCount; i++) {
            const PARTITION_INFORMATION_EX *p = &d.lay->PartitionEntry[i];
            if (p->PartitionStyle == PARTITION_STYLE_GPT && p->PartitionLength.QuadPart)
                lay->PartitionEntry[count++] = *p;
        }
        if (count + plan.n > d.lay->Gpt.MaxPartitionCount) {
            free(lay);
            gk3w_err(GK3W_TOUCHED_NO, "gpt-full", "分区表已满（盘还没动过）", 0);
            goto out;
        }
        for (size_t i = 0; i < plan.n; i++) {
            PARTITION_INFORMATION_EX *e = &lay->PartitionEntry[count++];
            wchar_t wn[GK3_GPT_NAME_CHARS + 1];
            memset(e, 0, sizeof *e);
            e->PartitionStyle = PARTITION_STYLE_GPT;
            e->StartingOffset.QuadPart = (LONGLONG)(plan.parts[i].first * d.bs);
            e->PartitionLength.QuadPart = (LONGLONG)((plan.parts[i].last - plan.parts[i].first + 1) * d.bs);
            e->Gpt.PartitionType = GK3W_LINUX_DATA;
            if (UuidCreate(&e->Gpt.PartitionId) != RPC_S_OK) {
                free(lay);
                gk3w_err(GK3W_TOUCHED_NO, "uuid", "生成分区 GUID 失败（盘还没动过）", 0);
                goto out;
            }
            gk3w_u2w(plan.parts[i].name, wn, GK3_GPT_NAME_CHARS + 1);
            wcsncpy(e->Gpt.Name, wn, GK3_GPT_NAME_CHARS);
            ids[i] = e->Gpt.PartitionId;
        }
        if (gk3w_disk_set_layout(&d, lay, count, T)) {
            free(lay);
            goto out;
        }
        free(lay);
        if (gk3w_disk_read_layout(&d)) {
            gk3w_err(T, "verify-layout", "写完分区表后重读失败", GetLastError());
            goto out;
        }
        for (size_t i = 0; i < plan.n; i++) {
            const PARTITION_INFORMATION_EX *p = gk3w_disk_find_part(&d, &ids[i]);
            if (!p || (uint64_t)p->StartingOffset.QuadPart != plan.parts[i].first * d.bs ||
                (uint64_t)p->PartitionLength.QuadPart != (plan.parts[i].last - plan.parts[i].first + 1) * d.bs) {
                gk3w_err_kv(T, "verify-layout", "name", plan.parts[i].name, "写完分区表后重读，新分区与方案不符", 0);
                goto out;
            }
            poff[i] = (uint64_t)p->StartingOffset.QuadPart;
        }
    }

    {
        size_t i_misc = SIZE_MAX, i_meta = SIZE_MAX, i_boota = SIZE_MAX, i_bootb = SIZE_MAX, i_super = SIZE_MAX,
               i_data = SIZE_MAX;
        for (size_t i = 0; i < plan.n; i++) {
            const char *n = plan.parts[i].name;
            if (!strcmp(n, "misc")) i_misc = i;
            else if (!strcmp(n, "metadata")) i_meta = i;
            else if (!strcmp(n, "boot_a")) i_boota = i;
            else if (!strcmp(n, "boot_b")) i_bootb = i;
            else if (!strcmp(n, "super")) i_super = i;
            else if (!strcmp(n, "userdata")) i_data = i;
        }

        /* ── 15 format：清零 + misc 初始状态 ── */
        gk3w_progress(15, "format");
        {
            struct {
                size_t idx;
                uint64_t bytes;
            } z[] = {{i_misc, (uint64_t)GK3W_MISC_MIB << 20}, {i_meta, (uint64_t)GK3W_METADATA_MIB << 20}, {i_data, ZERO_HEAD}};
            /* 用 mke2fs 时 userdata 不先清零：镜像里没写的块与 fastboot -w 一样留着（ext4 用不到它们） */
            for (size_t k = 0; k < sizeof z / sizeof z[0] - (mke2fs ? 1 : 0); k++)
                for (uint64_t off = 0; off < z[k].bytes; off += IO_CHUNK) {
                    DWORD n = (DWORD)(z[k].bytes - off < IO_CHUNK ? z[k].bytes - off : IO_CHUNK);
                    int w = gk3w_write_verify(d.h, zero, n, poff[z[k].idx] + off, chk);
                    if (w) {
                        gk3w_err_kv(T, w == -1 ? "disk-write" : "disk-verify", "part", plan.parts[z[k].idx].name,
                                    w == -1 ? "清零失败" : "清零后读回不是零", GetLastError());
                        goto out;
                    }
                }
            if (!mke2fs)
                gk3w_log("metadata 整个清零、userdata 开头 %u MiB 清零：首次开机由 fstab 的 formattable 格式化", ZERO_HEAD >> 20);
        }
        if (mke2fs) {
            struct {
                size_t idx;
                const wchar_t *img;
                const char *label;
            } fs[2] = {{i_meta, img_meta, "metadata"}, {i_data, img_data, "userdata"}};
            for (int k = 0; k < 2; k++) {
                super_sink ss = {d.h, poff[fs[k].idx], chk, _aligned_malloc(IO_CHUNK, 4096), 0};
                gk3w_sparse_sink sink = {&ss, ss_write, ss_fill};
                uint64_t bytes = (plan.parts[fs[k].idx].last - plan.parts[fs[k].idx].first + 1) * d.bs;
                int r = ss.pat ? run_image(fs[k].img, 0, bytes, &sink, 0, &why) : -1;
                _aligned_free(ss.pat);
                if (r || check_ext4(d.h, poff[fs[k].idx], fs[k].label, zero)) {
                    gk3w_err_kv(T, "mkfs", "part", fs[k].label, r ? "写 ext4 镜像失败" : "写完之后分区开头不是预期的 ext4", GetLastError());
                    goto out;
                }
            }
            memset(zero, 0, IO_CHUNK);
            gk3w_log("metadata、userdata：ext4 已写入（mke2fs 生成，写后读回核对、超级块魔数与卷标正确）");
        }
        {
            uint8_t head[GK3_MISC_INIT_HEAD_SIZE], rec[GK3_REC_SIZE];
            gk3_setdef def = esp.windows ? (defos_windows ? GK3_SETDEF_WINDOWS : GK3_SETDEF_ANDROID) : GK3_SETDEF_NONE;
            uint8_t *m = zero;   /* 64 KiB：BCB + BCAB 在 0，GK3 记录在 8 KiB，其余是零 */
            char hex[65];
            int w;
            if (gk3_misc_build_install(head, rec, 0, def)) {
                gk3w_err(T, "misc-init", "内部错误：misc 初始状态自检不过", 0);
                goto out;
            }
            memset(m, 0, GK3_MISC_READ_SIZE);
            memcpy(m, head, sizeof head);
            memcpy(m + GK3_MISC_GK3_OFF, rec, sizeof rec);
            w = gk3w_write_verify(d.h, m, GK3_MISC_READ_SIZE, poff[i_misc], chk);
            if (w) {
                gk3w_err(T, "misc-init", "写 misc 初始状态失败", GetLastError());
                goto out;
            }
            gk3w_hex(head + GK3_MISC_BCAB_OFF, GK3_MISC_BCAB_SIZE, hex);
            gk3w_log("misc：MISCINIT slot=a default=%s bcab=%s", def == GK3_SETDEF_WINDOWS ? "windows" : def == GK3_SETDEF_ANDROID ? "android" : "none", hex);
            memset(zero, 0, IO_CHUNK);
        }

        /* ── 30 write-super ── */
        gk3w_progress(30, "write-super");
        {
            super_sink ss = {d.h, poff[i_super], chk, _aligned_malloc(IO_CHUNK, 4096), 0};
            gk3w_sparse_sink sink = {&ss, ss_write, ss_fill};
            uint8_t *lp = zero;
            if (!ss.pat) {
                gk3w_err(T, "nomem", "内存不足", 0);
                goto out;
            }
            if (run_image(p_super, 1, (uint64_t)GK3W_SUPER_MIB << 20, &sink, 1, &why)) {
                _aligned_free(ss.pat);
                gk3w_err_kv(T, "super-write", "why", why, ss.failed == -2 ? "super 写后读回不一致" : "super 写入失败", GetLastError());
                goto out;
            }
            _aligned_free(ss.pat);
            /* 偏移 4096 处必须是 LP geometry 魔数（installer-lib.sh gk3__write_super 的同一道检查） */
            if (gk3w_pread(d.h, lp, 4096, poff[i_super] + 4096) || memcmp(lp, "gDla", 4)) {
                gk3w_err(T, "super-bad-lp", "super 偏移 4096 处不是 LP geometry 魔数 —— 写进去的不是一份能用的 super", 0);
                goto out;
            }
            gk3w_log("super：LP geometry 魔数正确");
            memset(zero, 0, IO_CHUNK);
        }

        /* ── 70 write-boot ── */
        gk3w_progress(70, "write-boot");
        {
            size_t padded = (bi.len + d.bs - 1) / d.bs * d.bs;
            uint8_t *c2 = _aligned_malloc(padded, 4096);
            size_t targets[2] = {i_boota, i_bootb};
            bootbuf = _aligned_malloc(padded, 4096);
            if (!bootbuf || !c2) {
                _aligned_free(c2);
                gk3w_err(T, "nomem", "内存不足", 0);
                goto out;
            }
            memset(bootbuf, 0, padded);
            memcpy(bootbuf, bi.img, bi.len);
            for (int k = 0; k < 2; k++) {
                int w = gk3w_write_verify(d.h, bootbuf, (DWORD)padded, poff[targets[k]], c2);
                if (w) {
                    _aligned_free(c2);
                    gk3w_err_kv(T, w == -1 ? "disk-write" : "disk-verify", "part", plan.parts[targets[k]].name,
                                w == -1 ? "写 boot 失败" : "boot 写后读回不一致", GetLastError());
                    goto out;
                }
            }
            _aligned_free(c2);
        }
    }

    /* ── 80 bootloader：ESP ── */
    gk3w_progress(80, "bootloader");
    {
        static const char *const fn[3] = {"Image", "gaokun3.dtb", "ramdisk.img"};
        const uint8_t *src[3] = {bi.img + bi.b.kernel_off, bi.img + bi.b.dtb_off, bi.img + bi.b.ramdisk_off};
        size_t sz[3] = {bi.b.kernel_size, bi.b.dtb_size, bi.b.ramdisk_size};
        static const char *const backups[2] = {"EFI/BOOT/BOOTAA64.EFI", "loader/loader.conf"};
        char relp[160], entry[GK3_BOOT_ARGS_SIZE + GK3_BOOT_EXTRA_ARGS_SIZE + 512];
        /* 原件留一份：只在第一次装时留，且与这份 systemd-boot 不同时才留（gk3_apply 同一条规矩） */
        for (int k = 0; k < 2; k++) {
            uint8_t *old;
            size_t olen;
            wchar_t p[MAX_PATH * 2];
            snprintf(relp, sizeof relp, "%s.before-gaokun3", backups[k]);
            esp_path(&esp, backups[k], p, MAX_PATH * 2);
            if (esp_size_of(&esp, relp) >= 0 || gk3w_read_file(p, &old, &olen, 64u << 20))
                continue;
            if (!(olen == sdboot_len && !memcmp(old, sdboot, olen))) {
                if (esp_write(&esp, relp, old, olen)) {
                    free(old);
                    goto out;
                }
                gk3w_log("原有的 %s 已备份为 %s", backups[k], relp);
            }
            free(old);
        }
        /* 只在字节不同时写（BitLocker 量的是固件每次加载的这个文件） */
        for (int k = 0; k < 2; k++) {
            const char *t = k ? "EFI/systemd/systemd-bootaa64.efi" : "EFI/BOOT/BOOTAA64.EFI";
            if (esp_same(&esp, t, sdboot, sdboot_len))
                gk3w_log("%s 已是这一份 systemd-boot，不重写", t);
            else if (esp_write(&esp, t, sdboot, sdboot_len))
                goto out;
        }
        for (char sl = 'a'; sl <= 'b'; sl++) {
            int n;
            for (int k = 0; k < 3; k++) {
                snprintf(relp, sizeof relp, "%s/android/slot_%c/%s", esp.mid, sl, fn[k]);
                if (esp_write(&esp, relp, src[k], sz[k]))
                    goto out;
            }
            snprintf(relp, sizeof relp, "loader/entries/%s-android-%c.conf", esp.mid, sl);
            n = gk3w_esp_android_entry(entry, sizeof entry, esp.mid, sl, bi.cmdline);
            if (n < 0) {
                gk3w_err(T, "esp-write", "内部错误：启动项生成失败", 0);
                goto out;
            }
            if (esp_write(&esp, relp, (const uint8_t *)entry, (size_t)n))
                goto out;
        }
        if (esp_write(&esp, "loader/loader.conf", (const uint8_t *)gk3w_loader_conf, strlen(gk3w_loader_conf)))
            goto out;
        gk3w_log("ESP：systemd-boot、两个槽的内核 / dtb / ramdisk、两个启动项、loader.conf 都已写好并读回核对");
    }
    {
        gk3w_line l = {0};
        gk3w_line_begin(&l, "NOTE");
        gk3w_line_kv(&l, "code", "efivars-skipped");
        gk3w_line_kv(&l, "why", "vhd");
        (void)gk3w_line_emit(&l, stdout);
        gk3w_line_free(&l);
    }
    gk3w_progress(100, "done");
    rc = 0;
out:
    if (img_meta[0])
        DeleteFileW(img_meta);
    if (img_data[0])
        DeleteFileW(img_data);
    if (opened)
        gk3w_disk_close(&d);
    free(bi.img);
    free(sdboot);
    free(man);
    _aligned_free(zero);
    _aligned_free(chk);
    _aligned_free(bootbuf);
    return rc;
}
