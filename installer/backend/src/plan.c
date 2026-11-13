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
    case GK3W_PLAN_DUP: return "reinstall-duplicate";
    case GK3W_PLAN_MISSING: return "reinstall-missing";
    case GK3W_PLAN_SMALL: return "reinstall-part-small";
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

static gk3w_plan_err plan_one(const gk3w_plan_req *r, gk3w_plan *out)
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

    fixed = GK3W_MISC_MIB + GK3W_METADATA_MIB + 2 * GK3W_BOOT_MIB + GK3W_SUPER_MIB + (r->rescue ? GK3W_RESCUE_MIB : 0) +
            (r->whole ? GK3W_ESP_MIB : 0);
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

    /* ⚠️ 顺序即磁盘顺序，userdata 必须最后（以后扩大 userdata 不用挪任何东西） */
    if (r->whole)
        add(out, "esp", GK3W_ESP_MIB, &cur, per_mib);
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

gk3w_plan_err gk3w_plan_alongside(const gk3w_plan_req *r, gk3w_plan *out)
{
    gk3w_plan_req q;
    gk3w_plan b;
    gk3w_plan_err ea = GK3W_PLAN_EINVAL, eb = GK3W_PLAN_EINVAL;
    if (!r->whole || !r->keep.last)
        return plan_one(r, out);
    memset(out, 0, sizeof *out);
    /* 保留的分区必须整个落在区间里 */
    if (r->keep.first > r->keep.last || r->keep.first < r->region.first || r->keep.last > r->region.last)
        return GK3W_PLAN_EINVAL;
    q = *r;
    q.keep = (gk3w_span){0, 0};
    memset(&b, 0, sizeof b);
    if (r->keep.first > r->region.first) {
        q.region = (gk3w_span){r->region.first, r->keep.first - 1};
        if ((ea = plan_one(&q, out)) == GK3W_PLAN_OK)
            return ea;
    }
    if (r->keep.last < r->region.last) {
        q.region = (gk3w_span){r->keep.last + 1, r->region.last};
        if ((eb = plan_one(&q, &b)) == GK3W_PLAN_OK) {
            *out = b;
            return eb;
        }
    }
    /* 两段都放不下：报大的那一段（界面据此说"还差多少"） */
    if (eb != GK3W_PLAN_EINVAL && (ea == GK3W_PLAN_EINVAL || b.avail_mib > out->avail_mib)) {
        *out = b;
        return eb;
    }
    return ea;
}

static void print_keep(const gk3w_plan_req *r, const char *disk, FILE *fp)
{
    gk3w_line l = {0};
    uint64_t per_mib = (1u << 20) / r->sector;
    gk3w_line_begin(&l, "PLAN");
    gk3w_line_kv(&l, "op", "keep");
    gk3w_line_kv(&l, "disk", disk);
    gk3w_line_kv(&l, "name", r->keep_name ? r->keep_name : "");
    gk3w_line_u64(&l, "start", r->keep.first);
    gk3w_line_u64(&l, "end", r->keep.last);
    gk3w_line_u64(&l, "size_mib", (r->keep.last - r->keep.first + 1) / per_mib);
    (void)gk3w_line_emit(&l, fp);
    gk3w_line_free(&l);
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
        /* /data 指定得不对时：允许的范围（界面据此写明能设多大） */
        if (pe == GK3W_PLAN_EUD_SMALL)
            gk3w_line_u64(&l, "min_mib", GK3W_USERDATA_MIN_MIB);
        if (pe == GK3W_PLAN_EUD_BIG)
            gk3w_line_u64(&l, "max_mib", out->avail_mib - out->fixed_mib);
        (void)gk3w_line_emit(&l, fp);
        gk3w_line_free(&l);
        return pe;
    }
    if (r->whole) {
        gk3w_line_begin(&l, "PLAN");
        gk3w_line_kv(&l, "op", "wipe");
        gk3w_line_kv(&l, "disk", disk);
        (void)gk3w_line_emit(&l, fp);
        gk3w_line_free(&l);
    }
    /* 保留的分区按盘上的先后排进新分区的列表里 */
    if (r->whole && r->keep.last && r->keep.last < out->parts[0].first)
        print_keep(r, disk, fp);
    for (size_t i = 0; i < out->n; i++) {
        gk3w_line_begin(&l, "PLAN");
        gk3w_line_kv(&l, "op", "mkpart");
        gk3w_line_kv(&l, "disk", disk);
        gk3w_line_kv(&l, "name", out->parts[i].name);
        gk3w_line_u64(&l, "start", out->parts[i].first);
        gk3w_line_u64(&l, "end", out->parts[i].last);
        gk3w_line_u64(&l, "size_mib", out->parts[i].mib);
        gk3w_line_kv(&l, "type", strcmp(out->parts[i].name, "esp") ? "8300" : "ef00");
        (void)gk3w_line_emit(&l, fp);
        gk3w_line_free(&l);
    }
    if (r->whole && r->keep.last && r->keep.first > out->parts[out->n - 1].last)
        print_keep(r, disk, fp);
    gk3w_line_begin(&l, "PLANSUM");
    gk3w_line_kv(&l, "mode", r->whole ? "wipe" : "alongside");
    gk3w_line_u64(&l, "avail_mib", out->avail_mib);
    gk3w_line_u64(&l, "fixed_mib", out->fixed_mib);
    gk3w_line_u64(&l, "userdata_mib", out->userdata_mib);
    (void)gk3w_line_emit(&l, fp);
    gk3w_line_free(&l);
    return pe;
}

/* ── 重新安装 ── */

const char *const gk3w_reuse_names[GK3W_REUSE_N] = {"misc", "metadata", "boot_a", "boot_b", "super", "userdata"};

static void list_add(char *buf, size_t cap, const char *name)
{
    if (strlen(buf) + strlen(name) + 2 > cap)
        return;
    if (buf[0])
        strcat(buf, ",");
    strcat(buf, name);
}

gk3w_plan_err gk3w_plan_reinstall(uint32_t sector, const gk3w_part_ref *parts, size_t n, int keep, gk3w_reinstall *out)
{
    static const uint64_t min_kib[GK3W_REUSE_N] = {1000, GK3W_METADATA_MIB * 1024ull, GK3W_BOOT_MIB * 1024ull,
                                                   GK3W_BOOT_MIB * 1024ull, GK3W_SUPER_MIB * 1024ull,
                                                   GK3W_USERDATA_MIN_MIB * 1024ull};
    char dup[128] = "", miss[128] = "";
    memset(out, 0, sizeof *out);
    out->keep = keep;
    if (sector != 512 && sector != 4096)
        return GK3W_PLAN_EINVAL;
    for (int k = 0; k < GK3W_REUSE_N; k++) {
        int c = 0;
        for (size_t i = 0; i < n; i++)
            if (!strcmp(parts[i].name, gk3w_reuse_names[k])) {
                if (!c)
                    out->p[k] = parts[i];
                c++;
            }
        if (!c)
            list_add(miss, sizeof miss, gk3w_reuse_names[k]);
        else if (c > 1)
            list_add(dup, sizeof dup, gk3w_reuse_names[k]);
    }
    if (dup[0]) {
        snprintf(out->bad, sizeof out->bad, "%s", dup);
        return GK3W_PLAN_DUP;
    }
    if (miss[0]) {
        snprintf(out->bad, sizeof out->bad, "%s", miss);
        return GK3W_PLAN_MISSING;
    }
    for (int k = 0; k < GK3W_REUSE_N; k++) {
        const gk3w_part_ref *p = &out->p[k];
        uint64_t blocks = p->last >= p->first ? p->last - p->first + 1 : 0;
        uint64_t kib = blocks > UINT64_MAX / sector ? UINT64_MAX / 1024 : blocks * sector / 1024;
        out->kib[k] = kib;
        if (kib < min_kib[k]) {
            snprintf(out->bad, sizeof out->bad, "%s", gk3w_reuse_names[k]);
            out->have_kib = kib;
            out->need_kib = min_kib[k];
            return GK3W_PLAN_SMALL;
        }
        out->act[k] = (k == 1 || k == 5) ? (keep ? GK3W_REUSE_KEEP : GK3W_REUSE_FORMAT) : GK3W_REUSE_WRITE;
        if (k == 5)
            out->userdata_mib = kib / 1024;
        else
            out->fixed_mib += kib / 1024;
    }
    return GK3W_PLAN_OK;
}

gk3w_plan_err gk3w_reinstall_print(uint32_t sector, const gk3w_part_ref *parts, const char *const *paths, size_t n, int keep,
                                   gk3w_reinstall *out, FILE *fp)
{
    static const char *const act[] = {"write", "format", "keep"};
    gk3w_plan_err pe = gk3w_plan_reinstall(sector, parts, n, keep, out);
    gk3w_line l = {0};
    if (pe != GK3W_PLAN_OK) {
        gk3w_line_begin(&l, "PLANERR");
        gk3w_line_kv(&l, "msg", gk3w_plan_strerror(pe));
        if (pe == GK3W_PLAN_SMALL) {
            gk3w_line_kv(&l, "name", out->bad);
            gk3w_line_u64(&l, "have_kib", out->have_kib);
            gk3w_line_u64(&l, "need_kib", out->need_kib);
        } else if (pe != GK3W_PLAN_EINVAL) {
            gk3w_line_kv(&l, "names", out->bad);
        }
        (void)gk3w_line_emit(&l, fp);
        gk3w_line_free(&l);
        return pe;
    }
    for (int k = 0; k < GK3W_REUSE_N; k++) {
        const char *path = "";
        for (size_t i = 0; i < n; i++)
            if (parts[i].idx == out->p[k].idx)
                path = paths[i];
        gk3w_line_begin(&l, "PLAN");
        gk3w_line_kv(&l, "op", "reuse");
        gk3w_line_kv(&l, "name", gk3w_reuse_names[k]);
        gk3w_line_kv(&l, "path", path);
        gk3w_line_u64(&l, "num", out->p[k].idx);
        gk3w_line_u64(&l, "start", out->p[k].first);
        gk3w_line_u64(&l, "end", out->p[k].last);
        gk3w_line_u64(&l, "size_mib", out->kib[k] / 1024);
        gk3w_line_u64(&l, "size_kib", out->kib[k]);
        gk3w_line_kv(&l, "action", act[out->act[k]]);
        (void)gk3w_line_emit(&l, fp);
        gk3w_line_free(&l);
    }
    gk3w_line_begin(&l, "PLANSUM");
    gk3w_line_kv(&l, "mode", "reinstall");
    gk3w_line_kv(&l, "keep_data", keep ? "yes" : "no");
    gk3w_line_u64(&l, "avail_mib", out->fixed_mib + out->userdata_mib);
    gk3w_line_u64(&l, "fixed_mib", out->fixed_mib);
    gk3w_line_u64(&l, "userdata_mib", out->userdata_mib);
    (void)gk3w_line_emit(&l, fp);
    gk3w_line_free(&l);
    return pe;
}
