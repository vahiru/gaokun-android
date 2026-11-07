/*
 * libgk3core —— 统一启动入口（tools/gk3boot/README.md）的决策核心。
 *
 * 同一份代码编进 gk3boot.efi（UEFI，无 libc）、执行端 gk3-fastbootd（静态 Linux）、
 * 主机测试和 gk3-misc CLI。约束：
 *   - freestanding C11：只用 <stdint.h> <stddef.h> <stdbool.h>，不分配内存、不调 libc；
 *     缓冲区一律由调用方给（UEFI 侧用 AllocatePool，Linux 侧用栈或 malloc）。
 *   - 盘上格式一律按字节、小端显式读写，不用位域、不靠结构体布局 —— 位域布局是编译器的事，
 *     盘上格式不能跟着编译器走。
 *   - 所有"写"都只改调用方的缓冲区；刷写、读回比对由 gk3_blk_* 的回调做（写前算 CRC、写后读回）。
 *
 * 出处约定：AOSP 源码行号对的是 scripts/clone-refs.sh 钉住的提交：
 *   hardware/interfaces 1a56e38（refs/aosp-hardware-interfaces）、
 *   GBL gbl-mainline e8577449（refs/gbl）、
 *   bootable/recovery lineage-23.0（refs/lineage-bootable-recovery）。
 *   crDroid 真实树（构建机 ~/crdroid）2026-10-05 已核对 bootloader_control 布局、BOOT_CTRL_MAGIC、
 *   misc 偏移常量与上述一致（S1，scratchpad/s1-grep.txt）。
 */
#ifndef GK3CORE_H
#define GK3CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ 错误码 */

typedef enum {
    GK3_OK = 0,
    GK3_EIO,        /* 回调读写失败 */
    GK3_EINVAL,     /* 参数不对 */
    GK3_EMAGIC,     /* 魔数不对 */
    GK3_EVERSION,   /* 版本不认识 */
    GK3_ECRC,       /* CRC 不对 */
    GK3_ERANGE,     /* 偏移 / 长度越界（含算术溢出） */
    GK3_ENOENT,     /* 按名字找不到 */
    GK3_EDUP,       /* 名字重复 —— 有歧义就不能用（§4.2 第 1 步） */
    GK3_ENOSPC,     /* 调用方给的缓冲区不够 */
    GK3_EVERIFY,    /* 写后读回不一致 */
    GK3_ESLOTS,     /* nb_slot 不是 2 */
} gk3_err;

const char *gk3_strerror(gk3_err e);

/* ------------------------------------------------------------------ 小工具（freestanding） */

void gk3_memcpy(void *dst, const void *src, size_t n);
void gk3_memset(void *dst, int c, size_t n);
int gk3_memcmp(const void *a, const void *b, size_t n);
size_t gk3_strnlen(const char *s, size_t max);
bool gk3_is_zero(const void *p, size_t n);

uint16_t gk3_le16(const uint8_t *p);
uint32_t gk3_le32(const uint8_t *p);
uint64_t gk3_le64(const uint8_t *p);
void gk3_put_le16(uint8_t *p, uint16_t v);
void gk3_put_le32(uint8_t *p, uint32_t v);
void gk3_put_le64(uint8_t *p, uint64_t v);

/* CRC-32/ISO-HDLC（zlib crc32，多项式 0xEDB88320 反射，初值与终值异或 0xFFFFFFFF）。
 * 与 libboot_control.cpp:50-71 的 CRC32()、GPT 规范、GBL crc32fast 是同一个算法。
 * 链式用法：crc = gk3_crc32(0, a, n); crc = gk3_crc32(crc, b, m); */
uint32_t gk3_crc32(uint32_t crc, const void *buf, size_t len);

/* SHA-1（FIPS 180-4），只用于 boot.img 头里的 id 校验（mkbootimg 的约定），不作安全用途。 */
typedef struct {
    uint32_t h[5];
    uint64_t len;
    uint8_t buf[64];
    uint32_t fill;
} gk3_sha1_ctx;
void gk3_sha1_init(gk3_sha1_ctx *c);
void gk3_sha1_update(gk3_sha1_ctx *c, const void *data, size_t len);
void gk3_sha1_final(gk3_sha1_ctx *c, uint8_t out[20]);

/* ------------------------------------------------------------------ 块设备抽象 */

/* UEFI 侧包 EFI_BLOCK_IO_PROTOCOL，Linux 侧包 pread/pwrite + fsync，测试里包内存。
 * read/write 以块为单位；返回 0 成功。write / flush 可以为 NULL（只读设备）。 */
typedef struct gk3_blk {
    void *ctx;
    uint32_t block_size;            /* 512 或 4096 */
    uint64_t num_blocks;
    int (*read)(void *ctx, uint64_t lba, uint32_t count, void *buf);
    int (*write)(void *ctx, uint64_t lba, uint32_t count, const void *buf);
    int (*flush)(void *ctx);
} gk3_blk;

/* 读分区内 [off, off+len) 的字节，off/len 不必对齐。scratch ≥ 一个块。 */
gk3_err gk3_blk_read_bytes(const gk3_blk *dev, uint64_t part_first_lba, uint64_t part_blocks,
                           uint64_t off, void *out, size_t len, void *scratch, size_t scratch_len);

/* 读-改-写：把 data 写到分区内 off 处（不必对齐，首尾块先读出来补齐），flush 后逐块读回比对。
 * 读回不一致返回 GK3_EVERIFY。scratch ≥ 两个块。 */
gk3_err gk3_blk_write_bytes_verify(const gk3_blk *dev, uint64_t part_first_lba, uint64_t part_blocks,
                                   uint64_t off, const void *data, size_t len,
                                   void *scratch, size_t scratch_len);

/* ------------------------------------------------------------------ GPT（只信主表） */

#define GK3_GPT_NAME_CHARS 36

typedef struct {
    uint8_t type_guid[16];          /* 盘上字节序（mixed-endian），原样 */
    uint8_t part_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;              /* 含 */
    uint64_t attrs;
    uint32_t index;                 /* 1 起，与 Linux 的 pN 一致 */
    char name[GK3_GPT_NAME_CHARS + 1]; /* 只收纯 ASCII 名字；含非 ASCII 的名字这里记成空串，不参与查找 */
} gk3_gpt_part;

typedef struct {
    uint32_t block_size;
    uint64_t my_lba, alt_lba, first_usable, last_usable, entries_lba;
    uint32_t num_entries, entry_size;
    uint8_t disk_guid[16];
    const uint8_t *entries;         /* 指向调用方的 entries 缓冲区 */
} gk3_gpt;

/* 解析主 GPT：hdr_block 是 LBA 1 的一整块；entries 是从 entries_lba 起读出的表
 * （长度 ≥ num_entries*entry_size，可先调 gk3_gpt_parse_header 得到要读多少）。
 * 校验：签名、revision 1.0、header_size ∈ [92, min(block_size,512)]、头 CRC、my_lba==1、
 *       usable 区间、表项大小 128·2^n、表 CRC、表不压 usable 区。备份表不读不修（§4.9）。
 * gk3_gpt 里的 entries 指针指回调用方的缓冲区，用完之前别释放。 */
gk3_err gk3_gpt_parse_header(const uint8_t *hdr_block, uint32_t block_size, gk3_gpt *out);
gk3_err gk3_gpt_parse_mem(const uint8_t *hdr_block, uint32_t block_size, const uint8_t *entries,
                          size_t entries_len, gk3_gpt *out);
/* 从块设备一步读完：entries_buf 至少 num_entries*entry_size 向上取整到块。 */
gk3_err gk3_gpt_read(const gk3_blk *dev, gk3_gpt *out, uint8_t *hdr_scratch,
                     uint8_t *entries_buf, size_t entries_buf_len);

uint32_t gk3_gpt_count(const gk3_gpt *g);                  /* 已用表项数（type 非零） */
gk3_err gk3_gpt_get(const gk3_gpt *g, uint32_t i, gk3_gpt_part *out); /* 第 i 个表项（0 起，含空项→ENOENT） */
/* 按名字唯一查找（大小写敏感、精确匹配）：0 个 → ENOENT，≥2 个 → EDUP。
 * 名字超出 usable 区或 first>last 的表项算坏表（ERANGE）。 */
gk3_err gk3_gpt_find(const gk3_gpt *g, const char *name, gk3_gpt_part *out);
/* 一组名字每个都恰好一个；失败时 *bad 指向出问题的那个名字。 */
gk3_err gk3_gpt_require_unique(const gk3_gpt *g, const char *const *names, size_t n, const char **bad);
/* GUID → "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"（小写，37 字节含 NUL） */
void gk3_guid_str(const uint8_t g[16], char out[37]);

/* ------------------------------------------------------------------ misc 布局 */

/* bootable/recovery bootloader_message.h:24-36（本机未设 BOARD_RECOVERY_BLDRMSG_OFFSET，= 0） */
#define GK3_MISC_BCB_OFF        0u
#define GK3_MISC_BCB_SIZE       2048u
#define GK3_MISC_BCAB_OFF       2048u   /* offsetof(bootloader_message_ab, slot_suffix)，libboot_control.cpp:48 */
#define GK3_MISC_BCAB_SIZE      32u
#define GK3_MISC_GK3_OFF        8192u   /* 设计稿 §4.5：vendor bootloader 区（2K–16K）里 */
#define GK3_MISC_GK3_SIZE       2048u
#define GK3_MISC_WIPE_OFF       16384u
#define GK3_MISC_SYSTEM_OFF     32768u  /* misc_virtual_ab_message 在系统区开头 */
#define GK3_MISC_READ_SIZE      65536u  /* 入口一次读 0–64 KiB（§4.2 第 2 步） */

/* ------------------------------------------------------------------ BCB（bootloader_message） */

/* bootloader_message.h:67-84：command[32] status[32] recovery[768] stage[32] reserved[1184] */
#define GK3_BCB_COMMAND_OFF   0u
#define GK3_BCB_COMMAND_LEN   32u
#define GK3_BCB_STATUS_OFF    32u
#define GK3_BCB_RECOVERY_OFF  64u
#define GK3_BCB_RECOVERY_LEN  768u
#define GK3_BCB_STAGE_OFF     832u

typedef enum {
    GK3_BCB_NONE = 0,       /* command 为空 → 正常启动 */
    GK3_BCB_BOOTLOADER,     /* bootonce-bootloader（adb reboot bootloader） */
    GK3_BCB_FASTBOOT,       /* boot-recovery + --fastboot（init/reboot.cpp），或 boot-fastboot */
    GK3_BCB_WIPE,           /* boot-recovery + --wipe_data（设置 → 清除所有数据） */
    GK3_BCB_PROMPT_WIPE,    /* boot-recovery + --prompt_and_wipe_data（RescueParty） */
    GK3_BCB_RECOVERY,       /* 其他 boot-recovery（含无参数、--update_package、--sideload、--wipe_cache、--rescue…） */
    GK3_BCB_UNKNOWN,        /* boot-quiescent、boot-rescue、乱码、command 没有 NUL → 记录并清掉（§4.3.4） */
} gk3_bcb_kind;

typedef struct {
    gk3_bcb_kind kind;
    char command[GK3_BCB_COMMAND_LEN + 1];  /* 原文（截到第一个 NUL；无 NUL 时 32 字节全收） */
    bool command_terminated;                /* command 字段里有 NUL */
    bool has_reason;                        /* recovery 里带 --reason= */
    uint32_t n_args;                        /* recovery 字段 "recovery\n" 之后的参数行数 */
    uint8_t digest[20];                     /* SHA-1(整个 2048 字节 BCB)，GK3 记录用它认"同一份 BCB" */
} gk3_bcb_info;

/* bcb 指向 misc 偏移 0 的 2048 字节。 */
void gk3_bcb_classify(const uint8_t *bcb, gk3_bcb_info *out);
const char *gk3_bcb_kind_name(gk3_bcb_kind k);
/* 只清 command（GBL / fastbootd 的"进入时就清"语义）。 */
void gk3_bcb_clear_command(uint8_t *bcb);
/* 整个 2048 字节清零（= recovery 的 clear_bootloader_message）。 */
void gk3_bcb_clear(uint8_t *bcb);
/* 按 update_bootloader_message_in_struct（bootloader_message.cpp:214-232）的格式写：
 * command="boot-recovery"，recovery="recovery\n" + 每个参数一行；status / stage 保留。主要给测试和执行端用。 */
gk3_err gk3_bcb_write_recovery(uint8_t *bcb, const char *const *args, size_t nargs);

/* ------------------------------------------------------------------ bootloader_control（BCAB） */

/* boot_control_definition.h:59-107（hardware/interfaces 1a56e38；crDroid 树已核对）。
 * 盘上 32 字节：
 *   0  slot_suffix[4]
 *   4  magic u32 = 0x42414342（字节 42 43 41 42）
 *   8  version u8 = 1
 *   9  u16 位：nb_slot[0:2] recovery_tries_remaining[3:5] merge_status[6:8]（跨 9、10 两字节）
 *   11 reserved0[1]
 *   12 slot_info[4]，每槽 u16：priority[0:3] tries_remaining[4:6] successful_boot[7]
 *                              verity_corrupted[8] reserved[9:15]
 *   20 reserved1[8]
 *   28 crc32_le = CRC32(前 28 字节)，小端
 * 位号与 GBL libgbl/src/slots/android.rs:77-87、146-153 的掩码 / 偏移一致。 */
#define GK3_BCAB_MAGIC   0x42414342u
#define GK3_BCAB_VERSION 1u
#define GK3_BCAB_MAX_SLOTS 4u

/* libboot_control.cpp:42、:292-293 */
#define GK3_DEFAULT_BOOT_ATTEMPTS 7u
#define GK3_ACTIVE_PRIORITY 15u
#define GK3_ACTIVE_TRIES 6u

typedef struct {
    uint8_t priority;       /* 0–15，0 = 不可启动 */
    uint8_t tries;          /* 0–7 */
    bool successful;
    bool verity_corrupted;
} gk3_slot_info;

typedef enum {
    GK3_MERGE_NONE = 0, GK3_MERGE_UNKNOWN = 1, GK3_MERGE_SNAPSHOTTED = 2,
    GK3_MERGE_MERGING = 3, GK3_MERGE_CANCELLED = 4,
} gk3_merge_status;  /* IBootControl 1.1 MergeStatus */

/* 校验：magic、version==1、CRC、nb_slot==2（§4.3.2 第 1 条）。CRC 在 version 之后查，
 * 所以返回值能分出是哪一项坏了。 */
gk3_err gk3_bcab_validate(const uint8_t bc[32]);
uint32_t gk3_bcab_crc(const uint8_t bc[32]);
void gk3_bcab_update_crc(uint8_t bc[32]);
uint8_t gk3_bcab_nb_slot(const uint8_t bc[32]);
uint8_t gk3_bcab_merge_status(const uint8_t bc[32]);
uint8_t gk3_bcab_recovery_tries(const uint8_t bc[32]);
void gk3_bcab_get_slot(const uint8_t bc[32], unsigned slot, gk3_slot_info *out);
/* 只改这一槽的这些位，其余位（reserved 等）原样保留；不重算 CRC。 */
void gk3_bcab_set_slot(uint8_t bc[32], unsigned slot, const gk3_slot_info *in);

/* —— libboot_control 原语（逐字节对拍 test_upstream.cpp）；都会重算 CRC —— */
/* SetActiveBootSlot（:282-314）：其他槽 ≥15 的降到 14；目标槽 15/6；目标 != current 时清 verity_corrupted。 */
gk3_err gk3_bcab_set_active(uint8_t bc[32], unsigned slot, unsigned current_slot);
/* MarkBootSuccessful（:252-262）：successful=1、tries=1。 */
gk3_err gk3_bcab_mark_successful(uint8_t bc[32], unsigned slot);
/* SetSlotAsUnbootable（:316-330）：successful=0、tries=0。 */
gk3_err gk3_bcab_set_unbootable(uint8_t bc[32], unsigned slot);
/* InitDefaultBootloaderControl（:115-182）：CRC 坏时 HAL 重建的那份；nb_slot 由调用方给（HAL 是 stat boot_x 数出来的）。 */
void gk3_bcab_init_default(uint8_t bc[32], unsigned current_slot, unsigned nb_slot);
/* 安装器初始化（§4.7）：目标槽 15/6/未成功，另一槽 0/0；nb_slot=2，suffix=目标槽。 */
void gk3_bcab_init_install(uint8_t bc[32], unsigned slot);

/* —— 入口侧：选槽与扣 tries（§4.3.2，选法按 GBL slots/android.rs:280-305、:328-356） —— */

/* 可启动 = priority>0 且 (tries>0 或 successful)。
 * ⚠️ 与 GBL 差一点：GBL 不看 priority（priority 0 但 tries>0 的槽它也会选）；
 *    boot_control_definition.h:63-64 写明 "0 the slot is unbootable"，这里从严。 */
bool gk3_slot_bootable(const gk3_slot_info *s);

typedef enum {
    GK3_SEL_BOOT = 0,       /* 启动 slot */
    GK3_SEL_BCAB_INVALID,   /* BCAB 坏：不写，按 hint 启动（slot = hint） */
    GK3_SEL_NOSLOT,         /* 两槽都不可启动 → 执行端 why=noslot */
    GK3_SEL_MERGING,        /* MERGING 且 active 槽不可启动 → 不回落，执行端 why=merging */
} gk3_sel_kind;

typedef struct {
    gk3_sel_kind kind;
    gk3_err bcab_err;       /* kind==BCAB_INVALID 时是哪一项坏了 */
    unsigned slot;          /* 要启动的槽（0=_a 1=_b） */
    unsigned active;        /* priority 最高的槽（不论可否启动）；slot != active ⇒ 回落 */
    bool fallback;          /* 因 active 不可启动而换了槽 → event=fallback */
    bool decremented;       /* 扣了 tries，调用方要把 bc 写回 misc（写后读回） */
    uint8_t tries_before, tries_after;
} gk3_sel;

/* bc 就地修改（只有 decremented 时才改，且只改该槽 tries + CRC）。
 * merge_status 取自 virtual_ab 消息（gk3_vab_*），已按 source_slot 规则处理过的那个值。 */
void gk3_select_slot(uint8_t bc[32], unsigned hint, uint8_t merge_status, gk3_sel *out);

/* ------------------------------------------------------------------ virtual_ab（只读） */

/* bootloader_message.h:88-94、:118-119：version u8 / magic u32 / merge_status u8 / source_slot u8 / reserved[57] */
#define GK3_VAB_MAGIC   0x56740AB0u
#define GK3_VAB_VERSION 2u

typedef struct {
    bool valid;             /* version==2 且 magic 对 */
    uint8_t version;
    uint32_t magic;
    uint8_t merge_status;
    uint8_t source_slot;
} gk3_vab;

/* msg 指向 misc 偏移 32 KiB 的 64 字节 */
void gk3_vab_parse(const uint8_t *msg, gk3_vab *out);
/* libboot_control.cpp:422-440：SNAPSHOTTED 且 current_slot==source_slot 视为 NONE；无效消息视为 UNKNOWN。 */
uint8_t gk3_vab_effective(const gk3_vab *v, unsigned current_slot);

/* ------------------------------------------------------------------ GK3 记录（misc 8 KiB，§4.5） */

/* 设计稿只列了字段，没定字节布局；这里是 v1 的定义（见 tools/gk3boot/README.md）。2048 字节：
 *   0    magic "GK3R"（u32 0x52334B47）
 *   4    version u16 = 1
 *   6    size u16 = 2048
 *   8    flags u32：bit0 已迁移；bit1 回落中；bit2 clean_poweroff（S15：Windows 为默认时从 Android 关机，
 *        下次冷开机由入口转去 Windows，§4.9.3）
 *   12   dispatch_ver u32：迁移时的"分派版本"（§4.10）
 *   16   seq u32：事件序号计数
 *   20   boot_streak u8：连续未完成启动（入口 +1，开机完成清零）
 *   21   next_kind u8：一次性意图 0=none 1=sdboot-menu 2=slot 3=windows（S15）
 *   22   next_slot u8
 *   23   dispatch_why u8（gk3_bcb_kind）
 *   24   dispatch_slot u8
 *   25   dispatch_count u8：同一份 BCB 连续进入次数（wipe 3 次上限，§4.3.4）
 *   26   ev_head u8：事件环下一个写入位置
 *   27   ok_streak u8：【已确认的槽】连续未完成启动（bootloop 判据，S7c；入口维护：上一次开机完成过
 *        （boot_streak 已被 HAL 清零）就从 0 数；启动未确认的槽（扣 tries 的那条路）清零；进执行端清零）
 *   28   dispatch_digest[20]：分派时 BCB 的 SHA-1
 *   48   migrated_digest[20]：迁移时被清掉的 BCB 的 SHA-1
 *   68   migrated_command[32]：原文
 *   100  migrated_recovery[256]：原文（截断）
 *   356  bcb_seen u32（见 gk3_rec_bcb_seen）
 *   360  set_default u8（S15）：一次性"设默认系统"请求 0=无 1=windows 2=android（HAL 应 Parts 请求写，入口消费后清零）
 *   361  default_os u8（S15）：默认系统的缓存 0=未知 1=android 2=windows（入口读 LoaderEntryDefault 后写，
 *        只在值变化时变；HAL 的 mark-poweroff 与 Parts 的显示读它 —— Android 不挂 efivarfs，§4.5）
 *   362  reserved[...]（全零；v1 的老读者不看这些字节，老写者整份原样带过 ⇒ 扩字段不升版本）
 *   1024 events[GK3_EV_N]，每条 16 字节：seq u32 / code u16 / slot u8 / flags u8（bit0 已通知）/ aux u32 / reserved u32
 *   2044 crc32 u32 = CRC32(前 2044 字节)
 */
#define GK3_REC_MAGIC   0x52334B47u
#define GK3_REC_VERSION 1u
#define GK3_REC_SIZE    2048u
#define GK3_EV_N        32u

typedef enum {
    GK3_EV_NONE = 0, GK3_EV_FALLBACK, GK3_EV_BOOT_CORRUPT, GK3_EV_BCB_DROPPED,
    GK3_EV_WIPE_FAILED, GK3_EV_REFUSED_MERGING, GK3_EV_BOOTLOOP, GK3_EV_NOSLOT,
    GK3_EV_MIGRATED,
    GK3_EV_BCB_IGNORED,     /* 分派开关关着时看到一份新的非空 BCB：只记录、不消费（aux = gk3_bcb_kind） */
    /* —— S15 双系统（§4.9.3、§4.9.4）—— */
    GK3_EV_DEFAULT_RESET,   /* 10 LoaderEntryDefault 不是合法值（不是 Windows 条目 id），入口删掉了它（= 默认 Android） */
    GK3_EV_INTENT_DROPPED,  /* 11 双系统意图作废：aux = gk3_intent（哪一个），slot = gk3_drop_why（为什么） */
    GK3_EV_TO_WINDOWS,      /* 12 入口转去了 Windows：aux = gk3_intent（next=windows 还是 clean_poweroff） */
    GK3_EV_DEFAULT_SET,     /* 13 应 set_default 请求改了默认系统：aux = gk3_os（改成了什么），slot = 0 成功 / 1 写变量失败 */
} gk3_ev_code;

typedef enum {
    GK3_NEXT_NONE = 0, GK3_NEXT_SDBOOT_MENU = 1, GK3_NEXT_SLOT = 2,
    GK3_NEXT_WINDOWS = 3,   /* S15：下一次开机去 Windows 一次（Parts "重启到 Windows"，§4.9.4） */
} gk3_next_kind;

/* S15：一次性"设默认系统"请求（偏移 360）与默认系统缓存（偏移 361） */
typedef enum { GK3_SETDEF_NONE = 0, GK3_SETDEF_WINDOWS = 1, GK3_SETDEF_ANDROID = 2 } gk3_setdef;
typedef enum { GK3_OS_UNKNOWN = 0, GK3_OS_ANDROID = 1, GK3_OS_WINDOWS = 2 } gk3_os;
/* 事件 INTENT_DROPPED / TO_WINDOWS 的 aux */
typedef enum { GK3_INTENT_NEXT_WINDOWS = 1, GK3_INTENT_CLEAN_POWEROFF = 2, GK3_INTENT_SET_DEFAULT = 3 } gk3_intent;
/* 事件 INTENT_DROPPED 的 slot 字段：为什么作废 */
typedef enum {
    GK3_DROP_ANDROID_PENDING = 1,   /* Android 有待办（BCB 要执行、新槽 tries 计数中、上次没开机完成、要进执行端、next=slot/sdboot-menu…） */
    GK3_DROP_NO_WINDOWS = 2,        /* ESP 上没有 \EFI\Microsoft\Boot\bootmgfw.efi */
    GK3_DROP_VAR_FAILED = 3,        /* 写 LoaderEntryOneShot 失败（入口改为启动 Android） */
} gk3_drop_why;

#define GK3_REC_F_MIGRATED    0x1u
#define GK3_REC_F_IN_FALLBACK 0x2u  /* 上一次是回落启动：回落只在"进入"的那一次记事件、写 ESP 日志 */
#define GK3_REC_F_CLEAN_POWEROFF 0x4u /* S15：Windows 为默认时从 Android 关机（vendor rc 的 on shutdown 写，入口消费） */
#define GK3_EVF_NOTIFIED      0x1u

typedef struct {
    uint32_t seq;
    uint16_t code;
    uint8_t slot;
    uint8_t flags;
    uint32_t aux;
} gk3_event;

/* 有效 = magic、version、size、CRC 都对。无效视为"无记录"，不影响启动（§4.5）。 */
gk3_err gk3_rec_validate(const uint8_t rec[GK3_REC_SIZE]);
/* 全新一份（全零 + 头），不设迁移标记。 */
void gk3_rec_init(uint8_t rec[GK3_REC_SIZE]);
void gk3_rec_seal(uint8_t rec[GK3_REC_SIZE]);           /* 重算 CRC，写盘前必调 */

uint32_t gk3_rec_flags(const uint8_t *rec);
bool gk3_rec_migrated(const uint8_t *rec);
/* 置 / 清 flags 里的一位（不碰其他位） */
void gk3_rec_set_flag(uint8_t *rec, uint32_t flag, bool on);
/* 偏移 356：分派开关关着时，上一次"看到但没消费"的那份 BCB 的 CRC32（0 = 没有）。
 * 只用来让同一份 BCB 只记一次事件 / 只写一次 ESP 日志（README §11）。 */
uint32_t gk3_rec_bcb_seen(const uint8_t *rec);
void gk3_rec_set_bcb_seen(uint8_t *rec, uint32_t crc);
/* 首跑迁移（§4.10）：把 bcb 原文 / 摘要抄进记录、置迁移标记与分派版本、记一条 MIGRATED（BCB 非空时
 * 另记 BCB_DROPPED）。不碰 bcb 本身 —— 清 BCB 是调用方的事（gk3_bcb_clear）。 */
void gk3_rec_migrate(uint8_t *rec, const uint8_t *bcb, uint32_t dispatch_ver);
void gk3_rec_migrated_command(const uint8_t *rec, char out[33]);

uint8_t gk3_rec_boot_streak(const uint8_t *rec);
void gk3_rec_set_boot_streak(uint8_t *rec, uint8_t v);
/* +1，饱和在 255 */
uint8_t gk3_rec_inc_boot_streak(uint8_t *rec);

gk3_next_kind gk3_rec_next(const uint8_t *rec, uint8_t *slot);
void gk3_rec_set_next(uint8_t *rec, gk3_next_kind k, uint8_t slot);

/* S15：set_default 请求 / 默认系统缓存（不认识的值读成 NONE / UNKNOWN）。只改调用方的缓冲区，不重算 CRC。 */
gk3_setdef gk3_rec_set_default_req(const uint8_t *rec);
void gk3_rec_put_set_default_req(uint8_t *rec, gk3_setdef v);
gk3_os gk3_rec_default_os(const uint8_t *rec);
void gk3_rec_put_default_os(uint8_t *rec, gk3_os v);
bool gk3_rec_clean_poweroff(const uint8_t *rec);
/* "现在实际上默认是不是 Windows"：有 set_default 请求就按请求（入口下次运行时先应用它，§4.9.3 b），否则看缓存。
 * HAL 的 mark-poweroff 与 Parts 的显示都用这一条规则。 */
bool gk3_rec_effective_default_windows(const uint8_t *rec);
/* mark-poweroff（§4.9.3"冷开机进 Windows"）：sys.powerctl 以 "shutdown" 开头、且实际默认是 Windows ⇒ 置
 * clean_poweroff 并返回 true（调用方 seal + 写回）；否则什么都不改、返回 false。rec 必须是有效记录。 */
bool gk3_rec_mark_poweroff(uint8_t *rec, const char *powerctl);

/* 分派计数：同一份 BCB（摘要相同）就 +1 并返回新值，不同就重置为 1。 */
uint8_t gk3_rec_dispatch_enter(uint8_t *rec, gk3_bcb_kind why, uint8_t slot, const uint8_t digest[20]);
uint8_t gk3_rec_dispatch_count(const uint8_t *rec);
gk3_bcb_kind gk3_rec_dispatch_why(const uint8_t *rec);
/* 已确认槽的连续未完成启动（偏移 27）。boot_streak 的问题：OTA 新槽 6 次 tries 也会把它推过阈值，
 * 而那 6 次本该由 tries 自动回滚（E8）；bootloop 只管"已确认的槽之后反复起不来"（§4.3.3），所以另记一个。 */
uint8_t gk3_rec_ok_streak(const uint8_t *rec);
void gk3_rec_set_ok_streak(uint8_t *rec, uint8_t v);
void gk3_rec_dispatch_digest(const uint8_t *rec, uint8_t out[20]);
void gk3_rec_dispatch_reset(uint8_t *rec);

/* ------------------------------------------------------------------ BCB 分派（§4.3.4、§4.10），只决定、不写盘 */

#define GK3_DISPATCH_VER     1u     /* 迁移标记里记的"分派版本"（§4.10）；打开分派的那一版入口用它 */
#define GK3_WIPE_MAX_ENTRIES 3u     /* 同一份 wipe BCB 进执行端的上限，超过 → 入口自己清、event=wipe_failed */
#define GK3_BOOTLOOP_THRESHOLD 5u   /* 已确认的槽连续这么多次没走到开机完成 → 下一次进执行端菜单 why=bootloop（§4.3.3） */

typedef enum {
    GK3_DISP_NONE = 0,      /* BCB 空：正常启动（若有旧的分派计数就清掉） */
    GK3_DISP_MIGRATE,       /* 还没迁移：只清 BCB、不执行，置迁移标记（§4.10） */
    GK3_DISP_CLEAR,         /* boot-quiescent / boot-rescue / 乱码：原文记进 GK3、清掉、正常启动 */
    GK3_DISP_EXECUTOR,      /* 进执行端，why = kind */
    GK3_DISP_WIPE_CAP,      /* 同一份 wipe BCB 已进入 3 次仍没被清：入口自己清、event=wipe_failed、正常启动 */
} gk3_disp_action;

typedef struct {
    gk3_disp_action action;
    gk3_bcb_kind why;
    bool clear_bcb_first;       /* bootloader / fastboot / recovery：进执行端之前先把整份 BCB 清掉写回（GBL 的"先清再进"语义，
                                 * 执行端坏了 / 崩了也不会循环；= fastbootd 进入时的 clear_bootloader_message）。
                                 * wipe / prompt_wipe 不清：要留给执行端核对、擦完再清（可重入） */
    uint8_t count;              /* EXECUTOR / WIPE_CAP：这份 BCB 第几次进入（gk3_rec_dispatch_enter 之后的值） */
} gk3_disp_plan;

/* 按 §4.3.4 的表决定这份 BCB 怎么处理。rec 必须是有效记录（无效时调用方先 gk3_rec_init）；
 * EXECUTOR / WIPE_CAP 分支会调 gk3_rec_dispatch_enter 改 rec 里的分派计数，NONE 分支会清掉旧计数 ——
 * 改的都只是调用方的缓冲区，写不写回由调用方定。不碰 BCB 本身。 */
void gk3_dispatch_plan(const gk3_bcb_info *bi, uint8_t *rec, uint8_t slot, gk3_disp_plan *out);
const char *gk3_disp_name(gk3_disp_action a);

/* 事件环：追加（覆盖最旧的），返回分到的 seq。 */
uint32_t gk3_rec_event_add(uint8_t *rec, gk3_ev_code code, uint8_t slot, uint32_t aux);
/* 按从旧到新取第 i 条（i < n）；返回环里有效的事件条数。 */
uint32_t gk3_rec_events(const uint8_t *rec, gk3_event *out, uint32_t max);
/* 把 seq ≤ upto 的事件都置"已通知"。 */
void gk3_rec_events_mark_notified(uint8_t *rec, uint32_t upto_seq);
const char *gk3_ev_name(gk3_ev_code c);

/* ------------------------------------------------------------------ 双系统，只决定、不写盘 */

/* LoaderEntryDefault 的值分三类：
 *   ABSENT   不存在 = 默认 Android（合法）
 *   WINDOWS  Windows 条目的 id：systemd-boot 自动生成的 "auto-windows"（boot.c:2147），或安装器自写的 type1 条目
 *            "gk3-windows.conf"（type1 的 id = 文件名、转小写，boot.c:1362、:1540-1541）—— 合法
 *   OTHER    其他一切（Android 条目的精确 id、@saved、live / 救援、通配…）—— 不合法，入口删掉
 * 比较不分大小写（systemd-boot 读入时转小写，boot.c:1645）。value 为 NULL = 不存在。 */
typedef enum { GK3_DEFVAR_ABSENT = 0, GK3_DEFVAR_WINDOWS, GK3_DEFVAR_OTHER } gk3_defvar;
gk3_defvar gk3_defvar_classify(const char *value);

typedef enum { GK3_DEFACT_NONE = 0, GK3_DEFACT_DELETE, GK3_DEFACT_SET_WINDOWS } gk3_defact;

typedef struct {
    gk3_defact action;          /* 入口要对 LoaderEntryDefault 做什么 */
    bool default_windows;       /* action 成功之后：默认是不是 Windows（= 变量是 Windows id 且 bootmgfw 在） */
    bool default_windows_before;/* action 失败时保持的值（= 入口进来时的样子） */
    bool reset;                 /* 删的是不合法的值（记了 DEFAULT_RESET） */
    gk3_setdef applied;         /* 这次消费掉的 set_default 请求（NONE = 没有） */
    bool dropped;               /* set_default=windows 但没有 Windows：作废（记了 INTENT_DROPPED） */
} gk3_def_plan;

/* §4.9.3 判定顺序的 a、b 两步：先把不合法的值删掉，再应用 set_default 请求（请求清零、记事件）。
 * windows_present = ESP 上有 \EFI\Microsoft\Boot\bootmgfw.efi。rec 必须是有效记录；只改 rec 缓冲区（不碰缓存 ——
 * 变量写没写成只有调用方知道，调用方之后用 gk3_rec_put_default_os 记结果）。 */
void gk3_dual_plan_default(uint8_t *rec, gk3_defvar var, bool windows_present, gk3_def_plan *out);

typedef enum {
    GK3_DUAL_ANDROID = 0,       /* 照常启动 Android（或执行端） */
    GK3_DUAL_WINDOWS,           /* 写 OneShot = Windows 条目 → 复位（标记已在 rec 里清掉，调用方先写 rec 再写变量） */
    GK3_DUAL_SDBOOT_MENU,       /* next=sdboot-menu：清掉后返回 EFI_SUCCESS，systemd-boot 停在不倒计时的菜单上（boot.c:2971-2976） */
} gk3_dual_kind;

typedef struct {
    gk3_dual_kind kind;
    gk3_intent why;             /* kind == WINDOWS：哪个意图（next=windows 优先） */
    bool preset_oneshot;        /* kind == ANDROID 且默认是 Windows：交接前预置 LoaderEntryOneShot（§4.9.3"预置 OneShot"） */
    uint8_t dropped;            /* 作废了哪些意图：bit0 next=windows、bit1 clean_poweroff（各记了一条 INTENT_DROPPED） */
} gk3_dual_boot;

/* §4.9.3 判定顺序的 c、d、e 三步（加上 §4.4.4 的 next=sdboot-menu）。android_pending 由调用方算（BCB 这一版会执行、
 * 选中的槽在扣 tries、上次没开机完成、这次要进执行端、强制槽……）；next=slot:x 在这里也算待办。
 * 消费掉的标记（next=windows / sdboot-menu、clean_poweroff）一律在 rec 里清掉 —— 去 Windows 之前先清（写盘），
 * 最坏多一次复位、不会循环。clean_poweroff 而默认不是 Windows：静默清掉。只改 rec 缓冲区。 */
void gk3_dual_plan_boot(uint8_t *rec, bool default_windows, bool windows_present, bool android_pending,
                        gk3_dual_boot *out);

/* ------------------------------------------------------------------ boot.img v0–v2 */

#define GK3_BOOT_MAGIC "ANDROID!"
#define GK3_BOOT_ARGS_SIZE 512u
#define GK3_BOOT_EXTRA_ARGS_SIZE 1024u
#define GK3_BOOT_HDR_V1_SIZE 1648u
#define GK3_BOOT_HDR_V2_SIZE 1660u

typedef struct {
    uint32_t version, page_size;
    uint32_t kernel_size, ramdisk_size, second_size, recovery_dtbo_size, dtb_size;
    uint64_t kernel_off, ramdisk_off, second_off, recovery_dtbo_off, dtb_off;
    uint64_t total_size;            /* 各段按页对齐后的总长 */
    uint32_t os_version;
    char name[17];
    uint8_t id[32];                 /* 头里的 id；前 20 字节是 SHA-1 */
    const uint8_t *hdr;             /* 指回调用方的头缓冲区（cmdline 从这里取） */
} gk3_bootimg;

/* hdr 至少 1660 字节（v2；v0/v1 至少 1648/1632）。part_size：所在分区的字节数（0 = 不查）。
 * 校验 magic、版本 0–2、page_size ∈ {2048,4096,8192,16384}、v1+ 的 header_size、各段不越界不溢出。 */
gk3_err gk3_bootimg_parse(const uint8_t *hdr, size_t hdr_len, uint64_t part_size, gk3_bootimg *out);
/* 按 mkbootimg 复算 id：SHA1(kernel‖le32 len, ramdisk‖len, second‖len[, recovery_dtbo‖len][, dtb‖len])。
 * img 是整个镜像（≥ total_size）。不一致返回 GK3_EVERIFY。 */
gk3_err gk3_bootimg_verify_id(const gk3_bootimg *b, const uint8_t *img, size_t img_len, uint8_t got[20]);

/* ------------------------------------------------------------------ cmdline（§4.3.1、§4.4.1） */

/* 头里的 cmdline（512，NUL 结尾）直接接 extra_cmdline（1024）—— mkbootimg 把超过 511 字符的部分
 * 原样切到 extra，所以中间不加空格。返回长度；out 不够返回 -1。 */
long gk3_bootimg_cmdline(const gk3_bootimg *b, char *out, size_t out_len);

typedef struct {
    unsigned slot;              /* 0/1 → androidboot.slot_suffix=_a/_b（必需，E-K3） */
    const char *bootloader;     /* "gk3boot-<ver>"，NULL 不加 */
    const char *event;          /* androidboot.gk3boot.event，NULL 不加 */
    const char *entry;          /* androidboot.gk3boot.entry（自己条目的文件名），NULL 不加 */
    const char *mode;           /* androidboot.gk3boot.mode=observe|action（E4/E5 的观察模式要能从 Android 侧认出来），NULL 不加 */
    const char *streak;         /* androidboot.gk3boot.streak=<GK3 连续未完成启动计数>（动作模式），NULL 不加 */
    const char *dispatch;       /* androidboot.gk3boot.dispatch=1：动作模式且 BCB 分派开着（S15：HAL 据此决定 Parts 的
                                 * "重启到引导菜单"有没有用 —— 分派关时 reboot,bootloader 写的 BCB 只记录不执行）；NULL 不加 */
} gk3_android_args;

/* Android 交接用：base 里已有的同名键先删掉（避免重复），再按顺序追加。值里只允许
 * [A-Za-z0-9._+-:,=/]，否则 GK3_EINVAL（不给 cmdline 注入空格或引号的机会）。 */
gk3_err gk3_cmdline_android(const char *base, const gk3_android_args *a, char *out, size_t out_len);

typedef struct {
    const char *why;            /* gk3.why */
    unsigned slot;              /* gk3.slot */
    const char *bootver;        /* gk3.bootver */
    const char *disk;           /* gk3.disk = misc 的 PARTUUID */
    const char *esp;            /* gk3.esp = 入口自己所在 ESP 的 PARTUUID（S7c），NULL 不加 */
    bool dispatch;              /* gk3.dispatch=1：入口开着 BCB 分派（执行端据此决定重启类命令写不写 BCB） */
    bool fbtcp;                 /* gk3.fbtcp=1：执行端打开 TCP 5554（只来自条目 options，开发用；不认证） */
} gk3_fastboot_args;

/* 执行端用（§4.4.1）：去掉 androidboot.*、init=、firmware_class.path=（同 installer-lib.sh
 * gk3__rescue_cmdline）、deferred_probe_timeout=、panic=、gk3.*，再追加
 * panic=10 gk3.mode=fastboot gk3.why= gk3.slot= gk3.bootver= gk3.disk= [gk3.esp=] [gk3.dispatch=1] [gk3.fbtcp=1]。 */
gk3_err gk3_cmdline_fastboot(const char *base, const gk3_fastboot_args *a, char *out, size_t out_len);

/* EFI LoadOptions：ASCII → UCS-2（含结尾 NUL）。非 ASCII 返回 GK3_EINVAL。out_chars 含 NUL。 */
gk3_err gk3_ascii_to_ucs2(const char *s, uint16_t *out, size_t out_chars);

#ifdef __cplusplus
}
#endif
#endif /* GK3CORE_H */
