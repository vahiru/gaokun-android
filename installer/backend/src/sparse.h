/* Android sparse 镜像（libsparse 格式）的流式展开：输入按任意大小的片段喂进来（zstd 解码器的输出），
 * 展开后的数据经回调写到目标。语义与 simg2img 相同：
 *   - RAW 原样写；FILL 按 4 字节图案填；DONT_CARE 跳过、【不写零】；CRC32 块只消费、不校验。
 *   - 输入提前结束、块数或 chunk 数对不上、chunk 长度与类型不符、展开后超过目标大小、最后一个 chunk 之后还有字节，
 *     一律失败。
 * 格式：refs/lineage-system-core/libsparse/sparse_format.h:25-51（与 tools/gk3boot/fastbootd/sparse.c 同一份规格）。 */
#ifndef GK3W_SPARSE_H
#define GK3W_SPARSE_H

#include <stddef.h>
#include <stdint.h>

#define GK3W_SPARSE_MAGIC 0xED26FF3Au

typedef struct {
    void *ctx;
    /* 把 data[0, len) 写到展开后偏移 off 处。off 与 len 都是 512 的整数倍。返回 0 成功。 */
    int (*write)(void *ctx, uint64_t off, const uint8_t *data, size_t len);
    /* 从 off 起 len 字节填 4 字节图案（小端）。off 与 len 都是 512 的整数倍。返回 0 成功。 */
    int (*fill)(void *ctx, uint64_t off, uint64_t len, uint32_t pattern);
} gk3w_sparse_sink;

typedef struct {
    int state;
    uint8_t hdr[28];
    size_t have, skip;
    uint32_t blk_sz, total_blks, total_chunks, chunk_hdr_sz;
    uint32_t chunks_seen;
    uint64_t blks_seen;
    uint64_t limit;             /* 目标的字节数：展开后的总长不得超过它 */
    uint64_t out_off;           /* 下一个字节在展开后的偏移 */
    uint64_t left;              /* 当前 chunk 还剩多少数据字节 */
    uint16_t chunk_type;
    uint32_t chunk_blocks;
    uint8_t *buf;               /* RAW 数据的攒写缓冲（调用方给，地址与容量按 4096 对齐） */
    size_t buf_cap, buf_len;
    uint64_t buf_off;
    const char *err;            /* 失败原因（英文代码，进 ERR 的 why=） */
} gk3w_sparse;

/* buf_cap 必须是 4096 的整数倍且非零。 */
int gk3w_sparse_init(gk3w_sparse *s, uint64_t limit, uint8_t *buf, size_t buf_cap);
/* 返回 0 成功；-1 失败，原因在 s->err。sink 为 NULL 时只校验不写（安装前的完整试解）。 */
int gk3w_sparse_feed(gk3w_sparse *s, const uint8_t *data, size_t len, const gk3w_sparse_sink *sink);
/* 输入结束：把攒着的数据写出去，并核对 chunk 数与块数。 */
int gk3w_sparse_finish(gk3w_sparse *s, const gk3w_sparse_sink *sink);
/* 展开后的总字节数（头读完之前是 0） */
uint64_t gk3w_sparse_total(const gk3w_sparse *s);

#endif
