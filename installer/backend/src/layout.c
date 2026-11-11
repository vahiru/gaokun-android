/* 分区布局计算（说明见 layout.h）。 */
#include "layout.h"

uint64_t gk3w_span_bytes(gk3w_span s, uint32_t sector_bytes)
{
    uint64_t n;
    if (s.last < s.first)
        return 0;
    n = s.last - s.first;
    if (n == UINT64_MAX)
        return UINT64_MAX;
    n += 1;
    if (sector_bytes && n > UINT64_MAX / sector_bytes)
        return UINT64_MAX;
    return n * sector_bytes;
}

gk3w_layout_err gk3w_free_spans(uint64_t usable_first, uint64_t usable_last, const gk3w_span *parts, size_t n,
                                uint32_t sector_bytes, uint64_t min_bytes, gk3w_span *out, size_t cap,
                                size_t *n_out)
{
    gk3w_span sorted[GK3W_MAX_PARTS];
    uint64_t cursor = usable_first;
    size_t found = 0;
    int done = 0;

    *n_out = 0;
    if (!sector_bytes || usable_last < usable_first || n > GK3W_MAX_PARTS)
        return GK3W_LAYOUT_EINVAL;

    /* 插入排序：表项至多几百个 */
    for (size_t i = 0; i < n; i++) {
        size_t j = i;
        if (parts[i].last < parts[i].first || parts[i].first < usable_first || parts[i].last > usable_last)
            return GK3W_LAYOUT_EBADPART;
        while (j > 0 && sorted[j - 1].first > parts[i].first) {
            sorted[j] = sorted[j - 1];
            j--;
        }
        sorted[j] = parts[i];
    }

    for (size_t i = 0; i <= n && !done; i++) {
        gk3w_span gap;
        if (i < n) {
            if (sorted[i].first < cursor)
                return GK3W_LAYOUT_EOVERLAP;
            if (sorted[i].first == cursor) {
                if (sorted[i].last == usable_last)
                    done = 1;   /* 最后一个分区顶到了 usable 区间的末尾 */
                cursor = sorted[i].last + 1;
                continue;
            }
            gap.first = cursor;
            gap.last = sorted[i].first - 1;
            if (sorted[i].last == usable_last)
                done = 1;
            cursor = sorted[i].last + 1;
        } else {
            gap.first = cursor;
            gap.last = usable_last;
        }
        if (gk3w_span_bytes(gap, sector_bytes) < min_bytes)
            continue;
        if (found < cap)
            out[found] = gap;
        found++;
    }
    *n_out = found;
    return GK3W_LAYOUT_OK;
}
