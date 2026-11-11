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

int gk3w_disk_open_vhd(DWORD num, gk3w_disk *d, gk3w_touched t)
{
    wchar_t path[40];
    STORAGE_PROPERTY_QUERY q;
    STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR align;
    GET_LENGTH_INFORMATION li;
    DWORD got = 0;

    memset(d, 0, sizeof *d);
    d->num = num;
    d->h = INVALID_HANDLE_VALUE;
    if (!gk3w_disk_is_vhd(num))
        return gk3w_err(t, "not-vhd", "原型只对 VHD 虚拟盘写入，这块盘不是 VHD", 0), -1;
    _snwprintf(path, 40, L"\\\\.\\PhysicalDrive%lu", (unsigned long)num);
    d->h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                       FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH, NULL);
    if (d->h == INVALID_HANDLE_VALUE)
        return gk3w_err(t, "disk-open", "无法以读写方式打开磁盘", GetLastError()), -1;
    memset(&q, 0, sizeof q);
    q.QueryType = PropertyStandardQuery;
    q.PropertyId = StorageAccessAlignmentProperty;
    d->bs = 512;
    if (DeviceIoControl(d->h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, &align, sizeof align, &got, NULL) &&
        got >= sizeof align)
        d->bs = align.BytesPerLogicalSector;
    if (d->bs != 512 && d->bs != 4096) {
        gk3w_disk_close(d);
        return gk3w_err(t, "disk-sector", "扇区大小既不是 512 也不是 4096 字节", 0), -1;
    }
    if (!DeviceIoControl(d->h, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &li, sizeof li, &got, NULL)) {
        DWORD e = GetLastError();
        gk3w_disk_close(d);
        return gk3w_err(t, "disk-size", "读不到磁盘容量", e), -1;
    }
    d->size = (uint64_t)li.Length.QuadPart;
    if (gk3w_disk_read_layout(d) || d->lay->PartitionStyle != PARTITION_STYLE_GPT) {
        DWORD e = GetLastError();
        gk3w_disk_close(d);
        return gk3w_err(t, "disk-not-gpt", "读不到 GPT 分区表", e), -1;
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
        return gk3w_err(t, "set-layout", "写分区表失败", GetLastError()), -1;
    if (!DeviceIoControl(d->h, IOCTL_DISK_UPDATE_PROPERTIES, NULL, 0, NULL, 0, &got, NULL))
        return gk3w_err(t, "update-properties", "分区表已写，但分区管理器没能重读", GetLastError()), -1;
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

void gk3w_disk_usable(const gk3w_disk *d, uint64_t *first, uint64_t *last)
{
    *first = (uint64_t)d->lay->Gpt.StartingUsableOffset.QuadPart / d->bs;
    *last = ((uint64_t)d->lay->Gpt.StartingUsableOffset.QuadPart + (uint64_t)d->lay->Gpt.UsableLength.QuadPart) / d->bs - 1;
}

int gk3w_pwrite(HANDLE h, const void *buf, DWORD len, uint64_t off)
{
    OVERLAPPED ov;
    DWORD got = 0;
    memset(&ov, 0, sizeof ov);
    ov.Offset = (DWORD)off;
    ov.OffsetHigh = (DWORD)(off >> 32);
    return WriteFile(h, buf, len, &got, &ov) && got == len ? 0 : -1;
}

int gk3w_pread(HANDLE h, void *buf, DWORD len, uint64_t off)
{
    OVERLAPPED ov;
    DWORD got = 0;
    memset(&ov, 0, sizeof ov);
    ov.Offset = (DWORD)off;
    ov.OffsetHigh = (DWORD)(off >> 32);
    return ReadFile(h, buf, len, &got, &ov) && got == len ? 0 : -1;
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
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    size_t done = 0;
    uint8_t *back = NULL;
    size_t blen = 0;
    int ok;
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
    if (gk3w_read_file(path, &back, &blen, len + 1))
        return -1;
    ok = blen == len && !memcmp(back, data, len);
    free(back);
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
