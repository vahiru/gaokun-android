/* Windows：ESP 上的文件读写，以及把回落路径换成 systemd-boot 的那一步。apply（双系统安装）与 media（写入本机磁盘）共用。 */
#ifndef GK3W_ESPW_H
#define GK3W_ESPW_H

#include <windows.h>

#include <stddef.h>
#include <stdint.h>

#include "disk.h"

typedef struct {
    wchar_t root[MAX_PATH];     /* \\?\Volume{…}\ */
    size_t root_len;
    char mid[33];
    int windows;                /* EFI\Microsoft\Boot\bootmgfw.efi 在 */
} gk3w_esp;

/* ESP 上相对路径（/ 分隔）对应的完整路径 */
void gk3w_esp_path(const gk3w_esp *e, const char *rel, wchar_t *out, size_t cap);
/* ESP 上 rel 这个文件与 data 是否逐字节相同；不存在算不同 */
int gk3w_esp_same(const gk3w_esp *e, const char *rel, const uint8_t *data, size_t len);
/* 文件大小；不存在返回 -1 */
int64_t gk3w_esp_size_of(const gk3w_esp *e, const char *rel);
/* 往 ESP 上写这个文件要多占多少 KiB（同名旧文件只算变大的部分，按 4 KiB 簇向上取整） */
int64_t gk3w_esp_delta(const gk3w_esp *e, const char *rel, uint64_t newsz);
int gk3w_esp_has_android_entries(const gk3w_esp *e);
/* ESP 根目录下第一个（按字节序）32 位十六进制的目录名 */
int gk3w_esp_pick_mid(const gk3w_esp *e, char out[33]);
/* 写一个文件并读回核对；失败已报 ERR（touched=yes），返回 1 */
int gk3w_esp_write(const gk3w_esp *e, const char *rel, const uint8_t *data, size_t len);
/* 写成 <rel>.tmp、读回核对，再改名覆盖 <rel>（保留 <rel> 原来的属性）：中途失败或掉电时 <rel> 要么是原来的、要么是
 * 完整的新文件。失败已报 ERR（touched=yes），返回 1 */
int gk3w_esp_write_renamed(const gk3w_esp *e, const char *rel, const uint8_t *data, size_t len);

/* 这块盘上唯一的 ESP：找到它、打开它的卷、确认是 FAT32，记下 Windows 启动管理器在不在。*part 指向 d->lay 里的
 * 表项（重读分区表之后失效）。失败已报 ERR（touched 照 T），返回 -1 */
int gk3w_esp_open(gk3w_disk *d, gk3w_esp *e, const PARTITION_INFORMATION_EX **part, gk3w_touched T);

/* 回落路径要换成 systemd-boot（sd）之前，EFI/BOOT/BOOTAA64.EFI 与 loader/loader.conf 的原件要不要留一份
 * （.before-gaokun3）：要留的读进 orig[0] / orig[1]（malloc，调用方 free），不用留的为 NULL。loader.conf 只在第一次装时、
 * 而且不是我们写的（gk3w_is_our_loader_conf）才留；回落路径按 gk3w_install_backup_decide。文件在而读不了、或者两份
 * 别的系统的引导程序都不能丢时拒绝。失败已报 ERR（touched 照 T），返回 -1 */
int gk3w_esp_plan_backups(const gk3w_esp *e, const uint8_t *sd, size_t sd_len, uint8_t *orig[2], size_t orig_len[2], gk3w_touched T);

/* 写 gk3w_esp_plan_backups 定下的备份；conf 不为 NULL 时接着写 loader/loader.conf；最后把 systemd-boot 写到
 * EFI/BOOT/BOOTAA64.EFI（改名写入）与 EFI/systemd/systemd-bootaa64.efi，字节相同的不重写（BitLocker 量的是固件每次
 * 加载的这个文件）。loader.conf 在回落路径之前：中途失败时，回落路径要么还是原来的文件，要么 systemd-boot 已有配置。
 * 失败已报 ERR（touched=yes），返回 -1 */
int gk3w_esp_write_sdboot(const gk3w_esp *e, const uint8_t *sd, size_t sd_len, uint8_t *const orig[2], const size_t orig_len[2],
                          const char *conf);

#endif
