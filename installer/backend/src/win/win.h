/* Windows 侧的公用小工具与命令入口。 */
#ifndef GK3W_WIN_H
#define GK3W_WIN_H

#include <windows.h>

#include <stddef.h>

/* UTF-16 → UTF-8，结果以 NUL 结尾；转换失败时 out 为空串 */
void gk3w_w2u(const wchar_t *w, char *out, size_t cap);
/* GUID → 大写的 "XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX"（与 sgdisk 的写法一致），out ≥ 37 字节 */
void gk3w_guid_upper(const GUID *g, char *out);

int gk3w_cmd_probe(int argc, wchar_t **argv);
int gk3w_cmd_mkpart(int argc, wchar_t **argv);
int gk3w_cmd_rmpart(int argc, wchar_t **argv);
int gk3w_cmd_write(int argc, wchar_t **argv);
int gk3w_cmd_esp_ls(int argc, wchar_t **argv);
int gk3w_cmd_esp_put(int argc, wchar_t **argv);

#endif
