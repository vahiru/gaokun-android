/* 行协议的输出端（说明见 proto.h）。 */
#include "proto.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

static int must_encode(unsigned char c)
{
    /* ≥0x80 原样透过：UTF-8 的中文卷标、分区名不编码，与 shell 版一致 */
    return c == '%' || c == ' ' || c < 0x20 || c == 0x7f;
}

size_t gk3w_enc(const char *v, char *out, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)v; *p; p++) {
        char tmp[3];
        size_t k = 1;
        tmp[0] = (char)*p;
        if (must_encode(*p)) {
            tmp[0] = '%';
            tmp[1] = hex[*p >> 4];
            tmp[2] = hex[*p & 15];
            k = 3;
        }
        for (size_t i = 0; i < k; i++, n++)
            if (cap && n < cap - 1)
                out[n] = tmp[i];
    }
    if (cap)
        out[n < cap - 1 ? n : cap - 1] = 0;
    return n;
}

static void put(gk3w_line *l, const char *s, size_t n)
{
    if (l->oom)
        return;
    if (n > SIZE_MAX - l->len - 1) {
        l->oom = 1;
        return;
    }
    if (l->len + n + 1 > l->cap) {
        size_t cap = l->cap ? l->cap : 256;
        while (cap < l->len + n + 1) {
            if (cap > SIZE_MAX / 2) {
                l->oom = 1;
                return;
            }
            cap *= 2;
        }
        char *b = realloc(l->buf, cap);
        if (!b) {
            l->oom = 1;
            return;
        }
        l->buf = b;
        l->cap = cap;
    }
    memcpy(l->buf + l->len, s, n);
    l->len += n;
    l->buf[l->len] = 0;
}

void gk3w_line_begin(gk3w_line *l, const char *type)
{
    l->len = 0;
    l->oom = 0;
    put(l, type, strlen(type));
}

void gk3w_line_kv(gk3w_line *l, const char *k, const char *v)
{
    size_t need = gk3w_enc(v, NULL, 0);
    char stackbuf[256];
    char *e = need < sizeof stackbuf ? stackbuf : malloc(need + 1);
    if (!e) {
        l->oom = 1;
        return;
    }
    gk3w_enc(v, e, need + 1);
    put(l, " ", 1);
    put(l, k, strlen(k));
    put(l, "=", 1);
    put(l, e, need);
    if (e != stackbuf)
        free(e);
}

void gk3w_line_u64(gk3w_line *l, const char *k, uint64_t v)
{
    char b[24];
    snprintf(b, sizeof b, "%" PRIu64, v);
    gk3w_line_kv(l, k, b);
}

void gk3w_line_hex64(gk3w_line *l, const char *k, uint64_t v)
{
    char b[24];
    snprintf(b, sizeof b, "0x%016" PRIX64, v);
    gk3w_line_kv(l, k, b);
}

int gk3w_line_emit(gk3w_line *l, FILE *f)
{
    if (l->oom)
        return -1;
    put(l, "\n", 1);
    if (l->oom)
        return -1;
    if (fwrite(l->buf, 1, l->len, f) != l->len)
        return -1;
    return fflush(f) == 0 ? 0 : -1;
}

void gk3w_line_free(gk3w_line *l)
{
    free(l->buf);
    l->buf = NULL;
    l->len = l->cap = 0;
}

int gk3w_fail(gk3w_line *l, gk3w_touched t, const char *human)
{
    if (t != GK3W_TOUCHED_NA)
        gk3w_line_kv(l, "touched", t == GK3W_TOUCHED_YES ? "yes" : "no");
    /* stderr 已经写不出去时没有别处可报，只能放弃 */
    (void)gk3w_line_emit(l, stderr);
    fprintf(stderr, "!! %s\n", human);
    fflush(stderr);
    return -1;
}

void gk3w_progress(unsigned pct, const char *code)
{
    fprintf(stderr, "PROGRESS %u %s\n", pct, code);
    fflush(stderr);
}
