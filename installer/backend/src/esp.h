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

/* loader/entries 里的这个文件名是不是我们写的，与 scripts/windows/gaokun3-setup.ps1 的 Test-OurEntryName 相同：
 * gaokun3-live[-N]、gk3*（统一启动入口与双系统的 gk3-windows.conf）、<mid>-android|rescue|recovery[-a|-b]；
 * 可带启动计数后缀 +N[-M]、可带 .disabled；ASCII 不分大小写。 */
int gk3w_our_entry_name(const char *name);

/* 共用 ESP 上卸载时，这个 Loader 变量值算不算我们写的（Test-OurVarValue）：auto-windows、gk3-windows.conf
 * （同 gk3_defvar_classify），指向某个槽的 *-android-<a|b>.conf，或 gk3w_our_entry_name 认得的条目名 */
int gk3w_our_var_value(const char *v);

/* 这个 EFI 程序是不是 Windows 启动管理器：PE 版本资源里 CompanyName 为 "Microsoft Corporation"、FileDescription 为
 * "Boot Manager"（bootmgfw.efi 的版本信息）。不能只找 UTF-16 的 "Windows Boot Manager"：systemd-boot 里也有这串
 * （auto-windows 菜单项的标题）。 */
int gk3w_is_windows_boot_manager(const uint8_t *b, size_t n);

/* 卸载时回落路径 EFI/BOOT/BOOTAA64.EFI 怎么办（判断表见 src/win/uninstall.c 开头的 1a–1d）。
 * cur = 现在的回落文件，w = EFI/Microsoft/Boot/bootmgfw.efi，bak = BOOTAA64.EFI.before-gaokun3；不存在的给 NULL。 */
typedef enum {
    GK3W_FB_LEAVE_OTHER = 0,    /* 1a：有备份，但装完之后别的系统换上了自己的引导程序：回落路径与备份都留着 */
    GK3W_FB_RESTORE_ORIGINAL,   /* 1b：换回备份（装之前那里是别的系统的引导程序） */
    GK3W_FB_USE_W,              /* 1c：换成当前的 bootmgfw.efi */
    GK3W_FB_USE_BAK,            /* 1c：没有 bootmgfw.efi，用装机那天的 Windows 拷贝 */
    GK3W_FB_NO_WINDOWS,         /* 1c：该换却没有可换的，拒绝 */
    GK3W_FB_UNTOUCHED,          /* 1d：这一套安装没动过回落路径，留着 */
} gk3w_fallback_action;
gk3w_fallback_action gk3w_fallback_decide(const uint8_t *cur, size_t cur_len, const uint8_t *w, size_t w_len,
                                          const uint8_t *bak, size_t bak_len);

/* 这个 EFI 程序是不是 systemd-boot：带 "#### LoaderInfo: systemd-boot "（systemd-boot 自己留的版本标记） */
int gk3w_is_systemd_boot(const uint8_t *b, size_t n);

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
