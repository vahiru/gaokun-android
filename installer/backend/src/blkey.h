/* BitLocker 恢复密钥（48 位数字密码）的格式与比较、系统卷 BitLocker 状态的判断，与平台无关（主机单测覆盖）。
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

/* 系统卷 BitLocker 的状态（gk3w_sys.bitlocker） */
#define GK3W_BL_STATE_OFF 0         /* 没有加密 */
#define GK3W_BL_STATE_ON 1          /* 保护已开启（或 WMI 读不到、而卷上有 BitLocker 签名） */
#define GK3W_BL_STATE_UNKNOWN 2     /* ProtectionStatus 2，或保护关着、转换状态读不到而卷上有签名 */
#define GK3W_BL_STATE_SUSPENDED 3   /* 已完全加密，保护已暂停或尚未激活：Windows 下次启动不会要求输入恢复密钥 */
#define GK3W_BL_STATE_CONVERTING 4  /* 正在加密或解密（含已暂停）：此时保护状态为 0，但 TPM 保护器可能在用，不改动启动链 */
#define GK3W_BL_STATE_UNREADABLE (-1)

/* 由 WMI 的 ProtectionStatus（0 关、1 开、2 未知，读不到 -1）、ConversionStatus（0–5，读不到 -1）与卷上的
 * BitLocker 签名（1 有、0 没有，没读或读不到 -1）得出状态。签名只在前两样不足以判断时需要：ProtectionStatus
 * 读不到，或保护关着而 ConversionStatus 读不到 */
int gk3w_bl_state(int protection, int conversion, int fve);

/* 清除内存里的密钥（编译器不会把它当作多余的写入省掉） */
void gk3w_wipe(void *p, size_t n);

#endif
