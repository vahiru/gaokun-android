/* 与平台无关的分区布局计算：空闲区。以后 plan 也放在这一层，Linux 与 Windows 共用。 */
#ifndef GK3W_LAYOUT_H
#define GK3W_LAYOUT_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t first, last;   /* 含两端，单位是逻辑扇区 */
} gk3w_span;

/* 空闲区上限：GPT 最多 128 个表项时，空闲区不会超过 129 段 */
#define GK3W_MAX_PARTS 256

typedef enum {
    GK3W_LAYOUT_OK = 0,
    GK3W_LAYOUT_EINVAL,     /* 参数不对：usable 区间为空、扇区大小为 0、表项太多 */
    GK3W_LAYOUT_EBADPART,   /* 有分区首大于尾，或超出 usable 区间 */
    GK3W_LAYOUT_EOVERLAP,   /* 有分区相互重叠 */
} gk3w_layout_err;

/* 在 [usable_first, usable_last] 里找出没被 parts 占用的区间，按起点升序写进 out。
 * 小于 min_bytes 的不报（GPT 对齐留下的缝隙）。*n_out 是找到的段数，可能大于 cap（只写了前 cap 段）。 */
gk3w_layout_err gk3w_free_spans(uint64_t usable_first, uint64_t usable_last, const gk3w_span *parts, size_t n,
                                uint32_t sector_bytes, uint64_t min_bytes, gk3w_span *out, size_t cap,
                                size_t *n_out);

/* 区间的字节数；溢出时饱和到 UINT64_MAX */
uint64_t gk3w_span_bytes(gk3w_span s, uint32_t sector_bytes);

#endif
