/* Windows：把装好的盘摘要成文本，与 LiveCD 在同一布局上装出来的结果对拍（只读）。
 *
 *   gk3w dump --disk N [--hash a,b,…]   每个 GPT 分区一行 DUMPPART（名字、起止、类型、属性），
 *                                       --hash 里列出的分区再带整个分区内容的 sha256
 *   gk3w esp-tree --vol \\?\Volume{…}\  ESP 上每个文件一行 ESPFILE（相对路径用 /、大小、sha256），按路径排序
 *   gk3w readpart --disk N --name NAME --out FILE   把一个分区（按 GPT 名字，恰好一个）整个读出来存成文件，至多 1 GiB */
#include <windows.h>
#include <winioctl.h>

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../proto.h"
#include "disk.h"
#include "gk3core.h"
#include "win.h"

#define CHUNK (8u << 20)

static int in_list(const char *list, const char *name)
{
    size_t n = strlen(name);
    for (const char *p = list; p && *p;) {
        const char *e = strchr(p, ',');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len == n && !memcmp(p, name, n))
            return 1;
        p = e ? e + 1 : NULL;
    }
    return 0;
}

int gk3w_cmd_dump(int argc, wchar_t **argv)
{
    uint64_t num;
    const wchar_t *hw = gk3w_arg(argc, argv, L"--hash");
    char hash_list[256] = "";
    wchar_t path[40];
    HANDLE h;
    DRIVE_LAYOUT_INFORMATION_EX *lay = NULL;
    DWORD sz = sizeof(DRIVE_LAYOUT_INFORMATION_EX) + 128 * sizeof(PARTITION_INFORMATION_EX), got = 0;
    uint8_t *buf = NULL;
    uint32_t bs = 512;
    int rc = 0;

    if (gk3w_arg_u64(argc, argv, L"--disk", &num) || num > 255)
        return gk3w_err(GK3W_TOUCHED_NA, "usage", "usage: dump --disk N [--hash a,b,...]", 0);
    if (hw)
        gk3w_w2u(hw, hash_list, sizeof hash_list);
    _snwprintf(path, 40, L"\\\\.\\PhysicalDrive%lu", (unsigned long)num);
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return gk3w_err(GK3W_TOUCHED_NA, "disk-open", "cannot open the disk", GetLastError());
    {
        STORAGE_PROPERTY_QUERY q = {StorageAccessAlignmentProperty, PropertyStandardQuery, {0}};
        STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR a;
        if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, &a, sizeof a, &got, NULL) && got >= sizeof a)
            bs = a.BytesPerLogicalSector;
    }
    for (;;) {
        lay = calloc(1, sz);
        if (!lay || DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, NULL, 0, lay, sz, &got, NULL))
            break;
        free(lay);
        lay = NULL;
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || sz > (1u << 24))
            break;
        sz *= 2;
    }
    buf = _aligned_malloc(CHUNK, 4096);
    if (!lay || !buf) {
        rc = gk3w_err(GK3W_TOUCHED_NA, "disk-layout", "cannot read the partition table", GetLastError());
        goto out;
    }
    for (DWORD i = 0; i < lay->PartitionCount; i++) {
        const PARTITION_INFORMATION_EX *p = &lay->PartitionEntry[i];
        gk3w_line l = {0};
        wchar_t wn[GK3_GPT_NAME_CHARS + 1];
        char name[GK3_GPT_NAME_CHARS * 3 + 1], type[40];
        uint64_t off = (uint64_t)p->StartingOffset.QuadPart, len = (uint64_t)p->PartitionLength.QuadPart;
        if (p->PartitionStyle != PARTITION_STYLE_GPT || !len)
            continue;
        memcpy(wn, p->Gpt.Name, sizeof p->Gpt.Name);
        wn[GK3_GPT_NAME_CHARS] = 0;
        gk3w_w2u(wn, name, sizeof name);
        gk3w_guid_upper(&p->Gpt.PartitionType, type);
        gk3w_line_begin(&l, "DUMPPART");
        gk3w_line_kv(&l, "name", name);
        gk3w_line_u64(&l, "start", off / bs);
        gk3w_line_u64(&l, "end", (off + len) / bs - 1);
        gk3w_line_kv(&l, "type", type);
        gk3w_line_hex64(&l, "attrs", p->Gpt.Attributes);
        if (in_list(hash_list, name)) {
            gk3w_sha256_ctx *c = gk3w_sha256_begin();
            uint8_t dg[32];
            char hex[65];
            for (uint64_t done = 0; c && done < len; done += CHUNK) {
                DWORD n = (DWORD)(len - done < CHUNK ? len - done : CHUNK);
                if (gk3w_pread(h, buf, n, off + done) || gk3w_sha256_update(c, buf, n)) {
                    gk3w_sha256_end(c, dg);
                    c = NULL;
                }
            }
            if (!c || gk3w_sha256_end(c, dg)) {
                gk3w_line_free(&l);
                rc = gk3w_err(GK3W_TOUCHED_NA, "disk-read", "reading the partition failed", GetLastError());
                goto out;
            }
            gk3w_hex(dg, 32, hex);
            gk3w_line_kv(&l, "sha256", hex);
        }
        (void)gk3w_line_emit(&l, stdout);
        gk3w_line_free(&l);
    }
out:
    free(lay);
    _aligned_free(buf);
    CloseHandle(h);
    return rc;
}

/* ── esp-tree ── */

typedef struct {
    char **v;
    size_t n, cap;
} strvec;

static void walk(const wchar_t *root, const wchar_t *sub, strvec *out)
{
    wchar_t pat[MAX_PATH * 2];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    _snwprintf(pat, MAX_PATH * 2, L"%ls%ls*", root, sub);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        wchar_t rel[MAX_PATH * 2];
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        _snwprintf(rel, MAX_PATH * 2, L"%ls%ls", sub, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            wchar_t next[MAX_PATH * 2];
            _snwprintf(next, MAX_PATH * 2, L"%ls\\", rel);
            walk(root, next, out);
        } else {
            char u[MAX_PATH * 3];
            gk3w_w2u(rel, u, sizeof u);
            for (char *c = u; *c; c++)
                if (*c == '\\')
                    *c = '/';
            if (out->n == out->cap) {
                out->cap = out->cap ? out->cap * 2 : 64;
                out->v = realloc(out->v, out->cap * sizeof *out->v);
            }
            out->v[out->n++] = _strdup(u);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

int gk3w_cmd_esp_tree(int argc, wchar_t **argv)
{
    const wchar_t *vol = gk3w_arg(argc, argv, L"--vol");
    strvec files = {0};
    if (!vol)
        return gk3w_err(GK3W_TOUCHED_NA, "usage", "usage: esp-tree --vol \\\\?\\Volume{...}\\", 0);
    walk(vol, L"", &files);
    qsort(files.v, files.n, sizeof *files.v, cmp_str);
    for (size_t i = 0; i < files.n; i++) {
        wchar_t w[MAX_PATH * 2], full[MAX_PATH * 2];
        uint8_t *data, dg[32];
        size_t len;
        char hex[65] = "?";
        gk3w_line l = {0};
        gk3w_u2w(files.v[i], w, MAX_PATH * 2);
        for (wchar_t *c = w; *c; c++)
            if (*c == L'/')
                *c = L'\\';
        _snwprintf(full, MAX_PATH * 2, L"%ls%ls", vol, w);
        if (!gk3w_read_file(full, &data, &len, 512u << 20)) {
            if (!gk3w_sha256(data, len, dg))
                gk3w_hex(dg, 32, hex);
            free(data);
        } else {
            len = 0;
        }
        gk3w_line_begin(&l, "ESPFILE");
        gk3w_line_kv(&l, "path", files.v[i]);
        gk3w_line_u64(&l, "size", len);
        gk3w_line_kv(&l, "sha256", hex);
        (void)gk3w_line_emit(&l, stdout);
        gk3w_line_free(&l);
        free(files.v[i]);
    }
    free(files.v);
    return 0;
}

/* ── readpart ── */

int gk3w_cmd_readpart(int argc, wchar_t **argv)
{
    uint64_t num;
    const wchar_t *wname = gk3w_arg(argc, argv, L"--name"), *outp = gk3w_arg(argc, argv, L"--out");
    wchar_t path[40];
    HANDLE h, o;
    DRIVE_LAYOUT_INFORMATION_EX *lay = NULL;
    DWORD sz = sizeof(DRIVE_LAYOUT_INFORMATION_EX) + 128 * sizeof(PARTITION_INFORMATION_EX), got = 0;
    const PARTITION_INFORMATION_EX *hit = NULL;
    uint8_t *buf = NULL;
    int found = 0, rc = 0;

    if (gk3w_arg_u64(argc, argv, L"--disk", &num) || num > 255 || !wname || !outp)
        return gk3w_err(GK3W_TOUCHED_NA, "usage", "usage: readpart --disk N --name NAME --out FILE", 0);
    _snwprintf(path, 40, L"\\\\.\\PhysicalDrive%lu", (unsigned long)num);
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return gk3w_err(GK3W_TOUCHED_NA, "disk-open", "cannot open the disk", GetLastError());
    for (;;) {
        lay = calloc(1, sz);
        if (!lay || DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, NULL, 0, lay, sz, &got, NULL))
            break;
        free(lay);
        lay = NULL;
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || sz > (1u << 24))
            break;
        sz *= 2;
    }
    if (!lay) {
        CloseHandle(h);
        return gk3w_err(GK3W_TOUCHED_NA, "disk-layout", "cannot read the partition table", GetLastError());
    }
    for (DWORD i = 0; i < lay->PartitionCount; i++) {
        const PARTITION_INFORMATION_EX *p = &lay->PartitionEntry[i];
        wchar_t wn[GK3_GPT_NAME_CHARS + 1];
        if (p->PartitionStyle != PARTITION_STYLE_GPT || !p->PartitionLength.QuadPart)
            continue;
        memcpy(wn, p->Gpt.Name, sizeof p->Gpt.Name);
        wn[GK3_GPT_NAME_CHARS] = 0;
        if (!wcscmp(wn, wname)) {
            hit = p;
            found++;
        }
    }
    if (found != 1 || (uint64_t)hit->PartitionLength.QuadPart > (1ull << 30)) {
        rc = gk3w_err(GK3W_TOUCHED_NA, "bad-part", "no partition with this name, the name is not unique, or the partition is larger than 1 GiB", 0);
        goto out;
    }
    buf = _aligned_malloc(CHUNK, 4096);
    o = CreateFileW(outp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (!buf || o == INVALID_HANDLE_VALUE) {
        rc = gk3w_err(GK3W_TOUCHED_NA, buf ? "file-open" : "nomem", buf ? "cannot create the output file" : "out of memory", GetLastError());
        if (o != INVALID_HANDLE_VALUE)
            CloseHandle(o);
        goto out;
    }
    for (uint64_t done = 0, len = (uint64_t)hit->PartitionLength.QuadPart; done < len; done += CHUNK) {
        DWORD n = (DWORD)(len - done < CHUNK ? len - done : CHUNK), w = 0;
        if (gk3w_pread(h, buf, n, (uint64_t)hit->StartingOffset.QuadPart + done) || !WriteFile(o, buf, n, &w, NULL) || w != n) {
            rc = gk3w_err(GK3W_TOUCHED_NA, "disk-read", "reading the partition or writing the output file failed", GetLastError());
            break;
        }
    }
    CloseHandle(o);
    if (!rc)
        printf("READPART bytes=%llu\n", (unsigned long long)hit->PartitionLength.QuadPart);
out:
    _aligned_free(buf);
    free(lay);
    CloseHandle(h);
    return rc ? 1 : 0;
}
