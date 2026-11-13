/* ESP 上要写的东西：启动项与 loader.conf 的文本、空间检查。LiveCD（gk3l）与 Windows 安装器（gk3w）共用
 * 写出的内容逐字节相同（对拍时比对文件），检查的算式与顺序也相同。 */
#ifndef GK3W_ESP_H
#define GK3W_ESP_H

#include <stddef.h>
#include <stdint.h>

#define GK3W_ESP_NEED_MIB 150u
#define GK3W_ESP_REINSTALL_NEED_MIB 16u
#define GK3W_ESP_MIN_SIZE_MIB 200u
#define GK3W_ESP_OTA_NEED_KIB 57344u
#define GK3W_ESP_WIN_RESERVE_KIB 32768u
#define GK3W_ESP_GK3BOOT_KIB 5120u
#define GK3W_LOADER_TIMEOUT 5u

/* loader/loader.conf 的全文 */
extern const char gk3w_loader_conf[];

/* mid 是不是 32 位小写十六进制（ESP 根目录下 <machine-id>/ 的名字） */
int gk3w_is_machine_id(const char *s);

/* loader/entries 里的这个文件名是不是我们写的：
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
    GK3W_FB_LEAVE_OTHER = 0,    /* 1a：有备份，但装完之后别的系统换上了自己的引导程序：回落路径留着；备份删了不丢就删
                                 * （gk3w_fallback_bak_disposable），是别的系统的原件才留着 */
    GK3W_FB_RESTORE_ORIGINAL,   /* 1b：换回备份（装之前那里是别的系统的引导程序） */
    GK3W_FB_USE_W,              /* 1c：换成当前的 bootmgfw.efi */
    GK3W_FB_USE_BAK,            /* 1c：没有 bootmgfw.efi，用装机那天的 Windows 拷贝 */
    GK3W_FB_NO_WINDOWS,         /* 1c：该换却没有可换的，拒绝 */
    GK3W_FB_UNTOUCHED,          /* 1d：这一套安装没动过回落路径，留着 */
} gk3w_fallback_action;
gk3w_fallback_action gk3w_fallback_decide(const uint8_t *cur, size_t cur_len, const uint8_t *w, size_t w_len,
                                          const uint8_t *bak, size_t bak_len);

/* .before-gaokun3 删了会不会丢东西：它是 systemd-boot（我们早先装的，或没有配置的），或者它是 Windows 启动管理器
 * 而 EFI/Microsoft/Boot/bootmgfw.efi 还在 —— 这两种删了都不丢 */
int gk3w_fallback_bak_disposable(const uint8_t *bak, size_t bak_len, const uint8_t *w, size_t w_len);

/* 安装时回落路径的原件怎么留（apply 写 systemd-boot 之前）。cur = 现在的 EFI/BOOT/BOOTAA64.EFI，w = bootmgfw.efi，
 * bak = 已有的 .before-gaokun3，sd = 要写的 systemd-boot；不存在的给 NULL。 */
typedef enum {
    GK3W_BK_NONE = 0,           /* 不用留：回落路径不存在，或已有备份而现在的文件覆盖了也不丢（systemd-boot、
                                 * Windows 启动管理器、或就是备份本身） */
    GK3W_BK_SAVE,               /* 还没有备份：把现在的文件留成 .before-gaokun3（与要写的 systemd-boot 相同时不留） */
    GK3W_BK_REPLACE,            /* 已有的备份删了不丢，现在的是别的系统的引导程序：用现在的换掉备份 */
    GK3W_BK_CONFLICT,           /* 现在的是别的系统的引导程序，已有的备份也是：两份都不能丢，拒绝安装 */
} gk3w_backup_action;
gk3w_backup_action gk3w_install_backup_decide(const uint8_t *cur, size_t cur_len, const uint8_t *w, size_t w_len,
                                              const uint8_t *bak, size_t bak_len, const uint8_t *sd, size_t sd_len);

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
    int reinstall;              /* 重新安装：我们已经在这个 ESP 上，不查分区大小，空闲下限 16 MiB 而不是 150 MiB
                                 * （覆盖的是自己的文件，不是新增） */
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
