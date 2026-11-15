/* gk3l：同一块盘上 Windows 卷的状态、systemd-boot 的 EFI 变量（说明见 sysstate.c）。 */
#ifndef GK3L_SYSSTATE_H
#define GK3L_SYSSTATE_H

#include <stddef.h>

typedef struct {
    int bitlocker;              /* 盘上有 BitLocker 卷 */
    int hibernated;             /* 1 有 NTFS 卷在休眠；0 都没在休眠；-1 判断不了（BitLocker 卷读不到里面，或有 NTFS 卷挂不上） */
    char hib_part[96];          /* 在休眠的那个卷 */
} gk3l_win;

/* disk 是整块盘的名字（nvme0n1）。只看这块盘：Windows 的 C 盘在别的盘上时这里看不到（本机只有一块 NVMe） */
void gk3l_win_state(const char *disk, gk3l_win *w);

/* systemd-boot 的变量（厂商 GUID 4a67b082-…）：0 = 读到（out 是值）；1 = 没有这个变量；2 = efivarfs 不可用 */
int gk3l_loader_var(const char *name, char *out, size_t cap);
/* 删；本来就没有也算成功。0 成功，-1 删不掉或 efivarfs 不可用 */
int gk3l_loader_var_delete(const char *name);

#endif
