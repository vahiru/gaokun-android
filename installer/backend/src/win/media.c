/* Windows：制作独立安装介质（界面上的"制作独立安装介质"）。
 *
 *   gk3w media --target usb --disk N --expect-mib M --livecd <ISO> --release <目录>
 *
 * 把 LiveCD 混合 ISO（installer/tools/build/build-iso.sh）里的文件，连同发布目录里的 boot.img、super.img.zst、
 * install-artifacts.sha256（放进 gaokun3/payload/），写进一个 FAT32 卷，卷标 GK3LIVE。
 *
 * U 盘：整个清空，建 MBR 与一个 FAT32（LBA，0x0C）分区，起点 1 MiB、占满整个 U 盘。这是 Windows 格式化 U 盘时的样子，
 *   把 ISO 里的文件拖进这样的 U 盘已上机验证能启动。只认 U 盘（总线 USB 或可移动介质），而且不是 Windows 所在的盘；
 *   VHD 也认（测试用）。
 *   --expect-mib 是界面确认时看到的容量（gk3w probe 的 size_mib）：从确认到写入之间隔着整个下载，这期间换插的另一块盘
 *   可能拿到同一个磁盘号，容量对不上就不写。写保护的、或者 LiveCD 与发布目录（以及 gk3w 自己）就在这块 U 盘上的，也不写。
 *   写法：U 盘上原有的卷全部锁定并卸下（句柄留到写完），先清掉盘头与盘尾各 1 MiB（旧的 GPT 主表与备份表、ISO 的卷描述符），
 *   再裸写整个 FAT32（src/fatw.c），最后写 MBR、让 Windows 重读分区表：Windows 重新识别这块盘时，卷已经格式化好了，
 *   不会提示"需要格式化"。每一块写完立即读回比对（写不进去、丢弃写入的假容量 U 盘在这里发现）；全部写完后再按记下的
 *   哈希整个读回一遍（地址回绕的假容量 U 盘把后写的数据写到了前面，只有这一遍才发现）。
 *   FAT32 分区最大 2 TiB（MBR 的 32 位扇区数），更大的 U 盘只用前 2 TiB。
 *
 * 所有输入在第一次写盘之前验完（发布目录的 sha256、boot.img 拆得开、ISO 里有启动所需的文件、放得下），之后 ERR 一律带
 * touched=yes。进度：1 check / verify-sha → 10 lock → 15 write-media（到 85）→ 85 verify-media（到 97）→ 97 partition
 * → 100 done。
 * stdout：MEDIA target=usb disk=N size_mib= files=N。 */
#include <windows.h>
#include <winioctl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../fatw.h"
#include "../iso9660.h"
#include "../proto.h"
#include "../release.h"
#include "disk.h"
#include "win.h"

#define PART_START (1u << 20)          /* 分区起点：1 MiB */
#define MAX_ENTRIES 4096
#define MAX_LOCKED 32

/* 启动 LiveCD 必须有的文件：缺一个都不是我们的 LiveCD ISO */
static const char *const LIVE_NEED[] = {
    "EFI/BOOT/BOOTAA64.EFI", "loader/loader.conf", "loader/entries/gaokun3-live.conf", "gaokun3/Image", "gaokun3/gaokun3.dtb",
    "gaokun3/initramfs.img", "gaokun3/live.squashfs",
};
/* 放进 gaokun3/payload/ 的发布文件（install-artifacts.sha256 可以没有） */
static const char *const PAYLOAD[] = {"boot.img", "super.img.zst", "install-artifacts.sha256"};

typedef struct {
    HANDLE h;
    uint64_t base;              /* ISO 里的文件：内容在 ISO 里的起点 */
} source;

/* 写过的每一块：位置、长度与哈希（写完之后整个读回再比一遍） */
typedef struct {
    uint64_t off;
    uint32_t len;
    uint8_t sha[32];
} written;

typedef struct {
    gk3w_disk *d;
    uint8_t *chk;               /* 读回比对用（与写缓冲同样大小、同样对齐） */
    written *w;
    size_t nw, capw;
} target;

static int file_pread(HANDLE h, void *buf, size_t len, uint64_t off)
{
    while (len) {
        OVERLAPPED ov;
        DWORD k = len > (1u << 30) ? (1u << 30) : (DWORD)len, got = 0;
        memset(&ov, 0, sizeof ov);
        ov.Offset = (DWORD)off;
        ov.OffsetHigh = (DWORD)(off >> 32);
        if (!ReadFile(h, buf, k, &got, &ov) || !got)
            return -1;
        buf = (uint8_t *)buf + got;
        len -= got;
        off += got;
    }
    return 0;
}

static int iso_read(void *ctx, uint64_t off, void *buf, size_t len)
{
    return file_pread(*(HANDLE *)ctx, buf, len, off);
}

static int src_read(void *ctx, const gk3w_fat_file *f, uint64_t off, void *buf, size_t len)
{
    const source *s = f->src;
    (void)ctx;
    return file_pread(s->h, buf, len, s->base + off);
}

static int disk_write(void *ctx, uint64_t off, const void *buf, size_t len)
{
    target *t = ctx;
    /* 最多 4 MiB 一块（fatw.c 的 CHUNK）：写完立即读回比对，并记下哈希 */
    if (gk3w_write_verify(t->d->h, buf, (DWORD)len, PART_START + off, t->chk))
        return -1;
    if (t->nw == t->capw) {
        size_t cap = t->capw ? t->capw * 2 : 1024;
        written *w = realloc(t->w, cap * sizeof *w);
        if (!w)
            return -1;
        t->w = w;
        t->capw = cap;
    }
    t->w[t->nw].off = PART_START + off;
    t->w[t->nw].len = (uint32_t)len;
    if (gk3w_sha256(buf, len, t->w[t->nw].sha))
        return -1;
    t->nw++;
    return 0;
}

/* 全部写完后按记下的哈希整个读回：同一位置后来又被写过的（目录、FAT 在文件之后写），以最后一次为准 */
static int verify_all(target *t)
{
    for (size_t i = 0; i < t->nw; i++) {
        uint8_t sha[32];
        int later = 0;
        for (size_t k = i + 1; k < t->nw && !later; k++)
            if (t->w[k].off < t->w[i].off + t->w[i].len && t->w[i].off < t->w[k].off + t->w[k].len)
                later = 1;
        if (later)
            continue;
        if (gk3w_pread(t->d->h, t->chk, t->w[i].len, t->w[i].off) || gk3w_sha256(t->chk, t->w[i].len, sha) ||
            memcmp(sha, t->w[i].sha, 32))
            return -1;
        if (i % 16 == 0)
            gk3w_progress(85 + (unsigned)(i * 12 / t->nw), "verify-media");
    }
    return 0;
}

static void on_progress(void *ctx, uint64_t done, uint64_t total)
{
    static unsigned last;
    unsigned pct = total ? 15 + (unsigned)(done * 70 / total) : 85;
    (void)ctx;
    if (pct != last) {
        gk3w_progress(pct, "write-media");
        last = pct;
    }
}

/* 这块盘上的卷全部锁定并卸下；句柄放进 out（写完关掉即解锁）。有卷锁不住（有文件开着）返回 -1 */
static int lock_volumes(DWORD disk, HANDLE *out, int cap, int *n)
{
    wchar_t name[MAX_PATH];
    HANDLE f = FindFirstVolumeW(name, MAX_PATH);
    int rc = 0;
    *n = 0;
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    do {
        size_t len = wcslen(name);
        wchar_t dev[MAX_PATH];
        HANDLE h;
        union {
            VOLUME_DISK_EXTENTS e;
            BYTE raw[sizeof(VOLUME_DISK_EXTENTS) + 16 * sizeof(DISK_EXTENT)];
        } ext;
        DWORD got = 0, i;
        int on = 0;
        if (len < 2 || len >= MAX_PATH)
            continue;
        wcscpy(dev, name);
        dev[len - 1] = 0;   /* 去掉末尾的反斜杠：打开的是卷本身 */
        h = CreateFileW(dev, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        if (DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0, &ext, sizeof ext, &got, NULL))
            for (i = 0; i < ext.e.NumberOfDiskExtents; i++)
                if (ext.e.Extents[i].DiskNumber == disk)
                    on = 1;
        if (!on) {
            CloseHandle(h);
            continue;
        }
        if (*n >= cap) {
            CloseHandle(h);
            rc = -1;
            break;
        }
        /* 锁不住时多试几次：资源管理器、杀毒软件可能刚好在读这个卷 */
        for (int tries = 0; tries < 10; tries++) {
            if (DeviceIoControl(h, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &got, NULL)) {
                on = 2;
                break;
            }
            Sleep(500);
        }
        if (on != 2 || !DeviceIoControl(h, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &got, NULL)) {
            gk3w_log("a volume on disk %lu could not be locked and dismounted (error %lu)", (unsigned long)disk, (unsigned long)GetLastError());
            CloseHandle(h);
            rc = -1;
            break;
        }
        out[(*n)++] = h;
    } while (FindNextVolumeW(f, name, MAX_PATH));
    FindVolumeClose(f);
    return rc;
}

/* path 所在的卷是否落在 disk 这块盘上（拿不准算不在：只用来给出更准确的报错，锁卷那一步照样会拦住） */
static int on_disk(const wchar_t *path, DWORD disk)
{
    wchar_t mp[MAX_PATH], vol[MAX_PATH];
    HANDLE h;
    union {
        VOLUME_DISK_EXTENTS e;
        BYTE raw[sizeof(VOLUME_DISK_EXTENTS) + 16 * sizeof(DISK_EXTENT)];
    } ext;
    DWORD got = 0;
    size_t n;
    int hit = 0;
    if (!GetVolumePathNameW(path, mp, MAX_PATH) || !GetVolumeNameForVolumeMountPointW(mp, vol, MAX_PATH))
        return 0;
    n = wcslen(vol);
    if (n && vol[n - 1] == L'\\')
        vol[n - 1] = 0;
    h = CreateFileW(vol, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    if (DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0, &ext, sizeof ext, &got, NULL))
        for (DWORD i = 0; i < ext.e.NumberOfDiskExtents; i++)
            if (ext.e.Extents[i].DiskNumber == disk)
                hit = 1;
    CloseHandle(h);
    return hit;
}

/* 一个扇区的 MBR：磁盘签名随机，第一个分区项是从 start 起 count 个扇区的 FAT32（LBA） */
static void make_mbr(uint8_t *b, uint32_t bs, uint32_t start, uint32_t count)
{
    uint8_t *e = b + 446;
    uint32_t sig = 0;
    memset(b, 0, bs);
    gk3w_random(&sig, sizeof sig);
    memcpy(b + 440, &sig, 4);
    e[0] = 0x80;                    /* 可启动标志：固件做 UEFI 启动时不看它，老固件的 USB 启动要它 */
    e[1] = 0xFE;                    /* CHS 起止：只用 LBA，填约定的"超出范围"值 */
    e[2] = 0xFF;
    e[3] = 0xFF;
    e[4] = 0x0C;
    e[5] = 0xFE;
    e[6] = 0xFF;
    e[7] = 0xFF;
    memcpy(e + 8, &start, 4);
    memcpy(e + 12, &count, 4);
    b[510] = 0x55;
    b[511] = 0xAA;
}

static int verify_release(const wchar_t *rel, gk3w_touched T)
{
    wchar_t p_boot[MAX_PATH], p_super[MAX_PATH], p_sha[MAX_PATH];
    uint8_t *man = NULL;
    size_t man_len;
    gk3w_boot bi;
    gk3w_boot_err be;
    _snwprintf(p_boot, MAX_PATH, L"%ls\\boot.img", rel);
    _snwprintf(p_super, MAX_PATH, L"%ls\\super.img.zst", rel);
    _snwprintf(p_sha, MAX_PATH, L"%ls\\install-artifacts.sha256", rel);
    if (!gk3w_exists(p_super))
        return gk3w_err(T, "release-no-super", "the release directory has no super.img.zst", 0), -1;
    if (gk3w_read_file(p_sha, &man, &man_len, 1u << 20) == 0) {
        static const char *const names[] = {"boot.img", "super.img.zst"};
        const wchar_t *paths[] = {p_boot, p_super};
        gk3w_progress(1, "verify-sha");
        for (int i = 0; i < 2; i++) {
            char want[65], got[65];
            if (gk3w_manifest_hash(man, man_len, names[i], want))
                continue;
            if (gk3w_file_sha256(paths[i], got, NULL) || strcmp(want, got)) {
                free(man);
                return gk3w_err_kv(T, "sha-mismatch", "file", names[i], "a downloaded file does not match the sha256 in the release manifest; download it again (disk not touched)", 0), -1;
            }
            gk3w_log("%s: sha256 matches the release manifest", names[i]);
        }
        free(man);
    } else {
        gk3w_log("no install-artifacts.sha256 in the release directory, skipping the sha256 check");
    }
    memset(&bi, 0, sizeof bi);
    if (gk3w_read_file(p_boot, &bi.img, &bi.len, GK3W_BOOT_MAX))
        return gk3w_err(T, "release-no-boot", "the release directory has no readable boot.img (or it is larger than 64 MiB)", GetLastError()), -1;
    be = gk3w_boot_check(&bi);
    free(bi.img);
    if (be != GK3W_BOOT_OK)
        return gk3w_err(T, "bootimg-unpack", gk3w_boot_strerror(be), 0), -1;
    return 0;
}

int gk3w_cmd_media(int argc, wchar_t **argv)
{
    const wchar_t *tgt = gk3w_arg(argc, argv, L"--target"), *iso_path = gk3w_arg(argc, argv, L"--livecd"),
                  *rel = gk3w_arg(argc, argv, L"--release");
    gk3w_touched T = GK3W_TOUCHED_NO;
    uint64_t num, iso_size, part_bytes, need, expect_mib;
    static gk3w_iso_entry ents[MAX_ENTRIES];
    static gk3w_fat_file files[MAX_ENTRIES + 3];
    static source srcs[MAX_ENTRIES + 3];
    char paths[3][64];
    size_t nent = 0, nf = 0;
    HANDLE iso = INVALID_HANDLE_VALUE, pay[3] = {INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE};
    HANDLE locked[MAX_LOCKED];
    int nlocked = 0, rc = 1;
    gk3w_disk d = {0};
    uint8_t *buf = NULL, *chk = NULL;
    LARGE_INTEGER sz;
    gk3w_iso_src isrc;
    gk3w_iso_err ie;
    gk3w_fat_params fp;
    gk3w_fat_io io;
    gk3w_fat_err fe;
    target tg = {0};
    SYSTEMTIME st;
    DWORD got = 0;
    d.h = INVALID_HANDLE_VALUE;

    if (!tgt || !iso_path || !rel || gk3w_arg_u64(argc, argv, L"--disk", &num) || gk3w_arg_u64(argc, argv, L"--expect-mib", &expect_mib))
        return gk3w_err(GK3W_TOUCHED_NA, "usage", "usage: gk3w media --target usb --disk N --expect-mib M --livecd ISO --release DIR", 0);
    if (wcscmp(tgt, L"usb"))
        return gk3w_err(GK3W_TOUCHED_NA, "usage", "--target internal is not implemented yet; only --target usb", 0);

    /* ── 输入：全部在动盘之前验完 ── */
    gk3w_progress(1, "check");
    if (verify_release(rel, T))
        goto out;
    iso = CreateFileW(iso_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (iso == INVALID_HANDLE_VALUE || !GetFileSizeEx(iso, &sz)) {
        gk3w_err(T, "livecd-open", "cannot open the LiveCD ISO", GetLastError());
        goto out;
    }
    iso_size = (uint64_t)sz.QuadPart;
    isrc.ctx = &iso;
    isrc.read = iso_read;
    ie = gk3w_iso_list(&isrc, iso_size, ents, MAX_ENTRIES, &nent);
    if (ie) {
        gk3w_err(T, "livecd-iso", gk3w_iso_strerror(ie), 0);
        goto out;
    }
    for (size_t i = 0; i < sizeof LIVE_NEED / sizeof LIVE_NEED[0]; i++) {
        int found = 0;
        for (size_t k = 0; k < nent; k++)
            if (!ents[k].is_dir && !strcmp(ents[k].path, LIVE_NEED[i]))
                found = 1;
        if (!found) {
            gk3w_err_kv(T, "livecd-incomplete", "file", LIVE_NEED[i], "the ISO is not a gaokun3 LiveCD: a file needed to boot it is missing", 0);
            goto out;
        }
    }
    /* ISO 里的文件：El Torito 目录（老的 ISO 没有隐藏它）与 ISO 自带的安装载荷（带系统文件的 ISO）不要，载荷用所选的版本 */
    for (size_t k = 0; k < nent; k++) {
        if (ents[k].is_dir || !strcmp(ents[k].path, "boot.catalog") || !strncmp(ents[k].path, "gaokun3/payload/", 16))
            continue;
        srcs[nf] = (source){iso, ents[k].offset};
        files[nf] = (gk3w_fat_file){ents[k].path, ents[k].size, &srcs[nf]};
        nf++;
    }
    for (int i = 0; i < 3; i++) {
        wchar_t p[MAX_PATH], wn[64];
        gk3w_u2w(PAYLOAD[i], wn, 64);
        _snwprintf(p, MAX_PATH, L"%ls\\%ls", rel, wn);
        pay[i] = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (pay[i] == INVALID_HANDLE_VALUE) {
            if (i == 2)
                continue;
            gk3w_err_kv(T, "release-open", "file", PAYLOAD[i], "cannot open a file in the release directory", GetLastError());
            goto out;
        }
        if (!GetFileSizeEx(pay[i], &sz)) {
            gk3w_err_kv(T, "release-open", "file", PAYLOAD[i], "cannot read the size of a file in the release directory", GetLastError());
            goto out;
        }
        snprintf(paths[i], sizeof paths[i], "gaokun3/payload/%s", PAYLOAD[i]);
        srcs[nf] = (source){pay[i], 0};
        files[nf] = (gk3w_fat_file){paths[i], (uint64_t)sz.QuadPart, &srcs[nf]};
        nf++;
    }

    if (gk3w_disk_open_removable((DWORD)num, &d, T))
        goto out;
    if ((d.size >> 20) != expect_mib) {
        char cur[32];
        snprintf(cur, sizeof cur, "%llu", (unsigned long long)(d.size >> 20));
        gk3w_err_kv(T, "media-changed", "size_mib", cur, "disk number now refers to a disk of a different size than the one confirmed; was the USB drive replaced? (disk not touched)", 0);
        goto out;
    }
    if (!DeviceIoControl(d.h, IOCTL_DISK_IS_WRITABLE, NULL, 0, NULL, 0, &got, NULL)) {
        gk3w_err(T, "media-readonly", "the USB drive is write-protected (disk not touched)", GetLastError());
        goto out;
    }
    {
        wchar_t self[MAX_PATH];
        const wchar_t *mine[3] = {iso_path, rel, GetModuleFileNameW(NULL, self, MAX_PATH) ? self : NULL};
        for (int i = 0; i < 3; i++)
            if (mine[i] && on_disk(mine[i], d.num)) {
                gk3w_err(T, "media-is-source", "the LiveCD, the system files or the installer itself are on this USB drive; it cannot overwrite itself (disk not touched)", 0);
                goto out;
            }
    }
    need = gk3w_fat_size_for(files, nf, d.bs);
    if (!need) {
        gk3w_err(T, "livecd-names", "a file in the LiveCD or the release directory cannot be stored on FAT32 (name or size) (disk not touched)", 0);
        goto out;
    }
    /* FAT32 最大 2 TiB，MBR 的扇区数也是 32 位：更大的 U 盘只用前面一段 */
    part_bytes = d.size > 2ull * PART_START ? (d.size - PART_START) / d.bs * d.bs : 0;
    if (part_bytes > (2ull << 40))
        part_bytes = 2ull << 40;
    if (part_bytes / d.bs > 0xFFFFFFFFull - PART_START / d.bs)
        part_bytes = (0xFFFFFFFFull - PART_START / d.bs) * d.bs;
    if (part_bytes < need) {
        char nb[32];
        snprintf(nb, sizeof nb, "%llu", (unsigned long long)((need + (1u << 20) - 1) >> 20));
        gk3w_err_kv(T, "media-too-small", "need_mib", nb, "the USB drive is too small for the LiveCD and the system files (disk not touched)", 0);
        goto out;
    }
    gk3w_log("USB drive: disk %lu, %llu MiB, %u-byte sectors; FAT32 from 1 MiB, %zu files, at least %llu MiB needed",
             (unsigned long)d.num, (unsigned long long)(d.size >> 20), d.bs, nf, (unsigned long long)(need >> 20));

    /* ── 锁住 U 盘上原有的卷 ── */
    gk3w_progress(10, "lock");
    if (lock_volumes(d.num, locked, MAX_LOCKED, &nlocked)) {
        gk3w_err(T, "media-in-use", "a volume on the USB drive is in use and cannot be locked; close the programs using it and try again (disk not touched)", 0);
        goto out;
    }
    buf = gk3w_aligned_alloc(PART_START);
    chk = gk3w_aligned_alloc(GK3W_IO_CHUNK);
    if (!buf || !chk) {
        gk3w_err(T, "nomem", "out of memory", 0);
        goto out;
    }

    /* ══ 从这里起动盘 ══ */
    T = GK3W_TOUCHED_YES;
    memset(buf, 0, PART_START);
    if (gk3w_write_verify(d.h, buf, PART_START, 0, chk) ||
        gk3w_write_verify(d.h, buf, PART_START, (d.size - PART_START) / d.bs * d.bs, chk)) {
        gk3w_err(T, "disk-write", "clearing the start and the end of the USB drive failed", GetLastError());
        goto out;
    }
    memset(&fp, 0, sizeof fp);
    fp.part_bytes = part_bytes;
    fp.sector = d.bs;
    fp.hidden = PART_START / d.bs;
    snprintf(fp.label, sizeof fp.label, "GK3LIVE");
    gk3w_random(&fp.volume_id, sizeof fp.volume_id);
    GetLocalTime(&st);
    gk3w_fat_stamp(st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, &fp.date, &fp.time);
    tg.d = &d;
    tg.chk = chk;
    io.ctx = &tg;
    io.write = disk_write;
    io.read = src_read;
    io.progress = on_progress;
    gk3w_progress(15, "write-media");
    fe = gk3w_fat_build(&fp, files, nf, &io);
    if (fe) {
        gk3w_err_kv(T, fe == GK3W_FAT_READ ? "media-read" : "disk-write", "why", gk3w_fat_strerror(fe),
                    fe == GK3W_FAT_READ ? "reading the LiveCD or the system files failed while writing the USB drive"
                                        : "writing the USB drive failed: a media error, or the drive is smaller than it reports",
                    GetLastError());
        goto out;
    }
    gk3w_progress(85, "verify-media");
    if (verify_all(&tg)) {
        gk3w_err(T, "disk-verify", "the USB drive did not read back what was written: it is faulty or smaller than it reports", GetLastError());
        goto out;
    }
    gk3w_progress(97, "partition");
    make_mbr(buf, d.bs, PART_START / d.bs, (uint32_t)(part_bytes / d.bs));
    if (gk3w_write_verify(d.h, buf, d.bs, 0, chk)) {
        gk3w_err(T, "disk-write", "writing the partition table of the USB drive failed", GetLastError());
        goto out;
    }
    /* 先让 Windows 重读分区表、再解锁原来的卷：解锁在前的话，Windows 可能按旧的分区表把旧卷重新挂上 */
    if (!DeviceIoControl(d.h, IOCTL_DISK_UPDATE_PROPERTIES, NULL, 0, NULL, 0, &got, NULL))
        gk3w_log("warning: Windows did not re-read the new partition table (error %lu); unplug and replug the USB drive", (unsigned long)GetLastError());
    for (int i = 0; i < nlocked; i++)
        CloseHandle(locked[i]);
    nlocked = 0;
    gk3w_log("USB drive written and verified: MBR + FAT32 \"GK3LIVE\", %zu files", nf);
    {
        gk3w_line l = {0};
        gk3w_line_begin(&l, "MEDIA");
        gk3w_line_kv(&l, "target", "usb");
        gk3w_line_u64(&l, "disk", d.num);
        gk3w_line_u64(&l, "size_mib", part_bytes >> 20);
        gk3w_line_u64(&l, "files", nf);
        (void)gk3w_line_emit(&l, stdout);
        gk3w_line_free(&l);
    }
    gk3w_progress(100, "done");
    rc = 0;
out:
    for (int i = 0; i < nlocked; i++)
        CloseHandle(locked[i]);
    gk3w_aligned_free(buf);
    gk3w_aligned_free(chk);
    free(tg.w);
    gk3w_disk_close(&d);
    if (iso != INVALID_HANDLE_VALUE)
        CloseHandle(iso);
    for (int i = 0; i < 3; i++)
        if (pay[i] != INVALID_HANDLE_VALUE)
            CloseHandle(pay[i]);
    return rc;
}
