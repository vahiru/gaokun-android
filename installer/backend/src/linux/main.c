/* gk3l：gaokun3 安装器的 Linux 后端（LiveCD 用），与 Windows 后端 gk3w 共用分区方案、行协议与 libgk3core。
 *
 *   gk3l probe        列出磁盘、分区、空闲区（只读，要 root）
 *   gk3l preflight    安装前检查：机型与电源（只读）
 *   gk3l plan ...     算分区方案（纯计算，不读盘）
 *   gk3l apply ...    安装：双系统 / 整盘清空 / 重新安装（见 apply.c）
 *   gk3l delete ...   删除一个分区；gk3l restore-table --file …  把动盘之前的分区表备份写回去（见 edit.c）
 *   gk3l esp-info ... 这块盘上 ESP 的状况；gk3l release-info [--dir DIR] 安装介质上的系统文件（只读，见 info.c）
 *   gk3l job ...      写盘进程脱离界面运行（见 job.c）
 *   gk3l log-targets / save-logs   把日志另存到用户拿得到的地方（见 logs.c）
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

static const struct {
    const char *name;
    int (*fn)(int, char **);
} cmds[] = {
    {"probe", gk3l_cmd_probe},
    {"preflight", gk3l_cmd_preflight},
    {"plan", gk3l_cmd_plan},
    {"apply", gk3l_cmd_apply},
    {"delete", gk3l_cmd_delete},
    {"restore-table", gk3l_cmd_restore_table},
    {"esp-info", gk3l_cmd_esp_info},
    {"release-info", gk3l_cmd_release_info},
    {"job", gk3l_cmd_job},
    {"log-targets", gk3l_cmd_log_targets},
    {"save-logs", gk3l_cmd_save_logs},
};

int gk3l_dispatch(int argc, char **argv)
{
    if (argc < 1)
        return 2;
    for (size_t i = 0; i < sizeof cmds / sizeof cmds[0]; i++)
        if (!strcmp(argv[0], cmds[i].name))
            return cmds[i].fn(argc - 1, argv + 1);
    {
        gk3w_line l = {0};
        gk3w_line_begin(&l, "ERR");
        gk3w_line_kv(&l, "code", "usage");
        gk3w_line_kv(&l, "cmd", argv[0]);
        gk3w_fail(&l, GK3W_TOUCHED_NA, "unknown command");
        gk3w_line_free(&l);
    }
    return 127;
}

int main(int argc, char **argv)
{
    /* 记录行逐行 fflush（gk3w_line_emit）；前端关掉管道时按写失败处理，而不是被 SIGPIPE 直接结束 */
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);

    if (argc < 2) {
        fprintf(stderr, "!! usage: gk3l probe|preflight|plan|apply|delete|restore-table|esp-info|release-info|job|log-targets|save-logs|--version\n");
        return 2;
    }
    if (!strcmp(argv[1], "--version")) {
        printf("gk3l %s\n", gk3l_version);
        return fflush(stdout) == 0 ? 0 : 1;
    }
    return gk3l_dispatch(argc - 1, argv + 1);
}
