/* ESP 上的启动项文本与空间检查（说明见 esp.h）。 */
#include "esp.h"

#include <stdio.h>
#include <string.h>

/* installer-lib.sh：纯 Android 与双系统统一 5 秒、不写 menu-hidden */
const char gk3w_loader_conf[] = "timeout 5\n"
                                "console-mode keep\n"
                                "editor no\n"
                                "default *-android-a.conf\n";

static char lower_ascii(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static int is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static int ends_with(const char *s, size_t n, const char *suf)
{
    size_t k = strlen(suf);
    return n >= k && !memcmp(s + n - k, suf, k);
}

int gk3w_our_entry_name(const char *name)
{
    char n[256];
    size_t len = strlen(name), i;
    if (!len || len >= sizeof n)
        return 0;
    for (i = 0; i < len; i++)
        n[i] = lower_ascii(name[i]);
    n[len] = 0;
    if (ends_with(n, len, ".disabled"))
        n[len -= 9] = 0;
    if (!ends_with(n, len, ".conf"))
        return 0;
    n[len -= 5] = 0;
    {
        /* 启动计数后缀：结尾的 +N 或 +N-M */
        char *plus = strrchr(n, '+');
        if (plus && is_digit(plus[1])) {
            const char *q = plus + 1;
            while (is_digit(*q))
                q++;
            if (*q == '-' && is_digit(q[1]))
                for (q++; is_digit(*q); q++)
                    ;
            if (!*q) {
                *plus = 0;
                len = (size_t)(plus - n);
            }
        }
    }
    if (!strncmp(n, "gk3", 3))
        return 1;
    if (!strncmp(n, "gaokun3-live", 12)) {
        const char *q = n + 12;
        if (!*q)
            return 1;
        if (*q != '-' || !is_digit(q[1]))
            return 0;
        for (q++; is_digit(*q); q++)
            ;
        return !*q;
    }
    if (len > 33 && n[32] == '-') {
        static const char *const kinds[] = {"android", "rescue", "recovery"};
        char mid[33];
        memcpy(mid, n, 32);
        mid[32] = 0;
        if (!gk3w_is_machine_id(mid))
            return 0;
        for (size_t k = 0; k < 3; k++) {
            size_t kl = strlen(kinds[k]);
            const char *q = n + 33;
            if (strncmp(q, kinds[k], kl))
                continue;
            q += kl;
            if (!*q || (q[0] == '-' && (q[1] == 'a' || q[1] == 'b') && !q[2]))
                return 1;
        }
    }
    return 0;
}

static int ieq_ascii(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (lower_ascii(*a) != lower_ascii(*b))
            return 0;
    return *a == *b;
}

int gk3w_our_var_value(const char *v)
{
    size_t len = strlen(v), i;
    char tail[16];
    if (ieq_ascii(v, "auto-windows") || ieq_ascii(v, "gk3-windows.conf"))
        return 1;
    /* ^[0-9A-Za-z*?._+-]*-android-[ab]\.conf$（PowerShell 的 -match 不分大小写） */
    for (i = 0; i < len; i++) {
        char c = v[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || strchr("*?._+-", c)))
            break;
    }
    if (i == len && len >= 15) {
        for (i = 0; i < 15; i++)
            tail[i] = lower_ascii(v[len - 15 + i]);
        tail[15] = 0;
        if (!strcmp(tail, "-android-a.conf") || !strcmp(tail, "-android-b.conf"))
            return 1;
    }
    return gk3w_our_entry_name(v);
}

int gk3w_is_machine_id(const char *s)
{
    size_t n = 0;
    for (; s[n]; n++)
        if (!((s[n] >= '0' && s[n] <= '9') || (s[n] >= 'a' && s[n] <= 'f')))
            return 0;
    return n == 32;
}

/* 文件名与各行都是承重的：boot_control HAL 按 *-android-a.conf / *-android-b.conf 改写 loader.conf 的 default
 * （EspSlot.cpp:41-43）；OTA postinstall 只改 options 那一行、只往 slot_<后缀>/ 写这三个文件名。
 * ⚠️ 标题里的 "crDroid 16.0" 照抄 installer-lib.sh（那边已经过时，要改两边一起改）。 */
int gk3w_esp_android_entry(char *out, size_t cap, const char *mid, char slot, const char *cmdline)
{
    int n;
    if ((slot != 'a' && slot != 'b') || !gk3w_is_machine_id(mid))
        return -1;
    n = snprintf(out, cap,
                 "title      crDroid 16.0 (gaokun3) \xe2\x80\x94 slot _%c\n"
                 "version    gaokun3-slot-%c\n"
                 "sort-key   zandroid%c\n"
                 "options    %s androidboot.slot_suffix=_%c\n"
                 "linux      /%s/android/slot_%c/Image\n"
                 "devicetree /%s/android/slot_%c/gaokun3.dtb\n"
                 "initrd     /%s/android/slot_%c/ramdisk.img\n",
                 slot, slot, slot, cmdline, slot, mid, slot, mid, slot, mid, slot);
    return n < 0 || (size_t)n >= cap ? -1 : n;
}

gk3w_esp_check gk3w_esp_space_check(const gk3w_esp_space *in)
{
    /* 顺序与 installer-lib.sh gk3_apply 双系统那段相同 */
    if (in->esp_size_mib > 0 && in->esp_size_mib < GK3W_ESP_MIN_SIZE_MIB)
        return GK3W_ESP_TOO_SMALL;
    if (in->free_kib < in->need_kib)
        return GK3W_ESP_FULL;
    if (in->free_kib - in->need_kib + in->slot_kib <= (int64_t)GK3W_ESP_OTA_NEED_KIB)
        return GK3W_ESP_OTA_ROOM;
    if (in->windows &&
        in->free_kib - in->need_kib - in->slot_kib - (int64_t)GK3W_ESP_GK3BOOT_KIB < (int64_t)GK3W_ESP_WIN_RESERVE_KIB)
        return GK3W_ESP_WIN_RESERVE;
    if (in->free_kib / 1024 < (int64_t)GK3W_ESP_NEED_MIB)
        return GK3W_ESP_FULL_MIN;
    return GK3W_ESP_OK;
}

const char *gk3w_esp_check_code(gk3w_esp_check c)
{
    switch (c) {
    case GK3W_ESP_OK: return "ok";
    case GK3W_ESP_TOO_SMALL: return "esp-too-small";
    case GK3W_ESP_FULL:
    case GK3W_ESP_FULL_MIN: return "esp-full";
    case GK3W_ESP_OTA_ROOM: return "esp-ota-room";
    case GK3W_ESP_WIN_RESERVE: return "esp-win-reserve";
    }
    return "unknown";
}
