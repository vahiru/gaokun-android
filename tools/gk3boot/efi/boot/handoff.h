/*
 * H2 交接：内存里的内核 PE（EFI stub / zboot）+ ramdisk + dtb + cmdline → LoadImage / StartImage。
 * 设计与交接防御见 tools/gk3boot/README.md。
 *
 * 照 systemd-boot 257.13 的写法重写（不拷代码）：
 *   - 缓冲区 LoadImage + Vendor 设备路径：refs/systemd-v257/src/boot/linux.c:44-91
 *   - LoadOptions = UCS-2 cmdline，Size 含结尾 NUL：linux.c:133-136、boot.c:2622-2624
 *   - DTB：拷进 EfiACPIReclaimMemory 页，InstallConfigurationTable(DEVICE_TREE_GUID)，
 *     撤销时装回原值（原来没有就是 NULL = 删表）：devicetree.c:9-21、:65-107
 *   - initrd：LINUX_EFI_INITRD_MEDIA_GUID 的 Vendor 媒体路径 + LoadFile2，先查有没有人已经装过：initrd.c:12-110
 *   - 顺序同 boot.c image_start（:2543-2656）：LoadImage(:2574) → DTB(:2582) → initrd(:2588)
 *     → LoadOptions(:2622-2624) → StartImage(:2631)
 *   - 不调 ExitBootServices，由内核 EFI stub 自己做（§2.2：dtb 的 memory 节点大小为 0，内存图只能来自 EFI）。
 *   - 不做 arm64 用不到的 x86 compat 入口回退（linux.c:146-151、boot.c:2636-2654）。
 *
 * 内核这一侧（libstub，v7.2-rc2）：zboot.c:35-100 只取 LoadedImage 的 LoadOptions，不看 FilePath / DeviceHandle；
 * initrd 先找 LINUX_EFI_INITRD_MEDIA 设备路径上的 LoadFile2（efi-stub.c:172 → efi_load_initrd）。
 * （这两处出自 git.kernel.org v7.2-rc2 的原文，本地 refs/ 里没有 libstub —— 见 README §10 的已知限制。）
 */
#ifndef GK3BOOT_HANDOFF_H
#define GK3BOOT_HANDOFF_H

#include "gk3efi.h"

typedef struct {
    const void *kernel;      /* PE（MZ），LoadImage 会自己拷一份 */
    size_t kernel_len;
    const void *initrd;      /* 原样交给 LoadFile2；内核 stub 在 ExitBootServices 之前把它拷走 */
    size_t initrd_len;
    const void *dtb;         /* FDT；这里拷进 EfiACPIReclaimMemory 页再装表 */
    size_t dtb_len;
    const CHAR16 *cmdline;   /* NUL 结尾；调用方保证在 StartImage 期间一直有效 */
} gk3_linux;

/* 交接前最后一刻的回调（日志 sync + 关文件）。StartImage 之前调一次。 */
typedef void (*gk3_pre_start_fn)(void);

/* 成功时不返回（内核接管）。返回值是失败原因；返回前已撤掉 LoadFile2 与 DTB 表、UnloadImage。
 * *stage 指向失败发生在哪一步（"LoadImage" / "dtb" / "initrd" / "StartImage" …）。 */
EFI_STATUS gk3_linux_boot(const gk3_linux *L, gk3_pre_start_fn pre_start, const char **stage);

#endif
