/* ISO 9660 的 Joliet 树（说明见 iso9660.h）。 */
#include "iso9660.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VD_FIRST 16u           /* 卷描述符从第 16 扇区开始 */
#define VD_MAX 64u             /* 最多看这么多个卷描述符（终止符之前） */
#define DEPTH_MAX 8            /* 目录层数上限（ISO 9660 本身限 8 层） */
#define DIR_MAX (4u << 20)     /* 单个目录的记录区上限 */

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Joliet 名（UCS-2 大端）→ UTF-8，去掉末尾的 ";数字" 版本号。不收控制字符、路径分隔符、代理项与 "." ".." */
static int joliet_name(const uint8_t *p, size_t len, char *out, size_t cap)
{
    size_t o = 0;
    char *semi;
    if (!len || len % 2)
        return -1;
    for (size_t i = 0; i < len; i += 2) {
        unsigned c = (unsigned)p[i] << 8 | p[i + 1];
        if (c < 0x20 || c == '/' || c == '\\' || (c >= 0xD800 && c <= 0xDFFF))
            return -1;
        if (o + 4 >= cap)
            return -1;
        if (c < 0x80) {
            out[o++] = (char)c;
        } else if (c < 0x800) {
            out[o++] = (char)(0xC0 | c >> 6);
            out[o++] = (char)(0x80 | (c & 0x3F));
        } else {
            out[o++] = (char)(0xE0 | c >> 12);
            out[o++] = (char)(0x80 | (c >> 6 & 0x3F));
            out[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[o] = 0;
    semi = strrchr(out, ';');
    if (semi && semi[1]) {
        char *d = semi + 1;
        while (*d >= '0' && *d <= '9')
            d++;
        if (!*d)
            *semi = 0;
    }
    if (!out[0] || !strcmp(out, ".") || !strcmp(out, ".."))
        return -1;
    return 0;
}

static gk3w_iso_err walk(const gk3w_iso_src *src, uint64_t iso_size, uint32_t lba, uint32_t len, const char *prefix,
                         int depth, gk3w_iso_entry *out, size_t cap, size_t *n)
{
    uint8_t *buf;
    size_t i = 0;
    gk3w_iso_err err = GK3W_ISO_OK;
    if (depth > DEPTH_MAX || !len || len > DIR_MAX || (uint64_t)lba * GK3W_ISO_SECTOR + len > iso_size)
        return GK3W_ISO_BAD;
    buf = malloc(len);
    if (!buf)
        return GK3W_ISO_IO;
    if (src->read(src->ctx, (uint64_t)lba * GK3W_ISO_SECTOR, buf, len)) {
        free(buf);
        return GK3W_ISO_IO;
    }
    while (i < len && !err) {
        const uint8_t *r = buf + i;
        uint8_t rl = r[0], nl, flags;
        uint32_t ext, size;
        char name[256];
        gk3w_iso_entry *e;
        int w;
        /* 记录不跨扇区：长度 0 表示这个扇区里没有了 */
        if (!rl) {
            i = (i / GK3W_ISO_SECTOR + 1) * GK3W_ISO_SECTOR;
            continue;
        }
        if (rl < 34 || i + rl > len || (i % GK3W_ISO_SECTOR) + rl > GK3W_ISO_SECTOR) {
            err = GK3W_ISO_BAD;
            break;
        }
        nl = r[32];
        flags = r[25];
        if ((size_t)33 + nl > rl) {
            err = GK3W_ISO_BAD;
            break;
        }
        i += rl;
        /* "." 与 ".."（名字是一个字节 0 或 1）、隐藏的条目 */
        if ((nl == 1 && (r[33] == 0 || r[33] == 1)) || (flags & 0x01))
            continue;
        /* 多区段文件、交错文件 */
        if ((flags & 0x80) || r[26] || r[27]) {
            err = GK3W_ISO_BAD;
            break;
        }
        ext = le32(r + 2);
        size = le32(r + 10);
        if (joliet_name(r + 33, nl, name, sizeof name)) {
            err = GK3W_ISO_BAD;
            break;
        }
        if (*n >= cap) {
            err = GK3W_ISO_TOO_MANY;
            break;
        }
        e = &out[*n];
        w = prefix[0] ? snprintf(e->path, sizeof e->path, "%s/%s", prefix, name) : snprintf(e->path, sizeof e->path, "%s", name);
        if (w < 0 || (size_t)w >= sizeof e->path) {
            err = GK3W_ISO_BAD;
            break;
        }
        e->offset = (uint64_t)ext * GK3W_ISO_SECTOR;
        e->size = size;
        e->is_dir = !!(flags & 0x02);
        if (e->offset + e->size > iso_size) {
            err = GK3W_ISO_BAD;
            break;
        }
        (*n)++;
        if (e->is_dir) {
            char sub[GK3W_ISO_PATH_MAX];
            memcpy(sub, e->path, sizeof sub);
            err = walk(src, iso_size, ext, size, sub, depth + 1, out, cap, n);
        }
    }
    free(buf);
    return err;
}

gk3w_iso_err gk3w_iso_list(const gk3w_iso_src *src, uint64_t iso_size, gk3w_iso_entry *out, size_t cap, size_t *n)
{
    uint8_t vd[GK3W_ISO_SECTOR];
    int pvd = 0, joliet = 0;
    uint32_t root_lba = 0, root_len = 0;
    *n = 0;
    for (uint32_t s = VD_FIRST; s < VD_FIRST + VD_MAX; s++) {
        if ((uint64_t)(s + 1) * GK3W_ISO_SECTOR > iso_size)
            return pvd ? GK3W_ISO_BAD : GK3W_ISO_NOT_ISO;
        if (src->read(src->ctx, (uint64_t)s * GK3W_ISO_SECTOR, vd, sizeof vd))
            return GK3W_ISO_IO;
        if (memcmp(vd + 1, "CD001", 5) || vd[6] != 1)
            return pvd ? GK3W_ISO_BAD : GK3W_ISO_NOT_ISO;
        if (vd[0] == 255)
            break;
        if (vd[0] == 1) {
            if (le16(vd + 128) != GK3W_ISO_SECTOR)
                return GK3W_ISO_BAD;
            pvd = 1;
        } else if (vd[0] == 2 && vd[88] == '%' && vd[89] == '/' && (vd[90] == '@' || vd[90] == 'C' || vd[90] == 'E')) {
            if (le16(vd + 128) != GK3W_ISO_SECTOR)
                return GK3W_ISO_BAD;
            /* 根目录记录在偏移 156，34 字节：区段起点在 +2、长度在 +10（小端那一半） */
            root_lba = le32(vd + 156 + 2);
            root_len = le32(vd + 156 + 10);
            joliet = 1;
        }
    }
    if (!pvd)
        return GK3W_ISO_NOT_ISO;
    if (!joliet)
        return GK3W_ISO_NO_JOLIET;
    return walk(src, iso_size, root_lba, root_len, "", 0, out, cap, n);
}

const char *gk3w_iso_strerror(gk3w_iso_err e)
{
    switch (e) {
    case GK3W_ISO_OK: return "ok";
    case GK3W_ISO_IO: return "the ISO file could not be read";
    case GK3W_ISO_NOT_ISO: return "the file is not an ISO 9660 image";
    case GK3W_ISO_NO_JOLIET: return "the ISO has no Joliet file names";
    case GK3W_ISO_BAD: return "the ISO directory tree is malformed or uses unsupported features";
    case GK3W_ISO_TOO_MANY: return "the ISO holds more files than expected";
    }
    return "unknown ISO error";
}
