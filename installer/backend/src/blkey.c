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

void gk3w_wipe(void *p, size_t n)
{
    volatile unsigned char *v = p;
    while (n--)
        *v++ = 0;
}
