/* GPT 的写：新建一张空表、往主表里加分区、由主表头生成备份表头。读与校验在 libgk3core（gk3_gpt_*），这里只管写出
 * 一份它（以及固件、内核、sgdisk）认的表。Windows 侧由系统的 IOCTL 改分区表，用不到这一层；Linux 侧（gk3l）自己写。
 *
 * 盘上格式（UEFI 规范 2.10 §5.3）：
 *   LBA 0          保护性 MBR（一个 0xEE 分区盖住整块盘）
 *   LBA 1          主表头；表项从 LBA 2 起，128 项 × 128 字节
 *   最后几块        备份表项，最后一块是备份表头（my_lba / alt_lba 对调，表项位置指向备份表项）
 * 表头与表项各有 CRC32（与 libgk3core 同一个 gk3_crc32）。 */
#ifndef GK3W_GPTW_H
#define GK3W_GPTW_H

#include <stddef.h>
#include <stdint.h>

#define GK3W_GPT_ENTRIES 128u
#define GK3W_GPT_ENTRY_SIZE 128u
#define GK3W_GPT_HDR_SIZE 92u

typedef struct {
    const char *name;           /* ASCII 可打印字符，最多 36 个 */
    uint8_t type[16];           /* 盘上字节序（mixed-endian） */
    uint8_t uuid[16];
    uint64_t first, last;       /* 含两端，逻辑扇区 */
    uint64_t attrs;
} gk3w_gpt_new;

typedef enum {
    GK3W_GPTW_OK = 0,
    GK3W_GPTW_EINVAL,           /* 参数不对：块大小、名字、主表本身校验不过 */
    GK3W_GPTW_FULL,             /* 空表项不够 */
    GK3W_GPTW_RANGE,            /* 新分区首大于尾，或超出 usable 区间 */
    GK3W_GPTW_OVERLAP,          /* 新分区与已有分区或彼此重叠 */
    GK3W_GPTW_LAYOUT,           /* 主表头的位置字段不合常规（备份表项放不下） */
} gk3w_gptw_err;

const char *gk3w_gptw_strerror(gk3w_gptw_err e);

/* 文本 GUID（"0FC63DAF-8483-4772-8E79-3D69D8477DE4"，大小写都行）→ 盘上字节序。格式不对返回 -1 */
int gk3w_guid_parse(const char *s, uint8_t out[16]);
/* 随机字节 → RFC 4122 第 4 版 GUID（就地改 version / variant 位）的盘上字节序 */
void gk3w_guid_v4(uint8_t g[16]);

/* 新建一张空 GPT：pmbr = LBA 0（一整块），hdr = LBA 1（一整块），entries = 表项（GK3W_GPT_ENTRIES × GK3W_GPT_ENTRY_SIZE 字节，
 * 全零）。total_blocks 是盘的逻辑块数，disk_guid 是盘的 GUID（调用方生成）。 */
gk3w_gptw_err gk3w_gpt_init(uint32_t bs, uint64_t total_blocks, const uint8_t disk_guid[16], uint8_t *pmbr, uint8_t *hdr,
                            uint8_t *entries);

/* 往主表加 n 个分区：依次放进序号最小的空表项，*idx 返回它们的序号（1 起，与 Linux 的 pN 一致），重算表项 CRC 与
 * 表头 CRC。hdr 是 LBA 1 的整块，entries 是从表项起点读出的整个表（≥ num_entries × entry_size）。出错时什么都不改。 */
gk3w_gptw_err gk3w_gpt_add(uint8_t *hdr, uint32_t bs, uint8_t *entries, size_t entries_len, const gk3w_gpt_new *np, size_t n,
                           uint32_t *idx);

/* 由主表头生成备份表头（一整块 bak_hdr，写到主表头的 alt_lba），*entries_lba 是备份表项的位置
 * （紧贴在备份表头之前，必须在 usable 区间之后）。备份表项与主表项逐字节相同。 */
gk3w_gptw_err gk3w_gpt_make_backup(const uint8_t *hdr, uint32_t bs, uint8_t *bak_hdr, uint64_t *entries_lba);

#endif
