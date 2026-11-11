/* ESP 上的启动项文本与空间检查（说明见 esp.h）。 */
#include "esp.h"

#include <stdio.h>
#include <string.h>

/* installer-lib.sh：纯 Android 与双系统统一 5 秒、不写 menu-hidden */
const char gk3w_loader_conf[] = "timeout 5\n"
                                "console-mode keep\n"
                                "editor no\n"
                                "default *-android-a.conf\n";

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
