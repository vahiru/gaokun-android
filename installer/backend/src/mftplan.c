#include "mftplan.h"

#include <stdlib.h>

static int push_seg(gk3w_mft_seg **v, size_t *n, size_t *cap, gk3w_mft_seg x)
{
    if (*n == *cap) {
        size_t nc = *cap ? *cap * 2 : 256;
        gk3w_mft_seg *nv = realloc(*v, nc * sizeof *nv);
        if (!nv)
            return -1;
        *v = nv;
        *cap = nc;
    }
    (*v)[(*n)++] = x;
    return 0;
}

int gk3w_mft_free_runs(const uint8_t *bitmap, int64_t base, int64_t bits, int64_t limit, int64_t min_run, gk3w_mft_run **out,
                       size_t *n)
{
    int64_t end = limit - base < bits ? limit - base : bits, start = -1;
    size_t cap = 0;
    *out = NULL;
    *n = 0;
    if (end < 0)
        end = 0;
    for (int64_t i = 0; i <= end; i++) {
        int used = i == end || (bitmap[i >> 3] >> (i & 7)) & 1;
        if (!used) {
            if (start < 0)
                start = i;
            continue;
        }
        if (start >= 0 && i - start >= min_run) {
            if (*n == cap) {
                size_t nc = cap ? cap * 2 : 256;
                gk3w_mft_run *nr = realloc(*out, nc * sizeof *nr);
                if (!nr)
                    return -1;
                *out = nr;
                cap = nc;
            }
            (*out)[(*n)++] = (gk3w_mft_run){base + start, i - start};
        }
        start = -1;
    }
    return 0;
}

int gk3w_mft_split(const gk3w_mft_ext *ext, size_t ne, int64_t keep, int64_t chunk, gk3w_mft_seg **segs, size_t *n, size_t *cap,
                   int64_t *fixed_end)
{
    for (size_t i = 0; i < ne; i++) {
        int64_t vcn = ext[i].vcn, lcn = ext[i].lcn, left = ext[i].count;
        if (ext[i].stream == 0 && vcn < keep) {
            int64_t k = keep - vcn < left ? keep - vcn : left;
            if (lcn + k > *fixed_end)
                *fixed_end = lcn + k;
            vcn += k;
            lcn += k;
            left -= k;
        }
        while (left > 0) {
            int64_t c = left < chunk ? left : chunk;
            if (push_seg(segs, n, cap, (gk3w_mft_seg){ext[i].stream, vcn, lcn, c, -1, 0, GK3W_SEG_TODO}))
                return -1;
            vcn += c;
            lcn += c;
            left -= c;
        }
    }
    return 0;
}

/* qsort 的比较函数拿不到上下文：排序前把段表放在这里 */
static const gk3w_mft_seg *g_sort_segs;

static int by_lcn_desc(const void *a, const void *b)
{
    const gk3w_mft_seg *x = &g_sort_segs[*(const size_t *)a], *y = &g_sort_segs[*(const size_t *)b];
    return x->lcn < y->lcn ? 1 : x->lcn > y->lcn ? -1 : 0;
}

/* 同一空隙里按流、VCN 排 */
static int by_gap_vcn(const void *a, const void *b)
{
    const gk3w_mft_seg *x = &g_sort_segs[*(const size_t *)a], *y = &g_sort_segs[*(const size_t *)b];
    if (x->gap != y->gap)
        return x->gap < y->gap ? -1 : 1;
    if (x->stream != y->stream)
        return x->stream < y->stream ? -1 : 1;
    return x->vcn < y->vcn ? -1 : x->vcn > y->vcn ? 1 : 0;
}

int gk3w_mft_plan(gk3w_mft_seg **segs, size_t *n, size_t *cap, const gk3w_mft_run *runs, size_t nr, int64_t min_run, int64_t align)
{
    int64_t *avail = malloc((nr ? nr : 1) * sizeof *avail);
    size_t *order = NULL, no = 0, n0 = *n;
    if (!avail)
        return -1;
    for (size_t r = 0; r < nr; r++)
        avail[r] = runs[r].len;
    for (size_t i = 0; i < *n; i++)
        if ((*segs)[i].state == GK3W_SEG_TODO)
            (*segs)[i].gap = -1;
    order = malloc((n0 ? n0 : 1) * sizeof *order);
    if (!order) {
        free(avail);
        return -1;
    }
    for (size_t i = 0; i < n0; i++)
        if ((*segs)[i].state == GK3W_SEG_TODO)
            order[no++] = i;
    g_sort_segs = *segs;
    qsort(order, no, sizeof *order, by_lcn_desc);

    /* 第 1 步：选空隙 */
    for (size_t k = 0; k < no; k++) {
        size_t idx = order[k];
        for (;;) {
            gk3w_mft_seg cur = (*segs)[idx];
            size_t r;
            int64_t usable = 0, cnt;
            for (r = 0; r < nr; r++) {
                if (runs[r].start >= cur.lcn) {
                    r = nr;
                    break;
                }
                /* 空隙够不够大按它本身的剩余容量判断；能放多少要扣掉对齐预留 */
                usable = avail[r] - (align - 1);
                if (usable > 0 && (avail[r] >= min_run || usable >= cur.count))
                    break;
            }
            if (r >= nr)
                break;   /* 没有放得下的空隙：留着（gap = -1） */
            cnt = cur.count < usable ? cur.count : usable;
            avail[r] -= cnt + (align - 1);
            if (cnt == cur.count) {
                (*segs)[idx].gap = (int)r;
                break;
            }
            /* 拆开：前一部分放进这个空隙，其余的（位置更高）接着找 */
            if (push_seg(segs, n, cap, (gk3w_mft_seg){cur.stream, cur.vcn, cur.lcn, cnt, (int)r, 0, GK3W_SEG_TODO})) {
                free(avail);
                free(order);
                return -1;
            }
            (*segs)[idx].vcn += cnt;
            (*segs)[idx].lcn += cnt;
            (*segs)[idx].count -= cnt;
        }
    }
    free(order);

    /* 第 2 步：每个空隙里从底部按流、VCN 排开 */
    order = malloc((*n ? *n : 1) * sizeof *order);
    if (!order) {
        free(avail);
        return -1;
    }
    no = 0;
    for (size_t i = 0; i < *n; i++)
        if ((*segs)[i].state == GK3W_SEG_TODO && (*segs)[i].gap >= 0)
            order[no++] = i;
    g_sort_segs = *segs;
    qsort(order, no, sizeof *order, by_gap_vcn);
    int64_t pos = 0;
    for (size_t k = 0; k < no; k++) {
        gk3w_mft_seg *g = &(*segs)[order[k]];
        if (!k || (*segs)[order[k - 1]].gap != g->gap)
            pos = runs[g->gap].start;
        /* lcn − target 取 align 的整数倍（源总在空隙上方，差为正） */
        g->target = pos + (g->lcn - pos) % align;
        pos = g->target + g->count;
    }
    free(order);
    free(avail);
    return 0;
}

int gk3w_mft_order(const gk3w_mft_seg *segs, size_t n, size_t **order, size_t *no)
{
    *no = 0;
    *order = malloc((n ? n : 1) * sizeof **order);
    if (!*order)
        return -1;
    for (size_t i = 0; i < n; i++)
        if (segs[i].state == GK3W_SEG_TODO && segs[i].gap >= 0)
            (*order)[(*no)++] = i;
    g_sort_segs = segs;
    qsort(*order, *no, sizeof **order, by_lcn_desc);
    return 0;
}

void gk3w_mft_summary(const gk3w_mft_seg *segs, size_t n, int execute, int64_t fixed_end, gk3w_mft_result *r)
{
    int64_t low_end = fixed_end;
    r->moved = r->unplaced = 0;
    r->steps = 0;
    for (size_t i = 0; i < n; i++) {
        const gk3w_mft_seg *g = &segs[i];
        int placed = g->state == GK3W_SEG_DONE || (!execute && g->state == GK3W_SEG_TODO && g->gap >= 0);
        int64_t e = placed ? g->target + g->count : g->lcn + g->count;
        if ((placed || g->state == GK3W_SEG_FIXED) && e > low_end)
            low_end = e;
        if (placed) {
            r->moved += g->count;
            r->steps++;
        }
    }
    r->end = low_end;
    for (size_t i = 0; i < n; i++) {
        const gk3w_mft_seg *g = &segs[i];
        int placed = g->state == GK3W_SEG_DONE || (!execute && g->state == GK3W_SEG_TODO && g->gap >= 0);
        if (placed || g->state == GK3W_SEG_FIXED)
            continue;
        if (g->lcn + g->count > r->end)
            r->end = g->lcn + g->count;
        /* 执行时分到了空隙却没挪成的（失败、中止）：原因是移动出错，另有 ERR */
        if (execute && g->gap >= 0)
            continue;
        if (g->lcn + g->count > low_end)
            r->unplaced += g->count;
    }
}
