#include "blkey.h"

#include <string.h>

int gk3w_blkey_normalize(const char *in, char out[GK3W_BLKEY_TEXT])
{
    char digits[GK3W_BLKEY_DIGITS];
    size_t n = 0;
    int bad = 0;
    out[0] = 0;
    for (const char *p = in; *p; p++) {
        if (*p == ' ' || *p == '-')
            continue;
        if (*p < '0' || *p > '9' || n == GK3W_BLKEY_DIGITS) {
            gk3w_wipe(digits, sizeof digits);
            return -1;
        }
        digits[n++] = *p;
    }
    if (n != GK3W_BLKEY_DIGITS) {
        gk3w_wipe(digits, sizeof digits);
        return -1;
    }
    for (int g = 0; g < GK3W_BLKEY_GROUPS && !bad; g++) {
        unsigned long v = 0;
        for (int i = 0; i < 6; i++)
            v = v * 10 + (unsigned long)(digits[g * 6 + i] - '0');
        if (v % 11 || v / 11 > 0xffff)
            bad = g + 1;
    }
    if (!bad) {
        char *o = out;
        for (int g = 0; g < GK3W_BLKEY_GROUPS; g++) {
            if (g)
                *o++ = '-';
            memcpy(o, digits + g * 6, 6);
            o += 6;
        }
        *o = 0;
    }
    gk3w_wipe(digits, sizeof digits);
    return bad;
}

int gk3w_blkey_equal(const char *a, const char *b)
{
    char na[GK3W_BLKEY_TEXT], nb[GK3W_BLKEY_TEXT];
    int eq = !gk3w_blkey_normalize(a, na) && !gk3w_blkey_normalize(b, nb) && !strcmp(na, nb);
    gk3w_wipe(na, sizeof na);
    gk3w_wipe(nb, sizeof nb);
    return eq;
}

int gk3w_bl_state(int protection, int conversion, int fve)
{
    /* WMI 读不到：按盘上签名判断（有签名当作保护开着）；签名也读不到就是读不到，由 gk3w_sys_gate 当作开着处理 */
    if (protection < 0)
        return fve < 0 ? GK3W_BL_STATE_UNREADABLE : fve ? GK3W_BL_STATE_ON : GK3W_BL_STATE_OFF;
    /* 正在加密或解密：保护状态这时是 0，但 TPM 保护器可能在用（例如登录 Microsoft 账户之后的设备加密），
     * 改了启动链 Windows 就会要求输入恢复密钥，而这时可能还没有可以校验的恢复密钥 —— 等转换完成 */
    if (conversion >= 2 && conversion <= 5)
        return GK3W_BL_STATE_CONVERTING;
    if (protection == 0) {
        /* 保护关着而卷已完全加密：暂停了保护，或设备加密尚未激活。转换状态读不到而卷上有签名：
         * 不能排除正在加密，当作状态不明（要恢复密钥） */
        if (conversion == 1)
            return GK3W_BL_STATE_SUSPENDED;
        if (conversion < 0 && fve == 1)
            return GK3W_BL_STATE_UNKNOWN;
        return GK3W_BL_STATE_OFF;
    }
    return protection == 1 ? GK3W_BL_STATE_ON : GK3W_BL_STATE_UNKNOWN;
}

void gk3w_wipe(void *p, size_t n)
{
    volatile unsigned char *v = p;
    while (n--)
        *v++ = 0;
}
