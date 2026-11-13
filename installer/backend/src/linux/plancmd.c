/* Linux：只算方案、不碰盘（界面问"这块空闲区够不够、userdata 能有多大"用）。
 *
 *   gk3l plan --sector 512|4096 --region-start LBA --region-end LBA [--userdata-mib N] [--disk /dev/…]
 *
 * 与 Windows 版的 plan 是同一份算术与输出（../plan.c 的 gk3w_plan_print）：PLAN… + PLANSUM，或 PLANERR。
 * 区间是不是真的空闲由 apply 在动盘前重新核对，这里不读盘。 */
#include <stdio.h>

#include "../plan.h"
#include "../proto.h"
#include "lx.h"

int gk3l_cmd_plan(int argc, char **argv)
{
    uint64_t sector, rstart, rend, ud = 0;
    const char *disk = gk3l_arg(argc, argv, "--disk");
    gk3w_plan_req req;
    gk3w_plan plan;
    gk3w_plan_err pe;
    if (gk3l_arg_u64(argc, argv, "--sector", &sector) || (sector != 512 && sector != 4096) ||
        gk3l_arg_u64(argc, argv, "--region-start", &rstart) || gk3l_arg_u64(argc, argv, "--region-end", &rend) ||
        (gk3l_arg(argc, argv, "--userdata-mib") && gk3l_arg_u64(argc, argv, "--userdata-mib", &ud)))
        return gk3l_err(GK3W_TOUCHED_NA, "usage", "usage: plan --sector 512|4096 --region-start LBA --region-end LBA [--userdata-mib N] [--disk /dev/...]", 0);
    req.sector = (uint32_t)sector;
    req.region = (gk3w_span){rstart, rend};
    req.userdata_mib = ud;
    req.rescue = 0;
    pe = gk3w_plan_print(&req, disk ? disk : "", &plan, stdout);
    if (pe != GK3W_PLAN_OK)
        return gk3l_err_kv(GK3W_TOUCHED_NA, "plan", "why", gk3w_plan_strerror(pe), "the selected free space is too small for Android", 0);
    return fflush(stdout) == 0 ? 0 : 1;
}
