/* 分区方案。算术与 scripts/live/installer-lib.sh 的 gk3_plan 相同：
 *   双系统：在用户选的一段空闲区里，起点向上对齐到 1 MiB，各分区按固定大小依次排，userdata 吃掉剩下的、排在最后
 *           （以后扩容不用挪任何东西）。
 *   整盘清空：区间是新分区表的整个 usable 区，最前面另放 300 MiB 的 ESP，其余同上。
 *   重新安装：不改分区表，按名字认出盘上现有的那一套，逐个决定写新系统、格式化还是保留。
 * 比 shell 版多做的：扇区大小显式给出（4Kn 盘不再按 512 算），算术全部防溢出，区间本身也要校验。 */
#ifndef GK3W_PLAN_H
#define GK3W_PLAN_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "gk3core.h"
#include "layout.h"

#define GK3W_MISC_MIB 4u
#define GK3W_METADATA_MIB 32u
#define GK3W_BOOT_MIB 64u
#define GK3W_SUPER_MIB 12288u
#define GK3W_RESCUE_MIB 1024u
#define GK3W_USERDATA_MIN_MIB 8192u

#define GK3W_ESP_MIB 300u

#define GK3W_PLAN_MAX 8

typedef struct {
    uint32_t sector;            /* 512 或 4096 */
    gk3w_span region;           /* 用户选的空闲区（含两端，逻辑扇区），调用方已核对它确实是空闲的 */
    uint64_t userdata_mib;      /* 0 = 吃掉剩下的全部 */
    int rescue;                 /* 1 = 带救援分区 gk3rescue（Windows 原型不用） */
    int whole;                  /* 1 = 整盘清空：最前面另放 ESP（名字 esp，类型 EFI System） */
} gk3w_plan_req;

typedef struct {
    char name[16];
    uint64_t first, last;       /* 含两端，逻辑扇区 */
    uint64_t mib;
} gk3w_plan_part;

typedef struct {
    gk3w_plan_part parts[GK3W_PLAN_MAX];
    size_t n;
    uint64_t avail_mib, fixed_mib, need_mib, userdata_mib;
} gk3w_plan;

typedef enum {
    GK3W_PLAN_OK = 0,
    GK3W_PLAN_EINVAL,           /* 扇区大小不对、区间为空 */
    GK3W_PLAN_ENOSPACE,         /* 区间放不下固定分区加最小的 userdata（avail_mib / need_mib / fixed_mib 已填） */
    GK3W_PLAN_EUD_SMALL,        /* 指定的 userdata 小于下限 */
    GK3W_PLAN_EUD_BIG,          /* 指定的 userdata 放不下 */
    GK3W_PLAN_DUP,              /* 重新安装：有名字重复（不知道写哪个） */
    GK3W_PLAN_MISSING,          /* 重新安装：缺了某个分区（不是一套完整的安装） */
    GK3W_PLAN_SMALL,            /* 重新安装：某个分区比这一版要的小 */
} gk3w_plan_err;

/* 双系统与整盘清空（r->whole）的方案 */
gk3w_plan_err gk3w_plan_alongside(const gk3w_plan_req *r, gk3w_plan *out);

/* 算方案并按行协议打印到 fp：成功时每个分区一行 PLAN（op=mkpart disk name start end size_mib type=8300），
 * 再一行 PLANSUM（avail_mib fixed_mib userdata_mib）；失败时一行 PLANERR（msg avail_mib need_mib fixed_mib）。
 * apply 与 plan 命令共用这一份输出，界面只认这些行，自己不算分区。 */
/* disk 是 PLAN 行里 disk= 的值：Windows 是磁盘号，Linux 是 /dev 下的路径。 */
gk3w_plan_err gk3w_plan_print(const gk3w_plan_req *r, const char *disk, gk3w_plan *out, FILE *fp);
const char *gk3w_plan_strerror(gk3w_plan_err e);

/* ── 重新安装 ── */

/* 盘上现有的一个分区 */
typedef struct {
    char name[GK3_GPT_NAME_CHARS * 4 + 1];   /* UTF-8 */
    uint32_t idx;               /* GPT 表项序号（1 起） */
    uint64_t first, last;       /* 含两端，逻辑扇区 */
} gk3w_part_ref;

typedef enum { GK3W_REUSE_WRITE = 0, GK3W_REUSE_FORMAT, GK3W_REUSE_KEEP } gk3w_reuse_action;

#define GK3W_REUSE_N 6          /* misc metadata boot_a boot_b super userdata，依此顺序 */
extern const char *const gk3w_reuse_names[GK3W_REUSE_N];

typedef struct {
    gk3w_part_ref p[GK3W_REUSE_N];
    gk3w_reuse_action act[GK3W_REUSE_N];
    uint64_t kib[GK3W_REUSE_N];
    uint64_t fixed_mib, userdata_mib;
    int keep;
    char bad[128];              /* 出错时：重复或缺的名字（逗号分隔），或太小的那个 */
    uint64_t have_kib, need_kib;  /* GK3W_PLAN_SMALL 时 */
} gk3w_reinstall;

/* parts 是盘上全部分区；keep = 1 时 metadata / userdata 保留（不格式化）。下限按 KiB 比：misc 只要 1000 KiB
 * （真机的 misc 是 GPT 头之后那段 1007 KiB），其余是这一版要的大小 */
gk3w_plan_err gk3w_plan_reinstall(uint32_t sector, const gk3w_part_ref *parts, size_t n, int keep, gk3w_reinstall *out);
/* 算方案并按行协议打印：成功时每个分区一行 PLAN op=reuse（name path num start end size_mib size_kib action），
 * 再一行 PLANSUM mode=reinstall keep_data=…；失败时一行 PLANERR（msg=reinstall-duplicate|missing|part-small …）。
 * paths[i] 是第 i 个分区（按 parts 的下标）的设备路径。 */
gk3w_plan_err gk3w_reinstall_print(uint32_t sector, const gk3w_part_ref *parts, const char *const *paths, size_t n, int keep,
                                   gk3w_reinstall *out, FILE *fp);

#endif
