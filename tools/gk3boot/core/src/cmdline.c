/* libgk3core：内核 cmdline 拼装（§4.3.1 Android 交接、§4.4.1 执行端）。
 *
 * 分词按内核 next_arg() 的规则：空白分隔，双引号里的空白不算。重拼时 token 之间一律单个空格。 */
#include "gk3core.h"

typedef struct {
    char *out;
    size_t cap, len;
    bool overflow;
} sbuf;

static void put(sbuf *b, const char *s, size_t n)
{
    if (b->overflow || b->len + n + 1 > b->cap) {
        b->overflow = true;
        return;
    }
    gk3_memcpy(b->out + b->len, s, n);
    b->len += n;
    b->out[b->len] = 0;
}

static void puts_(sbuf *b, const char *s) { put(b, s, gk3_strnlen(s, 4096)); }

static void token(sbuf *b, const char *s, size_t n)
{
    if (b->len)
        put(b, " ", 1);
    put(b, s, n);
}

static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

/* 取下一个 token：返回长度，*start 指向开头；没有了返回 0。 */
static size_t next_token(const char **p, const char **start)
{
    const char *s = *p;
    bool q = false;
    size_t n = 0;
    while (*s && is_space(*s))
        s++;
    *start = s;
    while (s[n] && (q || !is_space(s[n]))) {
        if (s[n] == '"')
            q = !q;
        n++;
    }
    *p = s + n;
    return n;
}

static bool has_prefix(const char *t, size_t n, const char *pfx)
{
    size_t k = 0;
    for (; pfx[k]; k++)
        if (k >= n || t[k] != pfx[k])
            return false;
    return true;
}

/* token 的键是否等于 key（"key" 或 "key=…"） */
static bool key_is(const char *t, size_t n, const char *key)
{
    size_t k = gk3_strnlen(key, 256);
    return has_prefix(t, n, key) && (n == k || t[k] == '=');
}

static bool value_ok(const char *v)
{
    size_t n = 0;
    if (!v || !*v)
        return false;
    for (; v[n]; n++) {
        char c = v[n];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '.' || c == '_' || c == '+' || c == '-' || c == ':' || c == ',' || c == '=' || c == '/';
        if (!ok || n > 200)
            return false;
    }
    return true;
}

static void kv(sbuf *b, const char *k, const char *v)
{
    if (b->len)
        put(b, " ", 1);
    puts_(b, k);
    put(b, "=", 1);
    puts_(b, v);
}

long gk3_bootimg_cmdline(const gk3_bootimg *bi, char *out, size_t out_len)
{
    sbuf b = {out, out_len, 0, false};
    size_t n1 = gk3_strnlen((const char *)bi->hdr + 64, GK3_BOOT_ARGS_SIZE);
    size_t n2 = gk3_strnlen((const char *)bi->hdr + 608, GK3_BOOT_EXTRA_ARGS_SIZE);
    if (!out_len)
        return -1;
    out[0] = 0;
    put(&b, (const char *)bi->hdr + 64, n1);
    put(&b, (const char *)bi->hdr + 608, n2);
    return b.overflow ? -1 : (long)b.len;
}

gk3_err gk3_cmdline_android(const char *base, const gk3_android_args *a, char *out, size_t out_len)
{
    static const char *const ours[] = {
        "androidboot.slot_suffix", "androidboot.bootloader",
        "androidboot.gk3boot.event", "androidboot.gk3boot.entry", "androidboot.gk3boot.mode",
        "androidboot.gk3boot.streak", "androidboot.gk3boot.dispatch",
    };
    sbuf b = {out, out_len, 0, false};
    const char *p = base, *t;
    size_t n;
    char suffix[3] = {'_', 0, 0};

    if (!out_len)
        return GK3_ENOSPC;
    out[0] = 0;
    if (a->slot > 1)
        return GK3_EINVAL;
    if ((a->bootloader && !value_ok(a->bootloader)) || (a->event && !value_ok(a->event)) ||
        (a->entry && !value_ok(a->entry)) || (a->mode && !value_ok(a->mode)) || (a->streak && !value_ok(a->streak)) ||
        (a->dispatch && !value_ok(a->dispatch)))
        return GK3_EINVAL;
    while ((n = next_token(&p, &t)) != 0) {
        bool drop = false;
        for (size_t k = 0; k < sizeof(ours) / sizeof(ours[0]); k++)
            if (key_is(t, n, ours[k]))
                drop = true;
        if (!drop)
            token(&b, t, n);
    }
    suffix[1] = (char)('a' + a->slot);
    kv(&b, "androidboot.slot_suffix", suffix);
    if (a->bootloader)
        kv(&b, "androidboot.bootloader", a->bootloader);
    if (a->event)
        kv(&b, "androidboot.gk3boot.event", a->event);
    if (a->entry)
        kv(&b, "androidboot.gk3boot.entry", a->entry);
    if (a->mode)
        kv(&b, "androidboot.gk3boot.mode", a->mode);
    if (a->streak)
        kv(&b, "androidboot.gk3boot.streak", a->streak);
    if (a->dispatch)
        kv(&b, "androidboot.gk3boot.dispatch", a->dispatch);
    return b.overflow ? GK3_ENOSPC : GK3_OK;
}

gk3_err gk3_cmdline_fastboot(const char *base, const gk3_fastboot_args *a, char *out, size_t out_len)
{
    sbuf b = {out, out_len, 0, false};
    const char *p = base, *t;
    size_t n;
    char slot[2] = {0, 0};

    if (!out_len)
        return GK3_ENOSPC;
    out[0] = 0;
    if (a->slot > 1 || !value_ok(a->why) || !value_ok(a->bootver) || !value_ok(a->disk) ||
        (a->esp && !value_ok(a->esp)))
        return GK3_EINVAL;
    while ((n = next_token(&p, &t)) != 0) {
        /* 同 LiveCD 启动项的过滤（installer/tools/build/build-usb.sh 的 live_cmdline）：去掉只给 Android 的；再去掉
         * deferred_probe_timeout（BoardConfig.mk:130，执行端不需要等 10 秒）与我们自己要加的键 */
        if (has_prefix(t, n, "androidboot.") || key_is(t, n, "init") ||
            key_is(t, n, "firmware_class.path") || key_is(t, n, "deferred_probe_timeout") ||
            key_is(t, n, "panic") || has_prefix(t, n, "gk3."))
            continue;
        token(&b, t, n);
    }
    slot[0] = (char)('a' + a->slot);
    kv(&b, "panic", "10");
    kv(&b, "gk3.mode", "fastboot");
    kv(&b, "gk3.why", a->why);
    kv(&b, "gk3.slot", slot);
    kv(&b, "gk3.bootver", a->bootver);
    kv(&b, "gk3.disk", a->disk);
    if (a->esp)
        kv(&b, "gk3.esp", a->esp);
    if (a->dispatch)
        kv(&b, "gk3.dispatch", "1");
    if (a->fbtcp)
        kv(&b, "gk3.fbtcp", "1");
    return b.overflow ? GK3_ENOSPC : GK3_OK;
}

gk3_err gk3_ascii_to_ucs2(const char *s, uint16_t *out, size_t out_chars)
{
    size_t i = 0;
    for (; s[i]; i++) {
        if ((unsigned char)s[i] > 0x7e || (unsigned char)s[i] < 0x20)
            return GK3_EINVAL;
        if (i + 1 >= out_chars)
            return GK3_ENOSPC;
        out[i] = (uint16_t)(unsigned char)s[i];
    }
    if (i >= out_chars)
        return GK3_ENOSPC;
    out[i] = 0;
    return GK3_OK;
}
