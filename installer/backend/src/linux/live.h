/* gk3l：安装器分区 —— 从内置盘启动、gaokun3/ 已读进内存的介质分区（GK3LIVE，见 lx.h 的安装介质一节）。
 * 整盘清空时原样保留它，装完可以删掉它、把空间并进 userdata。 */
#ifndef GK3L_LIVE_H
#define GK3L_LIVE_H

#include <stdint.h>
#include <stdio.h>

#include "../gptw.h"
#include "../layout.h"
#include "disk.h"
#include "gk3core.h"

typedef struct {
    uint32_t idx;                       /* GPT 表项序号（1 起） */
    uint64_t first, last;               /* 含两端，逻辑扇区 */
    uint8_t raw[GK3W_GPT_ENTRY_SIZE];   /* 原表项（整盘清空时原样抄进新表） */
    char name[GK3_GPT_NAME_CHARS * 4 + 1];
    gk3l_kpart k;
} gk3l_live;

/* 读进了内存的介质分区像不像安装器分区：FAT（vfat）、不是 ESP、类型是 XBOOTLDR 或者卷标 / 分区名是 GK3LIVE。
 * initramfs 认介质只看有没有那个 squashfs 文件 —— 把 LiveCD 解压到 D: 根目录、从 D: 启动时，D: 也会被读进内存；
 * 那样的分区不能当安装器分区去删。type 是大写的类型 GUID，name 是 GPT 名字（UTF-8） */
int gk3l_live_like(const char *type, const char *fs, const char *label, const char *name);

/* d 上有没有安装器分区：有返回 1 并填 *lv，没有返回 0。d 的分区表要已读（gk3l_disk_read_table）且是 gpt */
int gk3l_live_find(const gk3l_disk *d, gk3l_live *lv);

/* 删掉安装器分区之后 userdata（ud）能扩到哪：ud 在 lv 前面、两者之间没有别的分区时，扩到 lv 后面下一个分区之前
 * （没有就到 usable 区末尾），大小按 1 MiB 取整。parts 是盘上全部分区（含 ud 与 lv）。不能扩返回 0 */
uint64_t gk3l_live_grow_to(const gk3w_span *parts, size_t n, gk3w_span ud, gk3w_span lv, uint64_t last_usable, uint32_t bs);

/* 方案预览：装完删掉安装器分区时 userdata 会怎样。parts 是装完之后盘上的全部分区（含 lv），ud 是装完之后的 userdata。
 * 打印一行 PLAN op=rmlive part= name= start= end= size_mib= merge=yes|no userdata_mib=（并进去之后的大小；不并时是原大小） */
void gk3l_live_preview(const gk3l_disk *d, const gk3l_live *lv, const gk3w_span *parts, size_t n, gk3w_span ud, FILE *fp);

/* 删掉 d 上的安装器分区（说明见 live.c 文件头）。d 已以读写打开并上锁。
 * as_note = 1（apply 装完之后那一步）：动盘之前的失败不算失败，报 NOTE code=installer-kept why=<代码> 并返回 1。
 * 返回 0 成功；-1 失败（ERR 已报，touched 按实际） */
int gk3l_live_remove(gk3l_disk *d, int as_note);

#endif
