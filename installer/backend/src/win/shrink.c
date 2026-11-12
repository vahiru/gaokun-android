/* Windows：系统卷最多还能压缩出多少、被哪个文件挡住了 ——"可用未分配空间"不够时给用户的指引。只读。
 *
 *   gk3w shrink-info [--drive X]
 *
 * 1. WMI 的 MSFT_Partition.GetSupportedSize（即 Get-PartitionSupportedSize）：分区最小能缩到多大。"磁盘管理"的
 *    "压缩卷"做的是同一个分析，大卷要几分钟。
 * 2. 这次分析会在"应用程序"日志里记一条 Microsoft-Windows-Defrag 的 259 号事件，写着最后一个不可移动的文件
 *    （压缩就停在它那里）。按卷的 GUID 路径，从分析开始之后的事件里找这个卷最新的一条。事件数据依次是：
 *    卷名、卷的 GUID 路径、文件、文件的最后一个簇、可压缩到的位置、NTFS 文件标志、阶段。
 *
 * 输出 SHRINK drive=X: size_mib= min_mib= max_shrink_mib= adjacent_mib= free_mib= [file= flags= limit=file|space]：
 *   adjacent_mib：分区之后紧挨着的未分配空间（SizeMax − Size），压缩出来的空间与它连成一片；
 *   limit：这个文件的末尾（最后一个簇 × 簇大小）离分区最小尺寸不到 1 GiB 时是 file（压缩停在它上面），
 *     否则是 space（最小尺寸由已用空间决定：C 盘太满，挪开这个文件也没用）。
 * 找不到事件时不带 file、flags 与 limit。 */
#include "wmi.h"

#include <winevt.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#include "../proto.h"
#include "disk.h"
#include "win.h"

/* 分析最多等这么久：1 TB 的卷实测约 2 分钟。超时之后这次分析仍在 WMI 里继续跑（释放调用结果不会取消它），
 * 紧接着再分析一次可能报"正在进行"，界面上显示为分析失败、请稍后再试 */
#define ANALYSIS_MS (15u * 60u * 1000u)

static int pick_drive(IWbemClassObject *o, void *ctx)
{
    uint64_t dl;
    return !gk3w_wmi_u64(o, L"DriveLetter", &dl) && (wchar_t)dl && towupper((wchar_t)dl) == *(const wchar_t *)ctx;
}

/* GUID 路径去掉结尾的反斜杠后的长度（事件里的不带，GetVolumeNameForVolumeMountPoint 给的带） */
static size_t guid_len(const wchar_t *g)
{
    size_t n = wcslen(g);
    return n && g[n - 1] == L'\\' ? n - 1 : n;
}

/* 事件里的十六进制簇号（"0xedb5623"）；读不出返回 -1 */
static int hex_lcn(const EVT_VARIANT *v, uint64_t *out)
{
    wchar_t *end = NULL;
    if (v->Type != EvtVarTypeString || !v->StringVal || _wcsnicmp(v->StringVal, L"0x", 2))
        return -1;
    *out = _wcstoui64(v->StringVal + 2, &end, 16);
    return end && end != v->StringVal + 2 && !*end ? 0 : -1;
}

typedef struct {
    char file[2048], flags[32];
    int have_lcn;               /* last 读出来了 */
    uint64_t last;              /* 文件的最后一个簇（LCN） */
} unmovable;

/* 分析开始以来（since_ms 毫秒内）这个卷的 259 号事件：最后一个不可移动的文件、它的 NTFS 标志与两个簇号。找到返回 0 */
static int last_unmovable(const wchar_t *vol_guid, ULONGLONG since_ms, unmovable *u)
{
    wchar_t q[256];
    EVT_HANDLE res, ctx, ev[16];
    DWORD got = 0;
    int found = -1;

    _snwprintf(q, 256,
               L"*[System[Provider[@Name='Microsoft-Windows-Defrag'] and (EventID=259) and TimeCreated[timediff(@SystemTime) <= %llu]]]",
               since_ms);
    res = EvtQuery(NULL, L"Application", q, EvtQueryChannelPath | EvtQueryReverseDirection);
    if (!res) {
        gk3w_log("shrink: cannot query the Application event log (%lu)", (unsigned long)GetLastError());
        return -1;
    }
    ctx = EvtCreateRenderContext(0, NULL, EvtRenderContextUser);
    while (found && ctx && EvtNext(res, 16, ev, 5000, 0, &got)) {
        for (DWORD i = 0; i < got; i++) {
            DWORD used = 0, count = 0;
            PEVT_VARIANT v = NULL;
            if (found && !EvtRender(ctx, ev[i], EvtRenderEventValues, 0, NULL, &used, &count) &&
                GetLastError() == ERROR_INSUFFICIENT_BUFFER && (v = malloc(used)) &&
                EvtRender(ctx, ev[i], EvtRenderEventValues, used, v, &used, &count) && count >= 6 &&
                v[1].Type == EvtVarTypeString && v[2].Type == EvtVarTypeString && v[1].StringVal && v[2].StringVal) {
                size_t n = guid_len(vol_guid);
                if (guid_len(v[1].StringVal) == n && !_wcsnicmp(v[1].StringVal, vol_guid, n)) {
                    gk3w_w2u(v[2].StringVal, u->file, sizeof u->file);
                    if (!u->file[0])
                        gk3w_log("shrink: the file name in event 259 could not be converted");
                    if (v[5].Type == EvtVarTypeString && v[5].StringVal)
                        gk3w_w2u(v[5].StringVal, u->flags, sizeof u->flags);
                    u->have_lcn = !hex_lcn(&v[3], &u->last);
                    found = 0;
                }
            }
            free(v);
            EvtClose(ev[i]);
        }
    }
    if (ctx)
        EvtClose(ctx);
    EvtClose(res);
    return found;
}

int gk3w_cmd_shrink_info(int argc, wchar_t **argv)
{
    const wchar_t *da = gk3w_arg(argc, argv, L"--drive");
    wchar_t drive, win[MAX_PATH], root[4], guid[64];
    gk3w_wmi w;
    IWbemClassObject *part = NULL, *out = NULL;
    uint64_t size = 0, min = 0, max = 0;
    ULARGE_INTEGER freeb;
    ULONGLONG t0;
    HRESULT hr;
    unmovable u;
    DWORD spc, bps, fc, tc;
    int got = -1;
    char dl[3];
    gk3w_line l = {0};
    int rc = 1;

    if (da && (!iswalpha(da[0]) || (da[1] && (da[1] != L':' || da[2]))))
        return gk3w_err(GK3W_TOUCHED_NA, "usage", "usage: shrink-info [--drive X]", 0);
    drive = da ? towupper(da[0]) : (GetSystemWindowsDirectoryW(win, MAX_PATH) && win[1] == L':' ? towupper(win[0]) : L'C');
    _snwprintf(root, 4, L"%lc:\\", drive);
    if (!GetVolumeNameForVolumeMountPointW(root, guid, 64))
        return gk3w_err(GK3W_TOUCHED_NA, "shrink-query", "cannot find the volume of this drive", GetLastError());

    gk3w_progress(0, "shrink-analyze");
    t0 = GetTickCount64();
    if (gk3w_wmi_open(&w, L"ROOT\\Microsoft\\Windows\\Storage") || gk3w_wmi_find(&w, L"SELECT * FROM MSFT_Partition", 30000, pick_drive, &drive, &part) ||
        gk3w_wmi_u64(part, L"Size", &size)) {
        gk3w_err(GK3W_TOUCHED_NA, "shrink-query", "cannot find the partition of this drive through WMI (MSFT_Partition)", 0);
        goto out;
    }
    hr = gk3w_wmi_call(&w, part, L"GetSupportedSize", NULL, NULL, ANALYSIS_MS, &out);
    if (FAILED(hr) || gk3w_wmi_u64(out, L"SizeMin", &min) || min > size || gk3w_wmi_u64(out, L"SizeMax", &max) || max < size) {
        char why[96];
        snprintf(why, sizeof why, "the shrink analysis (GetSupportedSize) failed (0x%08lx)", (unsigned long)hr);
        gk3w_err(GK3W_TOUCHED_NA, "shrink-query", why, 0);
        goto out;
    }
    gk3w_log("shrink: analysis of %c: took %.1f s", (char)drive, (double)(GetTickCount64() - t0) / 1000.0);
    /* 事件在分析结束前写下；往前多留 10 秒，防止日志与本机计时的细小偏差。日志服务写入可能稍晚，找不到时再等几次 */
    memset(&u, 0, sizeof u);
    for (int i = 0; i < 3 && got; i++) {
        if (i)
            Sleep(1000);
        got = last_unmovable(guid, GetTickCount64() - t0 + 10000, &u);
    }
    if (got)
        gk3w_log("shrink: no event 259 for this volume since the analysis started");
    if (!GetDiskFreeSpaceExW(root, &freeb, NULL, NULL))
        freeb.QuadPart = 0;
    if (!GetDiskFreeSpaceW(root, &spc, &bps, &fc, &tc))
        spc = bps = 0;

    dl[0] = (char)drive;
    dl[1] = ':';
    dl[2] = 0;
    gk3w_line_begin(&l, "SHRINK");
    gk3w_line_kv(&l, "drive", dl);
    gk3w_line_u64(&l, "size_mib", size >> 20);
    gk3w_line_u64(&l, "min_mib", (min + (1u << 20) - 1) >> 20);
    gk3w_line_u64(&l, "max_shrink_mib", (size - min) >> 20);
    gk3w_line_u64(&l, "adjacent_mib", (max - size) >> 20);
    gk3w_line_u64(&l, "free_mib", freeb.QuadPart >> 20);
    if (!got && u.file[0]) {
        gk3w_line_kv(&l, "file", u.file);
        if (u.flags[0])
            gk3w_line_kv(&l, "flags", u.flags);
        if (u.have_lcn && spc && bps) {
            uint64_t end = (u.last + 1) * spc * bps;
            gk3w_line_kv(&l, "limit", end + (1ull << 30) >= min ? "file" : "space");
        }
    }
    (void)gk3w_line_emit(&l, stdout);
    gk3w_line_free(&l);
    rc = 0;
out:
    if (out)
        IWbemClassObject_Release(out);
    if (part)
        IWbemClassObject_Release(part);
    gk3w_wmi_close(&w);
    return rc;
}
