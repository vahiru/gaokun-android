/* Windows：把 NTFS 的主文件表（$MFT 的 $DATA、$BITMAP 与 $ATTRIBUTE_LIST）挪到卷的前部，让 C 盘能压缩得更多。
 *
 *   gk3w mft-move [--drive X] [--execute] [--chunk-mib N] [--min-run-mib N]
 *
 * "压缩卷"的分析常常停在 $Mft 上（shrink.c 读到的 259 号事件），这时 MFT 的片段落在卷的后部。这里用 Windows 的
 * 碎片整理接口（FSCTL_GET_VOLUME_BITMAP、FSCTL_GET_RETRIEVAL_POINTERS、FSCTL_MOVE_FILE，与 defrag.exe、Sysinternals
 * Contig 相同）把它们挪进前部的空隙：每一步都由 NTFS 完成，中途停止时已挪的部分保持有效。MFT 可以拆成几段放进
 * 不同的空隙，不要求一整块连续的空闲空间；小于 --min-run-mib（默认 8）的空隙不用，免得把 MFT 拆得太碎。
 *
 * 规划（plan）分两步：先从位置最高的一段起（每段至多 --chunk-mib，默认 16），给每段找它下方最低的、放得下的空隙；
 * 再在每个空隙里按 VCN 从底部依次排开 —— 同一片段的相邻各段首尾相接，挪完仍是连续的一段，MFT 不会因此变碎，
 * 也尽量靠前。移动的距离取 16 KiB 的整数倍（"Defragmenting Files"：否则卷影副本要为整段做写时复制）。
 * 执行时位置高的先挪；某一步失败（空闲位图是快照，期间可能被别的写入占用）就重读位图、对剩下的重新规划，
 * 累计失败 3 次即停（已挪的保持原样，再运行一次可以继续）。
 * MFT 开头的 16 条记录（$MFT、$MFTMirr、$LogFile … 这些系统文件）必须留在引导扇区记录的位置（MftStartLcn）上，
 * NTFS 拒绝挪它们（错误 87；记录 1 KiB、簇 4 KiB 时是开头 4 个簇）：规划时就把这几个簇留在原处；紧接其后的
 * 一段仍被以错误 87 拒绝时，把它也留在原处。
 * 不带 --execute 只分析、给出计划。--execute 改动系统卷时要求本机是 MateBook E Go（--drive 用于对着测试卷）。
 * 输出 MFTMOVE drive=X: execute=yes|no end_before_mib= end_after_mib= moved_mib= steps= unplaced_mib= fixed_end_mib=
 * （end_after_mib：执行时是挪完之后实际的位置，分析时是预计的位置；fixed_end_mib：留在原处的开头的末尾）。
 * 累计失败 3 次时照样输出这一行（已挪的部分），再报 ERR。 */
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
    int stream;                 /* STREAM_NAMES 的下标 */
    LONGLONG vcn, lcn, count;
} piece;

typedef struct {
    LONGLONG start, len;
} run;

/* 文档列出的、可以用 FSCTL_MOVE_FILE 移动的 $MFT 的流 */
#define N_STREAMS 3
static const char *const STREAM_NAMES[N_STREAMS] = {"$MFT::$DATA", "$MFT::$BITMAP", "$MFT::$ATTRIBUTE_LIST"};

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

/* 要挪的一段（至多 chunk）：规划时分到一个空隙（gap）并排好目标位置（target） */
enum { SEG_TODO, SEG_DONE, SEG_FIXED };
typedef struct {
    int stream;
    LONGLONG vcn, lcn, count;
    int gap;                    /* 分到的空隙（runs 的下标），-1 = 没地方放 */
    LONGLONG target;
    int state;                  /* SEG_TODO / SEG_DONE（挪好了）/ SEG_FIXED（NTFS 不让挪，留在原处） */
} seg;

static int push_seg(seg **v, size_t *n, size_t *cap, seg x)
{
    if (*n == *cap) {
        size_t nc = *cap ? *cap * 2 : 256;
        seg *nv = realloc(*v, nc * sizeof *nv);
        if (!nv)
            return -1;
        *v = nv;
        *cap = nc;
    }
    (*v)[(*n)++] = x;
    return 0;
}

/* qsort 的比较函数拿不到上下文：排序前把段表放在这里 */
static const seg *g_sort_segs;

static int by_lcn_desc(const void *a, const void *b)
{
    const seg *x = &g_sort_segs[*(const size_t *)a], *y = &g_sort_segs[*(const size_t *)b];
    return x->lcn < y->lcn ? 1 : x->lcn > y->lcn ? -1 : 0;
}

/* 同一空隙里按 $DATA / $BITMAP、VCN 排 */
static int by_gap_vcn(const void *a, const void *b)
{
    const seg *x = &g_sort_segs[*(const size_t *)a], *y = &g_sort_segs[*(const size_t *)b];
    if (x->gap != y->gap)
        return x->gap < y->gap ? -1 : 1;
    if (x->stream != y->stream)
        return x->stream < y->stream ? -1 : 1;
    return x->vcn < y->vcn ? -1 : x->vcn > y->vcn ? 1 : 0;
}

/* 给还没挪的段分配位置，分两步：
 * 1. 从位置最高的一段起，放进它下方最低的、放得下的空隙（剩余容量不小于 min_run，或者正好装得下）；放不下整段时
 *    拆开，前一部分放进这个空隙，其余的接着找下一个。每段按最坏情况多预留 align − 1 个簇给对齐。
 * 2. 每个空隙里按 VCN 从空隙底部依次排开：同一片段的相邻各段首尾相接、顺序不变，挪完仍是连续的一段；
 *    目标位置与源位置之差取 align 的整数倍。
 * 失败返回 -1（内存不足） */
static int plan(seg **segs, size_t *n, size_t *cap, const run *runs, size_t nr, LONGLONG min_run, LONGLONG align)
{
    LONGLONG *avail = malloc((nr ? nr : 1) * sizeof *avail);
    size_t *order = NULL, no = 0, n0 = *n;
    if (!avail)
        return -1;
    for (size_t r = 0; r < nr; r++)
        avail[r] = runs[r].len;
    for (size_t i = 0; i < *n; i++)
        if ((*segs)[i].state == SEG_TODO)
            (*segs)[i].gap = -1;
    order = malloc((n0 ? n0 : 1) * sizeof *order);
    if (!order) {
        free(avail);
        return -1;
    }
    for (size_t i = 0; i < n0; i++)
        if ((*segs)[i].state == SEG_TODO)
            order[no++] = i;
    g_sort_segs = *segs;
    qsort(order, no, sizeof *order, by_lcn_desc);

    for (size_t k = 0; k < no; k++) {
        size_t idx = order[k];
        for (;;) {
            seg cur = (*segs)[idx];
            size_t r;
            LONGLONG usable = 0, cnt;
            for (r = 0; r < nr; r++) {
                if (runs[r].start >= cur.lcn) {
                    r = nr;
                    break;
                }
                /* 空隙够不够大按它本身的剩余容量判断；能放多少要扣掉对齐预留 */
                usable = avail[r] - (align - 1);
                if (usable > 0 && (avail[r] >= min_run || usable >= cur.count))
                    break;
            }
            if (r >= nr)
                break;   /* 没有放得下的空隙：留着（gap = -1） */
            cnt = cur.count < usable ? cur.count : usable;
            avail[r] -= cnt + (align - 1);
            if (cnt == cur.count) {
                (*segs)[idx].gap = (int)r;
                break;
            }
            /* 拆开：前一部分放进这个空隙，其余的（位置更高）接着找 */
            if (push_seg(segs, n, cap, (seg){cur.stream, cur.vcn, cur.lcn, cnt, (int)r, 0, SEG_TODO})) {
                free(avail);
                free(order);
                return -1;
            }
            (*segs)[idx].vcn += cnt;
            (*segs)[idx].lcn += cnt;
            (*segs)[idx].count -= cnt;
        }
    }
    free(order);

    /* 第 2 步：每个空隙里从底部按 VCN 排开 */
    order = malloc((*n ? *n : 1) * sizeof *order);
    if (!order) {
        free(avail);
        return -1;
    }
    no = 0;
    for (size_t i = 0; i < *n; i++)
        if ((*segs)[i].state == SEG_TODO && (*segs)[i].gap >= 0)
            order[no++] = i;
    g_sort_segs = *segs;
    qsort(order, no, sizeof *order, by_gap_vcn);
    for (size_t k = 0, pos = 0; k < no; k++) {
        seg *g = &(*segs)[order[k]];
        if (!k || (*segs)[order[k - 1]].gap != g->gap)
            pos = (size_t)runs[g->gap].start;
        /* lcn − target 取 align 的整数倍 */
        g->target = (LONGLONG)pos + ((g->lcn - (LONGLONG)pos) % align);
        pos = (size_t)(g->target + g->count);
    }
    free(order);
    free(avail);
    return 0;
}

int gk3w_cmd_mft_move(int argc, wchar_t **argv)
{
    const wchar_t *da = gk3w_arg(argc, argv, L"--drive");
    int execute = gk3w_flag(argc, argv, L"--execute"), rc = 1, failures = 0, steps = 0;
    uint64_t chunk_mib = 16, min_run_mib = 8;
    wchar_t drive, sysdrive, win[MAX_PATH], path[MAX_PATH];
    HANDLE vol = INVALID_HANDLE_VALUE, files[N_STREAMS] = {INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE};
    NTFS_VOLUME_DATA_BUFFER vd;
    DWORD got = 0;
    seg *pieces = NULL;
    piece *ext = NULL;
    size_t np = 0, capp = 0;
    run *runs = NULL;
    size_t nr = 0;
    LONGLONG bpc, keep, align, chunk, min_run, end_before = 0, source_limit = 0, predicted = 0, moved = 0, unplaced = 0, fixed_end = 0;
    int aborted = 0;
    gk3w_touched T = GK3W_TOUCHED_NO;
    char dl[3];
    gk3w_line l = {0};

    if ((da && (!iswalpha(da[0]) || (da[1] && (da[1] != L':' || da[2])))) ||
        (gk3w_arg(argc, argv, L"--chunk-mib") && (gk3w_arg_u64(argc, argv, L"--chunk-mib", &chunk_mib) || !chunk_mib || chunk_mib > 1024)) ||
        (gk3w_arg(argc, argv, L"--min-run-mib") && (gk3w_arg_u64(argc, argv, L"--min-run-mib", &min_run_mib) || !min_run_mib || min_run_mib > 1024)))
        return gk3w_err(GK3W_TOUCHED_NA, "usage", "usage: mft-move [--drive X] [--execute] [--chunk-mib 1-1024] [--min-run-mib 1-1024]", 0);
    sysdrive = GetSystemWindowsDirectoryW(win, MAX_PATH) && win[1] == L':' ? towupper(win[0]) : L'C';
    drive = da ? towupper(da[0]) : sysdrive;
    /* 改动系统卷只在这台平板上做；--drive 指别的卷用于测试 */
    if (execute && drive == sysdrive && !gk3w_model_ok_now())
        return gk3w_err(GK3W_TOUCHED_NO, "wrong-model", "this computer is not a Huawei MateBook E Go (gaokun3); not moving its MFT (disk not touched)", 0);
    if (enable_manage_volume())
        gk3w_log("cannot enable SeManageVolumePrivilege; moving may be refused");

    _snwprintf(path, MAX_PATH, L"\\\\.\\%lc:", drive);
    vol = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (vol == INVALID_HANDLE_VALUE) {
        gk3w_err(T, "volume-open", "cannot open the volume", GetLastError());
        goto out;
    }
    if (!DeviceIoControl(vol, FSCTL_GET_NTFS_VOLUME_DATA, NULL, 0, &vd, sizeof vd, &got, NULL)) {
        gk3w_err(T, "not-ntfs", "FSCTL_GET_NTFS_VOLUME_DATA failed; is this an NTFS volume?", GetLastError());
        goto out;
    }
    bpc = vd.BytesPerCluster;
    /* MFT 开头的 16 条系统记录所在的簇，留在原处 */
    keep = (16 * (LONGLONG)vd.BytesPerFileRecordSegment + bpc - 1) / bpc;
    /* 移动的距离取 16 KiB 的整数倍：否则卷影副本（还原点）要为整段做写时复制，占用的空间随之膨胀 */
    align = 16384 / bpc > 1 ? 16384 / bpc : 1;
    chunk = (LONGLONG)(chunk_mib << 20) / bpc;
    min_run = (LONGLONG)(min_run_mib << 20) / bpc;
    if (chunk < 1)
        chunk = 1;
    if (min_run < 1)
        min_run = 1;
    gk3w_log("volume %c: %lld MiB, %lld MiB free, %lu-byte clusters; MFT starts at %lld MiB", (char)drive,
             (long long)(vd.TotalClusters.QuadPart * bpc >> 20), (long long)(vd.FreeClusters.QuadPart * bpc >> 20),
             (unsigned long)bpc, (long long)(vd.MftStartLcn.QuadPart * bpc >> 20));

    for (int f = 0; f < N_STREAMS; f++) {
        static const wchar_t *const SUFFIX[N_STREAMS] = {L"", L"::$BITMAP", L"::$ATTRIBUTE_LIST"};
        size_t first = np;
        LONGLONG fend = 0, fsize = 0;
        _snwprintf(path, MAX_PATH, L"%lc:\\$Mft%ls", drive, SUFFIX[f]);
        files[f] = CreateFileW(path, FILE_READ_ATTRIBUTES | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
        if (files[f] == INVALID_HANDLE_VALUE) {
            if (!f) {
                gk3w_err(T, "mft-open", "cannot open $MFT", GetLastError());
                goto out;
            }
            /* 没有属性列表的 $MFT 就没有这个流 */
            gk3w_log("cannot open %s (%lu), skipping it", STREAM_NAMES[f], (unsigned long)GetLastError());
            continue;
        }
        if (get_extents(files[f], &ext, &np, &capp, f)) {
            if (!f) {
                gk3w_err(T, "mft-extents", "FSCTL_GET_RETRIEVAL_POINTERS failed on the MFT", GetLastError());
                goto out;
            }
            /* $BITMAP / $ATTRIBUTE_LIST 读不出位置：不挪它（已读的一部分也不要） */
            gk3w_log("cannot read the extents of %s (%lu), skipping it", STREAM_NAMES[f], (unsigned long)GetLastError());
            np = first;
            CloseHandle(files[f]);
            files[f] = INVALID_HANDLE_VALUE;
            continue;
        }
        for (size_t i = first; i < np; i++) {
            if (ext[i].lcn + ext[i].count > fend)
                fend = ext[i].lcn + ext[i].count;
            fsize += ext[i].count;
        }
        gk3w_log("%s: %zu fragment(s), %lld MiB, ends at %lld MiB", STREAM_NAMES[f], np - first, (long long)(fsize * bpc >> 20),
                 (long long)(fend * bpc >> 20));
        if (fend > end_before)
            end_before = fend;
    }
    source_limit = end_before;

    /* 按 chunk 拆段；$DATA 开头的 keep 个簇（16 条系统记录）留在原处 */
    {
        size_t ne = np;
        np = 0;
        capp = 0;
        for (size_t i = 0; i < ne; i++) {
            LONGLONG vcn = ext[i].vcn, lcn = ext[i].lcn, left = ext[i].count;
            if (ext[i].stream == 0 && vcn < keep) {
                LONGLONG k = keep - vcn < left ? keep - vcn : left;
                if (lcn + k > fixed_end)
                    fixed_end = lcn + k;
                vcn += k;
                lcn += k;
                left -= k;
            }
            while (left > 0) {
                LONGLONG c = left < chunk ? left : chunk;
                if (push_seg(&pieces, &np, &capp, (seg){ext[i].stream, vcn, lcn, c, -1, 0, 0})) {
                    gk3w_err(T, "nomem", "out of memory", 0);
                    goto out;
                }
                vcn += c;
                lcn += c;
                left -= c;
            }
        }
    }
    gk3w_log(execute ? "moving" : "dry run: planning only, nothing is moved");

    /* 规划 → 依次移动；移动失败时重读空闲位图、对剩下的重新规划 */
    for (;;) {
        size_t *order = NULL, no = 0;
        int replan = 0;
        free(runs);
        runs = NULL;
        if (get_free_runs(vol, vd.TotalClusters.QuadPart, source_limit, min_run, &runs, &nr)) {
            gk3w_log("FSCTL_GET_VOLUME_BITMAP failed (%lu)", (unsigned long)GetLastError());
            if (!steps && !failures) {
                gk3w_err(T, "volume-bitmap", "FSCTL_GET_VOLUME_BITMAP failed", GetLastError());
                goto out;
            }
            nr = 0;
        }
        if (plan(&pieces, &np, &capp, runs, nr, min_run, align)) {
            gk3w_err(T, "nomem", "out of memory", 0);
            goto out;
        }
        if (!execute)
            break;
        /* 位置高的先挪 */
        order = malloc((np ? np : 1) * sizeof *order);
        if (!order) {
            gk3w_err(T, "nomem", "out of memory", 0);
            goto out;
        }
        for (size_t i = 0; i < np; i++)
            if (pieces[i].state == SEG_TODO && pieces[i].gap >= 0)
                order[no++] = i;
        g_sort_segs = pieces;
        qsort(order, no, sizeof *order, by_lcn_desc);
        for (size_t k = 0; k < no && !replan; k++) {
            seg *g = &pieces[order[k]];
            DWORD err = move_clusters(vol, files[g->stream], g->vcn, g->target, g->count);
            if (!err) {
                T = GK3W_TOUCHED_YES;
                g->state = SEG_DONE;
                moved += g->count;
                steps++;
                gk3w_log("moved %s VCN %lld (%lld clusters): %lld MiB -> %lld MiB", STREAM_NAMES[g->stream], (long long)g->vcn,
                         (long long)g->count, (long long)(g->lcn * bpc >> 20), (long long)(g->target * bpc >> 20));
                continue;
            }
            gk3w_log("cannot move %s VCN %lld (%lld clusters) to %lld MiB: error %lu", STREAM_NAMES[g->stream], (long long)g->vcn,
                     (long long)g->count, (long long)(g->target * bpc >> 20), (unsigned long)err);
            if (err == ERROR_INVALID_PARAMETER && g->stream == 0 && g->vcn == keep) {
                /* NTFS 留在原处的开头比 16 条记录更长：这一段不挪，不算失败 */
                gk3w_log("NTFS keeps more of the start of the MFT in place; leaving this piece");
                g->state = SEG_FIXED;
                if (g->lcn + g->count > fixed_end)
                    fixed_end = g->lcn + g->count;
            } else if (++failures >= 3) {
                aborted = 1;
            }
            replan = 1;   /* 空闲位图可能已过期：重读、重新规划剩下的 */
        }
        free(order);
        if (!replan || aborted)
            break;
    }

    /* 结果：挪完（或预计挪完）之后 MFT 的末尾。unplaced 只算没挪、而且仍在挪好的部分上方的段 —— 它们决定了
     * MFT 的末尾；本来就在前部、下面没有空隙的段不算（再运行一次时它们全都"放不下"） */
    {
        LONGLONG low_end = fixed_end;
        for (size_t i = 0; i < np; i++) {
            const seg *g = &pieces[i];
            int placed = g->state == SEG_DONE || (!execute && g->state == SEG_TODO && g->gap >= 0);
            LONGLONG e = placed ? g->target + g->count : g->lcn + g->count;
            if (placed && e > low_end)
                low_end = e;
            if (g->state == SEG_FIXED && e > low_end)
                low_end = e;
            if (placed && !execute) {
                moved += g->count;
                steps++;
            }
        }
        predicted = low_end;
        for (size_t i = 0; i < np; i++) {
            const seg *g = &pieces[i];
            int placed = g->state == SEG_DONE || (!execute && g->state == SEG_TODO && g->gap >= 0);
            if (placed || g->state == SEG_FIXED)
                continue;
            if (g->lcn + g->count > low_end)
                unplaced += g->count;
            if (g->lcn + g->count > predicted)
                predicted = g->lcn + g->count;
        }
    }
    if (execute) {
        /* 重读实际的位置；读不出来就用上面算的 */
        piece *after = NULL;
        size_t na = 0, capa = 0;
        int ok = 1;
        LONGLONG real = 0;
        for (int f = 0; f < N_STREAMS; f++)
            if (files[f] != INVALID_HANDLE_VALUE && get_extents(files[f], &after, &na, &capa, f)) {
                gk3w_log("cannot re-read the extents of %s", STREAM_NAMES[f]);
                ok = 0;
            }
        for (size_t i = 0; i < na; i++)
            if (after[i].lcn + after[i].count > real)
                real = after[i].lcn + after[i].count;
        if (ok && real)
            predicted = real;
        {
            size_t frag = 0;
            for (size_t i = 0; i < na; i++)
                frag += after[i].stream == 0;
            gk3w_log("$MFT::$DATA now has %zu fragment(s)", frag);
        }
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
    /* MFT 开头必须留在原处的部分的末尾：end_after_mib 等于它时，MFT 已经不能再往前挪 */
    gk3w_line_u64(&l, "fixed_end_mib", ((uint64_t)(fixed_end * bpc) + (1u << 20) - 1) >> 20);
    (void)gk3w_line_emit(&l, stdout);
    gk3w_line_free(&l);
    if (aborted)
        gk3w_err(T, "mft-move-failed", "moving the MFT failed 3 times; stopped (what was moved stays valid, running again continues)", 0);
    else
        rc = 0;
out:
    for (int f = 0; f < N_STREAMS; f++)
        if (files[f] != INVALID_HANDLE_VALUE)
            CloseHandle(files[f]);
    if (vol != INVALID_HANDLE_VALUE)
        CloseHandle(vol);
    free(pieces);
    free(ext);
    free(runs);
    return rc;
}
