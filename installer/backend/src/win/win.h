/* Windows 侧的公用小工具与命令入口。 */
#ifndef GK3W_WIN_H
#define GK3W_WIN_H

#include <windows.h>

#include <stddef.h>

/* UTF-16 → UTF-8，结果以 NUL 结尾；转换失败时 out 为空串 */
void gk3w_w2u(const wchar_t *w, char *out, size_t cap);
/* GUID → 大写的 "XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX"（与 sgdisk 的写法一致），out ≥ 37 字节 */
void gk3w_guid_upper(const GUID *g, char *out);

/* GPT 分区类型 */
extern const GUID GK3W_LINUX_DATA;   /* 0FC63DAF-8483-4772-8E79-3D69D8477DE4（sgdisk 8300，Android 的分区都用它） */
extern const GUID GK3W_ESP_TYPE;     /* C12A7328-F81F-11D2-BA4B-00A0C93EC93B */

/* 版本号（main.c，GK3W_VERSION） */
extern const char gk3w_version[];

int gk3w_cmd_probe(int argc, wchar_t **argv);
int gk3w_cmd_apply(int argc, wchar_t **argv);
int gk3w_cmd_dump(int argc, wchar_t **argv);
int gk3w_cmd_esp_tree(int argc, wchar_t **argv);
int gk3w_cmd_readpart(int argc, wchar_t **argv);
int gk3w_cmd_uninstall(int argc, wchar_t **argv);
int gk3w_cmd_media(int argc, wchar_t **argv);
int gk3w_cmd_preflight(int argc, wchar_t **argv);
int gk3w_cmd_plan(int argc, wchar_t **argv);
int gk3w_cmd_hash(int argc, wchar_t **argv);
int gk3w_cmd_space(int argc, wchar_t **argv);
int gk3w_cmd_bitlocker_key(int argc, wchar_t **argv);
int gk3w_cmd_shrink_info(int argc, wchar_t **argv);
int gk3w_cmd_mft_move(int argc, wchar_t **argv);
int gk3w_cmd_mkpart(int argc, wchar_t **argv);
int gk3w_cmd_rmpart(int argc, wchar_t **argv);
int gk3w_cmd_write(int argc, wchar_t **argv);
int gk3w_cmd_esp_ls(int argc, wchar_t **argv);
int gk3w_cmd_esp_put(int argc, wchar_t **argv);

#endif
