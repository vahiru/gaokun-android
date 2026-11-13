/* 分区方案（说明见 plan.h）。 */
#include "plan.h"

#include "proto.h"

#include <stdio.h>
#include <string.h>

const char *gk3w_plan_strerror(gk3w_plan_err e)
{
    switch (e) {
    case GK3W_PLAN_OK: return "ok";
    case GK3W_PLAN_EINVAL: return "bad-region";
    case GK3W_PLAN_ENOSPACE: return "not-enough-space";
    case GK3W_PLAN_EUD_SMALL: return "userdata-too-small";
    case GK3W_PLAN_EUD_BIG: return "userdata-too-big";
    }
    return "unknown";
}

static void add(gk3w_plan *p, const char *name, uint64_t mib, uint64_t *cur, uint64_t per_mib)
{
    gk3w_plan_part *e = &p->parts[p->n++];
    snprintf(e->name, sizeof e->name, "%s", name);
    e->first = *cur;
    e->last = *cur + mib * per_mib - 1;
    e->mib = mib;
    *cur = e->last + 1;
}

gk3w_plan_err gk3w_plan_alongside(const gk3w_plan_req *r, gk3w_plan *out)
{
    uint64_t per_mib, cur, avail, fixed, need, ud;

    memset(out, 0, sizeof *out);
    if ((r->sector != 512 && r->sector != 4096) || r->region.last < r->region.first)
        return GK3W_PLAN_EINVAL;
    per_mib = (1u << 20) / r->sector;
    /* 起点向上对齐到 1 MiB（溢出时区间必然放不下：直接算没空间） */
    if (r->region.first > UINT64_MAX - (per_mib - 1))
        return GK3W_PLAN_EINVAL;
    cur = (r->region.first + per_mib - 1) / per_mib * per_mib;
    avail = cur > r->region.last ? 0 : (r->region.last - cur + 1) / per_mib;

    fixed = GK3W_MISC_MIB + GK3W_METADATA_MIB + 2 * GK3W_BOOT_MIB + GK3W_SUPER_MIB + (r->rescue ? GK3W_RESCUE_MIB : 0);
    need = fixed + GK3W_USERDATA_MIN_MIB;
    out->avail_mib = avail;
    out->fixed_mib = fixed;
    out->need_mib = need;
    if (avail < need)
        return GK3W_PLAN_ENOSPACE;
    ud = avail - fixed;
    if (r->userdata_mib) {
        if (r->userdata_mib < GK3W_USERDATA_MIN_MIB)
            return GK3W_PLAN_EUD_SMALL;
        if (r->userdata_mib > ud)
            return GK3W_PLAN_EUD_BIG;
        ud = r->userdata_mib;
    }
    out->userdata_mib = ud;

    /* ⚠️ 顺序即磁盘顺序，userdata 必须最后（installer-lib.sh gk3_plan 的同一条规矩） */
    add(out, "misc", GK3W_MISC_MIB, &cur, per_mib);
    add(out, "metadata", GK3W_METADATA_MIB, &cur, per_mib);
    add(out, "boot_a", GK3W_BOOT_MIB, &cur, per_mib);
    add(out, "boot_b", GK3W_BOOT_MIB, &cur, per_mib);
    add(out, "super", GK3W_SUPER_MIB, &cur, per_mib);
    if (r->rescue)
        add(out, "gk3rescue", GK3W_RESCUE_MIB, &cur, per_mib);
    add(out, "userdata", ud, &cur, per_mib);

    /* 再独立验一次：最后一个分区不能越过区间末尾（算错的代价是写到别人的分区上） */
    if (out->parts[out->n - 1].last > r->region.last) {
        memset(out, 0, sizeof *out);
        return GK3W_PLAN_EINVAL;
    }
    return GK3W_PLAN_OK;
}

gk3w_plan_err gk3w_plan_print(const gk3w_plan_req *r, const char *disk, gk3w_plan *out, FILE *fp)
{
    gk3w_plan_err pe = gk3w_plan_alongside(r, out);
    gk3w_line l = {0};
    if (pe != GK3W_PLAN_OK) {
        gk3w_line_begin(&l, "PLANERR");
        gk3w_line_kv(&l, "msg", gk3w_plan_strerror(pe));
        gk3w_line_u64(&l, "avail_mib", out->avail_mib);
        gk3w_line_u64(&l, "need_mib", out->need_mib);
        gk3w_line_u64(&l, "fixed_mib", out->fixed_mib);
        (void)gk3w_line_emit(&l, fp);
        gk3w_line_free(&l);
        return pe;
    }
    for (size_t i = 0; i < out->n; i++) {
        gk3w_line_begin(&l, "PLAN");
        gk3w_line_kv(&l, "op", "mkpart");
        gk3w_line_kv(&l, "disk", disk);
        gk3w_line_kv(&l, "name", out->parts[i].name);
        gk3w_line_u64(&l, "start", out->parts[i].first);
        gk3w_line_u64(&l, "end", out->parts[i].last);
        gk3w_line_u64(&l, "size_mib", out->parts[i].mib);
        gk3w_line_kv(&l, "type", "8300");
        (void)gk3w_line_emit(&l, fp);
        gk3w_line_free(&l);
    }
    gk3w_line_begin(&l, "PLANSUM");
    gk3w_line_u64(&l, "avail_mib", out->avail_mib);
    gk3w_line_u64(&l, "fixed_mib", out->fixed_mib);
    gk3w_line_u64(&l, "userdata_mib", out->userdata_mib);
    (void)gk3w_line_emit(&l, fp);
    gk3w_line_free(&l);
    return pe;
}
