/* Windows：制作独立安装介质（界面上的"制作独立安装介质"）。
 *
 *   gk3w media --target usb --disk N --expect-mib M --livecd <ISO> --release <目录>
 *   gk3w media --target internal --disk N --region-start LBA --region-end LBA --livecd <ISO> --release <目录>
 *              --sdboot FILE --backup-dir DIR [--real-disk [--bitlocker-key -]]
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
 * 本机磁盘：在所选未分配空间的【末尾】建 GK3LIVE 分区（至少 2 GiB，内容更大时按 256 MiB 向上取整；XBOOTLDR 类型、GPT 名与
 *   卷标都是 GK3LIVE）。之后在 LiveCD
 *   里保留现有系统安装 Android 时，userdata 排在最后、紧挨着它，装完删掉它就能并进 userdata（gk3l remove-installer）。
 *   systemd-boot 从 XBOOTLDR 分区读启动项与内核（loader/entries/gaokun3-live.conf、gaokun3/；ISO 里的 EFI/ 与
 *   loader/loader.conf 不放进去，理由见 inputs_open），ESP 上只要 systemd-boot：
 *   ESP 上还没有 Android 时，按 apply 的同一套规则把回落路径换成 systemd-boot（原件留 .before-gaokun3），写默认进
 *   Windows 的 loader.conf（gk3w_loader_conf_windows），LiveCD 在开机菜单里选；已经装了 Android 的 ESP 不动（启动链本来
 *   就是我们的，LiveCD 的启动项出现在同一个菜单里），只有回落路径不在了或被换回了 Windows 启动管理器时补回 systemd-boot。
 *   把关同 apply：默认只写 VHD；--real-disk 只认 Windows 所在的系统盘，先过 gk3w_sys_gate（BitLocker 保护开着时
 *   --bitlocker-key - 从标准输入读恢复密钥，要换回落路径时在第一次写盘之前暂停保护到下次重启）。盘上已有 GK3LIVE
 *   （XBOOTLDR 类型，GPT 名或卷标为 GK3LIVE）时不写。
 *   写的顺序：先写 ESP（loader.conf 在回落路径之前），再把 FAT32 写进那段未分配空间（逐块读回比对，再整个按哈希读回
 *   一遍），最后写分区表 —— 中途失败时不会留下 GK3LIVE 分区（重跑不会被"已有 GK3LIVE"挡住），开机菜单里只有 Windows。
 *   写分区表之前再核对一次分区表没被别的工具改过（写 FAT32 要好几分钟）；动盘之前先备份分区表（--backup-dir，同 apply）。
 *
 * 所有输入在第一次写盘之前验完（发布目录的 sha256、boot.img 拆得开、ISO 里有启动所需的文件、放得下），之后 ERR 一律带
 * touched=yes。进度：1 check / verify-sha → 10 lock（U 盘）或 bootloader（本机磁盘，要换回落路径时）→ 15 write-media
 * （到 85）→ 85 verify-media（到 97）→ 97 partition → 100 done。
 * stdout：MEDIA target=usb disk=N size_mib= files=N；
 *         MEDIA target=internal disk=N start= end=（逻辑扇区，含两端） size_mib= files=N bootloader=yes|no。 */
#include <windows.h>
#include <winioctl.h>
#include <rpc.h>

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../esp.h"
#include "../fatw.h"
#include "../iso9660.h"
#include "../layout.h"
#include "../proto.h"
#include "../release.h"
#include "disk.h"
#include "espw.h"
#include "gk3core.h"
#include "win.h"

#define PART_START (1u << 20)          /* 分区起点：1 MiB */
#define LIVE_MIB 2048u                 /* 本机磁盘上 GK3LIVE 分区的大小（界面 kMediaNeedMib 与它相同） */
#define MAX_ENTRIES 4096
#define MAX_LOCKED 32

/* 启动 LiveCD 必须有的文件：缺一个都不是我们的 LiveCD ISO */
static const char *const LIVE_NEED[] = {
    "EFI/BOOT/BOOTAA64.EFI", "loader/loader.conf", "loader/entries/gaokun3-live.conf", "gaokun3/Image", "gaokun3/gaokun3.dtb",
    "gaokun3/initramfs.img", "gaokun3/live.squashfs",
};
/* XBOOTLDR：BC13C2FF-59E6-4262-A352-B275FD6F7172（与 LiveCD 认安装器分区用的同一个，见 src/linux/live.c） */
static const GUID XBOOTLDR_TYPE = {0xBC13C2FF, 0x59E6, 0x4262, {0xA3, 0x52, 0xB2, 0x75, 0xFD, 0x6F, 0x71, 0x72}};
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
    uint64_t base;              /* FAT32 卷在盘上的起点（字节） */
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
    if (gk3w_write_verify(t->d->h, buf, (DWORD)len, t->base + off, t->chk))
        return -1;
    if (t->nw == t->capw) {
        size_t cap = t->capw ? t->capw * 2 : 1024;
        written *w = realloc(t->w, cap * sizeof *w);
        if (!w)
            return -1;
        t->w = w;
        t->capw = cap;
    }
    t->w[t->nw].off = t->base + off;
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

/* LiveCD 与发布目录：ISO 里的文件清单 + 三个发布文件，全部打开、核对完 */
typedef struct {
    HANDLE iso, pay[3];
    gk3w_fat_file files[MAX_ENTRIES + 3];
    source srcs[MAX_ENTRIES + 3];
    char paths[3][64];
    size_t nf;
} inputs;

static void inputs_close(inputs *in)
{
    if (in->iso != INVALID_HANDLE_VALUE)
        CloseHandle(in->iso);
    for (int i = 0; i < 3; i++)
        if (in->pay[i] != INVALID_HANDLE_VALUE)
            CloseHandle(in->pay[i]);
}

/* 验发布目录、读 ISO 的目录树、核对启动所需的文件、打开发布文件。失败已报 ERR（touched=no），返回 -1；
 * 无论成败都要 inputs_close。xbootldr：写进本机磁盘的 GK3LIVE（XBOOTLDR 分区）—— 不放 ISO 里的 EFI/ 与
 * loader/loader.conf：systemd-boot 只从 XBOOTLDR 读启动项与内核，引导程序与 loader.conf 在 ESP 上；内置盘上多一份
 * 引导程序，固件或 systemd-boot 就可能从 GK3LIVE 启动、绕开 ESP 上的配置 */
static int inputs_open(const wchar_t *iso_path, const wchar_t *rel, inputs *in, int xbootldr, gk3w_touched T)
{
    static gk3w_iso_entry ents[MAX_ENTRIES];
    size_t nent = 0;
    uint64_t iso_size;
    LARGE_INTEGER sz;
    gk3w_iso_src isrc;
    gk3w_iso_err ie;
    in->iso = INVALID_HANDLE_VALUE;
    for (int i = 0; i < 3; i++)
        in->pay[i] = INVALID_HANDLE_VALUE;
    in->nf = 0;
    gk3w_progress(1, "check");
    if (verify_release(rel, T))
        return -1;
    in->iso = CreateFileW(iso_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (in->iso == INVALID_HANDLE_VALUE || !GetFileSizeEx(in->iso, &sz))
        return gk3w_err(T, "livecd-open", "cannot open the LiveCD ISO", GetLastError()), -1;
    iso_size = (uint64_t)sz.QuadPart;
    isrc.ctx = &in->iso;
    isrc.read = iso_read;
    ie = gk3w_iso_list(&isrc, iso_size, ents, MAX_ENTRIES, &nent);
    if (ie)
        return gk3w_err(T, "livecd-iso", gk3w_iso_strerror(ie), 0), -1;
    for (size_t i = 0; i < sizeof LIVE_NEED / sizeof LIVE_NEED[0]; i++) {
        int found = 0;
        for (size_t k = 0; k < nent; k++)
            if (!ents[k].is_dir && !strcmp(ents[k].path, LIVE_NEED[i]))
                found = 1;
        if (!found)
            return gk3w_err_kv(T, "livecd-incomplete", "file", LIVE_NEED[i], "the ISO is not a gaokun3 LiveCD: a file needed to boot it is missing", 0), -1;
    }
    /* ISO 里的文件：El Torito 目录（老的 ISO 没有隐藏它）与 ISO 自带的安装载荷（带系统文件的 ISO）不要，载荷用所选的版本 */
    for (size_t k = 0; k < nent; k++) {
        if (ents[k].is_dir || !strcmp(ents[k].path, "boot.catalog") || !strncmp(ents[k].path, "gaokun3/payload/", 16))
            continue;
        if (xbootldr && (!strncmp(ents[k].path, "EFI/", 4) || !strcmp(ents[k].path, "loader/loader.conf")))
            continue;
        in->srcs[in->nf] = (source){in->iso, ents[k].offset};
        in->files[in->nf] = (gk3w_fat_file){ents[k].path, ents[k].size, &in->srcs[in->nf]};
        in->nf++;
    }
    for (int i = 0; i < 3; i++) {
        wchar_t pth[MAX_PATH], wn[64];
        gk3w_u2w(PAYLOAD[i], wn, 64);
        _snwprintf(pth, MAX_PATH, L"%ls\\%ls", rel, wn);
        in->pay[i] = CreateFileW(pth, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (in->pay[i] == INVALID_HANDLE_VALUE) {
            if (i == 2)
                continue;
            return gk3w_err_kv(T, "release-open", "file", PAYLOAD[i], "cannot open a file in the release directory", GetLastError()), -1;
        }
        if (!GetFileSizeEx(in->pay[i], &sz))
            return gk3w_err_kv(T, "release-open", "file", PAYLOAD[i], "cannot read the size of a file in the release directory", GetLastError()), -1;
        snprintf(in->paths[i], sizeof in->paths[i], "gaokun3/payload/%s", PAYLOAD[i]);
        in->srcs[in->nf] = (source){in->pay[i], 0};
        in->files[in->nf] = (gk3w_fat_file){in->paths[i], (uint64_t)sz.QuadPart, &in->srcs[in->nf]};
        in->nf++;
    }
    return 0;
}

/* 在盘上 base 起、part_bytes 长的一段写出 FAT32（卷标 GK3LIVE），再整个按哈希读回。hidden：分区起点的扇区号。
 * 失败已报 ERR（touched=yes），返回 -1；what 是给 ERR 的说明里用的盘的称呼 */
static int write_fat(gk3w_disk *d, uint64_t base, uint64_t part_bytes, uint32_t hidden, const inputs *in, uint8_t *chk, target *tg,
                     const char *what)
{
    gk3w_fat_params fp;
    gk3w_fat_io io;
    gk3w_fat_err fe;
    SYSTEMTIME st;
    char msg[160];
    memset(&fp, 0, sizeof fp);
    fp.part_bytes = part_bytes;
    fp.sector = d->bs;
    fp.hidden = hidden;
    snprintf(fp.label, sizeof fp.label, "GK3LIVE");
    gk3w_random(&fp.volume_id, sizeof fp.volume_id);
    GetLocalTime(&st);
    gk3w_fat_stamp(st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, &fp.date, &fp.time);
    tg->d = d;
    tg->base = base;
    tg->chk = chk;
    io.ctx = tg;
    io.write = disk_write;
    io.read = src_read;
    io.progress = on_progress;
    gk3w_progress(15, "write-media");
    fe = gk3w_fat_build(&fp, in->files, in->nf, &io);
    if (fe) {
        snprintf(msg, sizeof msg, fe == GK3W_FAT_READ ? "reading the LiveCD or the system files failed while writing the %s"
                                                      : "writing the %s failed: a media error, or the disk is smaller than it reports", what);
        return gk3w_err_kv(GK3W_TOUCHED_YES, fe == GK3W_FAT_READ ? "media-read" : "disk-write", "why", gk3w_fat_strerror(fe), msg, GetLastError()), -1;
    }
    gk3w_progress(85, "verify-media");
    if (verify_all(tg)) {
        snprintf(msg, sizeof msg, "the %s did not read back what was written: it is faulty or smaller than it reports", what);
        return gk3w_err(GK3W_TOUCHED_YES, "disk-verify", msg, GetLastError()), -1;
    }
    return 0;
}

static int media_usb(int argc, wchar_t **argv)
{
    const wchar_t *iso_path = gk3w_arg(argc, argv, L"--livecd"), *rel = gk3w_arg(argc, argv, L"--release");
    gk3w_touched T = GK3W_TOUCHED_NO;
    uint64_t num, part_bytes, need, expect_mib;
    static inputs in;
    HANDLE locked[MAX_LOCKED];
    int nlocked = 0, rc = 1;
    gk3w_disk d = {0};
    uint8_t *buf = NULL, *chk = NULL;
    target tg = {0};
    DWORD got = 0;
    d.h = INVALID_HANDLE_VALUE;

    if (!iso_path || !rel || gk3w_arg_u64(argc, argv, L"--disk", &num) || gk3w_arg_u64(argc, argv, L"--expect-mib", &expect_mib))
        return gk3w_err(GK3W_TOUCHED_NA, "usage", "usage: gk3w media --target usb --disk N --expect-mib M --livecd ISO --release DIR", 0);

    /* ── 输入：全部在动盘之前验完 ── */
    if (inputs_open(iso_path, rel, &in, 0, T))
        goto out;
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
    need = gk3w_fat_size_for(in.files, in.nf, d.bs);
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
             (unsigned long)d.num, (unsigned long long)(d.size >> 20), d.bs, in.nf, (unsigned long long)(need >> 20));

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
    if (write_fat(&d, PART_START, part_bytes, PART_START / d.bs, &in, chk, &tg, "USB drive"))
        goto out;
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
    gk3w_log("USB drive written and verified: MBR + FAT32 \"GK3LIVE\", %zu files", in.nf);
    {
        gk3w_line l = {0};
        gk3w_line_begin(&l, "MEDIA");
        gk3w_line_kv(&l, "target", "usb");
        gk3w_line_u64(&l, "disk", d.num);
        gk3w_line_u64(&l, "size_mib", part_bytes >> 20);
        gk3w_line_u64(&l, "files", in.nf);
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
    inputs_close(&in);
    return rc;
}

/* 盘上已有的 GK3LIVE：XBOOTLDR 类型、GPT 名为 GK3LIVE，或卷标为 GK3LIVE（旧版脚本建的是 Basic data 类型、只有卷标），
 * 都不分大小写。两份并存时 initramfs 用先找到的那份，所以不再建第二份 */
static int has_live_part(const gk3w_disk *d)
{
    for (DWORD i = 0; i < d->lay->PartitionCount; i++) {
        const PARTITION_INFORMATION_EX *p = &d->lay->PartitionEntry[i];
        wchar_t wn[GK3_GPT_NAME_CHARS + 1], vol[MAX_PATH], label[MAX_PATH + 1] = L"";
        if (p->PartitionStyle != PARTITION_STYLE_GPT || !p->PartitionLength.QuadPart)
            continue;
        memcpy(wn, p->Gpt.Name, sizeof p->Gpt.Name);
        wn[GK3_GPT_NAME_CHARS] = 0;
        if (!memcmp(&p->Gpt.PartitionType, &XBOOTLDR_TYPE, sizeof(GUID)) || !_wcsicmp(wn, L"GK3LIVE"))
            return 1;
        if (!gk3w_find_volume(d->num, (uint64_t)p->StartingOffset.QuadPart, vol, MAX_PATH) &&
            GetVolumeInformationW(vol, label, MAX_PATH + 1, NULL, NULL, NULL, NULL, 0) && !_wcsicmp(label, L"GK3LIVE"))
            return 1;
    }
    return 0;
}

/* 分区表与 snap 逐项相同 */
static int layout_same(gk3w_disk *d, const DRIVE_LAYOUT_INFORMATION_EX *snap)
{
    return !gk3w_disk_read_layout(d) && d->lay->PartitionCount == snap->PartitionCount && !memcmp(&d->lay->Gpt, &snap->Gpt, sizeof snap->Gpt) &&
           !memcmp(d->lay->PartitionEntry, snap->PartitionEntry, snap->PartitionCount * sizeof(PARTITION_INFORMATION_EX));
}

static int media_internal(int argc, wchar_t **argv)
{
    const wchar_t *iso_path = gk3w_arg(argc, argv, L"--livecd"), *rel = gk3w_arg(argc, argv, L"--release"),
                  *sdboot_path = gk3w_arg(argc, argv, L"--sdboot"), *backup_dir = gk3w_arg(argc, argv, L"--backup-dir"),
                  *blkey = gk3w_arg(argc, argv, L"--bitlocker-key");
    gk3w_touched T = GK3W_TOUCHED_NO;
    uint64_t num, rstart, rend, need, part_bytes, first = 0, last = 0;
    static inputs in;
    int rc = 1, real = gk3w_flag(argc, argv, L"--real-disk"), android = 0, chain = 0;
    char key[128] = "";
    gk3w_sys sys;
    gk3w_disk d = {0};
    gk3w_esp esp;
    const PARTITION_INFORMATION_EX *esp_part = NULL;
    DRIVE_LAYOUT_INFORMATION_EX *snap = NULL;
    uint8_t *sdboot = NULL, *chk = NULL, *orig[2] = {NULL, NULL};
    size_t sdboot_len = 0, orig_len[2] = {0, 0};
    target tg = {0};
    GUID id;
    d.h = INVALID_HANDLE_VALUE;
    memset(&sys, 0, sizeof sys);
    memset(&esp, 0, sizeof esp);
    in.iso = INVALID_HANDLE_VALUE;
    for (int i = 0; i < 3; i++)
        in.pay[i] = INVALID_HANDLE_VALUE;

    if (!iso_path || !rel || !sdboot_path || !backup_dir || gk3w_arg_u64(argc, argv, L"--disk", &num) || num > 255 ||
        gk3w_arg_u64(argc, argv, L"--region-start", &rstart) || gk3w_arg_u64(argc, argv, L"--region-end", &rend))
        return gk3w_err(GK3W_TOUCHED_NA, "usage",
                        "usage: gk3w media --target internal --disk N --region-start LBA --region-end LBA --livecd ISO --release DIR "
                        "--sdboot FILE --backup-dir DIR [--real-disk [--bitlocker-key -]]",
                        0);
    /* 恢复密钥只从标准输入读：参数会原样记进日志 */
    if (blkey && (wcscmp(blkey, L"-") || gk3w_read_key_stdin(key, sizeof key))) {
        gk3w_wipe(key, sizeof key);
        return gk3w_err(T, "usage", "--bitlocker-key takes - and reads the recovery key from standard input", 0);
    }
    if (real) {
        gk3w_sys_read(&sys);
        if (gk3w_sys_gate(&sys, blkey ? key : NULL, GK3W_GATE_APPLY)) {
            gk3w_wipe(key, sizeof key);
            return 1;
        }
    }
    gk3w_wipe(key, sizeof key);
    /* 写入期间阻止系统因空闲进入睡眠状态；进程退出时自动撤销 */
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);

    /* ── 输入：全部在动盘之前验完 ── */
    if (inputs_open(iso_path, rel, &in, 1, T))
        goto out;
    if (gk3w_read_file(sdboot_path, &sdboot, &sdboot_len, 16u << 20) || sdboot_len < 2 || memcmp(sdboot, "MZ", 2)) {
        gk3w_err(T, "tool-missing", "systemd-boot (--sdboot) is unreadable or not a PE file", 0);
        goto out;
    }

    /* ── 盘：分区表快照、已有的 GK3LIVE、所选的空闲区 ── */
    if (gk3w_disk_open((DWORD)num, real, &d, T))
        goto out;
    snap = malloc(d.laysz);
    if (!snap) {
        gk3w_err(T, "nomem", "out of memory", 0);
        goto out;
    }
    memcpy(snap, d.lay, d.laysz);
    need = gk3w_fat_size_for(in.files, in.nf, d.bs);
    if (!need) {
        gk3w_err(T, "livecd-names", "a file in the LiveCD or the release directory cannot be stored on FAT32 (name or size) (disk not touched)", 0);
        goto out;
    }
    /* 至少 LIVE_MIB；内容更大时按 256 MiB 向上取整（界面按 LIVE_MIB 选空闲区，放不下时下面报 media-too-small） */
    part_bytes = (uint64_t)LIVE_MIB << 20;
    if (need > part_bytes)
        part_bytes = (need + (256u << 20) - 1) / (256u << 20) * (256u << 20);
    if (has_live_part(&d)) {
        gk3w_err(T, "live-exists", "this disk already has a GK3LIVE partition (disk not touched)", 0);
        goto out;
    }
    {
        gk3w_span parts[GK3W_MAX_PARTS], spans[GK3W_MAX_PARTS + 1];
        size_t np = 0, nfree = 0;
        uint64_t uf, ul, end_b;
        DWORD count = 0;
        int inside = 0;
        for (DWORD i = 0; i < d.lay->PartitionCount; i++) {
            const PARTITION_INFORMATION_EX *p = &d.lay->PartitionEntry[i];
            if (p->PartitionStyle != PARTITION_STYLE_GPT || !p->PartitionLength.QuadPart)
                continue;
            count++;
            if (np < GK3W_MAX_PARTS)
                parts[np++] = (gk3w_span){(uint64_t)p->StartingOffset.QuadPart / d.bs,
                                          ((uint64_t)p->StartingOffset.QuadPart + (uint64_t)p->PartitionLength.QuadPart) / d.bs - 1};
        }
        if (count + 1 > d.lay->Gpt.MaxPartitionCount) {
            gk3w_err(T, "gpt-full", "the partition table is full (disk not touched)", 0);
            goto out;
        }
        gk3w_disk_usable(&d, &uf, &ul);
        if (gk3w_free_spans(uf, ul, parts, np, d.bs, 0, spans, GK3W_MAX_PARTS + 1, &nfree) != GK3W_LAYOUT_OK) {
            gk3w_err(T, "disk-layout", "the existing partition table has out-of-range or overlapping entries (disk not touched)", 0);
            goto out;
        }
        for (size_t i = 0; i < nfree && i < GK3W_MAX_PARTS + 1; i++)
            if (rstart >= spans[i].first && rend <= spans[i].last && rstart <= rend)
                inside = 1;
        if (!inside) {
            gk3w_err(T, "bad-region", "the selected region is not free space (disk not touched)", 0);
            goto out;
        }
        /* 放在空闲区的末尾，终点按 1 MiB 对齐（大小是整 MiB，起点也就对齐了） */
        end_b = (rend + 1) * d.bs / PART_START * PART_START;
        if (end_b < part_bytes || end_b - part_bytes < rstart * d.bs) {
            char nb[32];
            snprintf(nb, sizeof nb, "%llu", (unsigned long long)((part_bytes >> 20) + 1));
            gk3w_err_kv(T, "media-too-small", "need_mib", nb, "the selected free space is too small for the GK3LIVE partition (disk not touched)", 0);
            goto out;
        }
        first = (end_b - part_bytes) / d.bs;
        last = end_b / d.bs - 1;
    }
    gk3w_log("GK3LIVE: disk %lu, sectors %llu-%llu (%llu MiB, %u-byte sectors), %zu files, at least %llu MiB needed",
             (unsigned long)d.num, (unsigned long long)first, (unsigned long long)last, (unsigned long long)(part_bytes >> 20), d.bs,
             in.nf, (unsigned long long)(need >> 20));

    /* ── ESP：回落路径要是 systemd-boot，LiveCD 才出现在开机菜单里 ──
     * 还没有 Android：换上 systemd-boot，写默认进 Windows 的 loader.conf。已有 Android：启动链本来是我们的，不动；
     * 只有回落路径不在了、或被换回了 Windows 启动管理器时补回 systemd-boot（Android 本身也要靠它启动），loader.conf
     * 不动。回落路径上是别的系统后来放的引导程序（例如 GRUB）时同样不动：放谁由用户决定（同卸载的规则） */
    if (gk3w_esp_open(&d, &esp, &esp_part, T))
        goto out;
    /* 真实磁盘上 Windows 靠的就是 bootmgfw.efi：它不在说明 Windows 的引导不完整，回落路径该不该留也无从判断（同 apply） */
    if (real && !esp.windows) {
        gk3w_err(T, "no-windows-bootmgr", "EFI/Microsoft/Boot/bootmgfw.efi is missing from the EFI system partition: the Windows boot files of this disk are incomplete; repair them first (disk not touched)", 0);
        goto out;
    }
    android = gk3w_esp_has_android_entries(&esp);
    chain = !android;
    if (android) {
        wchar_t pth[MAX_PATH * 2];
        uint8_t *cur = NULL;
        size_t cur_len = 0;
        gk3w_esp_path(&esp, "EFI/BOOT/BOOTAA64.EFI", pth, MAX_PATH * 2);
        if (gk3w_exists(pth) && gk3w_read_file(pth, &cur, &cur_len, 64u << 20)) {
            gk3w_err_kv(T, "esp-unreadable", "file", "EFI/BOOT/BOOTAA64.EFI", "cannot read a file on the EFI system partition that must be checked first (disk not touched)", GetLastError());
            goto out;
        }
        if (!cur || gk3w_is_windows_boot_manager(cur, cur_len) || gk3w_esp_same(&esp, "EFI/Microsoft/Boot/bootmgfw.efi", cur, cur_len)) {
            chain = 1;
            gk3w_log("ESP: Android is installed but the fallback path holds no systemd-boot (missing, or the Windows boot manager); putting systemd-boot back (loader.conf left as it is)");
        } else if (gk3w_is_systemd_boot(cur, cur_len)) {
            gk3w_log("ESP: Android is installed and the fallback path holds systemd-boot; the ESP is left as it is");
        } else {
            gk3w_log("ESP: Android is installed and the fallback path holds another system's boot loader; the ESP is left as it is "
                     "(the LiveCD appears in the systemd-boot menu only when that loader chains to systemd-boot)");
        }
        free(cur);
    }
    if (chain) {
        ULARGE_INTEGER avail;
        int64_t kib = 256, aa = gk3w_esp_size_of(&esp, "EFI/BOOT/BOOTAA64.EFI");
        if (!esp.windows) {
            gk3w_err(T, "no-windows-bootmgr", "there is no Windows boot manager on the EFI system partition (disk not touched)", 0);
            goto out;
        }
        if (gk3w_esp_plan_backups(&esp, sdboot, sdboot_len, orig, orig_len, T))
            goto out;
        kib += gk3w_esp_delta(&esp, "EFI/BOOT/BOOTAA64.EFI", sdboot_len);
        kib += gk3w_esp_delta(&esp, "EFI/systemd/systemd-bootaa64.efi", sdboot_len);
        if (!android)
            kib += gk3w_esp_delta(&esp, "loader/loader.conf", strlen(gk3w_loader_conf_windows));
        /* 新写的备份，以及改名写入时 .tmp 与旧文件同时占着的空间 */
        for (int k = 0; k < 2; k++)
            if (orig[k])
                kib += (int64_t)orig_len[k] / 1024 + 4;
        if (aa >= 0 && !gk3w_esp_same(&esp, "EFI/BOOT/BOOTAA64.EFI", sdboot, sdboot_len))
            kib += aa / 1024 + 4;
        if (!GetDiskFreeSpaceExW(esp.root, &avail, NULL, NULL)) {
            gk3w_err(T, "esp-mount", "cannot read the free space of the EFI system partition (disk not touched)", GetLastError());
            goto out;
        }
        if ((int64_t)(avail.QuadPart / 1024) < kib) {
            gk3w_err(T, "esp-full", "not enough space on the EFI system partition for systemd-boot (disk not touched)", 0);
            goto out;
        }
        gk3w_log("ESP: systemd-boot goes to the fallback path%s; %lld KiB to write, %lld KiB free",
                 android ? "" : ", loader.conf keeps Windows as the default", (long long)kib, (long long)(avail.QuadPart / 1024));
    }

    /* ── 分区表没被别的工具改过；备份；BitLocker ── */
    chk = gk3w_aligned_alloc(GK3W_IO_CHUNK);
    if (!chk) {
        gk3w_err(T, "nomem", "out of memory", 0);
        goto out;
    }
    if (!layout_same(&d, snap)) {
        gk3w_err(T, "layout-changed", "the partition table changed while preparing; run the installer again (disk not touched)", 0);
        goto out;
    }
    esp_part = NULL;   /* 指向的是重读之前的表，已经释放 */
    if (gk3w_gpt_backup(&d, backup_dir, L"media", T))
        goto out;
    /* 回落路径要换成 systemd-boot：启动链变了，BitLocker 保护开着的话 Windows 下次启动会要求输入恢复密钥。
     * 暂停到下次重启，Windows 那时按新的启动链重新绑定；暂停不了就不动盘 */
    if (real && chain && gk3w_sys_bitlocker_on(&sys) && gk3w_bitlocker_suspend(sys.system_drive, 1)) {
        gk3w_err(T, "bitlocker-suspend-failed", "cannot suspend BitLocker protection of the system volume; Windows would ask for the recovery key after the boot chain changes (disk not touched)", 0);
        goto out;
    }

    /* ══ 从这里起动盘。顺序：ESP → 把 FAT32 写进未分配空间 → 分区表最后才指向它。
     *    ESP 先写：之后哪一步失败都不会留下 GK3LIVE 分区，重跑不会被"已有 GK3LIVE"挡住；这时开机菜单里只有 Windows
     *    （默认），重跑时备份规则认得出 systemd-boot 与 loader.conf 都是我们的，不会再留一份 ══ */
    T = GK3W_TOUCHED_YES;
    if (chain) {
        gk3w_progress(10, "bootloader");
        if (gk3w_esp_write_sdboot(&esp, sdboot, sdboot_len, orig, orig_len, android ? NULL : gk3w_loader_conf_windows))
            goto out;
        gk3w_log("ESP: systemd-boot%s written and verified", android ? "" : " and loader.conf (Windows by default)");
    }
    if (first > 0xFFFFFFFFull)
        gk3w_log("note: the GK3LIVE partition starts beyond sector 2^32; the FAT32 hidden-sectors field is left at 0");
    if (write_fat(&d, first * d.bs, part_bytes, first > 0xFFFFFFFFull ? 0 : (uint32_t)first, &in, chk, &tg, "disk"))
        goto out;
    gk3w_progress(97, "partition");
    /* 写 FAT32 要好几分钟：这期间分区表被别的工具（例如磁盘管理）改过的话，下面的 SET 会把那些改动撤销 */
    if (!layout_same(&d, snap)) {
        gk3w_err(T, "layout-changed", "the partition table changed while writing; no GK3LIVE partition was created (the existing partitions were not touched); run the installer again", 0);
        goto out;
    }
    {
        DWORD count = 0;
        DRIVE_LAYOUT_INFORMATION_EX *lay =
            calloc(1, offsetof(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry) + (d.lay->PartitionCount + 1) * sizeof(PARTITION_INFORMATION_EX));
        PARTITION_INFORMATION_EX *e;
        const PARTITION_INFORMATION_EX *got;
        if (!lay) {
            gk3w_err(T, "nomem", "out of memory", 0);
            goto out;
        }
        lay->PartitionStyle = PARTITION_STYLE_GPT;
        lay->Gpt = d.lay->Gpt;
        for (DWORD i = 0; i < d.lay->PartitionCount; i++) {
            const PARTITION_INFORMATION_EX *p = &d.lay->PartitionEntry[i];
            if (p->PartitionStyle == PARTITION_STYLE_GPT && p->PartitionLength.QuadPart)
                lay->PartitionEntry[count++] = *p;
        }
        e = &lay->PartitionEntry[count++];
        memset(e, 0, sizeof *e);
        e->PartitionStyle = PARTITION_STYLE_GPT;
        e->StartingOffset.QuadPart = (LONGLONG)(first * d.bs);
        e->PartitionLength.QuadPart = (LONGLONG)part_bytes;
        e->Gpt.PartitionType = XBOOTLDR_TYPE;
        if (UuidCreate(&e->Gpt.PartitionId) != RPC_S_OK) {
            free(lay);
            gk3w_err(T, "uuid", "cannot generate a partition GUID", 0);
            goto out;
        }
        wcsncpy(e->Gpt.Name, L"GK3LIVE", GK3_GPT_NAME_CHARS);
        id = e->Gpt.PartitionId;
        if (gk3w_disk_set_layout(&d, lay, count, T)) {
            free(lay);
            goto out;
        }
        free(lay);
        if (gk3w_disk_read_layout(&d)) {
            gk3w_err(T, "verify-layout", "re-reading the partition table after writing it failed", GetLastError());
            goto out;
        }
        got = gk3w_disk_find_part(&d, &id);
        if (!got || (uint64_t)got->StartingOffset.QuadPart != first * d.bs || (uint64_t)got->PartitionLength.QuadPart != part_bytes) {
            gk3w_err_kv(T, "verify-layout", "name", "GK3LIVE", "after writing the partition table, the new partition does not match the plan", 0);
            goto out;
        }
    }
    gk3w_log("GK3LIVE partition created and its FAT32 verified (%zu files)", in.nf);
    {
        gk3w_line l = {0};
        gk3w_line_begin(&l, "MEDIA");
        gk3w_line_kv(&l, "target", "internal");
        gk3w_line_u64(&l, "disk", d.num);
        gk3w_line_u64(&l, "start", first);
        gk3w_line_u64(&l, "end", last);
        gk3w_line_u64(&l, "size_mib", part_bytes >> 20);
        gk3w_line_u64(&l, "files", in.nf);
        gk3w_line_kv(&l, "bootloader", chain ? "yes" : "no");
        (void)gk3w_line_emit(&l, stdout);
        gk3w_line_free(&l);
    }
    gk3w_progress(100, "done");
    rc = 0;
out:
    gk3w_aligned_free(chk);
    free(tg.w);
    free(snap);
    free(sdboot);
    free(orig[0]);
    free(orig[1]);
    gk3w_disk_close(&d);
    inputs_close(&in);
    return rc;
}

int gk3w_cmd_media(int argc, wchar_t **argv)
{
    const wchar_t *tgt = gk3w_arg(argc, argv, L"--target");
    if (tgt && !wcscmp(tgt, L"usb"))
        return media_usb(argc, argv);
    if (tgt && !wcscmp(tgt, L"internal"))
        return media_internal(argc, argv);
    return gk3w_err(GK3W_TOUCHED_NA, "usage", "usage: gk3w media --target usb|internal ...", 0);
}
