/* Windows：用户在"磁盘管理"里压缩一个分区时，压缩停在哪个文件上 ——"可用未分配空间"不够时给用户的指引。只读。
 *
 *   gk3w shrink-info [--drive X]
 *
 * 不自己做压缩分析，只解读用户刚做过的那次：在"磁盘管理"里对一个卷点"压缩卷"时，Windows 先分析这个卷最多能压缩
 * 多少（对话框里的"可用压缩空间大小"），并在"应用程序"日志里记一条 Microsoft-Windows-Defrag 的 259 号事件，写着
 * 最后一个不可移动的文件（压缩就停在它那里）。取本次开机以来最新的、卷在系统盘上的 NTFS 卷的那条（给了 --drive
 * 时只看这个卷的）：用户在"磁盘管理"里换一个分区试过之后，再读就跟着换过去；只看本次开机，是因为不少处理方法
 * 要重启，重启之前的记录已经不能说明现在的情况。
 *
 * 事件数据依次是：卷名、卷的 GUID 路径、文件、文件的最后一个簇、压缩的目标位置（按已用空间算出的、最多能缩到的
 * 位置）、NTFS 文件标志、阶段（簇号都是"0x…"的十六进制）。阶段只写进日志、不用来筛选：见到过的都是"<analysis>"，
 * 但不知道别的语言的 Windows 会不会翻译它。
 *
 * 输出：
 *   SHRINK drive=X: record= time=HH:MM free_mib= [file= flags= limit=file|space]
 *     record：事件的记录号（界面据此认出又读到了同一条：用户处理之后还没重新"压缩卷"）；time：事件的本地时间；
 *     limit：文件的末尾（最后一个簇 + 1，乘簇大小）在目标位置之后、或离它不到 64 MiB 时是 file（压缩停在这个文件上），
 *       否则是 space（最多能缩到哪里由已用空间决定，挪开这个文件也没用）。簇号读不出时不带 limit；
 *   SHRINK event=none：本次开机以来没有这样的事件（用户还没在"磁盘管理"里点过"压缩卷"）。
 * 读不了事件日志时报 ERR code=shrink-query。 */
#include <windows.h>
#include <winevt.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#include "../proto.h"
#include "disk.h"
#include "win.h"

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

/* 可以解读的卷：固定磁盘上的 NTFS 卷，只在系统盘（sysdisk）上 */
static int on_system_disk(wchar_t drive, long sysdisk)
{
    wchar_t root[4], fs[MAX_PATH + 1];
    _snwprintf(root, 4, L"%lc:\\", drive);
    return sysdisk >= 0 && GetDriveTypeW(root) == DRIVE_FIXED &&
           GetVolumeInformationW(root, NULL, 0, NULL, NULL, NULL, fs, MAX_PATH + 1) && !_wcsicmp(fs, L"NTFS") &&
           gk3w_drive_disk_number(drive) == sysdisk;
}

/* 卷的 GUID 路径（事件里的，不带结尾的反斜杠）换成盘符：只认"X:\"这样的根，挂在文件夹上的路径不算；没有返回 0 */
static wchar_t guid_drive(const wchar_t *g)
{
    wchar_t name[64], paths[512];
    DWORD plen = 0;
    size_t n = guid_len(g);
    if (n + 2 > 64)
        return 0;
    wmemcpy(name, g, n);
    name[n] = L'\\';
    name[n + 1] = 0;
    if (!GetVolumePathNamesForVolumeNameW(name, paths, 512, &plen))
        return 0;
    /* 多个路径以 \0 分隔、\0\0 结尾 */
    for (const wchar_t *p = paths; *p; p += wcslen(p) + 1)
        if (iswalpha(p[0]) && p[1] == L':' && p[2] == L'\\' && !p[3])
            return towupper(p[0]);
    return 0;
}

typedef struct {
    wchar_t drive;              /* 卷的盘符 */
    uint64_t record;            /* 事件的记录号 */
    char time[8];               /* 事件的本地时间 HH:MM；读不出为空 */
    char file[2048], flags[32];
    int have_lcn;               /* last 与 target 都读出来了 */
    uint64_t last;              /* 文件的最后一个簇（LCN） */
    uint64_t target;            /* 压缩的目标位置（LCN） */
} unmovable;

/* 事件的记录号与本地时间（系统部分的属性） */
static void event_system(EVT_HANDLE sys, EVT_HANDLE ev, unmovable *u)
{
    DWORD used = 0, count = 0;
    PEVT_VARIANT v = NULL;
    if (!sys || EvtRender(sys, ev, EvtRenderEventValues, 0, NULL, &used, &count) || GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
        !(v = malloc(used)) || !EvtRender(sys, ev, EvtRenderEventValues, used, v, &used, &count)) {
        free(v);
        return;
    }
    if (count > EvtSystemEventRecordId && v[EvtSystemEventRecordId].Type == EvtVarTypeUInt64)
        u->record = v[EvtSystemEventRecordId].UInt64Val;
    if (count > EvtSystemTimeCreated && v[EvtSystemTimeCreated].Type == EvtVarTypeFileTime) {
        FILETIME ft, lt;
        SYSTEMTIME st;
        ft.dwLowDateTime = (DWORD)v[EvtSystemTimeCreated].FileTimeVal;
        ft.dwHighDateTime = (DWORD)(v[EvtSystemTimeCreated].FileTimeVal >> 32);
        if (FileTimeToLocalFileTime(&ft, &lt) && FileTimeToSystemTime(&lt, &st))
            snprintf(u->time, sizeof u->time, "%02u:%02u", (unsigned)st.wHour, (unsigned)st.wMinute);
    }
    free(v);
}

/* 本次开机以来最新的一条可以解读的 259 号事件（only 不为 0 时只看这个盘符的卷）。找到返回 0，没有返回 1，
 * 读不了日志返回 -1 */
static int newest_event(long sysdisk, wchar_t only, unmovable *u)
{
    wchar_t q[256];
    EVT_HANDLE res, ctx, sys, ev[16];
    DWORD got = 0, why = ERROR_NO_MORE_ITEMS;
    int found = 1;

    /* 系统启动以来的毫秒数（含睡眠）：本次开机之后的事件；往前多留 10 秒，防止日志与本机计时的细小偏差 */
    _snwprintf(q, 256,
               L"*[System[Provider[@Name='Microsoft-Windows-Defrag'] and (EventID=259) and TimeCreated[timediff(@SystemTime) <= %llu]]]",
               (unsigned long long)GetTickCount64() + 10000);
    res = EvtQuery(NULL, L"Application", q, EvtQueryChannelPath | EvtQueryReverseDirection);
    if (!res) {
        gk3w_log("shrink: cannot query the Application event log (%lu)", (unsigned long)GetLastError());
        return -1;
    }
    ctx = EvtCreateRenderContext(0, NULL, EvtRenderContextUser);
    sys = EvtCreateRenderContext(0, NULL, EvtRenderContextSystem);
    if (!ctx) {
        gk3w_log("shrink: cannot create an event render context (%lu)", (unsigned long)GetLastError());
        found = -1;
    }
    while (found > 0) {
        if (!EvtNext(res, 16, ev, 5000, 0, &got)) {
            why = GetLastError();
            break;
        }
        for (DWORD i = 0; i < got; i++) {
            DWORD used = 0, count = 0;
            PEVT_VARIANT v = NULL;
            wchar_t d;
            if (found > 0 && !EvtRender(ctx, ev[i], EvtRenderEventValues, 0, NULL, &used, &count) &&
                GetLastError() == ERROR_INSUFFICIENT_BUFFER && (v = malloc(used)) &&
                EvtRender(ctx, ev[i], EvtRenderEventValues, used, v, &used, &count) && count >= 6 &&
                v[1].Type == EvtVarTypeString && v[2].Type == EvtVarTypeString && v[1].StringVal && v[2].StringVal &&
                (d = guid_drive(v[1].StringVal)) && (!only || d == only) && on_system_disk(d, sysdisk)) {
                memset(u, 0, sizeof *u);
                u->drive = d;
                gk3w_w2u(v[2].StringVal, u->file, sizeof u->file);
                if (!u->file[0])
                    gk3w_log("shrink: the file name in event 259 could not be converted");
                if (v[5].Type == EvtVarTypeString && v[5].StringVal)
                    gk3w_w2u(v[5].StringVal, u->flags, sizeof u->flags);
                u->have_lcn = !hex_lcn(&v[3], &u->last) && !hex_lcn(&v[4], &u->target);
                if (count > 6 && v[6].Type == EvtVarTypeString && v[6].StringVal) {
                    char phase[64];
                    gk3w_w2u(v[6].StringVal, phase, sizeof phase);
                    gk3w_log("shrink: event 259 phase %s", phase);
                }
                event_system(sys, ev[i], u);
                found = 0;
            }
            free(v);
            EvtClose(ev[i]);
        }
    }
    /* 没读完就出错（不是"没有更多"，例如超时）：不能当作没有事件 */
    if (found > 0 && why != ERROR_NO_MORE_ITEMS) {
        gk3w_log("shrink: reading the Application event log failed (%lu)", (unsigned long)why);
        found = -1;
    }
    if (sys)
        EvtClose(sys);
    if (ctx)
        EvtClose(ctx);
    EvtClose(res);
    return found;
}

int gk3w_cmd_shrink_info(int argc, wchar_t **argv)
{
    const wchar_t *da = gk3w_arg(argc, argv, L"--drive");
    wchar_t root[4];
    long sysdisk;
    unmovable u;
    ULARGE_INTEGER freeb;
    DWORD spc, bps, fc, tc;
    int got;
    char dl[3];
    gk3w_line l = {0};

    if (da && (!iswalpha(da[0]) || (da[1] && (da[1] != L':' || da[2]))))
        return gk3w_err(GK3W_TOUCHED_NA, "usage", "usage: shrink-info [--drive X]", 0);
    sysdisk = gk3w_system_disk_number();
    if (sysdisk < 0)
        return gk3w_err(GK3W_TOUCHED_NA, "shrink-query", "cannot find the disk that holds Windows", 0);
    got = newest_event(sysdisk, da ? towupper(da[0]) : 0, &u);
    if (got < 0)
        return gk3w_err(GK3W_TOUCHED_NA, "shrink-query", "cannot read the Application event log", 0);
    if (got) {
        gk3w_log("shrink: no event 259 for a volume on the system disk since this boot");
        gk3w_line_begin(&l, "SHRINK");
        gk3w_line_kv(&l, "event", "none");
        (void)gk3w_line_emit(&l, stdout);
        gk3w_line_free(&l);
        return 0;
    }
    gk3w_log("shrink: newest event 259 since this boot is for %c: (%s)", (char)u.drive, u.file[0] ? u.file : "no file");

    _snwprintf(root, 4, L"%lc:\\", u.drive);
    if (!GetDiskFreeSpaceExW(root, &freeb, NULL, NULL))
        freeb.QuadPart = 0;
    if (!GetDiskFreeSpaceW(root, &spc, &bps, &fc, &tc))
        spc = bps = 0;

    dl[0] = (char)u.drive;
    dl[1] = ':';
    dl[2] = 0;
    gk3w_line_begin(&l, "SHRINK");
    gk3w_line_kv(&l, "drive", dl);
    gk3w_line_u64(&l, "record", u.record);
    gk3w_line_kv(&l, "time", u.time);
    gk3w_line_u64(&l, "free_mib", freeb.QuadPart >> 20);
    if (u.file[0]) {
        gk3w_line_kv(&l, "file", u.file);
        if (u.flags[0])
            gk3w_line_kv(&l, "flags", u.flags);
        if (u.have_lcn && spc && bps) {
            uint64_t cs = (uint64_t)spc * bps, end = (u.last + 1) * cs, target = u.target * cs;
            gk3w_line_kv(&l, "limit", end + (64ull << 20) >= target ? "file" : "space");
        }
    }
    (void)gk3w_line_emit(&l, stdout);
    gk3w_line_free(&l);
    return 0;
}
