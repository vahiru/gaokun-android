/* Windows：只算方案、不碰盘（界面问"这块空闲区够不够、userdata 能有多大"用；不需要管理员）。
 *
 *   gk3w plan --sector 512|4096 --region-start LBA --region-end LBA [--userdata-mib N] [--disk N]
 *
 * 输出与 apply 的同一段相同（gk3w_plan_print）：PLAN… + PLANSUM，或 PLANERR。区间是不是真的空闲由 apply 在动盘前
 * 重新核对，这里不读盘。 */
#include <windows.h>

#include "../plan.h"
#include "../proto.h"
#include "disk.h"
#include "win.h"

int gk3w_cmd_plan(int argc, wchar_t **argv)
{
    uint64_t sector, rstart, rend, ud = 0, disk = 0;
    gk3w_plan_req req;
    gk3w_plan plan;
    gk3w_plan_err pe;
    if (gk3w_arg_u64(argc, argv, L"--sector", &sector) || (sector != 512 && sector != 4096) ||
        gk3w_arg_u64(argc, argv, L"--region-start", &rstart) || gk3w_arg_u64(argc, argv, L"--region-end", &rend) ||
        (gk3w_arg(argc, argv, L"--userdata-mib") && gk3w_arg_u64(argc, argv, L"--userdata-mib", &ud)) ||
        (gk3w_arg(argc, argv, L"--disk") && gk3w_arg_u64(argc, argv, L"--disk", &disk)))
        return gk3w_err(GK3W_TOUCHED_NA, "usage", "usage: plan --sector 512|4096 --region-start LBA --region-end LBA [--userdata-mib N] [--disk N]", 0);
    req.sector = (uint32_t)sector;
    req.region = (gk3w_span){rstart, rend};
    req.userdata_mib = ud;
    req.rescue = 0;
    pe = gk3w_plan_print(&req, disk, &plan, stdout);
    if (pe != GK3W_PLAN_OK)
        return gk3w_err_kv(GK3W_TOUCHED_NA, "plan", "why", gk3w_plan_strerror(pe), "the selected free space is too small for Android", 0);
    return fflush(stdout) == 0 ? 0 : 1;
}
