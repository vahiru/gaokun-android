/* H2 交接（见 handoff.h 的出处与顺序）。 */
#include "handoff.h"

/* 内核载荷的 Vendor 媒体设备路径：给缓冲区镜像一个可辨认的路径（同 systemd-boot linux.c:49-68 的做法，
 * 它用 STUB_PAYLOAD_GUID；这里用自己的 GUID，安全钩子 / 日志里能认出是 gk3boot 交的）。 */
#define GK3_PAYLOAD_GUID {0x6b8d7a1e, 0x3f0c, 0x4c52, {0x9a, 0x61, 0x67, 0x6b, 0x33, 0x62, 0x6f, 0x6f}}
/* LINUX_EFI_INITRD_MEDIA_GUID，同 gk3_guid_initrd_media（静态初始化里不能引用另一个 const 对象） */
#define GK3_INITRD_MEDIA_GUID {0x5568e427, 0x68fc, 0x4f3d, {0xac, 0x74, 0xca, 0x55, 0x52, 0x31, 0xcc, 0x68}}

typedef struct __attribute__((packed)) {
    uint8_t type, sub;
    uint16_t len;
    EFI_GUID g;
    uint8_t et, es;
    uint16_t elen;
} vendor_dp;

/* Type 4 Media / SubType 3 Vendor，长度 20；结束节点 7f/ff，长度 4（UEFI 规范 10.3.5.3 / 10.3.1） */
static const vendor_dp kernel_dp = {4, 3, 20, GK3_PAYLOAD_GUID, 0x7f, 0xff, 4};
/* initrd.c:22-39：LINUX_EFI_INITRD_MEDIA_GUID 的 Vendor 媒体路径 —— 内核按这条路径 LocateDevicePath 找 LoadFile2 */
static const vendor_dp initrd_dp = {4, 3, 20, GK3_INITRD_MEDIA_GUID, 0x7f, 0xff, 4};

/* initrd.c:12-17：LoadFile2 后面跟着数据指针 */
typedef struct {
    EFI_LOAD_FILE_PROTOCOL lf;
    const void *addr;
    size_t len;
} initrd_loader;

static initrd_loader g_initrd;

/* initrd.c:41-68 的语义：BootPolicy 必须为假；缓冲区不够大时报所需大小。 */
static EFI_STATUS EFIAPI initrd_load_file(EFI_LOAD_FILE_PROTOCOL *this, EFI_DEVICE_PATH *file_path, BOOLEAN boot_policy,
                                          UINTN *buffer_size, VOID *buffer)
{
    initrd_loader *l = (initrd_loader *)this;
    if (!this || !buffer_size || !file_path)
        return EFI_INVALID_PARAMETER;
    if (boot_policy)
        return EFI_UNSUPPORTED;
    if (!l->addr || !l->len)
        return EFI_NOT_FOUND;
    if (!buffer || *buffer_size < l->len) {
        *buffer_size = l->len;
        return EFI_BUFFER_TOO_SMALL;
    }
    gk3_memcpy(buffer, l->addr, l->len);
    *buffer_size = l->len;
    gk3_logd("handoff: initrd LoadFile2 served %llu bytes\n", (unsigned long long)l->len);
    return EFI_SUCCESS;
}

static void *find_config_table(const EFI_GUID *g)
{
    for (UINTN i = 0; i < gk3_st->NumberOfTableEntries; i++)
        if (gk3_guid_eq(&gk3_st->ConfigurationTable[i].VendorGuid, g))
            return gk3_st->ConfigurationTable[i].VendorTable;
    return NULL;
}

EFI_STATUS gk3_linux_boot(const gk3_linux *L, gk3_pre_start_fn pre_start, const char **stage)
{
    EFI_HANDLE kimg = NULL, initrd_h = NULL;
    EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
    EFI_PHYSICAL_ADDRESS dtb_addr = 0;
    UINTN dtb_pages = 0;
    void *dtb_orig = NULL;
    bool dtb_installed = false;
    EFI_STATUS st;
    uint64_t t;

    *stage = "args";
    if (!L->kernel || L->kernel_len < 64 || !L->cmdline || !L->dtb || L->dtb_len < 40 || !L->initrd || !L->initrd_len)
        return EFI_INVALID_PARAMETER;

    /* 1. LoadImage（缓冲区）—— 已在本机验证可用（见 tools/gk3boot/README.md） */
    *stage = "LoadImage";
    t = gk3_ticks();
    st = gk3_bs->LoadImage(FALSE, gk3_image, (EFI_DEVICE_PATH *)&kernel_dp, (void *)L->kernel, L->kernel_len, &kimg);
    gk3_logf("handoff: LoadImage(%llu bytes) %s in %llu ms\n", (unsigned long long)L->kernel_len,
             gk3_efi_strerror(st), (unsigned long long)(gk3_us_since(t) / 1000));
    if (EFI_ERROR(st)) {
        /* UEFI 规范：SECURITY_VIOLATION 时镜像已加载、句柄有效，必须 Unload */
        if (kimg && st == EFI_SECURITY_VIOLATION)
            gk3_bs->UnloadImage(kimg);
        return st;
    }
    *stage = "LoadedImage";
    st = gk3_bs->HandleProtocol(kimg, (EFI_GUID *)&gk3_guid_loaded_image, (void **)&li);
    if (EFI_ERROR(st) || !li) {
        st = EFI_ERROR(st) ? st : EFI_NOT_FOUND;
        goto unload;
    }
    gk3_logd("handoff: kernel image base=%p size=0x%llx code_type=%u data_type=%u\n", li->ImageBase,
             (unsigned long long)li->ImageSize, li->ImageCodeType, li->ImageDataType);

    /* 2. DTB：devicetree.c:9-21 用 EfiACPIReclaimMemory —— 内核 stub 只读它、拷一份再改；
     *    这份页在 ExitBootServices 之后也不会被当成空闲内存先用掉。没有 DT_FIXUP 协议（§2.1 字节扫描零命中），
     *    有的话只记录不调用（devicetree.c:28-63 会调；本机没有，最小版本不做）。 */
    *stage = "dtb";
    dtb_orig = find_config_table(&gk3_guid_dtb_table);
    {
        void *fx = NULL;
        if (!EFI_ERROR(gk3_bs->LocateProtocol((EFI_GUID *)&gk3_guid_dt_fixup, NULL, &fx)) && fx)
            gk3_logf("handoff: note: EFI_DT_FIXUP_PROTOCOL present but not applied (minimal build)\n");
    }
    dtb_pages = (L->dtb_len + 4095) / 4096;
    st = gk3_bs->AllocatePages(AllocateAnyPages, EfiACPIReclaimMemory, dtb_pages, &dtb_addr);
    if (EFI_ERROR(st)) {
        dtb_pages = 0;
        goto unload;
    }
    gk3_memcpy((void *)(uintptr_t)dtb_addr, L->dtb, L->dtb_len);
    st = gk3_bs->InstallConfigurationTable((EFI_GUID *)&gk3_guid_dtb_table, (void *)(uintptr_t)dtb_addr);
    if (EFI_ERROR(st))
        goto unload;
    dtb_installed = true;
    gk3_logf("handoff: dtb %llu bytes installed as config table @%p (replaced %p)\n", (unsigned long long)L->dtb_len,
             (void *)(uintptr_t)dtb_addr, dtb_orig);

    /* 3. initrd：initrd.c:85-91 —— 已经有人在这条路径上装了 LoadFile2 就不装（会让内核拿错 initrd） */
    *stage = "initrd";
    {
        EFI_DEVICE_PATH *dp = (EFI_DEVICE_PATH *)&initrd_dp;
        EFI_HANDLE h = NULL;
        st = gk3_bs->LocateDevicePath((EFI_GUID *)&gk3_guid_load_file2, &dp, &h);
        if (st != EFI_NOT_FOUND) {
            gk3_logf("!! handoff: a LoadFile2 already sits on the initrd media path (%s)\n", gk3_efi_strerror(st));
            st = EFI_ALREADY_STARTED;
            goto unload;
        }
    }
    g_initrd.lf.LoadFile = initrd_load_file;
    g_initrd.addr = L->initrd;
    g_initrd.len = L->initrd_len;
    st = gk3_bs->InstallMultipleProtocolInterfaces(&initrd_h, (EFI_GUID *)&gk3_guid_device_path, (void *)&initrd_dp,
                                                   (EFI_GUID *)&gk3_guid_load_file2, (void *)&g_initrd, NULL);
    if (EFI_ERROR(st)) {
        initrd_h = NULL;
        goto unload;
    }
    gk3_logf("handoff: initrd %llu bytes on LINUX_EFI_INITRD_MEDIA LoadFile2 (handle %p)\n",
             (unsigned long long)L->initrd_len, initrd_h);

    /* 4. LoadOptions：linux.c:133-136 —— Size 是字节数、含结尾 NUL */
    *stage = "LoadOptions";
    li->LoadOptions = (void *)L->cmdline;
    li->LoadOptionsSize = (UINT32)((gk3_strlen16(L->cmdline) + 1) * sizeof(CHAR16));
    gk3_logf("handoff: LoadOptions %u bytes\n", li->LoadOptionsSize);

    /* 5. StartImage：成功就不回来了（stub 自己 ExitBootServices；UEFI 看门狗随 ExitBootServices 关掉） */
    *stage = "StartImage";
    gk3_logf("handoff: StartImage ...\n");
    if (pre_start)
        pre_start();
    {
        UINTN xs = 0;
        CHAR16 *xd = NULL;
        st = gk3_bs->StartImage(kimg, &xs, &xd);
        if (xd)
            gk3_bs->FreePool(xd);
    }
    kimg = NULL;   /* StartImage 返回后镜像已由固件卸载（UEFI 规范：Exit 时卸载 application） */
    gk3_logf("!! handoff: StartImage returned %s\n", gk3_efi_strerror(st));
    if (!EFI_ERROR(st))
        st = EFI_LOAD_ERROR;   /* 内核返回 SUCCESS 也是失败：它本该接管 */

unload:
    /* §4.12 "StartImage 返回时撤掉 LoadFile2 和 DTB 表再走阶梯" */
    if (initrd_h)
        gk3_bs->UninstallMultipleProtocolInterfaces(initrd_h, (EFI_GUID *)&gk3_guid_device_path, (void *)&initrd_dp,
                                                    (EFI_GUID *)&gk3_guid_load_file2, (void *)&g_initrd, NULL);
    if (dtb_installed)
        gk3_bs->InstallConfigurationTable((EFI_GUID *)&gk3_guid_dtb_table, dtb_orig);
    if (dtb_pages)
        gk3_bs->FreePages(dtb_addr, dtb_pages);
    if (kimg)
        gk3_bs->UnloadImage(kimg);
    return st;
}
