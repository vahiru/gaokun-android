/* BitLocker 恢复密钥（48 位数字密码）的格式与比较，与平台无关（主机单测覆盖）。
 *
 * 恢复密钥分 8 组、每组 6 位：每组是 11 的倍数，除以 11 之后小于 65536（每组编码密钥的 16 位，乘 11 作校验），
 * 所以多数输错一位的情况在比对之前就能发现。用户输入里的空格与连字符忽略（Microsoft 账户页面与打印件按
 * "123456-123456-…" 分组显示，复制时可能带着空格）。 */
#ifndef GK3W_BLKEY_H
#define GK3W_BLKEY_H

#include <stddef.h>

#define GK3W_BLKEY_DIGITS 48
#define GK3W_BLKEY_GROUPS 8
/* 规整后的文本 "dddddd-…-dddddd" 加 NUL */
#define GK3W_BLKEY_TEXT (GK3W_BLKEY_DIGITS + GK3W_BLKEY_GROUPS - 1 + 1)

/* 把 in 规整成 "dddddd-dddddd-…"，写进 out。
 * 返回 0：格式正确；-1：不是 48 位数字（位数不对，或有空格、连字符以外的字符）；
 * 1–8：位数对，但这一组（从 1 数）校验不过。失败时 out 被清空。 */
int gk3w_blkey_normalize(const char *in, char out[GK3W_BLKEY_TEXT]);

/* 两个恢复密钥是不是同一个（都先规整；任一个格式不对就算不同） */
int gk3w_blkey_equal(const char *a, const char *b);

/* 清除内存里的密钥（编译器不会把它当作多余的写入省掉） */
void gk3w_wipe(void *p, size_t n);

#endif
