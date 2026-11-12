/* 把 MFT 挪到卷前部的规划（gk3w mft-move，见 win/mftmove.c），与平台无关（主机单测覆盖）：
 * 从卷的空闲位图里找空隙、把 MFT 的片段拆段、给每段安排目标位置、汇总结果。真正的移动在 mftmove.c。
 * 位置与长度都以簇计。 */
#ifndef GK3W_MFTPLAN_H
#define GK3W_MFTPLAN_H

#include <stddef.h>
#include <stdint.h>

/* $MFT 某个流的一个片段（FSCTL_GET_RETRIEVAL_POINTERS） */
typedef struct {
    int stream;                 /* 流的编号（mftmove.c 的 STREAM_NAMES 下标；0 = $DATA） */
    int64_t vcn, lcn, count;
} gk3w_mft_ext;

/* 一块空闲空隙 */
typedef struct {
    int64_t start, len;
} gk3w_mft_run;

enum { GK3W_SEG_TODO, GK3W_SEG_DONE, GK3W_SEG_FIXED };

/* 要挪的一段（至多 chunk）：规划时分到一个空隙（gap）并排好目标位置（target） */
typedef struct {
    int stream;
    int64_t vcn, lcn, count;
    int gap;                    /* 分到的空隙（runs 的下标），-1 = 没地方放 */
    int64_t target;
    int state;                  /* GK3W_SEG_TODO / _DONE（挪好了）/ _FIXED（NTFS 不让挪，留在原处） */
} gk3w_mft_seg;

/* 空闲位图（第 i 位为 1 = 簇 base + i 已占用）中，limit 以下、不短于 min_run 的空隙，按位置排好。
 * 失败（内存不足）返回 -1 */
int gk3w_mft_free_runs(const uint8_t *bitmap, int64_t base, int64_t bits, int64_t limit, int64_t min_run, gk3w_mft_run **out,
                       size_t *n);

/* 把片段按 chunk 拆段（追加到 *segs）。$DATA 开头（VCN < keep：16 条系统记录所在的簇）留在原处，
 * 它们的末尾记进 *fixed_end。失败返回 -1 */
int gk3w_mft_split(const gk3w_mft_ext *ext, size_t ne, int64_t keep, int64_t chunk, gk3w_mft_seg **segs, size_t *n, size_t *cap,
                   int64_t *fixed_end);

/* 给还没挪的段（GK3W_SEG_TODO）安排位置，分两步：
 * 1. 从位置最高的一段起，放进它下方最低的、放得下的空隙（剩余容量不小于 min_run，或者正好装得下）；放不下整段时
 *    拆开，前一部分放进这个空隙，其余的接着找下一个。每段按最坏情况多预留 align − 1 个簇给对齐。
 * 2. 每个空隙里按流与 VCN 从空隙底部依次排开：同一片段的相邻各段首尾相接、顺序不变，挪完仍是连续的一段；
 *    目标位置与源位置之差取 align 的整数倍。
 * 失败（内存不足）返回 -1 */
int gk3w_mft_plan(gk3w_mft_seg **segs, size_t *n, size_t *cap, const gk3w_mft_run *runs, size_t nr, int64_t min_run, int64_t align);

/* 执行顺序：已安排位置、还没挪的段，位置高的在前（下标数组，调用方 free）。失败返回 -1 */
int gk3w_mft_order(const gk3w_mft_seg *segs, size_t n, size_t **order, size_t *no);

typedef struct {
    int64_t end;                /* 挪完（或预计挪完）之后 MFT 的末尾 */
    int64_t moved;              /* 挪了（或预计挪）多少簇 */
    int64_t unplaced;           /* 没地方放、仍在挪好的部分上方的簇：它们决定了 MFT 的末尾 */
    int steps;
} gk3w_mft_result;

/* 汇总：execute 时按 GK3W_SEG_DONE 算，只出计划时按已安排位置的算。本来就在前部、下面没有空隙的段，以及执行时
 * 分到了空隙却没挪成的段（原因是移动出错）都不算 unplaced */
void gk3w_mft_summary(const gk3w_mft_seg *segs, size_t n, int execute, int64_t fixed_end, gk3w_mft_result *r);

#endif
