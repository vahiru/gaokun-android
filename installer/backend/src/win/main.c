/* gk3w：gaokun3 安装器的 Windows 后端（原型）。
 *
 *   gk3w probe        列出磁盘、分区、空闲区（只读，要管理员权限）
 *   gk3w apply ...    装到一段空闲空间（双系统），对应 gk3_apply --mode alongside（见 apply.c；原型只对 VHD 动手）
 *   gk3w uninstall ...  卸载 apply 装上去的那一套，留下空闲空间（见 uninstall.c）
 *   gk3w preflight      安装前检查要看的系统状态（只读，见 sys.c）
 *   gk3w bitlocker-key  校验标准输入上的 BitLocker 恢复密钥，可选暂停保护（见 bitlocker.c）
 *   gk3w shrink-info    系统卷最多还能压缩出多少、被哪个文件挡住（只读，见 shrink.c）
 *   gk3w mft-move       把 NTFS 主文件表挪到卷的前部，让 C 盘能压缩得更多（默认只出计划，见 mftmove.c）
 *   gk3w dump|esp-tree|readpart  把装好的盘摘要成文本或导出小分区，与 LiveCD 的结果对拍（只读，见 dump.c）
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
    /* 标准输出不缓冲：记录行本来就逐行 fflush（gk3w_line_emit），这里管住其余直接 printf 的输出，
     * 进程被中途结束时也不会丢。Windows 的 C 运行库里 _IOLBF 等同于全缓冲，所以用 _IONBF */
    setvbuf(stdout, NULL, _IONBF, 0);
    /* 记录行里不能出现 \r\n：stdout / stderr 切成二进制模式 */
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    SetConsoleOutputCP(CP_UTF8);

    if (argc < 2) {
        fprintf(stderr, "!! usage: gk3w probe|preflight|bitlocker-key|shrink-info|mft-move|plan|hash|space|apply|uninstall|dump|esp-tree|readpart|mkpart|rmpart|write|esp-ls|esp-put|--version\n");
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
            {L"apply", gk3w_cmd_apply},   {L"dump", gk3w_cmd_dump},     {L"esp-tree", gk3w_cmd_esp_tree},
            {L"readpart", gk3w_cmd_readpart}, {L"uninstall", gk3w_cmd_uninstall}, {L"preflight", gk3w_cmd_preflight}, {L"plan", gk3w_cmd_plan},
            {L"hash", gk3w_cmd_hash}, {L"space", gk3w_cmd_space}, {L"bitlocker-key", gk3w_cmd_bitlocker_key}, {L"shrink-info", gk3w_cmd_shrink_info}, {L"mft-move", gk3w_cmd_mft_move},
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
        gk3w_fail(&l, GK3W_TOUCHED_NA, "unknown command");
        gk3w_line_free(&l);
    }
    return 127;
}
