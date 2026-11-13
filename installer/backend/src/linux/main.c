/* gk3l：gaokun3 安装器的 Linux 后端（LiveCD 用），与 Windows 后端 gk3w 共用分区方案、行协议与 libgk3core。
 *
 *   gk3l probe        列出磁盘、分区、空闲区（只读，要 root）
 *   gk3l preflight    安装前检查：机型与电源（只读）
 *   gk3l plan ...     算分区方案（纯计算，不读盘）
 *   gk3l apply ...    装到一段空闲空间（双系统），见 apply.c
 *   gk3l --version
 *
 * 输出走行协议（../proto.h）：记录在 stdout，进度与错误在 stderr，都是 UTF-8。 */
#include <signal.h>
#include <stdio.h>
#include <string.h>

#include "../proto.h"
#include "lx.h"

/* 版本号：构建时传入（build.sh 的 GK3W_VERSION），本地构建是 dev */
#ifndef GK3W_VERSION
#define GK3W_VERSION "dev"
#endif

const char gk3l_version[] = GK3W_VERSION;

int main(int argc, char **argv)
{
    static const struct {
        const char *name;
        int (*fn)(int, char **);
    } cmds[] = {
        {"probe", gk3l_cmd_probe},
        {"preflight", gk3l_cmd_preflight},
        {"plan", gk3l_cmd_plan},
        {"apply", gk3l_cmd_apply},
    };

    /* 记录行逐行 fflush（gk3w_line_emit）；前端关掉管道时按写失败处理，而不是被 SIGPIPE 直接结束 */
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);

    if (argc < 2) {
        fprintf(stderr, "!! usage: gk3l probe|preflight|plan|apply|--version\n");
        return 2;
    }
    if (!strcmp(argv[1], "--version")) {
        printf("gk3l %s\n", gk3l_version);
        return fflush(stdout) == 0 ? 0 : 1;
    }
    for (size_t i = 0; i < sizeof cmds / sizeof cmds[0]; i++)
        if (!strcmp(argv[1], cmds[i].name))
            return cmds[i].fn(argc - 2, argv + 2);

    {
        gk3w_line l = {0};
        gk3w_line_begin(&l, "ERR");
        gk3w_line_kv(&l, "code", "usage");
        gk3w_line_kv(&l, "cmd", argv[1]);
        gk3w_fail(&l, GK3W_TOUCHED_NA, "unknown command");
        gk3w_line_free(&l);
    }
    return 127;
}
