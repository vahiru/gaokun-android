/* Windows：写盘相关的原型命令（阶段 1：验证 Windows 运行时能否这样改分区、写分区、写 ESP）。
 *
 *   gk3w mkpart  --disk N --start LBA --size-mib M --name NAME   在空闲区里加一个 GPT 分区（类型 Linux 数据）
 *   gk3w rmpart  --disk N --uuid GUID                            删掉一个我们建的分区（只认 Linux 数据类型）
 *   gk3w write   --disk N --uuid GUID --file PATH                把文件写进分区开头，写完逐块读回比对
 *   gk3w esp-ls  --vol \\?\Volume{…}\ [--dir SUB]                列 ESP 目录（只读，不需要盘符）
 *   gk3w esp-put --vol \\?\Volume{…}\ --file PATH --dest SUB     往 ESP 写一个文件并读回比对
 *
 * ★ 原型阶段只对 VHD 虚拟盘动手：会写盘的命令先查总线类型，不是 BusTypeFileBackedVirtual 一律拒绝。 */
#include <windows.h>
#include <winioctl.h>
#include <rpc.h>

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../layout.h"
#include "../proto.h"
#include "gk3core.h"
#include "win.h"

/* Linux 文件系统数据：0FC63DAF-8483-4772-8E79-3D69D8477DE4（与 installer-lib.sh 的 sgdisk 类型 8300 相同） */
static const GUID LINUX_DATA = {0x0FC63DAF, 0x8483, 0x4772, {0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4}};

#define ALIGN_SECTORS_BYTES (1u << 20)
#define CHUNK (4u << 20)

static const wchar_t *arg(int argc, wchar_t **argv, const wchar_t *key)
{
    for (int i = 0; i + 1 < argc; i++)
        if (!wcscmp(argv[i], key))
            return argv[i + 1];
    return NULL;
}

static int arg_u64(int argc, wchar_t **argv, const wchar_t *key, uint64_t *out)
{
    const wchar_t *v = arg(argc, argv, key);
    wchar_t *end = NULL;
    if (!v || !*v || *v == L'-')
        return -1;
    errno = 0;
    *out = wcstoull(v, &end, 10);
    return (errno || !end || *end) ? -1 : 0;
}

static int fail(const char *code, const char *human, DWORD winerr)
{
    gk3w_line l = {0};
    gk3w_line_begin(&l, "ERR");
    gk3w_line_kv(&l, "code", code);
    if (winerr)
        gk3w_line_u64(&l, "winerr", winerr);
    gk3w_fail(&l, GK3W_TOUCHED_NA, human);
    gk3w_line_free(&l);
    return 1;
}

typedef struct {
    HANDLE h;
    DWORD num;
    uint32_t bs;
    uint64_t size;
    DRIVE_LAYOUT_INFORMATION_EX *lay;
    DWORD laysz;
} wdisk;

static void wdisk_close(wdisk *d)
{
    free(d->lay);
    d->lay = NULL;
    if (d->h != INVALID_HANDLE_VALUE && d->h)
        CloseHandle(d->h);
    d->h = INVALID_HANDLE_VALUE;
}

static int wdisk_read_layout(wdisk *d)
{
    DWORD got = 0, sz = sizeof(DRIVE_LAYOUT_INFORMATION_EX) + 128 * sizeof(PARTITION_INFORMATION_EX);
    free(d->lay);
    d->lay = NULL;
    for (;;) {
        d->lay = calloc(1, sz);
        if (!d->lay)
            return -1;
        if (DeviceIoControl(d->h, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, NULL, 0, d->lay, sz, &got, NULL)) {
            d->laysz = sz;
            return 0;
        }
        free(d->lay);
        d->lay = NULL;
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || sz > (1u << 24))
            return -1;
        sz *= 2;
    }
}

/* 这块盘是不是 VHD 虚拟盘（只查，不报错） */
static int disk_is_vhd(DWORD num)
{
    wchar_t path[40];
    STORAGE_PROPERTY_QUERY q;
    BYTE descbuf[512];
    STORAGE_DEVICE_DESCRIPTOR *desc = (STORAGE_DEVICE_DESCRIPTOR *)descbuf;
    DWORD got = 0;
    HANDLE h;
    int vhd;
    _snwprintf(path, 40, L"\\\\.\\PhysicalDrive%lu", (unsigned long)num);
    h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    memset(&q, 0, sizeof q);
    q.PropertyId = StorageDeviceProperty;
    q.QueryType = PropertyStandardQuery;
    memset(descbuf, 0, sizeof descbuf);
    vhd = DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, descbuf, sizeof descbuf, &got, NULL) &&
          desc->BusType == BusTypeFileBackedVirtual;
    CloseHandle(h);
    return vhd;
}

/* 打开一块 VHD 读写；不是 VHD 就拒绝。出错时已经报了 ERR，返回 -1。 */
static int wdisk_open_vhd(DWORD num, wdisk *d)
{
    wchar_t path[40];
    STORAGE_PROPERTY_QUERY q;
    STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR align;
    GET_LENGTH_INFORMATION li;
    DWORD got = 0;

    memset(d, 0, sizeof *d);
    d->num = num;
    d->h = INVALID_HANDLE_VALUE;
    /* 先查类型、再以读写方式打开：真实磁盘连写句柄都不拿 */
    if (!disk_is_vhd(num))
        return fail("not-vhd", "原型只对 VHD 虚拟盘写入，这块盘不是 VHD", 0), -1;
    _snwprintf(path, 40, L"\\\\.\\PhysicalDrive%lu", (unsigned long)num);
    d->h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                       FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH, NULL);
    if (d->h == INVALID_HANDLE_VALUE)
        return fail("disk-open", "无法以读写方式打开磁盘", GetLastError()), -1;

    memset(&q, 0, sizeof q);
    q.QueryType = PropertyStandardQuery;
    q.PropertyId = StorageAccessAlignmentProperty;
    d->bs = 512;
    if (DeviceIoControl(d->h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, &align, sizeof align, &got, NULL) &&
        got >= sizeof align)
        d->bs = align.BytesPerLogicalSector;
    if (d->bs != 512 && d->bs != 4096) {
        wdisk_close(d);
        return fail("disk-sector", "扇区大小既不是 512 也不是 4096 字节", 0), -1;
    }
    if (!DeviceIoControl(d->h, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &li, sizeof li, &got, NULL)) {
        DWORD e = GetLastError();
        wdisk_close(d);
        return fail("disk-size", "读不到磁盘容量", e), -1;
    }
    d->size = (uint64_t)li.Length.QuadPart;
    if (wdisk_read_layout(d) || d->lay->PartitionStyle != PARTITION_STYLE_GPT) {
        wdisk_close(d);
        return fail("disk-not-gpt", "读不到 GPT 分区表", GetLastError()), -1;
    }
    return 0;
}

static int parse_guid(const wchar_t *s, GUID *g)
{
    RPC_WSTR w = (RPC_WSTR)(uintptr_t)s;
    return s && UuidFromStringW(w, g) == RPC_S_OK ? 0 : -1;
}

static PARTITION_INFORMATION_EX *find_part(wdisk *d, const GUID *id)
{
    for (DWORD i = 0; i < d->lay->PartitionCount; i++) {
        PARTITION_INFORMATION_EX *p = &d->lay->PartitionEntry[i];
        if (p->PartitionStyle == PARTITION_STYLE_GPT && p->PartitionLength.QuadPart &&
            !memcmp(&p->Gpt.PartitionId, id, sizeof *id))
            return p;
    }
    return NULL;
}

/* 设置新的分区表并让分区管理器重读 */
static int set_layout(wdisk *d, DRIVE_LAYOUT_INFORMATION_EX *lay, DWORD count)
{
    DWORD got = 0, sz = (DWORD)(offsetof(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry) + count * sizeof(PARTITION_INFORMATION_EX));
    lay->PartitionCount = count;
    for (DWORD i = 0; i < count; i++)
        lay->PartitionEntry[i].RewritePartition = TRUE;
    if (!DeviceIoControl(d->h, IOCTL_DISK_SET_DRIVE_LAYOUT_EX, lay, sz, NULL, 0, &got, NULL))
        return fail("set-layout", "写分区表失败", GetLastError());
    if (!DeviceIoControl(d->h, IOCTL_DISK_UPDATE_PROPERTIES, NULL, 0, NULL, 0, &got, NULL))
        return fail("update-properties", "分区表已写，但分区管理器没能重读", GetLastError());
    return 0;
}

static void emit_part(const char *type, const wdisk *d, const PARTITION_INFORMATION_EX *p)
{
    gk3w_line l = {0};
    char uuid[40], name[GK3_GPT_NAME_CHARS * 3 + 1];
    wchar_t wname[GK3_GPT_NAME_CHARS + 1];
    memcpy(wname, p->Gpt.Name, sizeof p->Gpt.Name);
    wname[GK3_GPT_NAME_CHARS] = 0;
    gk3w_w2u(wname, name, sizeof name);
    gk3w_guid_upper(&p->Gpt.PartitionId, uuid);
    gk3w_line_begin(&l, type);
    gk3w_line_u64(&l, "disk", d->num);
    gk3w_line_u64(&l, "num", p->PartitionNumber);
    gk3w_line_u64(&l, "start", (uint64_t)p->StartingOffset.QuadPart / d->bs);
    gk3w_line_u64(&l, "end", ((uint64_t)p->StartingOffset.QuadPart + (uint64_t)p->PartitionLength.QuadPart) / d->bs - 1);
    gk3w_line_kv(&l, "uuid", uuid);
    gk3w_line_kv(&l, "name", name);
    (void)gk3w_line_emit(&l, stdout);
    gk3w_line_free(&l);
}

int gk3w_cmd_mkpart(int argc, wchar_t **argv)
{
    uint64_t num, start, mib, ufirst, ulast, len_sectors;
    const wchar_t *name = arg(argc, argv, L"--name");
    wdisk d;
    gk3w_span parts[GK3W_MAX_PARTS], spans[GK3W_MAX_PARTS + 1], want;
    size_t np = 0, nf = 0;
    int inside = 0, rc;
    DRIVE_LAYOUT_INFORMATION_EX *lay;
    PARTITION_INFORMATION_EX *np_ent;
    DWORD count = 0;

    if (arg_u64(argc, argv, L"--disk", &num) || arg_u64(argc, argv, L"--start", &start) ||
        arg_u64(argc, argv, L"--size-mib", &mib) || !name || !*name || wcslen(name) > GK3_GPT_NAME_CHARS ||
        num > 255 || !mib || mib > (1ull << 32))
        return fail("usage", "用法：mkpart --disk N --start LBA --size-mib M --name NAME", 0);
    if (wdisk_open_vhd((DWORD)num, &d))
        return 1;

    /* 起点对齐到 1 MiB；整个区间必须落在某段空闲区里 */
    len_sectors = (mib << 20) / d.bs;
    if (start % (ALIGN_SECTORS_BYTES / d.bs) || start > UINT64_MAX - len_sectors) {
        wdisk_close(&d);
        return fail("bad-region", "起点没有按 1 MiB 对齐", 0);
    }
    want.first = start;
    want.last = start + len_sectors - 1;
    ufirst = (uint64_t)d.lay->Gpt.StartingUsableOffset.QuadPart / d.bs;
    ulast = ((uint64_t)d.lay->Gpt.StartingUsableOffset.QuadPart + (uint64_t)d.lay->Gpt.UsableLength.QuadPart) / d.bs - 1;
    for (DWORD i = 0; i < d.lay->PartitionCount; i++) {
        const PARTITION_INFORMATION_EX *p = &d.lay->PartitionEntry[i];
        if (p->PartitionStyle != PARTITION_STYLE_GPT || !p->PartitionLength.QuadPart)
            continue;
        count++;
        if (np < GK3W_MAX_PARTS)
            parts[np++] = (gk3w_span){(uint64_t)p->StartingOffset.QuadPart / d.bs,
                                      ((uint64_t)p->StartingOffset.QuadPart + (uint64_t)p->PartitionLength.QuadPart) / d.bs - 1};
    }
    if (gk3w_free_spans(ufirst, ulast, parts, np, d.bs, 0, spans, GK3W_MAX_PARTS + 1, &nf) != GK3W_LAYOUT_OK) {
        wdisk_close(&d);
        return fail("disk-layout", "现有分区表有越界或重叠", 0);
    }
    for (size_t i = 0; i < nf && i < GK3W_MAX_PARTS + 1; i++)
        if (want.first >= spans[i].first && want.last <= spans[i].last)
            inside = 1;
    if (!inside || count >= d.lay->Gpt.MaxPartitionCount) {
        wdisk_close(&d);
        return fail("bad-region", "这段区间不在任何一段空闲区里（或分区表已满）", 0);
    }

    /* 现有表项按原样带上，新表项接在后面（只带有效表项：空项不传） */
    lay = calloc(1, offsetof(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry) + (count + 1) * sizeof(PARTITION_INFORMATION_EX));
    if (!lay) {
        wdisk_close(&d);
        return fail("nomem", "内存不足", 0);
    }
    lay->PartitionStyle = PARTITION_STYLE_GPT;
    lay->Gpt = d.lay->Gpt;
    count = 0;
    for (DWORD i = 0; i < d.lay->PartitionCount; i++) {
        const PARTITION_INFORMATION_EX *p = &d.lay->PartitionEntry[i];
        if (p->PartitionStyle == PARTITION_STYLE_GPT && p->PartitionLength.QuadPart)
            lay->PartitionEntry[count++] = *p;
    }
    np_ent = &lay->PartitionEntry[count++];
    memset(np_ent, 0, sizeof *np_ent);
    np_ent->PartitionStyle = PARTITION_STYLE_GPT;
    np_ent->StartingOffset.QuadPart = (LONGLONG)(start * d.bs);
    np_ent->PartitionLength.QuadPart = (LONGLONG)(len_sectors * d.bs);
    np_ent->Gpt.PartitionType = LINUX_DATA;
    if (UuidCreate(&np_ent->Gpt.PartitionId) != RPC_S_OK) {
        free(lay);
        wdisk_close(&d);
        return fail("uuid", "生成分区 GUID 失败", 0);
    }
    wcsncpy(np_ent->Gpt.Name, name, GK3_GPT_NAME_CHARS);

    rc = set_layout(&d, lay, count);
    if (!rc) {
        GUID id = np_ent->Gpt.PartitionId;
        const PARTITION_INFORMATION_EX *p;
        if (wdisk_read_layout(&d) || !(p = find_part(&d, &id)))
            rc = fail("verify-layout", "写完分区表后重读，找不到新分区", 0);
        else
            emit_part("MKPART", &d, p);
    }
    free(lay);
    wdisk_close(&d);
    return rc;
}

int gk3w_cmd_rmpart(int argc, wchar_t **argv)
{
    uint64_t num;
    GUID id;
    wdisk d;
    DRIVE_LAYOUT_INFORMATION_EX *lay;
    DWORD count = 0;
    PARTITION_INFORMATION_EX *victim;
    int rc;

    if (arg_u64(argc, argv, L"--disk", &num) || num > 255 || parse_guid(arg(argc, argv, L"--uuid"), &id))
        return fail("usage", "用法：rmpart --disk N --uuid GUID", 0);
    if (wdisk_open_vhd((DWORD)num, &d))
        return 1;
    victim = find_part(&d, &id);
    if (!victim || memcmp(&victim->Gpt.PartitionType, &LINUX_DATA, sizeof LINUX_DATA)) {
        wdisk_close(&d);
        return fail("bad-part", "找不到这个分区，或它不是 Linux 数据类型（原型只删自己建的分区）", 0);
    }
    lay = calloc(1, d.laysz);
    if (!lay) {
        wdisk_close(&d);
        return fail("nomem", "内存不足", 0);
    }
    lay->PartitionStyle = PARTITION_STYLE_GPT;
    lay->Gpt = d.lay->Gpt;
    for (DWORD i = 0; i < d.lay->PartitionCount; i++) {
        const PARTITION_INFORMATION_EX *p = &d.lay->PartitionEntry[i];
        if (p->PartitionStyle == PARTITION_STYLE_GPT && p->PartitionLength.QuadPart && p != victim)
            lay->PartitionEntry[count++] = *p;
    }
    rc = set_layout(&d, lay, count);
    if (!rc) {
        if (wdisk_read_layout(&d) || find_part(&d, &id))
            rc = fail("verify-layout", "删完重读，分区还在", 0);
        else
            printf("RMPART disk=%lu ok\n", (unsigned long)num);
    }
    free(lay);
    wdisk_close(&d);
    return rc;
}

static int pwrite_all(HANDLE h, const void *buf, DWORD len, uint64_t off)
{
    OVERLAPPED ov;
    DWORD got = 0;
    memset(&ov, 0, sizeof ov);
    ov.Offset = (DWORD)off;
    ov.OffsetHigh = (DWORD)(off >> 32);
    return WriteFile(h, buf, len, &got, &ov) && got == len ? 0 : -1;
}

static int pread_all(HANDLE h, void *buf, DWORD len, uint64_t off)
{
    OVERLAPPED ov;
    DWORD got = 0;
    memset(&ov, 0, sizeof ov);
    ov.Offset = (DWORD)off;
    ov.OffsetHigh = (DWORD)(off >> 32);
    return ReadFile(h, buf, len, &got, &ov) && got == len ? 0 : -1;
}

int gk3w_cmd_write(int argc, wchar_t **argv)
{
    uint64_t num, part_off, part_len, fsize, done = 0;
    GUID id;
    wdisk d;
    const wchar_t *file = arg(argc, argv, L"--file");
    PARTITION_INFORMATION_EX *p;
    HANDLE f;
    LARGE_INTEGER fl;
    uint8_t *buf = NULL, *chk = NULL;
    int rc = 0;

    if (arg_u64(argc, argv, L"--disk", &num) || num > 255 || parse_guid(arg(argc, argv, L"--uuid"), &id) || !file)
        return fail("usage", "用法：write --disk N --uuid GUID --file PATH", 0);
    if (wdisk_open_vhd((DWORD)num, &d))
        return 1;
    p = find_part(&d, &id);
    if (!p || memcmp(&p->Gpt.PartitionType, &LINUX_DATA, sizeof LINUX_DATA)) {
        wdisk_close(&d);
        return fail("bad-part", "找不到这个分区，或它不是 Linux 数据类型", 0);
    }
    part_off = (uint64_t)p->StartingOffset.QuadPart;
    part_len = (uint64_t)p->PartitionLength.QuadPart;
    f = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        wdisk_close(&d);
        return fail("file-open", "打不开镜像文件", GetLastError());
    }
    if (!GetFileSizeEx(f, &fl) || (fsize = (uint64_t)fl.QuadPart) > part_len) {
        CloseHandle(f);
        wdisk_close(&d);
        return fail("file-too-big", "镜像比分区大", 0);
    }
    buf = _aligned_malloc(CHUNK, 4096);
    chk = _aligned_malloc(CHUNK, 4096);
    if (!buf || !chk) {
        rc = fail("nomem", "内存不足", 0);
        goto out;
    }
    gk3w_progress(0, "write");
    while (done < fsize) {
        DWORD want = (DWORD)(fsize - done < CHUNK ? fsize - done : CHUNK), got = 0, padded;
        if (!ReadFile(f, buf, want, &got, NULL) || got != want) {
            rc = fail("file-read", "读镜像文件失败", GetLastError());
            goto out;
        }
        /* 最后一块补零到扇区边界（无缓冲写要求长度按扇区对齐） */
        padded = (want + d.bs - 1) / d.bs * d.bs;
        memset(buf + want, 0, padded - want);
        if (pwrite_all(d.h, buf, padded, part_off + done)) {
            rc = fail("disk-write", "写分区失败", GetLastError());
            goto out;
        }
        if (pread_all(d.h, chk, padded, part_off + done) || memcmp(buf, chk, padded)) {
            rc = fail("disk-verify", "写后读回不一致", GetLastError());
            goto out;
        }
        done += want;
        gk3w_progress((unsigned)(done * 100 / fsize), "write");
    }
    printf("WRITE disk=%lu bytes=%llu verified=yes\n", (unsigned long)num, (unsigned long long)fsize);
out:
    _aligned_free(buf);
    _aligned_free(chk);
    CloseHandle(f);
    wdisk_close(&d);
    return rc ? 1 : 0;
}

/* 卷 GUID 路径对应的卷是否在 VHD 上（往 ESP 写之前查） */
static int volume_on_vhd(const wchar_t *vol)
{
    wchar_t dev[MAX_PATH];
    size_t n = wcslen(vol);
    HANDLE h;
    union {
        VOLUME_DISK_EXTENTS e;
        BYTE raw[sizeof(VOLUME_DISK_EXTENTS) + 4 * sizeof(DISK_EXTENT)];
    } ext;
    DWORD got = 0;
    if (n < 2 || n >= MAX_PATH)
        return 0;
    wcscpy(dev, vol);
    if (dev[n - 1] == L'\\')
        dev[n - 1] = 0;
    h = CreateFileW(dev, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    if (!DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0, &ext, sizeof ext, &got, NULL) ||
        ext.e.NumberOfDiskExtents != 1) {
        CloseHandle(h);
        return 0;
    }
    CloseHandle(h);
    return disk_is_vhd(ext.e.Extents[0].DiskNumber);
}

int gk3w_cmd_esp_ls(int argc, wchar_t **argv)
{
    const wchar_t *vol = arg(argc, argv, L"--vol"), *sub = arg(argc, argv, L"--dir");
    wchar_t pat[MAX_PATH * 2];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    if (!vol)
        return fail("usage", "用法：esp-ls --vol \\\\?\\Volume{…}\\ [--dir SUB]", 0);
    _snwprintf(pat, MAX_PATH * 2, L"%ls%ls%ls*", vol, sub ? sub : L"", sub ? L"\\" : L"");
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return fail("esp-list", "列不出 ESP 目录", GetLastError());
    do {
        gk3w_line l = {0};
        char name[MAX_PATH * 3];
        gk3w_w2u(fd.cFileName, name, sizeof name);
        gk3w_line_begin(&l, "ESPENT");
        gk3w_line_kv(&l, "name", name);
        gk3w_line_kv(&l, "dir", (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? "yes" : "no");
        gk3w_line_u64(&l, "size", ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow);
        (void)gk3w_line_emit(&l, stdout);
        gk3w_line_free(&l);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return 0;
}

int gk3w_cmd_esp_put(int argc, wchar_t **argv)
{
    const wchar_t *vol = arg(argc, argv, L"--vol"), *file = arg(argc, argv, L"--file"), *dest = arg(argc, argv, L"--dest");
    wchar_t path[MAX_PATH * 2], dir[MAX_PATH * 2];
    HANDLE in, out;
    uint8_t *data = NULL, *back = NULL;
    LARGE_INTEGER sz;
    DWORD got = 0;
    int rc = 0;
    if (!vol || !file || !dest)
        return fail("usage", "用法：esp-put --vol \\\\?\\Volume{…}\\ --file PATH --dest SUB\\NAME", 0);
    if (!volume_on_vhd(vol))
        return fail("not-vhd", "原型只往 VHD 上的 ESP 写", 0);
    in = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (in == INVALID_HANDLE_VALUE)
        return fail("file-open", "打不开源文件", GetLastError());
    if (!GetFileSizeEx(in, &sz) || sz.QuadPart > (64ll << 20) || !(data = malloc((size_t)sz.QuadPart + 1)) ||
        !(back = malloc((size_t)sz.QuadPart + 1)) || !ReadFile(in, data, (DWORD)sz.QuadPart, &got, NULL) ||
        got != (DWORD)sz.QuadPart) {
        CloseHandle(in);
        free(data);
        free(back);
        return fail("file-read", "读源文件失败（或大于 64 MiB）", GetLastError());
    }
    CloseHandle(in);
    /* 逐级建目录 */
    _snwprintf(path, MAX_PATH * 2, L"%ls%ls", vol, dest);
    wcscpy(dir, path);
    for (wchar_t *s = dir + wcslen(vol); *s; s++) {
        if (*s == L'\\') {
            *s = 0;
            if (!CreateDirectoryW(dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
                rc = fail("esp-mkdir", "在 ESP 上建目录失败", GetLastError());
                goto done;
            }
            *s = L'\\';
        }
    }
    out = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_FLAG_WRITE_THROUGH, NULL);
    if (out == INVALID_HANDLE_VALUE) {
        rc = fail("esp-open", "在 ESP 上建文件失败", GetLastError());
        goto done;
    }
    if (!WriteFile(out, data, (DWORD)sz.QuadPart, &got, NULL) || got != (DWORD)sz.QuadPart || !FlushFileBuffers(out)) {
        CloseHandle(out);
        rc = fail("esp-write", "写 ESP 文件失败", GetLastError());
        goto done;
    }
    CloseHandle(out);
    in = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (in == INVALID_HANDLE_VALUE || !ReadFile(in, back, (DWORD)sz.QuadPart, &got, NULL) || got != (DWORD)sz.QuadPart ||
        memcmp(data, back, (size_t)sz.QuadPart)) {
        if (in != INVALID_HANDLE_VALUE)
            CloseHandle(in);
        rc = fail("esp-verify", "ESP 文件读回不一致", GetLastError());
        goto done;
    }
    CloseHandle(in);
    printf("ESPPUT bytes=%lld verified=yes\n", (long long)sz.QuadPart);
done:
    free(data);
    free(back);
    return rc;
}
