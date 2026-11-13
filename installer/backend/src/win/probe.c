/* Windows：列出磁盘、分区、空闲区（只读）。
 *
 * 输出与 installer-lib.sh 的 gk3_probe 同样是 DISK / PART / FREE 三种记录，字段尽量同名；不同之处：
 *   - path= 不是 /dev 节点：磁盘是 disk<N>（\\.\PhysicalDrive<N>），分区是 disk<N>p<M>（M 是 Windows 的分区号，
 *     不一定等于 GPT 表项序号；表项序号另报 idx=，即 Linux 上的 pN）。
 *   - start= / end= 是逻辑扇区号，sector= 给出扇区字节数（4Kn 盘上不是 512）。
 *   - 分区多报 uuid=（唯一 GUID）、attrs=、letter=（盘符）、vol=（卷 GUID 路径）。
 * 每块 GPT 盘另用 libgk3core 直接读一遍主 GPT，与 IOCTL 给出的分区表逐项核对，结果报在 DISK 的 rawgpt=。 */
#include <windows.h>
#include <winioctl.h>

#include <stdlib.h>
#include <string.h>

#include "../layout.h"
#include "../proto.h"
#include "disk.h"
#include "gk3core.h"
#include "win.h"

#define MAX_DISKS 64
#define MAX_VOLS 256
#define FREE_MIN_BYTES (16ull << 20)   /* 小于 16 MiB 的缝隙不报，与 shell 版一致 */

typedef struct {
    DWORD disk;
    uint64_t offset, length;
    char vol[MAX_PATH * 3];             /* \\?\Volume{…}\（UTF-8） */
    char letter[4];
    char fs[32];
    char label[MAX_PATH * 3];
    DWORD fs_err;                       /* GetVolumeInformationW 失败时的错误码；0 = 成功 */
} vol_info;

static vol_info g_vols[MAX_VOLS];
static size_t g_nvols;

static void collect_volumes(void)
{
    wchar_t name[MAX_PATH];
    HANDLE f = FindFirstVolumeW(name, MAX_PATH);
    if (f == INVALID_HANDLE_VALUE)
        return;
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
        /* 打开卷设备要去掉结尾的反斜杠；访问权限给 0 就能查盘区 */
        wcscpy(dev, name);
        dev[n - 1] = 0;
        h = CreateFileW(dev, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        if (!DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0, &ext, sizeof ext, &got, NULL)) {
            CloseHandle(h);
            continue;
        }
        CloseHandle(h);
        for (DWORD i = 0; i < ext.e.NumberOfDiskExtents && i < 16 && g_nvols < MAX_VOLS; i++) {
            vol_info *v = &g_vols[g_nvols++];
            wchar_t paths[512], label[MAX_PATH + 1], fs[MAX_PATH + 1];
            DWORD plen = 0;
            memset(v, 0, sizeof *v);
            v->disk = ext.e.Extents[i].DiskNumber;
            v->offset = (uint64_t)ext.e.Extents[i].StartingOffset.QuadPart;
            v->length = (uint64_t)ext.e.Extents[i].ExtentLength.QuadPart;
            gk3w_w2u(name, v->vol, sizeof v->vol);
            if (GetVolumePathNamesForVolumeNameW(name, paths, 512, &plen) && paths[0] && paths[1] == L':') {
                v->letter[0] = (char)paths[0];
                v->letter[1] = 0;
            }
            if (GetVolumeInformationW(name, label, MAX_PATH + 1, NULL, NULL, NULL, fs, MAX_PATH + 1)) {
                gk3w_w2u(fs, v->fs, sizeof v->fs);
                gk3w_w2u(label, v->label, sizeof v->label);
            } else {
                v->fs_err = GetLastError();
            }
        }
    } while (FindNextVolumeW(f, name, MAX_PATH));
    FindVolumeClose(f);
}

static const vol_info *find_volume(DWORD disk, uint64_t offset)
{
    for (size_t i = 0; i < g_nvols; i++)
        if (g_vols[i].disk == disk && g_vols[i].offset == offset)
            return &g_vols[i];
    return NULL;
}

static const char *bus_name(STORAGE_BUS_TYPE t)
{
    switch ((int)t) {
    case BusTypeNvme: return "nvme";
    case BusTypeUsb: return "usb";
    case BusTypeSata: return "sata";
    case BusTypeAta: return "ata";
    case BusTypeSd: return "sd";
    case BusTypeMmc: return "mmc";
    case BusTypeScsi: return "scsi";
    case BusTypeSas: return "sas";
    case BusTypeRAID: return "raid";
    case BusTypeVirtual: return "virtual";
    case BusTypeFileBackedVirtual: return "vhd";
    case BusTypeSpaces: return "spaces";
    default: return "other";
    }
}

/* libgk3core 的块设备回调：按逻辑扇区读 \\.\PhysicalDrive<N> */
typedef struct {
    HANDLE h;
    uint32_t bs;
} raw_disk;

static int raw_read(void *ctx, uint64_t lba, uint32_t count, void *buf)
{
    raw_disk *d = ctx;
    OVERLAPPED ov;
    DWORD got = 0;
    uint64_t off, len = (uint64_t)count * d->bs;
    if (lba > UINT64_MAX / d->bs || len > 0x7fffffffu)
        return -1;
    off = lba * d->bs;
    memset(&ov, 0, sizeof ov);
    ov.Offset = (DWORD)off;
    ov.OffsetHigh = (DWORD)(off >> 32);
    if (!ReadFile(d->h, buf, (DWORD)len, &got, &ov) || got != len)
        return -1;
    return 0;
}

/* 用 libgk3core 直接读一遍主 GPT（与 Windows 的分区表互为旁证） */
typedef struct {
    gk3_gpt g;
    uint8_t *hdr, *ent;
    gk3_err err;
} raw_gpt;

#define RAW_ENTRIES_MAX (1u << 20)

static void raw_gpt_load(raw_gpt *r, HANDLE h, uint32_t bs, uint64_t blocks)
{
    raw_disk rd = {h, bs};
    gk3_blk dev = {&rd, bs, blocks, raw_read, NULL, NULL};
    memset(r, 0, sizeof *r);
    r->hdr = _aligned_malloc(bs, 4096);
    r->ent = _aligned_malloc(RAW_ENTRIES_MAX, 4096);
    if (!r->hdr || !r->ent) {
        r->err = GK3_ENOSPC;
        return;
    }
    r->err = gk3_gpt_read(&dev, &r->g, r->hdr, r->ent, RAW_ENTRIES_MAX);
}

static void raw_gpt_free(raw_gpt *r)
{
    _aligned_free(r->hdr);
    _aligned_free(r->ent);
    r->hdr = r->ent = NULL;
}

/* 按唯一 GUID 找表项；找不到返回 0，找到返回 1 并填 out */
static int raw_gpt_find(const raw_gpt *r, const GUID *id, gk3_gpt_part *out)
{
    if (r->err)
        return 0;
    for (uint32_t k = 0; k < r->g.num_entries; k++)
        if (gk3_gpt_get(&r->g, k, out) == GK3_OK && !memcmp(out->part_guid, id, 16))
            return 1;
    return 0;
}

/* 与 IOCTL 的分区表逐项核对：起止、类型、属性；表项数也要相等。结果如 "ok"、"differs"、"err-<错误>" */
static void raw_gpt_check(const raw_gpt *r, uint32_t bs, const DRIVE_LAYOUT_INFORMATION_EX *lay, char *out, size_t cap)
{
    uint32_t used = 0;
    if (r->err) {
        snprintf(out, cap, "err-%s", gk3_strerror(r->err));
        return;
    }
    snprintf(out, cap, "ok");
    for (DWORD i = 0; i < lay->PartitionCount; i++) {
        const PARTITION_INFORMATION_EX *p = &lay->PartitionEntry[i];
        gk3_gpt_part q;
        if (p->PartitionStyle != PARTITION_STYLE_GPT || !p->PartitionLength.QuadPart)
            continue;
        used++;
        if (!raw_gpt_find(r, &p->Gpt.PartitionId, &q) || memcmp(q.type_guid, &p->Gpt.PartitionType, 16) ||
            q.attrs != p->Gpt.Attributes || q.first_lba * bs != (uint64_t)p->StartingOffset.QuadPart ||
            (q.last_lba - q.first_lba + 1) * bs != (uint64_t)p->PartitionLength.QuadPart)
            snprintf(out, cap, "differs");
    }
    if (used != gk3_gpt_count(&r->g))
        snprintf(out, cap, "differs");
}

static int probe_disk(DWORD n, int system_disk, gk3w_line *l)
{
    wchar_t path[40];
    HANDLE h;
    STORAGE_PROPERTY_QUERY q;
    BYTE descbuf[1024];
    STORAGE_DEVICE_DESCRIPTOR *desc = (STORAGE_DEVICE_DESCRIPTOR *)descbuf;
    STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR align;
    GET_LENGTH_INFORMATION li;
    DRIVE_LAYOUT_INFORMATION_EX *lay = NULL;
    DWORD got = 0, laysz = sizeof(DRIVE_LAYOUT_INFORMATION_EX) + 128 * sizeof(PARTITION_INFORMATION_EX);
    uint32_t bs = 512, pbs = 0;
    uint64_t size;
    char model[160] = "", disk_id[16], rawgpt[32] = "";
    raw_gpt rg = {0};
    const char *table = "raw";
    int rc = 0;

    _snwprintf(path, 40, L"\\\\.\\PhysicalDrive%lu", (unsigned long)n);
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND)
            return 1;   /* 没有这个号：磁盘号可能不连续，接着找 */
        gk3w_line_begin(l, "ERR");
        gk3w_line_kv(l, "code", e == ERROR_ACCESS_DENIED ? "not-admin" : "disk-open");
        gk3w_line_u64(l, "disk", n);
        gk3w_line_u64(l, "winerr", e);
        return gk3w_fail(l, GK3W_TOUCHED_NA, e == ERROR_ACCESS_DENIED ? "must run as administrator" : "cannot open the disk");
    }

    memset(&q, 0, sizeof q);
    q.PropertyId = StorageDeviceProperty;
    q.QueryType = PropertyStandardQuery;
    memset(descbuf, 0, sizeof descbuf);
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, descbuf, sizeof descbuf, &got, NULL)) {
        size_t m = 0;
        DWORD offs[2] = {desc->VendorIdOffset, desc->ProductIdOffset};
        for (int i = 0; i < 2; i++) {
            if (!offs[i] || offs[i] >= got)
                continue;
            for (DWORD k = offs[i]; k < got && descbuf[k] && m + 1 < sizeof model; k++)
                model[m++] = (char)descbuf[k];
            while (m && model[m - 1] == ' ')
                m--;
            if (m && m + 1 < sizeof model)
                model[m++] = ' ';
        }
        while (m && model[m - 1] == ' ')
            m--;
        model[m] = 0;
    }
    q.PropertyId = StorageAccessAlignmentProperty;
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, &align, sizeof align, &got, NULL) &&
        got >= sizeof align) {
        bs = align.BytesPerLogicalSector;
        pbs = align.BytesPerPhysicalSector;
    } else {
        DISK_GEOMETRY_EX geo;
        if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL, 0, &geo, sizeof geo, &got, NULL))
            bs = geo.Geometry.BytesPerSector;
    }
    if (bs != 512 && bs != 4096) {
        CloseHandle(h);
        gk3w_line_begin(l, "ERR");
        gk3w_line_kv(l, "code", "disk-sector");
        gk3w_line_u64(l, "disk", n);
        gk3w_line_u64(l, "sector", bs);
        return gk3w_fail(l, GK3W_TOUCHED_NA, "the disk sector size is neither 512 nor 4096 bytes");
    }
    if (!DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO, NULL, 0, &li, sizeof li, &got, NULL)) {
        DWORD e = GetLastError();
        CloseHandle(h);
        gk3w_line_begin(l, "ERR");
        gk3w_line_kv(l, "code", "disk-size");
        gk3w_line_u64(l, "disk", n);
        gk3w_line_u64(l, "winerr", e);
        return gk3w_fail(l, GK3W_TOUCHED_NA, "cannot read the disk size");
    }
    size = (uint64_t)li.Length.QuadPart;

    for (;;) {
        lay = malloc(laysz);
        if (!lay)
            break;
        if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, NULL, 0, lay, laysz, &got, NULL))
            break;
        free(lay);
        lay = NULL;
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || laysz > (1u << 24))
            break;
        laysz *= 2;
    }
    if (lay)
        table = lay->PartitionStyle == PARTITION_STYLE_GPT ? "gpt" : lay->PartitionStyle == PARTITION_STYLE_MBR ? "mbr" : "raw";
    if (lay && lay->PartitionStyle == PARTITION_STYLE_GPT) {
        raw_gpt_load(&rg, h, bs, size / bs);
        raw_gpt_check(&rg, bs, lay, rawgpt, sizeof rawgpt);
    }

    snprintf(disk_id, sizeof disk_id, "disk%lu", (unsigned long)n);
    gk3w_line_begin(l, "DISK");
    gk3w_line_kv(l, "path", disk_id);
    gk3w_line_u64(l, "num", n);
    gk3w_line_u64(l, "size_mib", size >> 20);
    gk3w_line_u64(l, "sector", bs);
    gk3w_line_u64(l, "physsector", pbs);
    gk3w_line_kv(l, "model", model[0] ? model : "?");
    gk3w_line_kv(l, "bus", bus_name(desc->BusType));
    gk3w_line_kv(l, "removable", desc->RemovableMedia ? "1" : "0");
    gk3w_line_kv(l, "system", system_disk ? "yes" : "no");
    gk3w_line_kv(l, "table", lay ? table : "unreadable");
    if (rawgpt[0])
        gk3w_line_kv(l, "rawgpt", rawgpt);
    if (gk3w_line_emit(l, stdout))
        rc = -1;

    if (lay && lay->PartitionStyle == PARTITION_STYLE_GPT) {
        gk3w_span parts[GK3W_MAX_PARTS], free_spans[GK3W_MAX_PARTS + 1];
        size_t np = 0, nf = 0;
        uint64_t ufirst = (uint64_t)lay->Gpt.StartingUsableOffset.QuadPart / bs;
        uint64_t ulast = ((uint64_t)lay->Gpt.StartingUsableOffset.QuadPart + (uint64_t)lay->Gpt.UsableLength.QuadPart) / bs - 1;
        gk3w_layout_err le;

        for (DWORD i = 0; i < lay->PartitionCount; i++) {
            const PARTITION_INFORMATION_EX *p = &lay->PartitionEntry[i];
            const vol_info *v;
            char name[GK3_GPT_NAME_CHARS * 3 + 1], id[32], type[40], uuid[40], fs[32] = "", label[MAX_PATH * 3] = "";
            wchar_t wname[GK3_GPT_NAME_CHARS + 1];
            uint64_t first, last;
            if (p->PartitionStyle != PARTITION_STYLE_GPT || !p->PartitionLength.QuadPart)
                continue;
            first = (uint64_t)p->StartingOffset.QuadPart / bs;
            last = first + (uint64_t)p->PartitionLength.QuadPart / bs - 1;
            if (np < GK3W_MAX_PARTS)
                parts[np++] = (gk3w_span){first, last};
            memcpy(wname, p->Gpt.Name, sizeof p->Gpt.Name);
            wname[GK3_GPT_NAME_CHARS] = 0;
            gk3w_w2u(wname, name, sizeof name);
            gk3w_guid_upper(&p->Gpt.PartitionType, type);
            gk3w_guid_upper(&p->Gpt.PartitionId, uuid);
            v = find_volume(n, (uint64_t)p->StartingOffset.QuadPart);
            if (v) {
                snprintf(fs, sizeof fs, "%s", v->fs);
                snprintf(label, sizeof label, "%s", v->label);
            }
            snprintf(id, sizeof id, "disk%lup%lu", (unsigned long)n, (unsigned long)p->PartitionNumber);
            gk3w_line_begin(l, "PART");
            gk3w_line_kv(l, "path", id);
            gk3w_line_kv(l, "disk", disk_id);
            gk3w_line_u64(l, "num", p->PartitionNumber);
            {
                gk3_gpt_part gq;
                gk3w_line_u64(l, "idx", raw_gpt_find(&rg, &p->Gpt.PartitionId, &gq) ? gq.index : 0);
            }
            gk3w_line_u64(l, "start", first);
            gk3w_line_u64(l, "end", last);
            gk3w_line_u64(l, "size_mib", (uint64_t)p->PartitionLength.QuadPart >> 20);
            gk3w_line_u64(l, "size_kib", (uint64_t)p->PartitionLength.QuadPart >> 10);
            gk3w_line_kv(l, "type", type);
            gk3w_line_kv(l, "uuid", uuid);
            gk3w_line_hex64(l, "attrs", p->Gpt.Attributes);
            gk3w_line_kv(l, "name", name);
            gk3w_line_kv(l, "fs", fs);
            gk3w_line_kv(l, "fslabel", label);
            gk3w_line_kv(l, "letter", v ? v->letter : "");
            gk3w_line_kv(l, "vol", v ? v->vol : "");
            if (v && v->fs_err)
                gk3w_line_u64(l, "fserr", v->fs_err);
            gk3w_line_kv(l, "os", gk3w_guess_os(name, type, fs));
            if (gk3w_line_emit(l, stdout))
                rc = -1;
        }
        le = gk3w_free_spans(ufirst, ulast, parts, np, bs, FREE_MIN_BYTES, free_spans,
                             sizeof free_spans / sizeof free_spans[0], &nf);
        if (le != GK3W_LAYOUT_OK) {
            gk3w_line_begin(l, "ERR");
            gk3w_line_kv(l, "code", "disk-layout");
            gk3w_line_kv(l, "disk", disk_id);
            gk3w_line_u64(l, "why", le);
            gk3w_fail(l, GK3W_TOUCHED_NA, "the partition table has out-of-range or overlapping partitions");
            rc = -1;
        }
        for (size_t i = 0; i < nf && i < sizeof free_spans / sizeof free_spans[0]; i++) {
            gk3w_line_begin(l, "FREE");
            gk3w_line_kv(l, "disk", disk_id);
            gk3w_line_u64(l, "start", free_spans[i].first);
            gk3w_line_u64(l, "end", free_spans[i].last);
            gk3w_line_u64(l, "size_mib", gk3w_span_bytes(free_spans[i], bs) >> 20);
            if (gk3w_line_emit(l, stdout))
                rc = -1;
        }
    }
    raw_gpt_free(&rg);
    free(lay);
    CloseHandle(h);
    return rc;
}

int gk3w_cmd_probe(int argc, wchar_t **argv)
{
    gk3w_line l = {0};
    long sysdisk = gk3w_system_disk_number();
    int rc = 0;
    (void)argc;
    (void)argv;
    collect_volumes();
    for (DWORD n = 0; n < MAX_DISKS; n++) {
        int r = probe_disk(n, sysdisk >= 0 && (DWORD)sysdisk == n, &l);
        if (r < 0) {
            rc = 1;
            /* 权限不够时每块盘都会失败，报一次就够了 */
            if (l.buf && strstr(l.buf, "code=not-admin"))
                break;
        }
    }
    gk3w_line_free(&l);
    return rc;
}
