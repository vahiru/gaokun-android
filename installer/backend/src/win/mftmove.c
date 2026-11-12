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

#include "../mftplan.h"
#include "../proto.h"
#include "disk.h"
#include "win.h"

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

/* 文件的片段（追加到 *out，*n 为个数）。失败返回 -1 */
static int get_extents(HANDLE f, gk3w_mft_ext **out, size_t *n, size_t *cap, int stream)
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
                    gk3w_mft_ext *np = realloc(*out, nc * sizeof *np);
                    if (!np)
                        goto done;
                    *out = np;
                    *cap = nc;
                }
                (*out)[(*n)++] = (gk3w_mft_ext){stream, vcn, lcn, next - vcn};
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

/* limit 以下、不短于 min_run 的空闲空隙（读卷的空闲位图，见 gk3w_mft_free_runs）。失败返回 -1 */
static int get_free_runs(HANDLE vol, LONGLONG total, LONGLONG limit, LONGLONG min_run, gk3w_mft_run **out, size_t *n)
{
    size_t bytes = sizeof(VOLUME_BITMAP_BUFFER) + (size_t)(total / 8) + 64;
    VOLUME_BITMAP_BUFFER *bm = malloc(bytes);
    STARTING_LCN_INPUT_BUFFER in = {{0}};
    DWORD got = 0;
    LONGLONG bits;
    int rc;
    *out = NULL;
    *n = 0;
    if (!bm)
        return -1;
    if (!DeviceIoControl(vol, FSCTL_GET_VOLUME_BITMAP, &in, sizeof in, bm, (DWORD)bytes, &got, NULL) && GetLastError() != ERROR_MORE_DATA) {
        free(bm);
        return -1;
    }
    bits = bm->BitmapSize.QuadPart;
    /* 只用实际返回了的那部分位图 */
    if (got > offsetof(VOLUME_BITMAP_BUFFER, Buffer) && bits > (LONGLONG)(got - offsetof(VOLUME_BITMAP_BUFFER, Buffer)) * 8)
        bits = (LONGLONG)(got - offsetof(VOLUME_BITMAP_BUFFER, Buffer)) * 8;
    rc = gk3w_mft_free_runs(bm->Buffer, bm->StartingLcn.QuadPart, bits, limit, min_run, out, n);
    free(bm);
    return rc;
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

int gk3w_cmd_mft_move(int argc, wchar_t **argv)
{
    const wchar_t *da = gk3w_arg(argc, argv, L"--drive");
    int execute = gk3w_flag(argc, argv, L"--execute"), rc = 1, failures = 0;
    uint64_t chunk_mib = 16, min_run_mib = 8;
    wchar_t drive, sysdrive, win[MAX_PATH], path[MAX_PATH];
    HANDLE vol = INVALID_HANDLE_VALUE, files[N_STREAMS] = {INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE};
    NTFS_VOLUME_DATA_BUFFER vd;
    DWORD got = 0;
    gk3w_mft_seg *pieces = NULL;
    gk3w_mft_ext *ext = NULL;
    size_t np = 0, capp = 0, ne = 0, cape = 0;
    gk3w_mft_run *runs = NULL;
    size_t nr = 0;
    LONGLONG bpc, keep, align, chunk, min_run, end_before = 0, source_limit = 0;
    int64_t fixed_end = 0;
    gk3w_mft_result res;
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
    /* 只出计划时只读打开 */
    vol = CreateFileW(path, execute ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                      OPEN_EXISTING, 0, NULL);
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
        size_t first = ne;
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
        if (get_extents(files[f], &ext, &ne, &cape, f)) {
            if (!f) {
                gk3w_err(T, "mft-extents", "FSCTL_GET_RETRIEVAL_POINTERS failed on the MFT", GetLastError());
                goto out;
            }
            /* $BITMAP / $ATTRIBUTE_LIST 读不出位置：不挪它（已读的一部分也不要） */
            gk3w_log("cannot read the extents of %s (%lu), skipping it", STREAM_NAMES[f], (unsigned long)GetLastError());
            ne = first;
            CloseHandle(files[f]);
            files[f] = INVALID_HANDLE_VALUE;
            continue;
        }
        for (size_t i = first; i < ne; i++) {
            if (ext[i].lcn + ext[i].count > fend)
                fend = ext[i].lcn + ext[i].count;
            fsize += ext[i].count;
        }
        gk3w_log("%s: %zu fragment(s), %lld MiB, ends at %lld MiB", STREAM_NAMES[f], ne - first, (long long)(fsize * bpc >> 20),
                 (long long)(fend * bpc >> 20));
        if (fend > end_before)
            end_before = fend;
    }
    source_limit = end_before;

    /* 按 chunk 拆段；$DATA 开头的 keep 个簇（16 条系统记录）留在原处 */
    if (gk3w_mft_split(ext, ne, keep, chunk, &pieces, &np, &capp, &fixed_end)) {
        gk3w_err(T, "nomem", "out of memory", 0);
        goto out;
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
            if (T == GK3W_TOUCHED_NO && !failures) {
                gk3w_err(T, "volume-bitmap", "FSCTL_GET_VOLUME_BITMAP failed", GetLastError());
                goto out;
            }
            nr = 0;
        }
        if (gk3w_mft_plan(&pieces, &np, &capp, runs, nr, min_run, align)) {
            gk3w_err(T, "nomem", "out of memory", 0);
            goto out;
        }
        if (!execute)
            break;
        /* 位置高的先挪 */
        if (gk3w_mft_order(pieces, np, &order, &no)) {
            gk3w_err(T, "nomem", "out of memory", 0);
            goto out;
        }
        for (size_t k = 0; k < no && !replan; k++) {
            gk3w_mft_seg *g = &pieces[order[k]];
            DWORD err = move_clusters(vol, files[g->stream], g->vcn, g->target, g->count);
            if (!err) {
                T = GK3W_TOUCHED_YES;
                g->state = GK3W_SEG_DONE;
                gk3w_log("moved %s VCN %lld (%lld clusters): %lld MiB -> %lld MiB", STREAM_NAMES[g->stream], (long long)g->vcn,
                         (long long)g->count, (long long)(g->lcn * bpc >> 20), (long long)(g->target * bpc >> 20));
                continue;
            }
            gk3w_log("cannot move %s VCN %lld (%lld clusters) to %lld MiB: error %lu", STREAM_NAMES[g->stream], (long long)g->vcn,
                     (long long)g->count, (long long)(g->target * bpc >> 20), (unsigned long)err);
            if (err == ERROR_INVALID_PARAMETER && g->stream == 0 && g->vcn == keep) {
                /* NTFS 留在原处的开头比 16 条记录更长：这一段不挪，不算失败 */
                gk3w_log("NTFS keeps more of the start of the MFT in place; leaving this piece");
                g->state = GK3W_SEG_FIXED;
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

    /* 结果：挪完（或预计挪完）之后 MFT 的末尾、挪了多少、多少放不下（见 gk3w_mft_summary） */
    gk3w_mft_summary(pieces, np, execute, fixed_end, &res);
    if (execute) {
        /* 重读实际的位置；读不出来就用上面算的 */
        gk3w_mft_ext *after = NULL;
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
            res.end = real;
        {
            size_t frag = 0;
            for (size_t i = 0; i < na; i++)
                frag += after[i].stream == 0;
            gk3w_log("$MFT::$DATA now has %zu fragment(s)", frag);
        }
        free(after);
    }
    gk3w_log("%s %lld MiB in %d step(s), %lld MiB could not be placed lower; the MFT %s at %lld MiB (was %lld MiB)",
             execute ? "moved" : "plan: move", (long long)(res.moved * bpc >> 20), res.steps, (long long)(res.unplaced * bpc >> 20),
             execute ? "now ends" : "would end", (long long)(res.end * bpc >> 20), (long long)(end_before * bpc >> 20));

    dl[0] = (char)drive;
    dl[1] = ':';
    dl[2] = 0;
    gk3w_line_begin(&l, "MFTMOVE");
    gk3w_line_kv(&l, "drive", dl);
    gk3w_line_kv(&l, "execute", execute ? "yes" : "no");
    gk3w_line_u64(&l, "end_before_mib", (uint64_t)(end_before * bpc) >> 20);
    gk3w_line_u64(&l, "end_after_mib", (uint64_t)(res.end * bpc) >> 20);
    gk3w_line_u64(&l, "moved_mib", (uint64_t)(res.moved * bpc) >> 20);
    gk3w_line_u64(&l, "steps", (uint64_t)res.steps);
    gk3w_line_u64(&l, "unplaced_mib", (uint64_t)(res.unplaced * bpc) >> 20);
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
