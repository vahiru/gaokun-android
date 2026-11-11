/* ESP 上要写的东西：启动项与 loader.conf 的文本、空间检查。与 scripts/live/installer-lib.sh 的 gk3_apply
 * 写出的内容逐字节相同（对拍时比对文件），检查的算式与顺序也相同。 */
#ifndef GK3W_ESP_H
#define GK3W_ESP_H

#include <stddef.h>
#include <stdint.h>

#define GK3W_ESP_NEED_MIB 150u
#define GK3W_ESP_MIN_SIZE_MIB 200u
#define GK3W_ESP_OTA_NEED_KIB 57344u
#define GK3W_ESP_WIN_RESERVE_KIB 32768u
#define GK3W_ESP_GK3BOOT_KIB 5120u
#define GK3W_LOADER_TIMEOUT 5u

/* loader/loader.conf 的全文 */
extern const char gk3w_loader_conf[];

/* mid 是不是 32 位小写十六进制（ESP 根目录下 <machine-id>/ 的名字） */
int gk3w_is_machine_id(const char *s);

/* loader/entries/<mid>-android-<slot>.conf 的全文。返回写入的长度，out 不够返回 -1。 */
int gk3w_esp_android_entry(char *out, size_t cap, const char *mid, char slot, const char *cmdline);

typedef struct {
    uint64_t esp_size_mib;      /* ESP 分区大小 */
    int64_t free_kib;           /* 现在的空闲 */
    int64_t need_kib;           /* 这次要多占的（已扣掉会被覆盖的同名文件，含 256 KiB 的启动项余量） */
    int64_t slot_kib;           /* 一个槽的 Image + dtb + ramdisk */
    int windows;                /* ESP 上有 Windows 的启动管理器 */
} gk3w_esp_space;

typedef enum {
    GK3W_ESP_OK = 0,
    GK3W_ESP_TOO_SMALL,         /* esp-too-small：重装 Windows 得到的 100 MiB ESP */
    GK3W_ESP_FULL,              /* esp-full：要写的比空闲的多 */
    GK3W_ESP_OTA_ROOM,          /* esp-ota-room：装完之后下一次 OTA 放不下 */
    GK3W_ESP_WIN_RESERVE,       /* esp-win-reserve：扣掉 OTA 的暂时空间后，给 Windows 与固件留的不够 32 MiB */
    GK3W_ESP_FULL_MIN,          /* esp-full：空闲不到 150 MiB */
} gk3w_esp_check;

gk3w_esp_check gk3w_esp_space_check(const gk3w_esp_space *in);
const char *gk3w_esp_check_code(gk3w_esp_check c);

#endif
