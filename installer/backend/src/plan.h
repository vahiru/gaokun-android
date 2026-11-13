/* 分区方案：在用户选的一段空闲区里排 Android 的分区。算术与 scripts/live/installer-lib.sh 的 gk3_plan
 * （--mode alongside）相同：起点向上对齐到 1 MiB，各分区按固定大小依次排，userdata 吃掉剩下的、排在最后
 * （以后扩容不用挪任何东西）。比 shell 版多做的：扇区大小显式给出（4Kn 盘不再按 512 算），算术全部防溢出，
 * 区间本身也要校验。 */
#ifndef GK3W_PLAN_H
#define GK3W_PLAN_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "layout.h"

#define GK3W_MISC_MIB 4u
#define GK3W_METADATA_MIB 32u
#define GK3W_BOOT_MIB 64u
#define GK3W_SUPER_MIB 12288u
#define GK3W_RESCUE_MIB 1024u
#define GK3W_USERDATA_MIN_MIB 8192u

#define GK3W_PLAN_MAX 8

typedef struct {
    uint32_t sector;            /* 512 或 4096 */
    gk3w_span region;           /* 用户选的空闲区（含两端，逻辑扇区），调用方已核对它确实是空闲的 */
    uint64_t userdata_mib;      /* 0 = 吃掉剩下的全部 */
    int rescue;                 /* 1 = 带救援分区 gk3rescue（Windows 原型不用） */
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
} gk3w_plan_err;

gk3w_plan_err gk3w_plan_alongside(const gk3w_plan_req *r, gk3w_plan *out);

/* 算方案并按行协议打印到 fp：成功时每个分区一行 PLAN（op=mkpart disk name start end size_mib type=8300），
 * 再一行 PLANSUM（avail_mib fixed_mib userdata_mib）；失败时一行 PLANERR（msg avail_mib need_mib fixed_mib）。
 * apply 与 plan 命令共用这一份输出，界面只认这些行，自己不算分区。 */
/* disk 是 PLAN 行里 disk= 的值：Windows 是磁盘号，Linux 是 /dev 下的路径。 */
gk3w_plan_err gk3w_plan_print(const gk3w_plan_req *r, const char *disk, gk3w_plan *out, FILE *fp);
const char *gk3w_plan_strerror(gk3w_plan_err e);

#endif
