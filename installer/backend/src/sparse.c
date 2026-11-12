/* sparse 镜像的流式展开（说明见 sparse.h）。 */
#include "sparse.h"

#include <string.h>

#define FILE_HDR_LEN 28u
#define CHUNK_HDR_LEN 12u
#define CH_RAW 0xCAC1u
#define CH_FILL 0xCAC2u
#define CH_DONT_CARE 0xCAC3u
#define CH_CRC32 0xCAC4u
#define MAX_BLK_SZ (64u << 20)

enum { ST_FHDR, ST_SKIP_FHDR, ST_CHDR, ST_SKIP_CHDR, ST_RAW, ST_WORD, ST_DONE, ST_ERR };

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int fail(gk3w_sparse *s, const char *why)
{
    s->state = ST_ERR;
    s->err = why;
    return -1;
}

int gk3w_sparse_init(gk3w_sparse *s, uint64_t limit, uint8_t *buf, size_t buf_cap)
{
    memset(s, 0, sizeof *s);
    s->limit = limit;
    s->buf = buf;
    s->buf_cap = buf_cap;
    if (!buf || !buf_cap || buf_cap % 4096)
        return fail(s, "bad-buffer");
    return 0;
}

uint64_t gk3w_sparse_total(const gk3w_sparse *s)
{
    return (uint64_t)s->total_blks * s->blk_sz;
}

static int flush(gk3w_sparse *s, const gk3w_sparse_sink *sink)
{
    if (s->buf_len && sink && sink->write(sink->ctx, s->buf_off, s->buf, s->buf_len))
        return fail(s, "write");
    s->buf_len = 0;
    return 0;
}

/* 一个 chunk 的数据收完 */
static int chunk_done(gk3w_sparse *s)
{
    s->chunks_seen++;
    s->blks_seen += s->chunk_blocks;
    s->state = s->chunks_seen == s->total_chunks ? ST_DONE : ST_CHDR;
    s->have = 0;
    return 0;
}

static int parse_file_hdr(gk3w_sparse *s)
{
    const uint8_t *h = s->hdr;
    uint32_t file_hdr_sz = le16(h + 8);
    if (le32(h) != GK3W_SPARSE_MAGIC)
        return fail(s, "bad-magic");
    if (le16(h + 4) != 1)
        return fail(s, "bad-major");
    s->chunk_hdr_sz = le16(h + 10);
    s->blk_sz = le32(h + 12);
    s->total_blks = le32(h + 16);
    s->total_chunks = le32(h + 20);
    if (file_hdr_sz < FILE_HDR_LEN || s->chunk_hdr_sz < CHUNK_HDR_LEN)
        return fail(s, "bad-hdr-size");
    /* libsparse 只要求 4 的倍数；写盘要按扇区对齐，所以这里要 512 的倍数（super 是 4096） */
    if (!s->blk_sz || s->blk_sz % 512 || s->blk_sz > MAX_BLK_SZ)
        return fail(s, "bad-blk-sz");
    if (gk3w_sparse_total(s) > s->limit)
        return fail(s, "too-big");
    s->skip = file_hdr_sz - FILE_HDR_LEN;
    s->have = 0;
    s->state = s->skip ? ST_SKIP_FHDR : (s->total_chunks ? ST_CHDR : ST_DONE);
    return 0;
}

static int parse_chunk_hdr(gk3w_sparse *s, const gk3w_sparse_sink *sink)
{
    const uint8_t *h = s->hdr;
    uint32_t total_sz = le32(h + 8);
    uint64_t data, bytes;
    s->chunk_type = le16(h);
    s->chunk_blocks = le32(h + 4);
    if (total_sz < s->chunk_hdr_sz)
        return fail(s, "bad-chunk-size");
    data = total_sz - s->chunk_hdr_sz;
    if (s->blks_seen + s->chunk_blocks > s->total_blks)
        return fail(s, "too-many-blocks");
    bytes = (uint64_t)s->chunk_blocks * s->blk_sz;
    switch (s->chunk_type) {
    case CH_RAW:
        if (data != bytes)
            return fail(s, "bad-raw-size");
        /* 与攒着的数据不连续就先写出去 */
        if (s->buf_len && s->buf_off + s->buf_len != s->out_off && flush(s, sink))
            return -1;
        s->left = bytes;
        break;
    case CH_FILL:
    case CH_CRC32:
        if (data != 4)
            return fail(s, "bad-word-size");
        if (s->chunk_type == CH_CRC32 && s->chunk_blocks)
            return fail(s, "bad-crc-chunk");
        s->left = 4;
        break;
    case CH_DONT_CARE:
        if (data)
            return fail(s, "bad-dont-care-size");
        s->left = 0;
        break;
    default:
        return fail(s, "bad-chunk-type");
    }
    s->skip = s->chunk_hdr_sz - CHUNK_HDR_LEN;
    s->have = 0;
    s->state = ST_SKIP_CHDR;
    return 0;
}

/* chunk 头（含多出来的部分）收完，进入数据阶段 */
static int begin_chunk_data(gk3w_sparse *s, const gk3w_sparse_sink *sink)
{
    uint64_t bytes = (uint64_t)s->chunk_blocks * s->blk_sz;
    switch (s->chunk_type) {
    case CH_RAW:
        if (!bytes)
            return chunk_done(s);
        s->state = ST_RAW;
        return 0;
    case CH_DONT_CARE:
        if (flush(s, sink))
            return -1;
        s->out_off += bytes;
        return chunk_done(s);
    default:
        s->state = ST_WORD;
        return 0;
    }
}

int gk3w_sparse_feed(gk3w_sparse *s, const uint8_t *data, size_t len, const gk3w_sparse_sink *sink)
{
    while (len) {
        size_t n;
        switch (s->state) {
        case ST_FHDR:
            n = FILE_HDR_LEN - s->have < len ? FILE_HDR_LEN - s->have : len;
            memcpy(s->hdr + s->have, data, n);
            s->have += n;
            if (s->have == FILE_HDR_LEN && parse_file_hdr(s))
                return -1;
            break;
        case ST_SKIP_FHDR:
        case ST_SKIP_CHDR:
            n = s->skip < len ? s->skip : len;
            s->skip -= n;
            if (!s->skip) {
                if (s->state == ST_SKIP_FHDR)
                    s->state = s->total_chunks ? ST_CHDR : ST_DONE;
                else if (begin_chunk_data(s, sink))
                    return -1;
            }
            break;
        case ST_CHDR:
            n = CHUNK_HDR_LEN - s->have < len ? CHUNK_HDR_LEN - s->have : len;
            memcpy(s->hdr + s->have, data, n);
            s->have += n;
            if (s->have == CHUNK_HDR_LEN) {
                if (parse_chunk_hdr(s, sink))
                    return -1;
                if (!s->skip && begin_chunk_data(s, sink))
                    return -1;
            }
            break;
        case ST_RAW:
            if (!s->buf_len)
                s->buf_off = s->out_off;
            n = s->buf_cap - s->buf_len;
            if (n > len)
                n = len;
            if (n > s->left)
                n = (size_t)s->left;
            if (sink)
                memcpy(s->buf + s->buf_len, data, n);
            s->buf_len += n;
            s->left -= n;
            s->out_off += n;
            if (s->buf_len == s->buf_cap && flush(s, sink))
                return -1;
            if (!s->left)
                chunk_done(s);
            break;
        case ST_WORD:
            n = 4 - s->have < len ? 4 - s->have : len;
            memcpy(s->hdr + s->have, data, n);
            s->have += n;
            if (s->have == 4) {
                if (s->chunk_type == CH_FILL) {
                    uint64_t bytes = (uint64_t)s->chunk_blocks * s->blk_sz;
                    if (flush(s, sink))
                        return -1;
                    if (bytes && sink && sink->fill(sink->ctx, s->out_off, bytes, le32(s->hdr)))
                        return fail(s, "write");
                    s->out_off += bytes;
                }
                chunk_done(s);
            }
            break;
        case ST_DONE:
            return fail(s, "trailing-data");
        default:
            return -1;
        }
        data += n;
        len -= n;
    }
    return 0;
}

int gk3w_sparse_finish(gk3w_sparse *s, const gk3w_sparse_sink *sink)
{
    if (s->state == ST_ERR)
        return -1;
    if (s->state != ST_DONE)
        return fail(s, "truncated");
    if (flush(s, sink))
        return -1;
    if (s->blks_seen != s->total_blks)
        return fail(s, "block-count");
    return 0;
}
