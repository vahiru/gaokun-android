/* Windows：算文件的 sha256、查目录所在卷的可用空间（安装器下载用；都不需要管理员）。
 *
 *   gk3w hash FILE...
 *   gk3w space DIR
 *
 * space：一行 SPACE path=<目录> avail=<当前用户可用的字节> total=<卷的字节>（GetDiskFreeSpaceExW）。目录必须已经存在，
 * 否则报 space-failed，所以调用方先建好目录。下载前用它判断放不放得下，免得把 C 盘写满了才失败。
 *
 * 每个文件一行 HASH file=<路径> size=<字节> sha256=<小写 hex>。比对由调用方做（它手上有校验清单）：
 * 安装器只用 Flutter SDK 自带的库，Dart 那边没有 sha256；散列用的是 apply 核对发布文件的同一个函数（CNG）。
 * 有一个读不了就报 ERR code=hash-failed file=…，退出码 1（前面已经算好的照样输出）。 */
#include <windows.h>

#include <stdio.h>

#include "../proto.h"
#include "disk.h"
#include "win.h"

int gk3w_cmd_hash(int argc, wchar_t **argv)
{
    if (argc < 1)
        return gk3w_err(GK3W_TOUCHED_NA, "usage", "usage: hash FILE...", 0);
    for (int i = 0; i < argc; i++) {
        char name[MAX_PATH * 3], hex[65];
        uint64_t size = 0;
        gk3w_line l = {0};
        gk3w_w2u(argv[i], name, sizeof name);
        if (gk3w_file_sha256(argv[i], hex, &size))
            return gk3w_err_kv(GK3W_TOUCHED_NA, "hash-failed", "file", name, "cannot read the file to compute its sha256", GetLastError());
        gk3w_line_begin(&l, "HASH");
        gk3w_line_kv(&l, "file", name);
        gk3w_line_u64(&l, "size", size);
        gk3w_line_kv(&l, "sha256", hex);
        gk3w_line_emit(&l, stdout);
        gk3w_line_free(&l);
    }
    return fflush(stdout) == 0 ? 0 : 1;
}

int gk3w_cmd_space(int argc, wchar_t **argv)
{
    ULARGE_INTEGER avail, total, freeb;
    char name[MAX_PATH * 3];
    gk3w_line l = {0};
    if (argc != 1)
        return gk3w_err(GK3W_TOUCHED_NA, "usage", "usage: space DIR", 0);
    gk3w_w2u(argv[0], name, sizeof name);
    if (!GetDiskFreeSpaceExW(argv[0], &avail, &total, &freeb))
        return gk3w_err_kv(GK3W_TOUCHED_NA, "space-failed", "path", name, "cannot query the free space of the volume", GetLastError());
    gk3w_line_begin(&l, "SPACE");
    gk3w_line_kv(&l, "path", name);
    gk3w_line_u64(&l, "avail", avail.QuadPart);
    gk3w_line_u64(&l, "total", total.QuadPart);
    gk3w_line_emit(&l, stdout);
    gk3w_line_free(&l);
    return fflush(stdout) == 0 ? 0 : 1;
}
