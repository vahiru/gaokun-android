/* FAT32 写入器：在一段分区里一次写出完整的 FAT32 文件系统与给定的文件，不读分区里原有的内容。
 * 用途：Windows 安装器制作安装介质（U 盘上的 FAT32 分区、本机磁盘上的 GK3LIVE）。Windows 自带的格式化不给 32 GB 以上的卷
 * 用 FAT32，也不会给 XBOOTLDR 类型的分区建卷，所以由我们自己写。
 * 布局：保留区之后是两份 FAT，数据区起点按 1 MiB 对齐，根目录在簇 2；先放目录、再放文件，每个对象占连续的簇。
 * 名字不是大写 8.3 的都带 VFAT 长文件名，另生成同一目录里不重复的 8.3 短名。簇大小与 Windows 格式化 FAT32 的默认一致，
 * 簇数不足 FAT32 的下限时减小簇。格式见 Microsoft 的 FAT 规范（fatgen103："FAT: General Overview of On-Disk Format"）。 */
#ifndef GK3W_FATW_H
#define GK3W_FATW_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *path;           /* 相对根目录、用 / 分隔的 UTF-8 路径；目录不用单独列出，按文件路径自动建 */
    uint64_t size;
    void *src;                  /* 交给 read 回调，用来区分是哪个文件 */
} gk3w_fat_file;

typedef struct {
    void *ctx;
    /* 写到分区内偏移 off 处；off 与 len 都是扇区大小的整数倍，buf 按 4096 字节对齐。返回 0 成功 */
    int (*write)(void *ctx, uint64_t off, const void *buf, size_t len);
    /* 读文件 f 从 off 起的 len 字节到 buf。返回 0 成功 */
    int (*read)(void *ctx, const gk3w_fat_file *f, uint64_t off, void *buf, size_t len);
    /* 进度：已写入的文件内容字节数与总数（可为 NULL） */
    void (*progress)(void *ctx, uint64_t done, uint64_t total);
} gk3w_fat_io;

typedef struct {
    uint64_t part_bytes;        /* 分区大小 */
    uint32_t sector;            /* 逻辑扇区大小：512 或 4096 */
    uint32_t hidden;            /* 分区起点（以扇区计），写进 BPB_HiddSec */
    char label[12];             /* 卷标：大写 ASCII，最多 11 个字符；空串写 "NO NAME" */
    uint32_t volume_id;
    uint16_t date, time;        /* 目录项的创建与修改时间（FAT 格式，见 gk3w_fat_stamp） */
} gk3w_fat_params;

typedef enum {
    GK3W_FAT_OK = 0,
    GK3W_FAT_ARG,               /* 参数不合法：扇区大小、卷标、路径（空的段、"." ".."、FAT 不允许的字符、过长、重名） */
    GK3W_FAT_TOO_SMALL,         /* 分区太小，放不下一个 FAT32 */
    GK3W_FAT_NO_SPACE,          /* 文件放不下 */
    GK3W_FAT_NOMEM,
    GK3W_FAT_WRITE,             /* write 回调失败 */
    GK3W_FAT_READ,              /* read 回调失败 */
} gk3w_fat_err;

gk3w_fat_err gk3w_fat_build(const gk3w_fat_params *p, const gk3w_fat_file *files, size_t n, const gk3w_fat_io *io);
/* 放下这些文件所需的最小分区大小（字节，按 1 MiB 取整），给建分区时定大小用；路径不合法时返回 0 */
uint64_t gk3w_fat_size_for(const gk3w_fat_file *files, size_t n, uint32_t sector);
/* 给日志与 `!!` 的英文说明 */
const char *gk3w_fat_strerror(gk3w_fat_err e);
/* 年月日时分秒 → FAT 目录项的日期与时间（年份限 1980–2107，秒按 2 秒取整） */
void gk3w_fat_stamp(int year, int mon, int day, int hour, int min, int sec, uint16_t *date, uint16_t *time);

#endif
