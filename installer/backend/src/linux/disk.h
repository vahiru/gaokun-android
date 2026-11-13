/* gk3l：块设备、sysfs 里内核的分区视图、盘上的主 GPT。probe 与写盘的命令共用。 */
#ifndef GK3L_DISK_H
#define GK3L_DISK_H

#include <stddef.h>
#include <stdint.h>

#include "../proto.h"
#include "gk3core.h"

#define GK3L_ENTRIES_MAX (1u << 20)

/* sysfs 里的一个分区（/sys/block/<盘>/<分区>/） */
typedef struct {
    char name[64];              /* nvme0n1p5 */
    char devno[32];             /* 259:5 */
    uint64_t num, start512, size512;   /* start / size 按 512 字节计（sysfs 的约定，与扇区大小无关） */
} gk3l_kpart;

typedef struct {
    char name[64];              /* nvme0n1 */
    char path[80];              /* /dev/nvme0n1 */
    char devno[32];
    int fd;
    int direct;                 /* 以 O_RDWR | O_DIRECT 打开（写盘的命令）；否则只读、经页缓存 */
    uint32_t bs, pbs;
    uint64_t bytes, blocks;
    /* 分区表（gk3l_disk_read_table）：gpt / damaged / mbr / raw / unreadable，含义见 probe.c 文件头 */
    const char *table;
    gk3_err gerr;               /* table 不是 gpt 时主 GPT 的错误 */
    gk3_gpt g;                  /* table 为 gpt 时有效；g.entries 指向 ent */
    uint8_t *hdr, *ent;         /* 按 4096 对齐（无缓冲 I/O 要求） */
    gk3l_kpart kp[256];
    size_t nk;
} gk3l_disk;

/* 打开 /dev/<name> 并读大小与扇区大小。rw = 1 时以 O_RDWR | O_DIRECT 打开。
 * 返回 0；失败返回 errno（d->fd 为 -1）。不读分区表。 */
int gk3l_disk_open(gk3l_disk *d, const char *name, int rw);
void gk3l_disk_close(gk3l_disk *d);
/* 读主 GPT 并分类（d->table），同时刷新 sysfs 的分区列表。返回 0；内存不够返回 -1。 */
int gk3l_disk_read_table(gk3l_disk *d);
/* 内核的分区（sysfs）与盘上 GPT 一致：个数相同、每个表项都有同号同起止的内核分区（table 必须是 gpt） */
int gk3l_disk_kernel_ok(const gk3l_disk *d);
/* 从 /dev 路径找出整块盘的名字（/dev/nvme0n1 → nvme0n1）；不是整块盘（是分区、或不是块设备）返回 -1 */
int gk3l_whole_disk_name(const char *path, char *out, size_t cap);

/* 按字节偏移读写；off、len 按扇区对齐，buf 按 4096 对齐（O_DIRECT）。0 成功、-1 失败（errno） */
int gk3l_pread(const gk3l_disk *d, void *buf, size_t len, uint64_t off);
int gk3l_pwrite(const gk3l_disk *d, const void *buf, size_t len, uint64_t off);
/* 写后立即读回比对（chk 与 buf 同样大小、同样对齐）。0 成功；-1 写失败；-2 读回不一致 */
int gk3l_write_verify(const gk3l_disk *d, const void *buf, size_t len, uint64_t off, void *chk);

/* sysfs：这块盘的分区；按号找 */
size_t gk3l_kernel_parts(const char *disk, gk3l_kpart *out, size_t cap);
const gk3l_kpart *gk3l_kernel_find(const gk3l_kpart *k, size_t n, uint64_t num);
/* 内核给分区起的名字（盘名以数字结尾加 p：nvme0n1 + 5 → nvme0n1p5，sda + 5 → sda5） */
void gk3l_part_devname(const char *disk, uint32_t idx, char *out, size_t cap);

/* GPT 文本：大写 GUID；表项名字 UTF-16LE → UTF-8（显示用，中文名照样给出）；主 GPT 不能用的原因代码 */
void gk3l_guid_upper(const uint8_t g[16], char out[37]);
void gk3l_gpt_name_utf8(const gk3_gpt *g, uint32_t idx0, char *out, size_t cap);
const char *gk3l_gpt_why(gk3_err e);

/* blkid -p：文件系统类型与卷标（不走 blkid 的缓存）。认不出时两个都是空串 */
void gk3l_blkid(const char *dev, char *fs, size_t fscap, char *label, size_t lcap);

#endif
