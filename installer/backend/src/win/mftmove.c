/* Windows：把 NTFS 的主文件表（$MFT 的 $DATA 与 $BITMAP）挪到卷的前部，让 C 盘能压缩得更多。
 *
 *   gk3w mft-move [--drive X] [--execute] [--chunk-mib N] [--min-run-mib N]
 *
 * "压缩卷"的分析常常停在 $Mft 上（shrink.c 读到的 259 号事件），这时 MFT 的片段落在卷的后部。这里用 Windows 的
 * 碎片整理接口（FSCTL_GET_VOLUME_BITMAP、FSCTL_GET_RETRIEVAL_POINTERS、FSCTL_MOVE_FILE，与 defrag.exe、Sysinternals
 * Contig 相同）把它们挪进前部的空隙：每一步都由 NTFS 自己完成，要么完成、要么失败而不改动。MFT 可以拆成几段放进
 * 不同的空隙，不要求一整块连续的空闲空间；小于 --min-run-mib（默认 8）的空隙不用，免得把 MFT 拆得太碎。
 *
 * 顺序：从位置最高的一段起，每段（至多 --chunk-mib，默认 16）放进它下方最低的空隙，所以只会往前挪。
 * MFT 开头的 16 条记录（$MFT、$MFTMirr、$LogFile … 这些系统文件）必须留在引导扇区记录的位置（MftStartLcn）上，
 * NTFS 拒绝挪它们（错误 87；记录 1 KiB、簇 4 KiB 时是开头 4 个簇）：计划里就把这几个簇留在原处，只挪其余部分；
 * 紧接其后的那一段仍被拒绝时跳过它，接着挪别的片段。
 * 空闲位图是快照，挪动失败时重读一次再试；累计失败 3 次就停（已挪的保持原样，再运行一次可以继续）。
 *
 * 不带 --execute 只分析、给出计划。--execute 改动系统卷时要求本机是 MateBook E Go（--drive 用于对着测试卷）。
 * 输出 MFTMOVE drive=X: execute=yes|no end_before_mib= end_after_mib= moved_mib= steps= unplaced_mib=
 * （end_after_mib：执行时是挪完之后实际的位置，分析时是预计的位置）。 */
#include <windows.h>
#include <winioctl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#include "../proto.h"
#include "disk.h"
#include "win.h"

typedef struct {
    int stream;                 /* 0 = $MFT::$DATA，1 = $MFT::$BITMAP */
    LONGLONG vcn, lcn, count;
} piece;

typedef struct {
    LONGLONG start, len;
} run;

static const char *const STREAM_NAMES[2] = {"$MFT::$DATA", "$MFT::$BITMAP"};

static int enable_manage_volume(void)
{
    HANDLE tok;
    TOKEN_PRIVILEGES tp;
    int ok;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok))
        return -1;
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    ok = LookupPrivilegeValueW(NULL, SE_MANAGE_VOLUME_NAME, &tp.Privileges[0].Luid) &&
         AdjustTokenPrivileges(tok, FALSE, &tp, 0, NULL, NULL) && GetLastError() == ERROR_SUCCESS;
    CloseHandle(tok);
    return ok ? 0 : -1;
}

/* 文件的片段 { vcn, lcn, count }（追加到 *out，*n 为个数）。失败返回 -1 */
static int get_extents(HANDLE f, piece **out, size_t *n, size_t *cap, int stream)
{
    union {
        RETRIEVAL_POINTERS_BUFFER rp;
        uint8_t raw[64 * 1024];
    } *buf = malloc(sizeof *buf);
    STARTING_VCN_INPUT_BUFFER in = {{0}};
    int rc = -1;
    if (!buf)
        return -1;
    for (;;) {
        DWORD got = 0, err;
        BOOL ok = DeviceIoControl(f, FSCTL_GET_RETRIEVAL_POINTERS, &in, sizeof in, buf, sizeof *buf, &got, NULL);
        LONGLONG vcn;
        err = ok ? 0 : GetLastError();
        if (!ok && err == ERROR_HANDLE_EOF) {
            rc = 0;
            break;
        }
        if (!ok && err != ERROR_MORE_DATA)
            break;
        vcn = buf->rp.StartingVcn.QuadPart;
        for (DWORD i = 0; i < buf->rp.ExtentCount; i++) {
            LONGLONG next = buf->rp.Extents[i].NextVcn.QuadPart, lcn = buf->rp.Extents[i].Lcn.QuadPart;
            if (lcn >= 0) {   /* -1 = 稀疏或压缩掉的部分，不占簇 */
                if (*n == *cap) {
                    size_t nc = *cap ? *cap * 2 : 64;
                    piece *np = realloc(*out, nc * sizeof *np);
                    if (!np)
                        goto done;
                    *out = np;
                    *cap = nc;
                }
                (*out)[(*n)++] = (piece){stream, vcn, lcn, next - vcn};
            }
            vcn = next;
        }
        if (ok || !buf->rp.ExtentCount) {
            rc = 0;
            break;
        }
        in.StartingVcn.QuadPart = vcn;
    }
done:
    free(buf);
    return rc;
}

/* limit 以下、不小于 min_run 的空闲空隙，按位置排好。失败返回 -1 */
static int get_free_runs(HANDLE vol, LONGLONG total, LONGLONG limit, LONGLONG min_run, run **out, size_t *n)
{
    size_t bytes = sizeof(VOLUME_BITMAP_BUFFER) + (size_t)(total / 8) + 64, cap = 0;
    VOLUME_BITMAP_BUFFER *bm = malloc(bytes);
    STARTING_LCN_INPUT_BUFFER in = {{0}};
    DWORD got = 0;
    LONGLONG base, bits, end, i, start = -1;
    *out = NULL;
    *n = 0;
    if (!bm)
        return -1;
    if (!DeviceIoControl(vol, FSCTL_GET_VOLUME_BITMAP, &in, sizeof in, bm, (DWORD)bytes, &got, NULL) && GetLastError() != ERROR_MORE_DATA) {
        free(bm);
        return -1;
    }
    base = bm->StartingLcn.QuadPart;
    bits = bm->BitmapSize.QuadPart;
    if (got > offsetof(VOLUME_BITMAP_BUFFER, Buffer) && bits > (LONGLONG)(got - offsetof(VOLUME_BITMAP_BUFFER, Buffer)) * 8)
        bits = (LONGLONG)(got - offsetof(VOLUME_BITMAP_BUFFER, Buffer)) * 8;
    end = limit - base < bits ? limit - base : bits;
    if (end < 0)
        end = 0;
    for (i = 0; i <= end; i++) {
        int used = i == end || (bm->Buffer[i >> 3] >> (i & 7)) & 1;
        if (!used) {
            if (start < 0)
                start = i;
            continue;
        }
        if (start >= 0 && i - start >= min_run) {
            if (*n == cap) {
                size_t nc = cap ? cap * 2 : 256;
                run *nr = realloc(*out, nc * sizeof *nr);
                if (!nr) {
                    free(bm);
                    return -1;
                }
                *out = nr;
                cap = nc;
            }
            (*out)[(*n)++] = (run){base + start, i - start};
        }
        start = -1;
    }
    free(bm);
    return 0;
}

/* below 以下第一个（最低的）放得下的空隙：剩余长度不小于 min_run，或者正好装得下 need */
static int find_run(const run *r, size_t n, LONGLONG below, LONGLONG need, LONGLONG min_run)
{
    for (size_t i = 0; i < n; i++) {
        if (r[i].start >= below)
            return -1;
        if (r[i].len > 0 && (r[i].len >= min_run || r[i].len >= need))
            return (int)i;
    }
    return -1;
}

static DWORD move_clusters(HANDLE vol, HANDLE file, LONGLONG vcn, LONGLONG lcn, LONGLONG count)
{
    MOVE_FILE_DATA m;
    DWORD got = 0;
    memset(&m, 0, sizeof m);
    m.FileHandle = file;
    m.StartingVcn.QuadPart = vcn;
    m.StartingLcn.QuadPart = lcn;
    m.ClusterCount = (DWORD)count;
    return DeviceIoControl(vol, FSCTL_MOVE_FILE, &m, sizeof m, NULL, 0, &got, NULL) ? 0 : GetLastError();
}

static int by_order(const void *a, const void *b)
{
    const piece *x = a, *y = b;
    return x->lcn < y->lcn ? 1 : x->lcn > y->lcn ? -1 : 0;   /* 位置高的先挪 */
}

int gk3w_cmd_mft_move(int argc, wchar_t **argv)
{
    const wchar_t *da = gk3w_arg(argc, argv, L"--drive");
    int execute = gk3w_flag(argc, argv, L"--execute"), rc = 1, failures = 0, steps = 0;
    uint64_t chunk_mib = 16, min_run_mib = 8;
    wchar_t drive, win[MAX_PATH], path[MAX_PATH];
    HANDLE vol = INVALID_HANDLE_VALUE, files[2] = {INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE};
    NTFS_VOLUME_DATA_BUFFER vd;
    DWORD got = 0;
    piece *pieces = NULL, *ext = NULL;
    size_t np = 0, capp = 0;
    run *runs = NULL;
    size_t nr = 0;
    LONGLONG bpc, keep, chunk, min_run, end_before = 0, source_limit = 0, predicted = 0, moved = 0, unplaced = 0;
    gk3w_touched T = GK3W_TOUCHED_NO;
    char dl[3];
    gk3w_line l = {0};

    if ((da && (!iswalpha(da[0]) || (da[1] && (da[1] != L':' || da[2])))) ||
        (gk3w_arg(argc, argv, L"--chunk-mib") && (gk3w_arg_u64(argc, argv, L"--chunk-mib", &chunk_mib) || !chunk_mib || chunk_mib > 1024)) ||
        (gk3w_arg(argc, argv, L"--min-run-mib") && (gk3w_arg_u64(argc, argv, L"--min-run-mib", &min_run_mib) || !min_run_mib || min_run_mib > 1024)))
        return gk3w_err(GK3W_TOUCHED_NA, "usage", "usage: mft-move [--drive X] [--execute] [--chunk-mib 1-1024] [--min-run-mib 1-1024]", 0);
    drive = da ? towupper(da[0]) : (GetSystemWindowsDirectoryW(win, MAX_PATH) && win[1] == L':' ? towupper(win[0]) : L'C');
    if (execute && !da && !gk3w_model_ok_now())
        return gk3w_err(GK3W_TOUCHED_NO, "wrong-model", "this computer is not a Huawei MateBook E Go (gaokun3); not moving its MFT (disk not touched)", 0);
    if (enable_manage_volume())
        gk3w_log("cannot enable SeManageVolumePrivilege; moving may be refused");

    _snwprintf(path, MAX_PATH, L"\\\\.\\%lc:", drive);
    vol = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (vol == INVALID_HANDLE_VALUE) {
        gk3w_err(T, "disk-open", "cannot open the volume", GetLastError());
        goto out;
    }
    if (!DeviceIoControl(vol, FSCTL_GET_NTFS_VOLUME_DATA, NULL, 0, &vd, sizeof vd, &got, NULL)) {
        gk3w_err(T, "not-ntfs", "FSCTL_GET_NTFS_VOLUME_DATA failed; is this an NTFS volume?", GetLastError());
        goto out;
    }
    bpc = vd.BytesPerCluster;
    /* MFT 开头的 16 条系统记录所在的簇，留在原处 */
    keep = (16 * (LONGLONG)vd.BytesPerFileRecordSegment + bpc - 1) / bpc;
    chunk = (LONGLONG)(chunk_mib << 20) / bpc;
    min_run = (LONGLONG)(min_run_mib << 20) / bpc;
    if (chunk < 1)
        chunk = 1;
    if (min_run < 1)
        min_run = 1;
    gk3w_log("volume %c: %lld MiB, %lld MiB free, %lu-byte clusters; MFT starts at %lld MiB", (char)drive,
             (long long)(vd.TotalClusters.QuadPart * bpc >> 20), (long long)(vd.FreeClusters.QuadPart * bpc >> 20),
             (unsigned long)bpc, (long long)(vd.MftStartLcn.QuadPart * bpc >> 20));

    for (int f = 0; f < 2; f++) {
        size_t first = np;
        LONGLONG fend = 0, fsize = 0;
        _snwprintf(path, MAX_PATH, f ? L"%lc:\\$Mft::$BITMAP" : L"%lc:\\$Mft", drive);
        files[f] = CreateFileW(path, FILE_READ_ATTRIBUTES | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
        if (files[f] == INVALID_HANDLE_VALUE) {
            if (!f) {
                gk3w_err(T, "mft-open", "cannot open $MFT", GetLastError());
                goto out;
            }
            gk3w_log("cannot open %s (%lu), skipping it", STREAM_NAMES[f], (unsigned long)GetLastError());
            continue;
        }
        if (get_extents(files[f], &pieces, &np, &capp, f)) {
            gk3w_err(T, "mft-extents", "FSCTL_GET_RETRIEVAL_POINTERS failed on the MFT", GetLastError());
            goto out;
        }
        for (size_t i = first; i < np; i++) {
            if (pieces[i].lcn + pieces[i].count > fend)
                fend = pieces[i].lcn + pieces[i].count;
            fsize += pieces[i].count;
        }
        gk3w_log("%s: %zu fragment(s), %lld MiB, ends at %lld MiB", STREAM_NAMES[f], np - first, (long long)(fsize * bpc >> 20),
                 (long long)(fend * bpc >> 20));
        if (fend > end_before)
            end_before = fend;
    }
    source_limit = end_before;

    /* 按 chunk 拆段 */
    {
        size_t ne = np, cap = 0;
        ext = pieces;
        pieces = NULL;
        np = 0;
        for (size_t i = 0; i < ne; i++) {
            LONGLONG vcn = ext[i].vcn, lcn = ext[i].lcn, left = ext[i].count;
            if (ext[i].stream == 0 && vcn == 0) {
                LONGLONG k = keep < left ? keep : left;
                predicted = lcn + k;   /* 留在原处的开头 */
                vcn += k;
                lcn += k;
                left -= k;
            }
            while (left > 0) {
                LONGLONG c = left < chunk ? left : chunk;
                if (np == cap) {
                    size_t nc = cap ? cap * 2 : 256;
                    piece *p = realloc(pieces, nc * sizeof *p);
                    if (!p) {
                        gk3w_err(T, "nomem", "out of memory", 0);
                        goto out;
                    }
                    pieces = p;
                    cap = nc;
                }
                pieces[np++] = (piece){ext[i].stream, vcn, lcn, c};
                vcn += c;
                lcn += c;
                left -= c;
            }
        }
    }
    qsort(pieces, np, sizeof *pieces, by_order);
    if (get_free_runs(vol, vd.TotalClusters.QuadPart, source_limit, min_run, &runs, &nr)) {
        gk3w_err(T, "volume-bitmap", "FSCTL_GET_VOLUME_BITMAP failed", GetLastError());
        goto out;
    }
    gk3w_log(execute ? "moving" : "dry run: planning only, nothing is moved");

    for (size_t k = 0; k < np; k++) {
        const piece *p = &pieces[k];
        LONGLONG vcn = p->vcn, lcn = p->lcn, left = p->count;
        while (left > 0) {
            int ri = find_run(runs, nr, lcn, left, min_run);
            LONGLONG target, cnt;
            if (ri < 0)
                break;
            target = runs[ri].start;
            cnt = left < runs[ri].len ? left : runs[ri].len;
            if (lcn - target < cnt)
                cnt = lcn - target;   /* 目标整段都在源的下方，不重叠 */
            if (execute) {
                DWORD err = move_clusters(vol, files[p->stream], vcn, target, cnt);
                if (err) {
                    /* 空闲位图是快照：重读一次再试 */
                    free(runs);
                    runs = NULL;
                    if (get_free_runs(vol, vd.TotalClusters.QuadPart, source_limit, min_run, &runs, &nr))
                        nr = 0;
                    ri = find_run(runs, nr, lcn, left, min_run);
                    if (ri < 0)
                        break;
                    target = runs[ri].start;
                    cnt = left < runs[ri].len ? left : runs[ri].len;
                    if (lcn - target < cnt)
                        cnt = lcn - target;
                    err = move_clusters(vol, files[p->stream], vcn, target, cnt);
                }
                if (err && p->stream == 0 && p->vcn == keep) {
                    gk3w_log("NTFS refused to move the MFT right after its first %lld cluster(s) (error %lu); leaving that piece in place",
                             (long long)keep, (unsigned long)err);
                    break;
                }
                if (err) {
                    gk3w_log("cannot move %s VCN %lld (%lld clusters): error %lu", STREAM_NAMES[p->stream], (long long)vcn, (long long)cnt,
                             (unsigned long)err);
                    if (++failures >= 3) {
                        gk3w_err(T, "mft-move-failed", "moving the MFT failed 3 times; stopped (what was moved stays valid, running again continues)", err);
                        goto out;
                    }
                    break;
                }
                T = GK3W_TOUCHED_YES;
                gk3w_log("moved %s VCN %lld (%lld clusters): %lld MiB -> %lld MiB", STREAM_NAMES[p->stream], (long long)vcn, (long long)cnt,
                         (long long)(lcn * bpc >> 20), (long long)(target * bpc >> 20));
            }
            runs[ri].start += cnt;
            runs[ri].len -= cnt;
            if (target + cnt > predicted)
                predicted = target + cnt;
            moved += cnt;
            steps++;
            vcn += cnt;
            lcn += cnt;
            left -= cnt;
        }
        if (left > 0) {
            unplaced += left;
            if (lcn + left > predicted)
                predicted = lcn + left;
        }
    }

    if (execute) {
        /* 挪完之后实际的末尾 */
        piece *after = NULL;
        size_t na = 0, capa = 0;
        predicted = 0;
        for (int f = 0; f < 2; f++)
            if (files[f] != INVALID_HANDLE_VALUE && get_extents(files[f], &after, &na, &capa, f))
                gk3w_log("cannot re-read the extents of %s", STREAM_NAMES[f]);
        for (size_t i = 0; i < na; i++)
            if (after[i].lcn + after[i].count > predicted)
                predicted = after[i].lcn + after[i].count;
        free(after);
    }
    gk3w_log("%s %lld MiB in %d step(s), %lld MiB could not be placed lower; the MFT %s at %lld MiB (was %lld MiB)",
             execute ? "moved" : "plan: move", (long long)(moved * bpc >> 20), steps, (long long)(unplaced * bpc >> 20),
             execute ? "now ends" : "would end", (long long)(predicted * bpc >> 20), (long long)(end_before * bpc >> 20));

    dl[0] = (char)drive;
    dl[1] = ':';
    dl[2] = 0;
    gk3w_line_begin(&l, "MFTMOVE");
    gk3w_line_kv(&l, "drive", dl);
    gk3w_line_kv(&l, "execute", execute ? "yes" : "no");
    gk3w_line_u64(&l, "end_before_mib", (uint64_t)(end_before * bpc) >> 20);
    gk3w_line_u64(&l, "end_after_mib", (uint64_t)(predicted * bpc) >> 20);
    gk3w_line_u64(&l, "moved_mib", (uint64_t)(moved * bpc) >> 20);
    gk3w_line_u64(&l, "steps", (uint64_t)steps);
    gk3w_line_u64(&l, "unplaced_mib", (uint64_t)(unplaced * bpc) >> 20);
    (void)gk3w_line_emit(&l, stdout);
    gk3w_line_free(&l);
    rc = 0;
out:
    for (int f = 0; f < 2; f++)
        if (files[f] != INVALID_HANDLE_VALUE)
            CloseHandle(files[f]);
    if (vol != INVALID_HANDLE_VALUE)
        CloseHandle(vol);
    free(pieces);
    free(ext);
    free(runs);
    return rc;
}
