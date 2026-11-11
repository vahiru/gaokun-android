/* gk3w：gaokun3 安装器的 Windows 后端（原型）。
 *
 *   gk3w probe        列出磁盘、分区、空闲区（只读，要管理员权限）
 *   gk3w mkpart|rmpart|write|esp-ls|esp-put   阶段 1 的写盘验证命令，只对 VHD 动手（见 write.c）
 *   gk3w --version
 *
 * 输出走行协议（../proto.h）：记录在 stdout，进度与错误在 stderr，都是 UTF-8。 */
#include <windows.h>

#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <wchar.h>

#include "../proto.h"
#include "win.h"

#ifndef GK3W_VERSION
#define GK3W_VERSION "0.0.0-proto"
#endif

void gk3w_w2u(const wchar_t *w, char *out, size_t cap)
{
    int n;
    if (!cap)
        return;
    n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)(cap > 0x7fffffff ? 0x7fffffff : cap), NULL, NULL);
    if (n <= 0)
        out[0] = 0;
}

void gk3w_guid_upper(const GUID *g, char *out)
{
    snprintf(out, 37, "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X", (unsigned long)g->Data1, g->Data2,
             g->Data3, g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6],
             g->Data4[7]);
}

int wmain(int argc, wchar_t **argv)
{
    /* 记录行里不能出现 \r\n：stdout / stderr 切成二进制模式 */
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    SetConsoleOutputCP(CP_UTF8);

    if (argc < 2) {
        fprintf(stderr, "!! 用法：gk3w probe|mkpart|rmpart|write|esp-ls|esp-put|--version\n");
        return 2;
    }
    if (!wcscmp(argv[1], L"--version")) {
        printf("gk3w %s\n", GK3W_VERSION);
        return fflush(stdout) == 0 ? 0 : 1;
    }
    {
        static const struct {
            const wchar_t *name;
            int (*fn)(int, wchar_t **);
        } cmds[] = {
            {L"probe", gk3w_cmd_probe},   {L"mkpart", gk3w_cmd_mkpart}, {L"rmpart", gk3w_cmd_rmpart},
            {L"write", gk3w_cmd_write},   {L"esp-ls", gk3w_cmd_esp_ls}, {L"esp-put", gk3w_cmd_esp_put},
        };
        for (size_t i = 0; i < sizeof cmds / sizeof cmds[0]; i++)
            if (!wcscmp(argv[1], cmds[i].name))
                return cmds[i].fn(argc - 2, argv + 2);
    }

    {
        gk3w_line l = {0};
        char cmd[128];
        gk3w_w2u(argv[1], cmd, sizeof cmd);
        gk3w_line_begin(&l, "ERR");
        gk3w_line_kv(&l, "code", "usage");
        gk3w_line_kv(&l, "cmd", cmd);
        gk3w_fail(&l, GK3W_TOUCHED_NA, "不认识的命令");
        gk3w_line_free(&l);
    }
    return 127;
}
