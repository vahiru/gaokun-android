/* Linux：算分区方案、不写盘（界面问"这块空闲区够不够、userdata 能有多大"用）。
 *
 *   gk3l plan --sector 512|4096 --region-start LBA --region-end LBA [--userdata-mib N] [--disk /dev/…]
 *   gk3l plan --mode wipe --disk /dev/X [--userdata-mib N]
 *   gk3l plan --mode reinstall --disk /dev/X [--keep-data yes|no]
 *
 * 双系统（默认）与 Windows 版的 plan 是同一份算术与输出（../plan.c）：PLAN… + PLANSUM，或 PLANERR；不读盘，区间是不是
 * 真的空闲由 apply 在动盘前重新核对。整盘清空要读盘的大小（新分区表的 usable 区），重新安装要读盘上现有的分区；
 * 与 apply 的同一步输出相同。 */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "../gptw.h"
#include "../plan.h"
#include "../proto.h"
#include "../release.h"
#include "disk.h"
#include "lx.h"

static int open_ro(gk3l_disk *d, const char *disk)
{
    char name[64];
    int err;
    if (gk3l_whole_disk_name(disk, name, sizeof name))
        return gk3l_err_kv(GK3W_TOUCHED_NA, "disk-open", "disk", disk, "--disk is not a whole disk", 0), -1;
    err = gk3l_disk_open(d, name, 0);
    if (err)
        return gk3l_err_kv(GK3W_TOUCHED_NA, err == EACCES || err == EPERM ? "not-root" : "disk-open", "disk", disk, "cannot open the disk", err), -1;
    if (d->bs != 512 && d->bs != 4096) {
        gk3l_disk_close(d);
        return gk3l_err(GK3W_TOUCHED_NA, "disk-sector", "the disk sector size is neither 512 nor 4096 bytes", 0), -1;
    }
    return 0;
}

static int plan_wipe(const char *disk, uint64_t ud)
{
    static gk3l_disk d;
    uint8_t *pm = gk3w_aligned_alloc(4096), *hdr = gk3w_aligned_alloc(4096), *ent = gk3w_aligned_alloc(GK3W_GPT_ENTRIES * GK3W_GPT_ENTRY_SIZE);
    uint8_t dg[16] = {0};
    gk3_gpt g;
    gk3w_plan_req req;
    gk3w_plan plan;
    gk3w_plan_err pe = GK3W_PLAN_EINVAL;
    int rc = 1;
    if (!pm || !hdr || !ent) {
        gk3l_err(GK3W_TOUCHED_NA, "nomem", "out of memory", ENOMEM);
        goto out;
    }
    if (open_ro(&d, disk))
        goto out;
    memset(&req, 0, sizeof req);
    req.sector = d.bs;
    req.userdata_mib = ud;
    req.whole = 1;
    if (gk3w_gpt_init(d.bs, d.blocks, dg, pm, hdr, ent) == GK3W_GPTW_OK && gk3_gpt_parse_header(hdr, d.bs, &g) == GK3_OK)
        req.region = (gk3w_span){g.first_usable, g.last_usable};
    else
        req.region = (gk3w_span){1, 0};   /* 盘小得放不下分区表：照样出 PLANERR */
    pe = gk3w_plan_print(&req, d.path, &plan, stdout);
    gk3l_disk_close(&d);
    if (pe != GK3W_PLAN_OK)
        gk3l_err_kv(GK3W_TOUCHED_NA, "plan", "why", gk3w_plan_strerror(pe), "the disk is too small for Android", 0);
    else
        rc = 0;
out:
    gk3w_aligned_free(pm);
    gk3w_aligned_free(hdr);
    gk3w_aligned_free(ent);
    return rc;
}

static int plan_reinstall(const char *disk, int keep)
{
    static gk3l_disk d;
    static gk3w_part_ref parts[256];
    static char pathbuf[256][96];
    const char *paths[256];
    size_t n = 0;
    gk3w_reinstall ri;
    gk3w_plan_err pe;
    if (open_ro(&d, disk))
        return 1;
    if (gk3l_disk_read_table(&d)) {
        gk3l_disk_close(&d);
        return gk3l_err(GK3W_TOUCHED_NA, "nomem", "out of memory", ENOMEM);
    }
    if (strcmp(d.table, "gpt")) {
        gk3l_err_kv(GK3W_TOUCHED_NA, !strcmp(d.table, "mbr") ? "mbr-disk" : !strcmp(d.table, "unreadable") ? "disk-unreadable" : "disk-not-gpt",
                    "table", d.table, "the disk has no usable GPT partition table", 0);
        gk3l_disk_close(&d);
        return 1;
    }
    for (uint32_t i = 0; i < d.g.num_entries && n < 256; i++) {
        gk3_gpt_part q;
        const gk3l_kpart *k;
        if (gk3_gpt_get(&d.g, i, &q) != GK3_OK)
            continue;
        gk3l_gpt_name_utf8(&d.g, i, parts[n].name, sizeof parts[n].name);
        parts[n].idx = q.index;
        parts[n].first = q.first_lba;
        parts[n].last = q.last_lba;
        k = gk3l_kernel_find(d.kp, d.nk, q.index);
        if (k)
            snprintf(pathbuf[n], sizeof pathbuf[n], "/dev/%s", k->name);
        else {
            char dn[64];
            gk3l_part_devname(d.name, q.index, dn, sizeof dn);
            snprintf(pathbuf[n], sizeof pathbuf[n], "/dev/%s", dn);
        }
        paths[n] = pathbuf[n];
        n++;
    }
    pe = gk3w_reinstall_print(d.bs, parts, paths, n, keep, &ri, stdout);
    gk3l_disk_close(&d);
    if (pe != GK3W_PLAN_OK)
        return gk3l_err_kv(GK3W_TOUCHED_NA, "plan", "why", gk3w_plan_strerror(pe), "the Android partitions on this disk do not form one complete installation of this version", 0);
    return fflush(stdout) == 0 ? 0 : 1;
}

int gk3l_cmd_plan(int argc, char **argv)
{
    uint64_t sector, rstart, rend, ud = 0;
    const char *disk = gk3l_arg(argc, argv, "--disk"), *mode = gk3l_arg(argc, argv, "--mode"), *keep = gk3l_arg(argc, argv, "--keep-data");
    gk3w_plan_req req;
    gk3w_plan plan;
    gk3w_plan_err pe;
    if (gk3l_arg(argc, argv, "--userdata-mib") && gk3l_arg_u64(argc, argv, "--userdata-mib", &ud))
        return gk3l_err(GK3W_TOUCHED_NA, "usage", "--userdata-mib takes a number", 0);
    if (mode && !strcmp(mode, "wipe")) {
        if (!disk)
            return gk3l_err(GK3W_TOUCHED_NA, "usage", "usage: plan --mode wipe --disk /dev/X [--userdata-mib N]", 0);
        return plan_wipe(disk, ud);
    }
    if (mode && !strcmp(mode, "reinstall")) {
        if (!disk || (keep && strcmp(keep, "yes") && strcmp(keep, "no")))
            return gk3l_err(GK3W_TOUCHED_NA, "usage", "usage: plan --mode reinstall --disk /dev/X [--keep-data yes|no]", 0);
        return plan_reinstall(disk, keep && !strcmp(keep, "yes"));
    }
    if ((mode && strcmp(mode, "alongside")) || gk3l_arg_u64(argc, argv, "--sector", &sector) || (sector != 512 && sector != 4096) ||
        gk3l_arg_u64(argc, argv, "--region-start", &rstart) || gk3l_arg_u64(argc, argv, "--region-end", &rend))
        return gk3l_err(GK3W_TOUCHED_NA, "usage", "usage: plan --sector 512|4096 --region-start LBA --region-end LBA [--userdata-mib N] [--disk /dev/...]", 0);
    memset(&req, 0, sizeof req);
    req.sector = (uint32_t)sector;
    req.region = (gk3w_span){rstart, rend};
    req.userdata_mib = ud;
    pe = gk3w_plan_print(&req, disk ? disk : "", &plan, stdout);
    if (pe != GK3W_PLAN_OK)
        return gk3l_err_kv(GK3W_TOUCHED_NA, "plan", "why", gk3w_plan_strerror(pe), "the selected free space is too small for Android", 0);
    return fflush(stdout) == 0 ? 0 : 1;
}
