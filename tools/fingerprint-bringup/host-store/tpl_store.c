/* SPDX-License-Identifier: GPL-2.0 */
#include "tpl_store.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static uint16_t rd16(const uint8_t *p)
{
        return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
               ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr16(uint8_t *p, uint16_t v)
{
        p[0] = (uint8_t)v;
        p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, uint32_t v)
{
        p[0] = (uint8_t)v;
        p[1] = (uint8_t)(v >> 8);
        p[2] = (uint8_t)(v >> 16);
        p[3] = (uint8_t)(v >> 24);
}

unsigned ft_tpl_crc16(const uint8_t *data, size_t len)
{
        unsigned crc = 0;
        size_t i;
        int bit;

        for (i = 0; i < len; ++i) {
                crc ^= data[i];
                for (bit = 0; bit < 8; ++bit)
                        crc = (crc & 1u) ? ((crc >> 1) ^ 0xA001u) : (crc >> 1);
        }
        return crc & 0xFFFFu;
}

enum ft_tpl_status ft_tpl_validate(const uint8_t *blob, size_t len,
                                   uint16_t width, uint16_t height,
                                   uint32_t slot_bytes)
{
        size_t fixed;
        unsigned expected;

        if (!blob || !width || !height)
                return FT_TPL_ERR_ARG;
        if (slot_bytes < FT_TPL_MIN_SLOT_BYTES ||
            slot_bytes > FT_TPL_MAX_SLOT_BYTES)
                return FT_TPL_ERR_ARG;
        if (len > FT_TPL_MAX_BLOB_BYTES)
                return FT_TPL_ERR_LAYOUT;

        fixed = (size_t)FT_TPL_HEADER_BYTES +
                (size_t)FT_TPL_SLOT_COUNT * slot_bytes + FT_TPL_CRC_BYTES;
        if (len < fixed || (len - fixed) % FT_TPL_FEATURE_BYTES)
                return FT_TPL_ERR_LAYOUT;
        if (rd32(blob) != FT_TPL_MAGIC)
                return FT_TPL_ERR_MAGIC;
        if (rd32(blob + 4) != FT_TPL_HEADER_SIZE_FLD)
                return FT_TPL_ERR_LAYOUT;

        expected = ft_tpl_crc16(blob, len - FT_TPL_CRC_BYTES);
        if (rd16(blob + len - FT_TPL_CRC_BYTES) != expected)
                return FT_TPL_ERR_CRC;
        return FT_TPL_OK;
}

static void build_header(uint8_t *h, const struct ft_tpl_meta *m)
{
        memset(h, 0, FT_STORE_HDR_BYTES);
        memcpy(h, FT_STORE_MAGIC, 4);
        wr16(h + 4, FT_STORE_VERSION);
        wr16(h + 6, FT_STORE_HDR_BYTES);
        h[8] = m->finger;
        wr16(h + 12, m->width);
        wr16(h + 14, m->height);
        wr32(h + 16, m->slot_bytes);
        wr32(h + 20, m->blob_len);
        memcpy(h + 24, m->config_digest, FT_TPL_CONFIG_DIGEST);
        /* blob_crc16 and meta_crc16 are filled in by the writer. */
}

/* Parse a store container header into meta.  Does not look at the blob. */
static enum ft_tpl_status parse_header(const uint8_t *h, size_t hlen,
                                       struct ft_tpl_meta *m)
{
        if (hlen != FT_STORE_HDR_BYTES)
                return FT_TPL_ERR_META;
        if (memcmp(h, FT_STORE_MAGIC, 4))
                return FT_TPL_ERR_MAGIC;
        if (rd16(h + 4) != FT_STORE_VERSION)
                return FT_TPL_ERR_VERSION;
        if (rd16(h + 6) != FT_STORE_HDR_BYTES)
                return FT_TPL_ERR_META;
        if (rd16(h + 58) != ft_tpl_crc16(h, 58))
                return FT_TPL_ERR_META;

        m->finger = h[8];
        m->width = rd16(h + 12);
        m->height = rd16(h + 14);
        m->slot_bytes = rd32(h + 16);
        m->blob_len = rd32(h + 20);
        memcpy(m->config_digest, h + 24, FT_TPL_CONFIG_DIGEST);

        if (m->finger > FT_TPL_MAX_FINGER || !m->width || !m->height ||
            m->slot_bytes < FT_TPL_MIN_SLOT_BYTES ||
            m->slot_bytes > FT_TPL_MAX_SLOT_BYTES ||
            m->blob_len < FT_TPL_HEADER_BYTES +
                          FT_TPL_SLOT_COUNT * FT_TPL_MIN_SLOT_BYTES +
                          FT_TPL_CRC_BYTES ||
            m->blob_len > FT_TPL_MAX_BLOB_BYTES)
                return FT_TPL_ERR_META;
        return FT_TPL_OK;
}

static int write_all(int fd, const void *buf, size_t len)
{
        const uint8_t *p = buf;

        while (len) {
                ssize_t n = write(fd, p, len);
                if (n < 0) {
                        if (errno == EINTR)
                                continue;
                        return -1;
                }
                p += n;
                len -= (size_t)n;
        }
        return 0;
}

static int read_all(int fd, void *buf, size_t len)
{
        uint8_t *p = buf;

        while (len) {
                ssize_t n = read(fd, p, len);
                if (n < 0) {
                        if (errno == EINTR)
                                continue;
                        return -1;
                }
                if (n == 0)
                        return -1;
                p += n;
                len -= (size_t)n;
        }
        return 0;
}

enum ft_tpl_status ft_tpl_store_write(const char *path,
                                      const struct ft_tpl_meta *meta,
                                      const uint8_t *blob, size_t len)
{
        char tmp[4096];
        uint8_t h[FT_STORE_HDR_BYTES];
        enum ft_tpl_status st;
        int fd;
        int n;

        if (!path || !meta || !blob || len != meta->blob_len)
                return FT_TPL_ERR_ARG;
        st = ft_tpl_validate(blob, len, meta->width, meta->height,
                             meta->slot_bytes);
        if (st != FT_TPL_OK)
                return st;
        if (meta->finger > FT_TPL_MAX_FINGER)
                return FT_TPL_ERR_ARG;

        build_header(h, meta);
        wr16(h + 56, rd16(blob + len - FT_TPL_CRC_BYTES));
        wr16(h + 58, (uint16_t)ft_tpl_crc16(h, 58));

        n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
        if (n < 0 || (size_t)n >= sizeof(tmp))
                return FT_TPL_ERR_ARG;

        fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
        if (fd < 0)
                return FT_TPL_ERR_IO;
        if (write_all(fd, h, sizeof(h)) || write_all(fd, blob, len) ||
            fsync(fd)) {
                close(fd);
                unlink(tmp);
                return FT_TPL_ERR_IO;
        }
        if (close(fd)) {
                unlink(tmp);
                return FT_TPL_ERR_IO;
        }
        if (rename(tmp, path)) {
                unlink(tmp);
                return FT_TPL_ERR_IO;
        }
        return FT_TPL_OK;
}

enum ft_tpl_status ft_tpl_store_read(const char *path,
                                     const struct ft_tpl_meta *expect,
                                     uint8_t **blob_out, size_t *len_out)
{
        uint8_t h[FT_STORE_HDR_BYTES];
        struct ft_tpl_meta meta;
        struct stat sb;
        enum ft_tpl_status st;
        uint8_t *blob;
        int fd;

        if (!path || !blob_out || !len_out)
                return FT_TPL_ERR_ARG;
        *blob_out = NULL;
        *len_out = 0;

        fd = open(path, O_RDONLY | O_NOFOLLOW);
        if (fd < 0)
                return FT_TPL_ERR_IO;
        if (fstat(fd, &sb) || sb.st_size < (off_t)FT_STORE_HDR_BYTES) {
                close(fd);
                return FT_TPL_ERR_META;
        }
        if (read_all(fd, h, sizeof(h))) {
                close(fd);
                return FT_TPL_ERR_IO;
        }
        st = parse_header(h, sizeof(h), &meta);
        if (st != FT_TPL_OK) {
                close(fd);
                return st;
        }
        if ((uint64_t)sb.st_size != (uint64_t)FT_STORE_HDR_BYTES + meta.blob_len) {
                close(fd);
                return FT_TPL_ERR_META;
        }
        if (expect) {
                if (expect->finger != meta.finger ||
                    expect->width != meta.width ||
                    expect->height != meta.height ||
                    expect->slot_bytes != meta.slot_bytes ||
                    memcmp(expect->config_digest, meta.config_digest,
                           FT_TPL_CONFIG_DIGEST)) {
                        close(fd);
                        return FT_TPL_ERR_META;
                }
        }
        blob = malloc(meta.blob_len ? meta.blob_len : 1);
        if (!blob) {
                close(fd);
                return FT_TPL_ERR_NOMEM;
        }
        if (read_all(fd, blob, meta.blob_len)) {
                free(blob);
                close(fd);
                return FT_TPL_ERR_IO;
        }
        close(fd);

        if (rd16(h + 56) != rd16(blob + meta.blob_len - FT_TPL_CRC_BYTES)) {
                free(blob);
                return FT_TPL_ERR_CRC;
        }
        st = ft_tpl_validate(blob, meta.blob_len, meta.width, meta.height,
                             meta.slot_bytes);
        if (st != FT_TPL_OK) {
                free(blob);
                return st;
        }
        *blob_out = blob;
        *len_out = meta.blob_len;
        return FT_TPL_OK;
}

void ft_tpl_free(void *ptr)
{
        free(ptr);
}

const char *ft_tpl_status_str(enum ft_tpl_status status)
{
        switch (status) {
        case FT_TPL_OK: return "ok";
        case FT_TPL_ERR_ARG: return "bad argument";
        case FT_TPL_ERR_MAGIC: return "bad magic";
        case FT_TPL_ERR_LAYOUT: return "bad layout";
        case FT_TPL_ERR_CRC: return "bad crc";
        case FT_TPL_ERR_IO: return "i/o error";
        case FT_TPL_ERR_META: return "bad metadata";
        case FT_TPL_ERR_VERSION: return "unsupported version";
        case FT_TPL_ERR_NOMEM: return "out of memory";
        }
        return "unknown";
}
