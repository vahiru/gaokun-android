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
 * 输出 SHRINK drive=X: size_mib= min_mib= max_shrink_mib= [file= flags=]：找不到事件时不带 file 与 flags。 */
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

/* 分析最多等这么久：1 TB 的卷实测约 2 分钟 */
#define ANALYSIS_MS (15u * 60u * 1000u)

static int pick_drive(IWbemClassObject *o, void *ctx)
{
    uint64_t dl;
    return !gk3w_wmi_u64(o, L"DriveLetter", &dl) && (wchar_t)dl && towupper((wchar_t)dl) == *(const wchar_t *)ctx;
}

/* 分析开始以来（since_ms 毫秒内）这个卷的 259 号事件：最后一个不可移动的文件与它的 NTFS 标志。找到返回 0 */
static int last_unmovable(const wchar_t *vol_guid, ULONGLONG since_ms, char *file, size_t fcap, char *flags, size_t gcap)
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
                size_t n = wcslen(vol_guid);
                /* 事件里的 GUID 路径不带结尾的反斜杠 */
                if (n && vol_guid[n - 1] == L'\\')
                    n--;
                if (wcslen(v[1].StringVal) == n && !_wcsnicmp(v[1].StringVal, vol_guid, n)) {
                    gk3w_w2u(v[2].StringVal, file, fcap);
                    if (v[5].Type == EvtVarTypeString && v[5].StringVal)
                        gk3w_w2u(v[5].StringVal, flags, gcap);
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
    uint64_t size = 0, min = 0;
    ULONGLONG t0;
    HRESULT hr;
    char file[512] = "", flags[32] = "", dl[3];
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
    if (FAILED(hr) || gk3w_wmi_u64(out, L"SizeMin", &min) || min > size) {
        char why[96];
        snprintf(why, sizeof why, "the shrink analysis (GetSupportedSize) failed (0x%08lx)", (unsigned long)hr);
        gk3w_err(GK3W_TOUCHED_NA, "shrink-query", why, 0);
        goto out;
    }
    gk3w_log("shrink: analysis of %c: took %.1f s", (char)drive, (double)(GetTickCount64() - t0) / 1000.0);
    /* 事件在分析结束前写下；往前多留 10 秒，防止日志与本机计时的细小偏差 */
    if (last_unmovable(guid, GetTickCount64() - t0 + 10000, file, sizeof file, flags, sizeof flags))
        gk3w_log("shrink: no event 259 for this volume since the analysis started");

    dl[0] = (char)drive;
    dl[1] = ':';
    dl[2] = 0;
    gk3w_line_begin(&l, "SHRINK");
    gk3w_line_kv(&l, "drive", dl);
    gk3w_line_u64(&l, "size_mib", size >> 20);
    gk3w_line_u64(&l, "min_mib", (min + (1u << 20) - 1) >> 20);
    gk3w_line_u64(&l, "max_shrink_mib", (size - min) >> 20);
    if (file[0]) {
        gk3w_line_kv(&l, "file", file);
        if (flags[0])
            gk3w_line_kv(&l, "flags", flags);
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
