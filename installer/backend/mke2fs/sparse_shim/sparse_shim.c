/* SPDX-License-Identifier: MIT */
/* libsparse 子集的实现（说明见 sparse/sparse.h）。输出格式：refs/lineage-system-core/libsparse/sparse_format.h:25-51。
 *   - 按块号排好序，数据段之间与末尾的空缺写 DONT_CARE；
 *   - 全零的块合成 FILL（图案 0）：mke2fs 在 Windows 上不走 lazy_itable_init，会把 inode 表整个写成零，
 *     不合并的话 userdata 的镜像要多几百 MiB；
 *   - RAW 每段至多 16 MiB（chunk 头里的 total_sz 是 32 位）。 */
#include "sparse/sparse.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SPARSE_MAGIC 0xED26FF3Au
#define CH_RAW 0xCAC1u
#define CH_FILL 0xCAC2u
#define CH_DONT_CARE 0xCAC3u
#define FILE_HDR_LEN 28u
#define CHUNK_HDR_LEN 12u
#define RAW_MAX_BYTES (16u << 20)

struct seg {
    const uint8_t *data;
    uint64_t block, nblocks;
};

struct sparse_file {
    unsigned int bs;
    uint64_t total_blocks;
    struct seg *v;
    size_t n, cap;
};

struct sparse_file *sparse_file_new(unsigned int block_size, int64_t len)
{
    struct sparse_file *s;
    if (!block_size || block_size % 4 || len < 0)
        return NULL;
    s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->bs = block_size;
    s->total_blocks = ((uint64_t)len + block_size - 1) / block_size;
    return s;
}

void sparse_file_destroy(struct sparse_file *s)
{
    if (!s)
        return;
    free(s->v);
    free(s);
}

int sparse_file_add_data(struct sparse_file *s, void *data, uint64_t len, unsigned int block)
{
    if (!s || !data || !len || len % s->bs || block >= s->total_blocks || len / s->bs > s->total_blocks - block)
        return -EINVAL;
    if (s->n == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 64;
        struct seg *v = realloc(s->v, cap * sizeof *v);
        if (!v)
            return -ENOMEM;
        s->v = v;
        s->cap = cap;
    }
    s->v[s->n++] = (struct seg){data, block, len / s->bs};
    return 0;
}

static int cmp_seg(const void *a, const void *b)
{
    const struct seg *x = a, *y = b;
    return x->block < y->block ? -1 : x->block > y->block;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static int write_all(int fd, const void *buf, size_t n)
{
    const uint8_t *p = buf;
    while (n) {
        ssize_t w = write(fd, p, n > (1u << 30) ? (1u << 30) : (unsigned int)n);
        if (w <= 0)
            return -EIO;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static int is_zero(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (p[i])
            return 0;
    return 1;
}

/* 走一遍要写的 chunk。fd < 0 时只计数。 */
static int emit_all(const struct sparse_file *s, int fd, uint32_t *count)
{
    uint64_t cur = 0;
    uint8_t h[CHUNK_HDR_LEN];
    uint32_t n = 0;
    const uint64_t raw_max = RAW_MAX_BYTES / s->bs ? RAW_MAX_BYTES / s->bs : 1;

#define CHUNK(type, blocks, datalen)                                                                \
    do {                                                                                            \
        n++;                                                                                        \
        if (fd >= 0) {                                                                              \
            put16(h, (type));                                                                       \
            put16(h + 2, 0);                                                                        \
            put32(h + 4, (uint32_t)(blocks));                                                       \
            put32(h + 8, (uint32_t)(CHUNK_HDR_LEN + (datalen)));                                    \
            if (write_all(fd, h, sizeof h))                                                         \
                return -EIO;                                                                        \
        }                                                                                           \
    } while (0)

    for (size_t i = 0; i <= s->n; i++) {
        uint64_t next = i < s->n ? s->v[i].block : s->total_blocks;
        /* 空缺：DONT_CARE（块数是 32 位，过大时拆开） */
        while (cur < next) {
            uint64_t k = next - cur > 0xFFFFFFFFu ? 0xFFFFFFFFu : next - cur;
            CHUNK(CH_DONT_CARE, k, 0);
            cur += k;
        }
        if (i == s->n)
            break;
        /* 数据段：全零的连续块合成 FILL，其余按 RAW_MAX 切 */
        for (uint64_t b = 0; b < s->v[i].nblocks;) {
            const uint8_t *p = s->v[i].data + b * s->bs;
            uint64_t k = 0;
            int zero = is_zero(p, s->bs);
            while (b + k < s->v[i].nblocks && k < (zero ? 0xFFFFFFFFu : raw_max) &&
                   is_zero(s->v[i].data + (b + k) * s->bs, s->bs) == zero)
                k++;
            if (zero) {
                uint8_t pat[4] = {0, 0, 0, 0};
                CHUNK(CH_FILL, k, 4);
                if (fd >= 0 && write_all(fd, pat, 4))
                    return -EIO;
            } else {
                CHUNK(CH_RAW, k, k * s->bs);
                if (fd >= 0 && write_all(fd, p, (size_t)(k * s->bs)))
                    return -EIO;
            }
            b += k;
        }
        cur = s->v[i].block + s->v[i].nblocks;
    }
#undef CHUNK
    *count = n;
    return 0;
}

int sparse_file_write(struct sparse_file *s, int fd, bool gz, bool sparse, bool crc)
{
    uint8_t h[FILE_HDR_LEN];
    uint32_t chunks = 0;
    int r;
    if (!s || gz || !sparse || crc || s->total_blocks > 0xFFFFFFFFu)
        return -EINVAL;
    qsort(s->v, s->n, sizeof *s->v, cmp_seg);
    for (size_t i = 1; i < s->n; i++)
        if (s->v[i].block < s->v[i - 1].block + s->v[i - 1].nblocks)
            return -EINVAL;   /* 重叠：sparse_io.c 不会这样加，出现就是用错了 */
    if ((r = emit_all(s, -1, &chunks)))
        return r;
    put32(h, SPARSE_MAGIC);
    put16(h + 4, 1);
    put16(h + 6, 0);
    put16(h + 8, FILE_HDR_LEN);
    put16(h + 10, CHUNK_HDR_LEN);
    put32(h + 12, s->bs);
    put32(h + 16, (uint32_t)s->total_blocks);
    put32(h + 20, chunks);
    put32(h + 24, 0);
    if (write_all(fd, h, sizeof h))
        return -EIO;
    return emit_all(s, fd, &chunks);
}

struct sparse_file *sparse_file_import(int fd, bool verbose, bool crc)
{
    (void)fd;
    (void)verbose;
    (void)crc;
    return NULL;
}

unsigned int sparse_file_block_size(struct sparse_file *s)
{
    return s ? s->bs : 0;
}

int64_t sparse_file_len(struct sparse_file *s, bool sparse, bool crc)
{
    (void)sparse;
    (void)crc;
    return s ? (int64_t)(s->total_blocks * s->bs) : -1;
}

int sparse_file_foreach_chunk(struct sparse_file *s, bool sparse, bool crc,
                              int (*write_cb)(void *priv, const void *data, size_t len, unsigned int block,
                                              unsigned int nr_blocks),
                              void *priv)
{
    (void)s;
    (void)sparse;
    (void)crc;
    (void)write_cb;
    (void)priv;
    return -EINVAL;
}
