/* gk3-fastbootd：Android sparse 镜像（libsparse 格式）。
 *
 * 格式与语义对齐 libsparse（system/core/libsparse/sparse_format.h、sparse_read.cpp）和本仓的
 * simg2img：
 *   文件头 28 字节：magic u32 / major u16(=1) / minor u16 / file_hdr_sz u16 / chunk_hdr_sz u16 /
 *                   blk_sz u32 / total_blks u32 / total_chunks u32 / image_checksum u32
 *   chunk 头 12 字节：chunk_type u16 / reserved u16 / chunk_sz u32（块数）/ total_sz u32（含头的字节数）
 *   RAW 0xCAC1（数据 chunk_sz*blk_sz）、FILL 0xCAC2（4 字节图案）、DONT_CARE 0xCAC3（不写）、CRC32 0xCAC4（4 字节）
 *
 * ★ 主机按 max-download-size 切片时，每一片都是一份完整的 sparse（total_blks 相同），自己不管的区间用
 *   DONT_CARE 跳过（fastboot/fastboot.cpp resparse_file）⇒ DONT_CARE 绝不能写零，否则后一片会抹掉前一片。
 * ★ 先整份校验、再写：任何 chunk 越界、长度对不上、块数合计不等于 total_blks、尾部有多余字节，一律整份拒绝，
 *   一个字节都不写（fastboot-design §4.2.1"任何 chunk 越界即拒"）。
 */
#include <string.h>

#include "fbd.h"

#define CH_RAW  0xCAC1u
#define CH_FILL 0xCAC2u
#define CH_DC   0xCAC3u
#define CH_CRC  0xCAC4u
#define FILE_HDR_LEN  28u
#define CHUNK_HDR_LEN 12u

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

bool fb_sparse_is(const void *img, size_t n)
{
    return n >= FILE_HDR_LEN && le32(img) == FB_SPARSE_MAGIC;
}

static const char *parse_hdr(const uint8_t *b, size_t n, fb_sparse_hdr *h, uint32_t *fhs, uint32_t *chs)
{
    if (n < FILE_HDR_LEN || le32(b) != FB_SPARSE_MAGIC)
        return "not a sparse image";
    if (le16(b + 4) != 1)
        return "sparse: unsupported major version";
    *fhs = le16(b + 8);
    *chs = le16(b + 10);
    if (*fhs < FILE_HDR_LEN || *fhs > n)
        return "sparse: bad file header size";
    if (*chs < CHUNK_HDR_LEN)
        return "sparse: bad chunk header size";
    h->blk_sz = le32(b + 12);
    h->total_blks = le32(b + 16);
    h->total_chunks = le32(b + 20);
    if (h->blk_sz == 0 || (h->blk_sz & 3))
        return "sparse: block size must be a non-zero multiple of 4";
    h->out_size = (uint64_t)h->total_blks * h->blk_sz;
    return NULL;
}

const char *fb_sparse_check(const void *img, size_t n, uint64_t part_size, fb_sparse_hdr *h)
{
    const uint8_t *b = img;
    uint32_t fhs, chs;
    uint64_t pos, blk = 0;
    const char *e = parse_hdr(b, n, h, &fhs, &chs);
    if (e)
        return e;
    if (h->out_size > part_size)
        return "sparse: image is larger than the partition";
    pos = fhs;
    for (uint32_t i = 0; i < h->total_chunks; i++) {
        uint16_t type;
        uint32_t csz, tsz;
        uint64_t data, bytes;
        if (pos > n || n - pos < chs)
            return "sparse: truncated chunk header";
        type = le16(b + pos);
        csz = le32(b + pos + 4);
        tsz = le32(b + pos + 8);
        if (tsz < chs || (uint64_t)tsz > n - pos)
            return "sparse: chunk extends past the end of the download";
        data = tsz - chs;
        bytes = (uint64_t)csz * h->blk_sz;
        switch (type) {
        case CH_RAW:
            if (data != bytes)
                return "sparse: raw chunk size mismatch";
            break;
        case CH_FILL:
            if (data != 4)
                return "sparse: fill chunk size mismatch";
            break;
        case CH_DC:
            if (data != 0)
                return "sparse: dont-care chunk size mismatch";
            break;
        case CH_CRC:
            if (data != 4)
                return "sparse: crc chunk size mismatch";
            if (csz != 0)
                return "sparse: crc chunk with blocks";
            break;
        default:
            return "sparse: unknown chunk type";
        }
        if (csz > h->total_blks - blk)
            return "sparse: chunk runs past total_blks (out of bounds)";
        blk += csz;
        pos += tsz;
    }
    if (blk != h->total_blks)
        return "sparse: chunk blocks do not add up to total_blks";
    if (pos != n)
        return "sparse: trailing bytes after the last chunk";
    return NULL;
}

int fb_sparse_walk(const void *img, size_t n, const fb_sparse_ops *ops)
{
    const uint8_t *b = img;
    fb_sparse_hdr h;
    uint32_t fhs, chs;
    uint64_t pos, blk = 0;
    if (parse_hdr(b, n, &h, &fhs, &chs))
        return -1;
    pos = fhs;
    for (uint32_t i = 0; i < h.total_chunks; i++) {
        uint16_t type = le16(b + pos);
        uint32_t csz = le32(b + pos + 4), tsz = le32(b + pos + 8);
        uint64_t off = blk * h.blk_sz, bytes = (uint64_t)csz * h.blk_sz;
        if (type == CH_RAW && csz) {
            if (ops->raw(ops->ctx, off, b + pos + chs, (size_t)bytes))
                return -1;
        } else if (type == CH_FILL && csz) {
            if (ops->fill(ops->ctx, off, le32(b + pos + chs), bytes))
                return -1;
        }
        blk += csz;
        pos += tsz;
    }
    return 0;
}
