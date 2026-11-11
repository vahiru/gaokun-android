/* Windows 侧共用的磁盘、卷、文件与参数工具（说明见 disk.h）。 */
#include "disk.h"

#include <bcrypt.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "win.h"

const GUID GK3W_LINUX_DATA = {0x0FC63DAF, 0x8483, 0x4772, {0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4}};
const GUID GK3W_ESP_TYPE = {0xC12A7328, 0xF81F, 0x11D2, {0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B}};

/* ── 参数 ── */

const wchar_t *gk3w_arg(int argc, wchar_t **argv, const wchar_t *key)
{
    for (int i = 0; i + 1 < argc; i++)
        if (!wcscmp(argv[i], key))
            return argv[i + 1];
    return NULL;
}

int gk3w_flag(int argc, wchar_t **argv, const wchar_t *key)
{
    for (int i = 0; i < argc; i++)
        if (!wcscmp(argv[i], key))
            return 1;
    return 0;
}

int gk3w_arg_u64(int argc, wchar_t **argv, const wchar_t *key, uint64_t *out)
{
    const wchar_t *v = gk3w_arg(argc, argv, key);
    wchar_t *end = NULL;
    if (!v || !*v || *v < L'0' || *v > L'9')
        return -1;
    errno = 0;
    *out = wcstoull(v, &end, 10);
    return (errno || !end || *end) ? -1 : 0;
}

/* ── 失败与日志 ── */

int gk3w_err_kv(gk3w_touched t, const char *code, const char *k, const char *v, const char *human, DWORD winerr)
{
    gk3w_line l = {0};
    gk3w_line_begin(&l, "ERR");
    gk3w_line_kv(&l, "code", code);
    if (k)
        gk3w_line_kv(&l, k, v);
    if (winerr)
        gk3w_line_u64(&l, "winerr", winerr);
    gk3w_fail(&l, t, human);
    gk3w_line_free(&l);
    return 1;
}

int gk3w_err(gk3w_touched t, const char *code, const char *human, DWORD winerr)
{
    return gk3w_err_kv(t, code, NULL, NULL, human, winerr);
}

void gk3w_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

int gk3w_u2w(const char *s, wchar_t *out, size_t cap)
{
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, out, (int)(cap > 0x7fffffff ? 0x7fffffff : cap));
    if (n <= 0) {
        if (cap)
            out[0] = 0;
        return -1;
    }
    return 0;
}

/* ── 磁盘 ── */

int gk3w_disk_is_vhd(DWORD num)
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

void gk3w_disk_close(gk3w_disk *d)
{
    free(d->lay);
    d->lay = NULL;
    if (d->h && d->h != INVALID_HANDLE_VALUE)
        CloseHandle(d->h);
    d->h = INVALID_HANDLE_VALUE;
}

int gk3w_disk_read_layout(gk3w_disk *d)
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

long gk3w_system_disk_number(void)
{
    wchar_t win[MAX_PATH], dev[8];
    HANDLE h;
    VOLUME_DISK_EXTENTS e;
    DWORD got = 0;
    if (!GetSystemWindowsDirectoryW(win, MAX_PATH) || win[1] != L':')
        return -1;
    _snwprintf(dev, 8, L"\\\\.\\%lc:", win[0]);
    h = CreateFileW(dev, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    /* 系统卷跨盘（动态卷）时这个调用会因缓冲区不够失败：这种布局本来就不支持 */
    if (!DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0, &e, sizeof e, &got, NULL)) {
        CloseHandle(h);
        return -1;
    }
    CloseHandle(h);
    return e.NumberOfDiskExtents == 1 ? (long)e.Extents[0].DiskNumber : -1;
}

static int disk_open_any(DWORD num, gk3w_disk *d, gk3w_touched t);

int gk3w_disk_open_vhd(DWORD num, gk3w_disk *d, gk3w_touched t)
{
    memset(d, 0, sizeof *d);
    d->num = num;
    d->h = INVALID_HANDLE_VALUE;
    /* 先查类型、再以读写方式打开：真实磁盘连写句柄都不拿 */
    if (!gk3w_disk_is_vhd(num))
        return gk3w_err(t, "not-vhd", "this disk is not a VHD; writing a real disk requires --real-disk", 0), -1;
    return disk_open_any(num, d, t);
}

int gk3w_disk_open(DWORD num, int real, gk3w_disk *d, gk3w_touched t)
{
    long sys;
    if (!real)
        return gk3w_disk_open_vhd(num, d, t);
    memset(d, 0, sizeof *d);
    d->num = num;
    d->h = INVALID_HANDLE_VALUE;
    sys = gk3w_system_disk_number();
    if (sys < 0 || (DWORD)sys != num || gk3w_disk_is_vhd(num))
        return gk3w_err(t, "not-system-disk", "--real-disk only accepts the disk that holds Windows; this is not it", 0), -1;
    return disk_open_any(num, d, t);
}

static int disk_open_any(DWORD num, gk3w_disk *d, gk3w_touched t)
{
    wchar_t path[40];
    STORAGE_PROPERTY_QUERY q;
    STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR align;
    GET_LENGTH_INFORMATION li;
    DWORD got = 0;

    _snwprintf(path, 40, L"\\\\.\\PhysicalDrive%lu", (unsigned long)num);
    d->h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                       FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH, NULL);
    if (d->h == INVALID_HANDLE_VALUE)
        return gk3w_err(t, "disk-open", "cannot open the disk for reading and writing", GetLastError()), -1;
    memset(&q, 0, sizeof q);
    q.QueryType = PropertyStandardQuery;
    q.PropertyId = StorageAccessAlignmentProperty;
    d->bs = 0;
    if (DeviceIoControl(d->h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, &align, sizeof align, &got, NULL) &&
        got >= sizeof align)
        d->bs = align.BytesPerLogicalSector;
    if (!d->bs) {
        /* 不支持对齐属性的盘：与 probe 一样退回几何信息，不猜 512（4Kn 盘上猜错会把区间整个解读错） */
        DISK_GEOMETRY_EX geo;
        if (DeviceIoControl(d->h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL, 0, &geo, sizeof geo, &got, NULL))
            d->bs = geo.Geometry.BytesPerSector;
    }
    if (d->bs != 512 && d->bs != 4096) {
        gk3w_disk_close(d);
        return gk3w_err(t, "disk-sector", "the sector size is neither 512 nor 4096 bytes", 0), -1;
    }
    if (!DeviceIoControl(d->h, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &li, sizeof li, &got, NULL)) {
        DWORD e = GetLastError();
        gk3w_disk_close(d);
        return gk3w_err(t, "disk-size", "cannot read the disk size", e), -1;
    }
    d->size = (uint64_t)li.Length.QuadPart;
    if (gk3w_disk_read_layout(d) || d->lay->PartitionStyle != PARTITION_STYLE_GPT) {
        DWORD e = GetLastError();
        gk3w_disk_close(d);
        return gk3w_err(t, "disk-not-gpt", "cannot read a GPT partition table", e), -1;
    }
    return 0;
}

int gk3w_disk_set_layout(gk3w_disk *d, DRIVE_LAYOUT_INFORMATION_EX *lay, DWORD count, gk3w_touched t)
{
    DWORD got = 0;
    DWORD sz = (DWORD)(offsetof(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry) + count * sizeof(PARTITION_INFORMATION_EX));
    lay->PartitionCount = count;
    for (DWORD i = 0; i < count; i++)
        lay->PartitionEntry[i].RewritePartition = TRUE;
    if (!DeviceIoControl(d->h, IOCTL_DISK_SET_DRIVE_LAYOUT_EX, lay, sz, NULL, 0, &got, NULL))
        return gk3w_err(t, "set-layout", "writing the partition table failed", GetLastError()), -1;
    if (!DeviceIoControl(d->h, IOCTL_DISK_UPDATE_PROPERTIES, NULL, 0, NULL, 0, &got, NULL))
        return gk3w_err(t, "update-properties", "the partition table was written but the partition manager did not re-read it", GetLastError()), -1;
    /* 此后的读写可能遇到 STATUS_VERIFY_REQUIRED，由 io_retry 处理 */
    return 0;
}

PARTITION_INFORMATION_EX *gk3w_disk_find_part(gk3w_disk *d, const GUID *id)
{
    for (DWORD i = 0; i < d->lay->PartitionCount; i++) {
        PARTITION_INFORMATION_EX *p = &d->lay->PartitionEntry[i];
        if (p->PartitionStyle == PARTITION_STYLE_GPT && p->PartitionLength.QuadPart &&
            !memcmp(&p->Gpt.PartitionId, id, sizeof *id))
            return p;
    }
    return NULL;
}

int gk3w_gpt_backup(gk3w_disk *d, const wchar_t *dir, const wchar_t *tag, gk3w_touched t)
{
    uint64_t uf, ul, total = d->size / d->bs, head, tail;
    wchar_t bpath[MAX_PATH * 2];
    SYSTEMTIME st;
    uint8_t *gb;
    char b8[MAX_PATH * 3];
    int w;
    gk3w_disk_usable(d, &uf, &ul);
    head = uf;
    tail = total - (ul + 1);
    if ((head + tail) * d->bs > (4u << 20))
        return gk3w_err(t, "gpt-backup", "the partition table area is unusually large; refusing to continue", 0), -1;
    gb = _aligned_malloc((size_t)((head + tail) * d->bs), 4096);
    if (!gb)
        return gk3w_err(t, "nomem", "out of memory", 0), -1;
    if (gk3w_pread(d->h, gb, (DWORD)(head * d->bs), 0) ||
        gk3w_pread(d->h, gb + head * d->bs, (DWORD)(tail * d->bs), (ul + 1) * d->bs)) {
        _aligned_free(gb);
        return gk3w_err(t, "gpt-backup", "reading the partition table area failed", GetLastError()), -1;
    }
    GetLocalTime(&st);
    _snwprintf(bpath, MAX_PATH * 2, L"%ls\\gpt-before-%ls-disk%lu-%04u%02u%02u-%02u%02u%02u-head%llu-tail%llu.bin", dir, tag,
               (unsigned long)d->num, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
               (unsigned long long)head, (unsigned long long)tail);
    gk3w_mkdirs(dir, (dir[0] && dir[1] == L':') ? 2 : 0);
    w = gk3w_write_file_verify(bpath, gb, (size_t)((head + tail) * d->bs));
    _aligned_free(gb);
    if (w)
        return gk3w_err(t, "gpt-backup", "writing the partition table backup failed", GetLastError()), -1;
    gk3w_w2u(bpath, b8, sizeof b8);
    gk3w_log("partition table backup: %s", b8);
    return 0;
}

void gk3w_disk_usable(const gk3w_disk *d, uint64_t *first, uint64_t *last)
{
    *first = (uint64_t)d->lay->Gpt.StartingUsableOffset.QuadPart / d->bs;
    *last = ((uint64_t)d->lay->Gpt.StartingUsableOffset.QuadPart + (uint64_t)d->lay->Gpt.UsableLength.QuadPart) / d->bs - 1;
}

/* 一次读或写。0 = 成功；1 = 设备要求先确认介质（STATUS_VERIFY_REQUIRED）；-1 = 失败（last error 已设）。
 *
 * ★ 2026-11-11 VHDX 上实测：分区表改动之后（IOCTL_DISK_SET_DRIVE_LAYOUT_EX），之后某一次读写会以 NTSTATUS
 *   0x80000010（STATUS_VERIFY_REQUIRED，警告级）完成 —— WriteFile 返回 TRUE、传了 0 字节，五轮里每轮一次，
 *   位置不固定（新分区开头附近）。平时由文件系统去确认介质，直接读写裸盘就得自己来：发 IOCTL_DISK_CHECK_VERIFY
 *   再重做这一次。 */
#define GK3W_STATUS_VERIFY_REQUIRED ((ULONG_PTR)0x80000010)

static int io_once(HANDLE h, void *buf, DWORD len, uint64_t off, int write)
{
    OVERLAPPED ov;
    DWORD got = 0;
    BOOL ok;
    memset(&ov, 0, sizeof ov);
    ov.Offset = (DWORD)off;
    ov.OffsetHigh = (DWORD)(off >> 32);
    ok = write ? WriteFile(h, buf, len, &got, &ov) : ReadFile(h, buf, len, &got, &ov);
    if (ok && got == len)
        return 0;
    if (ok && ov.Internal == GK3W_STATUS_VERIFY_REQUIRED)
        return 1;
    if (ok)
        SetLastError(ERROR_WRITE_FAULT);
    gk3w_log("%s failed: off=%llu len=%lu got=%lu ok=%d err=%lu status=0x%08llx", write ? "WriteFile" : "ReadFile",
             (unsigned long long)off, (unsigned long)len, (unsigned long)got, ok, (unsigned long)GetLastError(),
             (unsigned long long)ov.Internal);
    return -1;
}

static int io_retry(HANDLE h, void *buf, DWORD len, uint64_t off, int write)
{
    for (int attempt = 1;; attempt++) {
        int r = io_once(h, buf, len, off, write);
        DWORD got = 0, err;
        if (!r)
            return 0;
        if (attempt == 4)
            return -1;
        if (r == 1) {
            /* 确认介质：这一次读写本身没有发生，确认之后重做 */
            if (!DeviceIoControl(h, IOCTL_DISK_CHECK_VERIFY, NULL, 0, NULL, 0, &got, NULL))
                gk3w_log("the disk asked for a media verify; IOCTL_DISK_CHECK_VERIFY failed (%lu), retrying anyway", (unsigned long)GetLastError());
            else
                gk3w_log("the disk asked for a media verify (STATUS_VERIFY_REQUIRED, off=%llu); verified, redoing the %s",
                         (unsigned long long)off, write ? "write" : "read");
            continue;
        }
        err = GetLastError();
        Sleep(500 * attempt);
        SetLastError(err);
    }
}

int gk3w_pwrite(HANDLE h, const void *buf, DWORD len, uint64_t off)
{
    return io_retry(h, (void *)(uintptr_t)buf, len, off, 1);
}

int gk3w_pread(HANDLE h, void *buf, DWORD len, uint64_t off)
{
    return io_retry(h, buf, len, off, 0);
}

int gk3w_write_verify(HANDLE h, const void *buf, DWORD len, uint64_t off, void *chk)
{
    if (gk3w_pwrite(h, buf, len, off))
        return -1;
    if (gk3w_pread(h, chk, len, off) || memcmp(buf, chk, len))
        return -2;
    return 0;
}

/* ── 卷 ── */

int gk3w_find_volume(DWORD disk, uint64_t offset, wchar_t *out, size_t cap)
{
    wchar_t name[MAX_PATH];
    HANDLE f = FindFirstVolumeW(name, MAX_PATH);
    int found = -1;
    if (f == INVALID_HANDLE_VALUE)
        return -1;
    do {
        size_t n = wcslen(name);
        wchar_t dev[MAX_PATH];
        HANDLE h;
        union {
            VOLUME_DISK_EXTENTS e;
            BYTE raw[sizeof(VOLUME_DISK_EXTENTS) + 16 * sizeof(DISK_EXTENT)];
        } ext;
        DWORD got = 0;
        if (n < 2 || n >= MAX_PATH)
            continue;
        wcscpy(dev, name);
        dev[n - 1] = 0;
        h = CreateFileW(dev, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        if (DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0, &ext, sizeof ext, &got, NULL) &&
            ext.e.NumberOfDiskExtents == 1 && ext.e.Extents[0].DiskNumber == disk &&
            (uint64_t)ext.e.Extents[0].StartingOffset.QuadPart == offset && n < cap) {
            wcscpy(out, name);
            found = 0;
        }
        CloseHandle(h);
    } while (found && FindNextVolumeW(f, name, MAX_PATH));
    FindVolumeClose(f);
    return found;
}

/* ── 文件 ── */

int gk3w_exists(const wchar_t *path)
{
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

int gk3w_read_file(const wchar_t *path, uint8_t **data, size_t *len, size_t max)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    LARGE_INTEGER sz;
    uint8_t *b;
    size_t done = 0;
    *data = NULL;
    *len = 0;
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    if (!GetFileSizeEx(h, &sz) || (uint64_t)sz.QuadPart > max || !(b = malloc((size_t)sz.QuadPart + 1))) {
        CloseHandle(h);
        return -1;
    }
    while (done < (size_t)sz.QuadPart) {
        DWORD want = (DWORD)((size_t)sz.QuadPart - done > (1u << 24) ? (1u << 24) : (size_t)sz.QuadPart - done), got = 0;
        if (!ReadFile(h, b + done, want, &got, NULL) || got != want) {
            free(b);
            CloseHandle(h);
            return -1;
        }
        done += got;
    }
    CloseHandle(h);
    *data = b;
    *len = done;
    return 0;
}

int gk3w_write_file_verify(const wchar_t *path, const uint8_t *data, size_t len)
{
    DWORD attrs = GetFileAttributesW(path), keep = 0;
    HANDLE h;
    size_t done = 0, cap;
    uint8_t *back;
    int ok;
    /* CREATE_ALWAYS 遇到带隐藏 / 系统 / 只读属性的现有文件会以 ERROR_ACCESS_DENIED 失败：先清掉，写完再还原 */
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        keep = attrs & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_ARCHIVE);
        if (keep & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_READONLY))
            SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL);
    }
    h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    while (done < len) {
        DWORD want = (DWORD)(len - done > (1u << 24) ? (1u << 24) : len - done), got = 0;
        if (!WriteFile(h, data + done, want, &got, NULL) || got != want) {
            CloseHandle(h);
            return -1;
        }
        done += got;
    }
    if (!FlushFileBuffers(h)) {
        CloseHandle(h);
        return -1;
    }
    CloseHandle(h);
    if (keep & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_READONLY))
        SetFileAttributesW(path, keep);

    /* 读回绕过缓存（FILE_FLAG_NO_BUFFERING）：读到的才是介质上的字节。无缓冲读要按扇区整读，缓冲区按 4096 对齐 */
    cap = (len + 4096) / 4096 * 4096;
    back = _aligned_malloc(cap, 4096);
    if (!back)
        return -1;
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        _aligned_free(back);
        return -1;
    }
    done = 0;
    for (;;) {
        DWORD got = 0, want = (DWORD)(cap - done > (1u << 24) ? (1u << 24) : cap - done);
        if (!want)
            break;
        if (!ReadFile(h, back + done, want, &got, NULL)) {
            CloseHandle(h);
            _aligned_free(back);
            return -1;
        }
        done += got;
        if (got < want)
            break;   /* 到文件尾 */
    }
    CloseHandle(h);
    ok = done == len && !memcmp(back, data, len);
    _aligned_free(back);
    return ok ? 0 : -2;
}

int gk3w_mkdirs(const wchar_t *path, size_t root_len)
{
    wchar_t buf[MAX_PATH * 2];
    size_t n = wcslen(path);
    if (n >= MAX_PATH * 2)
        return -1;
    wcscpy(buf, path);
    for (size_t i = root_len; i <= n; i++) {
        if (buf[i] == L'\\' || buf[i] == 0) {
            wchar_t c = buf[i];
            buf[i] = 0;
            if (i > root_len && !CreateDirectoryW(buf, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
                return -1;
            buf[i] = c;
        }
    }
    return 0;
}

/* ── 散列与随机数（CNG） ── */

struct gk3w_sha256_ctx {
    BCRYPT_ALG_HANDLE alg;
    BCRYPT_HASH_HANDLE h;
};

gk3w_sha256_ctx *gk3w_sha256_begin(void)
{
    gk3w_sha256_ctx *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    if (BCryptOpenAlgorithmProvider(&c->alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) ||
        BCryptCreateHash(c->alg, &c->h, NULL, 0, NULL, 0, 0)) {
        if (c->alg)
            BCryptCloseAlgorithmProvider(c->alg, 0);
        free(c);
        return NULL;
    }
    return c;
}

int gk3w_sha256_update(gk3w_sha256_ctx *c, const void *data, size_t len)
{
    const uint8_t *p = data;
    while (len) {
        ULONG n = (ULONG)(len > (1u << 30) ? (1u << 30) : len);
        if (BCryptHashData(c->h, (PUCHAR)(uintptr_t)p, n, 0))
            return -1;
        p += n;
        len -= n;
    }
    return 0;
}

int gk3w_sha256_end(gk3w_sha256_ctx *c, uint8_t out[32])
{
    int rc = BCryptFinishHash(c->h, out, 32, 0) ? -1 : 0;
    BCryptDestroyHash(c->h);
    BCryptCloseAlgorithmProvider(c->alg, 0);
    free(c);
    return rc;
}

int gk3w_sha256(const void *data, size_t len, uint8_t out[32])
{
    gk3w_sha256_ctx *c = gk3w_sha256_begin();
    if (!c)
        return -1;
    if (gk3w_sha256_update(c, data, len)) {
        uint8_t dummy[32];
        gk3w_sha256_end(c, dummy);
        return -1;
    }
    return gk3w_sha256_end(c, out);
}

void gk3w_hex(const uint8_t *b, size_t n, char *out)
{
    static const char hx[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = hx[b[i] >> 4];
        out[2 * i + 1] = hx[b[i] & 15];
    }
    out[2 * n] = 0;
}

int gk3w_random(void *buf, size_t n)
{
    return BCryptGenRandom(NULL, buf, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) ? -1 : 0;
}
