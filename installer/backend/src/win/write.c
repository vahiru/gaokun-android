/* Windows：阶段 1 的写盘验证命令（验证 Windows 运行时能否这样改分区、写分区、写 ESP）。
 *
 *   gk3w mkpart  --disk N --start LBA --size-mib M --name NAME   在空闲区里加一个 GPT 分区（类型 Linux 数据）
 *   gk3w rmpart  --disk N --uuid GUID                            删掉一个 Linux 数据类型的分区
 *   gk3w write   --disk N --uuid GUID --file PATH                把文件写进分区开头，写完逐块读回比对
 *   gk3w esp-ls  --vol \\?\Volume{…}\ [--dir SUB]                列 ESP 目录（只读，不需要盘符）
 *   gk3w esp-put --vol \\?\Volume{…}\ --file PATH --dest SUB     往 ESP 写一个文件并读回比对
 *
 * ★ 原型阶段只对 VHD 虚拟盘动手：会写盘的命令先查总线类型，不是 BusTypeFileBackedVirtual 一律拒绝。 */
#include <windows.h>
#include <winioctl.h>
#include <rpc.h>

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../layout.h"
#include "../proto.h"
#include "disk.h"
#include "gk3core.h"
#include "win.h"

#define CHUNK (4u << 20)

static int parse_guid(const wchar_t *s, GUID *g)
{
    RPC_WSTR w = (RPC_WSTR)(uintptr_t)s;
    return s && UuidFromStringW(w, g) == RPC_S_OK ? 0 : -1;
}

static void emit_part(const char *type, const gk3w_disk *d, const PARTITION_INFORMATION_EX *p)
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
    const wchar_t *name = gk3w_arg(argc, argv, L"--name");
    gk3w_disk d;
    gk3w_span parts[GK3W_MAX_PARTS], spans[GK3W_MAX_PARTS + 1], want;
    size_t np = 0, nf = 0;
    int inside = 0, rc;
    DRIVE_LAYOUT_INFORMATION_EX *lay;
    PARTITION_INFORMATION_EX *e;
    DWORD count = 0;

    if (gk3w_arg_u64(argc, argv, L"--disk", &num) || gk3w_arg_u64(argc, argv, L"--start", &start) ||
        gk3w_arg_u64(argc, argv, L"--size-mib", &mib) || !name || !*name || wcslen(name) > GK3_GPT_NAME_CHARS ||
        num > 255 || !mib || mib > (1ull << 32))
        return gk3w_err(GK3W_TOUCHED_NA, "usage", "usage: mkpart --disk N --start LBA --size-mib M --name NAME", 0);
    if (gk3w_disk_open_vhd((DWORD)num, &d, GK3W_TOUCHED_NA))
        return 1;
    len_sectors = (mib << 20) / d.bs;
    if (start % ((1u << 20) / d.bs) || start > UINT64_MAX - len_sectors) {
        gk3w_disk_close(&d);
        return gk3w_err(GK3W_TOUCHED_NA, "bad-region", "the start is not aligned to 1 MiB", 0);
    }
    want.first = start;
    want.last = start + len_sectors - 1;
    gk3w_disk_usable(&d, &ufirst, &ulast);
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
        gk3w_disk_close(&d);
        return gk3w_err(GK3W_TOUCHED_NA, "disk-layout", "the existing partition table has out-of-range or overlapping entries", 0);
    }
    for (size_t i = 0; i < nf && i < GK3W_MAX_PARTS + 1; i++)
        if (want.first >= spans[i].first && want.last <= spans[i].last)
            inside = 1;
    if (!inside || count >= d.lay->Gpt.MaxPartitionCount) {
        gk3w_disk_close(&d);
        return gk3w_err(GK3W_TOUCHED_NA, "bad-region", "this range is not inside a free region (or the partition table is full)", 0);
    }
    lay = calloc(1, offsetof(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry) + (count + 1) * sizeof(PARTITION_INFORMATION_EX));
    if (!lay) {
        gk3w_disk_close(&d);
        return gk3w_err(GK3W_TOUCHED_NA, "nomem", "out of memory", 0);
    }
    lay->PartitionStyle = PARTITION_STYLE_GPT;
    lay->Gpt = d.lay->Gpt;
    count = 0;
    for (DWORD i = 0; i < d.lay->PartitionCount; i++) {
        const PARTITION_INFORMATION_EX *p = &d.lay->PartitionEntry[i];
        if (p->PartitionStyle == PARTITION_STYLE_GPT && p->PartitionLength.QuadPart)
            lay->PartitionEntry[count++] = *p;
    }
    e = &lay->PartitionEntry[count++];
    memset(e, 0, sizeof *e);
    e->PartitionStyle = PARTITION_STYLE_GPT;
    e->StartingOffset.QuadPart = (LONGLONG)(start * d.bs);
    e->PartitionLength.QuadPart = (LONGLONG)(len_sectors * d.bs);
    e->Gpt.PartitionType = GK3W_LINUX_DATA;
    if (UuidCreate(&e->Gpt.PartitionId) != RPC_S_OK) {
        free(lay);
        gk3w_disk_close(&d);
        return gk3w_err(GK3W_TOUCHED_NA, "uuid", "cannot generate a partition GUID", 0);
    }
    wcsncpy(e->Gpt.Name, name, GK3_GPT_NAME_CHARS);
    rc = gk3w_disk_set_layout(&d, lay, count, GK3W_TOUCHED_YES) ? 1 : 0;
    if (!rc) {
        GUID id = e->Gpt.PartitionId;
        const PARTITION_INFORMATION_EX *p;
        if (gk3w_disk_read_layout(&d) || !(p = gk3w_disk_find_part(&d, &id)))
            rc = gk3w_err(GK3W_TOUCHED_YES, "verify-layout", "after writing the partition table, the new partition is missing", 0);
        else
            emit_part("MKPART", &d, p);
    }
    free(lay);
    gk3w_disk_close(&d);
    return rc;
}

int gk3w_cmd_rmpart(int argc, wchar_t **argv)
{
    uint64_t num;
    GUID id;
    gk3w_disk d;
    DRIVE_LAYOUT_INFORMATION_EX *lay;
    DWORD count = 0;
    PARTITION_INFORMATION_EX *victim;
    int rc;

    if (gk3w_arg_u64(argc, argv, L"--disk", &num) || num > 255 || parse_guid(gk3w_arg(argc, argv, L"--uuid"), &id))
        return gk3w_err(GK3W_TOUCHED_NA, "usage", "usage: rmpart --disk N --uuid GUID", 0);
    if (gk3w_disk_open_vhd((DWORD)num, &d, GK3W_TOUCHED_NA))
        return 1;
    victim = gk3w_disk_find_part(&d, &id);
    if (!victim || memcmp(&victim->Gpt.PartitionType, &GK3W_LINUX_DATA, sizeof(GUID))) {
        gk3w_disk_close(&d);
        return gk3w_err(GK3W_TOUCHED_NA, "bad-part", "partition not found, or not of the Linux data type (only our own partitions are removed)", 0);
    }
    lay = calloc(1, d.laysz);
    if (!lay) {
        gk3w_disk_close(&d);
        return gk3w_err(GK3W_TOUCHED_NA, "nomem", "out of memory", 0);
    }
    lay->PartitionStyle = PARTITION_STYLE_GPT;
    lay->Gpt = d.lay->Gpt;
    for (DWORD i = 0; i < d.lay->PartitionCount; i++) {
        const PARTITION_INFORMATION_EX *p = &d.lay->PartitionEntry[i];
        if (p->PartitionStyle == PARTITION_STYLE_GPT && p->PartitionLength.QuadPart && p != victim)
            lay->PartitionEntry[count++] = *p;
    }
    rc = gk3w_disk_set_layout(&d, lay, count, GK3W_TOUCHED_YES) ? 1 : 0;
    if (!rc) {
        if (gk3w_disk_read_layout(&d) || gk3w_disk_find_part(&d, &id))
            rc = gk3w_err(GK3W_TOUCHED_YES, "verify-layout", "after removing and re-reading, a partition is still there", 0);
        else
            printf("RMPART disk=%lu ok\n", (unsigned long)num);
    }
    free(lay);
    gk3w_disk_close(&d);
    return rc;
}

int gk3w_cmd_write(int argc, wchar_t **argv)
{
    uint64_t num, part_off, part_len, fsize, done = 0;
    GUID id;
    gk3w_disk d;
    const wchar_t *file = gk3w_arg(argc, argv, L"--file");
    PARTITION_INFORMATION_EX *p;
    HANDLE f;
    LARGE_INTEGER fl;
    uint8_t *buf = NULL, *chk = NULL;
    int rc = 0;

    if (gk3w_arg_u64(argc, argv, L"--disk", &num) || num > 255 || parse_guid(gk3w_arg(argc, argv, L"--uuid"), &id) ||
        !file)
        return gk3w_err(GK3W_TOUCHED_NA, "usage", "usage: write --disk N --uuid GUID --file PATH", 0);
    if (gk3w_disk_open_vhd((DWORD)num, &d, GK3W_TOUCHED_NA))
        return 1;
    p = gk3w_disk_find_part(&d, &id);
    if (!p || memcmp(&p->Gpt.PartitionType, &GK3W_LINUX_DATA, sizeof(GUID))) {
        gk3w_disk_close(&d);
        return gk3w_err(GK3W_TOUCHED_NA, "bad-part", "partition not found, or not of the Linux data type", 0);
    }
    part_off = (uint64_t)p->StartingOffset.QuadPart;
    part_len = (uint64_t)p->PartitionLength.QuadPart;
    f = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        gk3w_disk_close(&d);
        return gk3w_err(GK3W_TOUCHED_NA, "file-open", "cannot open the image file", GetLastError());
    }
    if (!GetFileSizeEx(f, &fl) || (fsize = (uint64_t)fl.QuadPart) > part_len) {
        CloseHandle(f);
        gk3w_disk_close(&d);
        return gk3w_err(GK3W_TOUCHED_NA, "file-too-big", "the image is larger than the partition", 0);
    }
    buf = _aligned_malloc(CHUNK, 4096);
    chk = _aligned_malloc(CHUNK, 4096);
    if (!buf || !chk) {
        rc = gk3w_err(GK3W_TOUCHED_NA, "nomem", "out of memory", 0);
        goto out;
    }
    gk3w_progress(0, "write");
    while (done < fsize) {
        DWORD want = (DWORD)(fsize - done < CHUNK ? fsize - done : CHUNK), got = 0, padded;
        int w;
        if (!ReadFile(f, buf, want, &got, NULL) || got != want) {
            rc = gk3w_err(GK3W_TOUCHED_YES, "file-read", "reading the image file failed", GetLastError());
            goto out;
        }
        padded = (want + d.bs - 1) / d.bs * d.bs;
        memset(buf + want, 0, padded - want);
        w = gk3w_write_verify(d.h, buf, padded, part_off + done, chk);
        if (w) {
            rc = gk3w_err(GK3W_TOUCHED_YES, w == -1 ? "disk-write" : "disk-verify",
                          w == -1 ? "writing the partition failed" : "data did not read back identically", GetLastError());
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
    gk3w_disk_close(&d);
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
    return gk3w_disk_is_vhd(ext.e.Extents[0].DiskNumber);
}

int gk3w_cmd_esp_ls(int argc, wchar_t **argv)
{
    const wchar_t *vol = gk3w_arg(argc, argv, L"--vol"), *sub = gk3w_arg(argc, argv, L"--dir");
    wchar_t pat[MAX_PATH * 2];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    if (!vol)
        return gk3w_err(GK3W_TOUCHED_NA, "usage", "usage: esp-ls --vol \\\\?\\Volume{...}\\ [--dir SUB]", 0);
    _snwprintf(pat, MAX_PATH * 2, L"%ls%ls%ls*", vol, sub ? sub : L"", sub ? L"\\" : L"");
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return gk3w_err(GK3W_TOUCHED_NA, "esp-list", "cannot list the ESP directory", GetLastError());
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
    const wchar_t *vol = gk3w_arg(argc, argv, L"--vol"), *file = gk3w_arg(argc, argv, L"--file"),
                  *dest = gk3w_arg(argc, argv, L"--dest");
    wchar_t path[MAX_PATH * 2], dir[MAX_PATH * 2], *slash;
    uint8_t *data = NULL;
    size_t len = 0;
    int w;
    if (!vol || !file || !dest)
        return gk3w_err(GK3W_TOUCHED_NA, "usage", "usage: esp-put --vol \\\\?\\Volume{...}\\ --file PATH --dest SUB\\NAME", 0);
    if (!volume_on_vhd(vol))
        return gk3w_err(GK3W_TOUCHED_NA, "not-vhd", "only an ESP on a VHD is written by this test command", 0);
    if (gk3w_read_file(file, &data, &len, 64u << 20))
        return gk3w_err(GK3W_TOUCHED_NA, "file-read", "reading the source file failed (or it is larger than 64 MiB)", GetLastError());
    _snwprintf(path, MAX_PATH * 2, L"%ls%ls", vol, dest);
    wcscpy(dir, path);
    slash = wcsrchr(dir, L'\\');
    if (slash && (size_t)(slash - dir) > wcslen(vol)) {
        *slash = 0;
        if (gk3w_mkdirs(dir, wcslen(vol))) {
            free(data);
            return gk3w_err(GK3W_TOUCHED_NA, "esp-mkdir", "cannot create a directory on the EFI system partition", GetLastError());
        }
    }
    w = gk3w_write_file_verify(path, data, len);
    free(data);
    if (w)
        return gk3w_err(GK3W_TOUCHED_NA, w == -1 ? "esp-write" : "esp-verify",
                        w == -1 ? "writing the ESP file failed" : "the ESP file did not read back identically", GetLastError());
    printf("ESPPUT bytes=%zu verified=yes\n", len);
    return 0;
}
