/* gk3l（Linux 版后端，LiveCD 用）共用的参数、报错、sysfs 与外部命令工具。 */
#ifndef GK3L_LX_H
#define GK3L_LX_H

#include <stddef.h>
#include <stdint.h>

#include "../proto.h"

/* 版本号（main.c，GK3W_VERSION） */
extern const char gk3l_version[];

/* ── 参数 ── */
const char *gk3l_arg(int argc, char **argv, const char *key);
int gk3l_flag(int argc, char **argv, const char *key);
/* 十进制非负整数；没有或格式不对返回 -1 */
int gk3l_arg_u64(int argc, char **argv, const char *key, uint64_t *out);

/* ── 失败：ERR code=<code> [k=v] [errno=] [touched=]，再一行 `!! human`。返回 1（命令的退出码）。
 * err 是 errno 的值，0 表示不带。 ── */
int gk3l_err(gk3w_touched t, const char *code, const char *human, int err);
int gk3l_err_kv(gk3w_touched t, const char *code, const char *k, const char *v, const char *human, int err);
/* 带两个字段（k2 可为 NULL） */
int gk3l_err_kv2(gk3w_touched t, const char *code, const char *k1, const char *v1, const char *k2, const char *v2, const char *human,
                 int err);
/* stderr 上给人看的日志行 */
void gk3l_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ── sysfs 等小文件 ── */
/* 读第一行（去掉行尾空白）；读不到返回 -1，out 为空串 */
int gk3l_read_line(const char *path, char *out, size_t cap);
/* 读一个十进制整数；读不到或格式不对返回 -1 */
int gk3l_read_u64(const char *path, uint64_t *out);

/* ── 外部命令 ── */
/* 运行 argv[0]（按 PATH 找），stdin 接 /dev/null，stdout 收进 out（最多 cap-1 字节，以 NUL 结尾，多出的丢弃）；
 * out 为 NULL 时子进程的 stdout 并进我们的 stderr（进安装日志，不混进协议记录）。子进程的 stderr 照原样进我们的 stderr。
 * 返回退出码；没能运行（找不到、fork 失败）返回 -1，被信号结束返回 128+信号。 */
int gk3l_run(char *const argv[], char *out, size_t cap);
/* 同上，但子进程的 stdout 与 stderr 都丢掉（只要退出码的试探） */
int gk3l_run_quiet(char *const argv[]);

/* ── 盘 ── */
/* 设备（"主设备号:次设备号"）挂在哪里；没挂返回 -1 */
int gk3l_mount_of(const char *devno, char *mnt, size_t cap);
/* 反过来：dir 上挂的是哪个设备（同一处挂了几层时取最上层）；dir 不是挂载点返回 -1 */
int gk3l_devno_at(const char *dir, char *out, size_t cap);

/* ── 安装介质（medium.c）──
 * initramfs-init 把介质记在 /run/gaokun3/medium（测试用 GK3_MEDIUM_FILE 换），每行一个 键=值：
 *   part=nvme0n1p8 disk=nvme0n1 start=… size=…（sysfs 的 512 字节单位） ram=yes|no
 * ram=yes：从内置盘启动，gaokun3/ 已读进内存，/media/gk3 是 tmpfs 上的副本，介质分区本身没有挂载（安装器分区，
 * 可以整盘清空时保留、装完删掉）。没有记录（U 盘启动的旧 initramfs、测试）时按 /media/gk3 上挂的设备认，
 * 测试可用 GK3_MEDIUM_DIR 换挂载点。
 * LiveCD 不自动往介质写任何东西（介质可能是只读的 ISO9660）：分区表备份与任务日志只放在内存（/run）里，
 * 需要时由用户另存（save-logs，logs.c；用户选了介质本身时才写它）。 */
/* 介质分区现在的 "主设备号:次设备号"（例如 "259:3"）；不是从安装介质启动的、或者介质分区已经删掉了，返回 -1 */
int gk3l_medium_devno(char *out, size_t cap);
/* 介质读进了内存 */
int gk3l_medium_ram(void);

/* ── 写盘之前的把关（sys.c）：机型（GK3_SKIP_MODEL_CHECK=1 放行，读不到只提醒）与电量。拒绝时已报 ERR（touched=no），返回 -1 ── */
int gk3l_sys_gate(void);

/* ── 命令 ── */
int gk3l_cmd_apply(int argc, char **argv);
int gk3l_cmd_delete(int argc, char **argv);
int gk3l_cmd_restore_table(int argc, char **argv);
int gk3l_cmd_esp_info(int argc, char **argv);
int gk3l_cmd_release_info(int argc, char **argv);
int gk3l_cmd_job(int argc, char **argv);
int gk3l_cmd_remove_installer(int argc, char **argv);
int gk3l_cmd_log_targets(int argc, char **argv);
int gk3l_cmd_save_logs(int argc, char **argv);
/* 按 argv[0] 的命令名分派（main 与 job body 共用）；返回命令的退出码 */
int gk3l_dispatch(int argc, char **argv);
int gk3l_cmd_probe(int argc, char **argv);
int gk3l_cmd_preflight(int argc, char **argv);
int gk3l_cmd_plan(int argc, char **argv);

#endif
