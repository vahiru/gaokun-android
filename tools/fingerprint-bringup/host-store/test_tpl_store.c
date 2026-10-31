/* SPDX-License-Identifier: GPL-2.0 */
/* Host-side tests for the template store reference implementation. */
#include "tpl_store.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SLOT_40X196 2454u   /* 489 + bin 984 + mask 981 */
#define SLOT_96X96  2802u   /* 489 + bin 1160 + mask 1153 */

static int failures;

#define CHECK(cond) do {                                                  \
        if (!(cond)) {                                                    \
                fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,   \
                        #cond);                                           \
                ++failures;                                               \
        }                                                                 \
} while (0)

static void test_crc_vector(void)
{
        static const uint8_t s[] = "123456789";
        CHECK(ft_tpl_crc16(s, 9) == 0xBB3Du);
        CHECK(ft_tpl_crc16(NULL, 0) == 0u);
}

static uint8_t *make_blob(uint16_t width, uint16_t height, uint32_t slot,
                          unsigned features, uint8_t seed, size_t *len_out)
{
        size_t fixed = FT_TPL_HEADER_BYTES +
                       (size_t)FT_TPL_SLOT_COUNT * slot + FT_TPL_CRC_BYTES;
        size_t len = fixed + (size_t)features * FT_TPL_FEATURE_BYTES;
        uint8_t *b = calloc(1, len);
        size_t i;

        if (!b)
                return NULL;
        (void)width;
        (void)height;
        for (i = 0; i < len - FT_TPL_CRC_BYTES; ++i)
                b[i] ^= (uint8_t)(seed + i * 31u);
        b[0] = (uint8_t)(FT_TPL_MAGIC);
        b[1] = (uint8_t)(FT_TPL_MAGIC >> 8);
        b[2] = (uint8_t)(FT_TPL_MAGIC >> 16);
        b[3] = (uint8_t)(FT_TPL_MAGIC >> 24);
        b[4] = (uint8_t)(FT_TPL_HEADER_SIZE_FLD);
        b[5] = (uint8_t)(FT_TPL_HEADER_SIZE_FLD >> 8);
        b[6] = (uint8_t)(FT_TPL_HEADER_SIZE_FLD >> 16);
        b[7] = (uint8_t)(FT_TPL_HEADER_SIZE_FLD >> 24);
        {
                unsigned crc = ft_tpl_crc16(b, len - FT_TPL_CRC_BYTES);
                b[len - 2] = (uint8_t)crc;
                b[len - 1] = (uint8_t)(crc >> 8);
        }
        *len_out = len;
        return b;
}

static void test_validate(void)
{
        size_t len = 0;
        uint8_t *b = make_blob(40, 196, SLOT_40X196, 72, 7, &len);

        CHECK(b != NULL);
        if (!b)
                return;
        CHECK(len == 238970u);
        CHECK(ft_tpl_validate(b, len, 40, 196, SLOT_40X196) == FT_TPL_OK);

        b[0] ^= 1;
        CHECK(ft_tpl_validate(b, len, 40, 196, SLOT_40X196) == FT_TPL_ERR_MAGIC);
        b[0] ^= 1;

        b[4] ^= 1;
        CHECK(ft_tpl_validate(b, len, 40, 196, SLOT_40X196) == FT_TPL_ERR_LAYOUT);
        b[4] ^= 1;

        b[1024] ^= 0x5A;
        CHECK(ft_tpl_validate(b, len, 40, 196, SLOT_40X196) == FT_TPL_ERR_CRC);
        b[1024] ^= 0x5A;
        CHECK(ft_tpl_validate(b, len, 40, 196, SLOT_40X196) == FT_TPL_OK);

        /* Dropping the 2-byte CRC leaves a non-integral feature count. */
        CHECK(ft_tpl_validate(b, len - 2, 40, 196, SLOT_40X196) ==
              FT_TPL_ERR_LAYOUT);
        /* Dropping one whole feature keeps the layout but breaks the CRC. */
        CHECK(ft_tpl_validate(b, len - FT_TPL_FEATURE_BYTES, 40, 196,
                              SLOT_40X196) == FT_TPL_ERR_CRC);
        /* Same length, wrong slot size -> layout mismatch. */
        CHECK(ft_tpl_validate(b, len, 40, 196, SLOT_96X96) ==
              FT_TPL_ERR_LAYOUT);
        CHECK(ft_tpl_validate(b, len, 0, 196, SLOT_40X196) == FT_TPL_ERR_ARG);
        CHECK(ft_tpl_validate(NULL, len, 40, 196, SLOT_40X196) ==
              FT_TPL_ERR_ARG);
        free(b);

        b = make_blob(96, 96, SLOT_96X96, 108, 9, &len);
        CHECK(b != NULL);
        if (b) {
                CHECK(ft_tpl_validate(b, len, 96, 96, SLOT_96X96) == FT_TPL_OK);
                CHECK(len == 273962u);
                free(b);
        }
}

static void test_store_roundtrip(void)
{
        char dir[] = "/tmp/fptpl.XXXXXX";
        char path[512];
        size_t len = 0;
        uint8_t *b = make_blob(40, 196, SLOT_40X196, 72, 3, &len);
        struct ft_tpl_meta meta;
        struct ft_tpl_meta expect;
        uint8_t *out = NULL;
        size_t out_len = 0;
        struct stat sb;
        int fd;

        CHECK(b != NULL);
        if (!b || !mkdtemp(dir)) {
                free(b);
                return;
        }
        snprintf(path, sizeof(path), "%s/finger1.tpl", dir);

        memset(&meta, 0, sizeof(meta));
        meta.finger = 1;
        meta.width = 40;
        meta.height = 196;
        meta.slot_bytes = SLOT_40X196;
        meta.blob_len = (uint32_t)len;
        memset(meta.config_digest, 0xA5, sizeof(meta.config_digest));

        CHECK(ft_tpl_store_write(path, &meta, b, len) == FT_TPL_OK);
        fd = open(path, O_RDONLY);
        CHECK(fd >= 0);
        if (fd >= 0) {
                CHECK(fstat(fd, &sb) == 0);
                CHECK((sb.st_mode & 0777) == 0600);
                close(fd);
        }
        CHECK(access(path, F_OK) == 0);
        {
                char tmp[512];
                snprintf(tmp, sizeof(tmp), "%s.tmp", path);
                CHECK(access(tmp, F_OK) != 0);
        }

        expect = meta;
        CHECK(ft_tpl_store_read(path, &expect, &out, &out_len) == FT_TPL_OK);
        CHECK(out_len == len);
        CHECK(out && memcmp(out, b, len) == 0);
        ft_tpl_free(out);
        out = NULL;

        expect = meta;
        expect.width = 196;
        expect.height = 40;
        CHECK(ft_tpl_store_read(path, &expect, &out, &out_len) ==
              FT_TPL_ERR_META);
        CHECK(out == NULL);

        expect = meta;
        expect.finger = 2;
        CHECK(ft_tpl_store_read(path, &expect, &out, &out_len) ==
              FT_TPL_ERR_META);

        expect = meta;
        expect.config_digest[0] ^= 1;
        CHECK(ft_tpl_store_read(path, &expect, &out, &out_len) ==
              FT_TPL_ERR_META);

        /* Corrupt one payload byte on disk: blob CRC must fail. */
        {
                uint8_t *file = malloc(FT_STORE_HDR_BYTES + len);
                FILE *fp = fopen(path, "rb");
                CHECK(fp != NULL);
                if (fp && file) {
                        CHECK(fread(file, 1, FT_STORE_HDR_BYTES + len, fp) ==
                              FT_STORE_HDR_BYTES + len);
                        fclose(fp);
                        file[FT_STORE_HDR_BYTES + 1024] ^= 0x5A;
                        fp = fopen(path, "wb");
                        CHECK(fp != NULL);
                        if (fp) {
                                CHECK(fwrite(file, 1, FT_STORE_HDR_BYTES + len,
                                             fp) == FT_STORE_HDR_BYTES + len);
                                fclose(fp);
                        }
                        CHECK(ft_tpl_store_read(path, &meta, &out, &out_len) ==
                              FT_TPL_ERR_CRC);
                }
                free(file);
        }

        /* Restore a good file, then corrupt a metadata field. */
        CHECK(ft_tpl_store_write(path, &meta, b, len) == FT_TPL_OK);
        {
                uint8_t *file = malloc(FT_STORE_HDR_BYTES + len);
                FILE *fp = fopen(path, "rb");
                CHECK(fp != NULL);
                if (fp && file) {
                        CHECK(fread(file, 1, FT_STORE_HDR_BYTES + len, fp) ==
                              FT_STORE_HDR_BYTES + len);
                        fclose(fp);
                        file[12] ^= 1;  /* width low byte */
                        fp = fopen(path, "wb");
                        CHECK(fp != NULL);
                        if (fp) {
                                CHECK(fwrite(file, 1, FT_STORE_HDR_BYTES + len,
                                             fp) == FT_STORE_HDR_BYTES + len);
                                fclose(fp);
                        }
                        CHECK(ft_tpl_store_read(path, &meta, &out, &out_len) ==
                              FT_TPL_ERR_META);
                }
                free(file);
        }

        /* A file shorter than its declared blob must be rejected. */
        CHECK(ft_tpl_store_write(path, &meta, b, len) == FT_TPL_OK);
        CHECK(truncate(path, (off_t)(FT_STORE_HDR_BYTES + len - 1)) == 0);
        CHECK(ft_tpl_store_read(path, &meta, &out, &out_len) == FT_TPL_ERR_META);

        unlink(path);
        rmdir(dir);
        free(b);
}

int main(void)
{
        test_crc_vector();
        test_validate();
        test_store_roundtrip();

        if (failures) {
                fprintf(stderr, "%d failure(s)\n", failures);
                return 1;
        }
        printf("tpl_store: all tests passed\n");
        return 0;
}
