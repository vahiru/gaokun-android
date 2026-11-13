/* 安装文件的体检与展开（说明见 release.h）。 */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200112L   /* posix_memalign */
#endif
#include "release.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zstd.h"

void *gk3w_aligned_alloc(size_t n)
{
#ifdef _WIN32
    return _aligned_malloc(n, 4096);
#else
    void *p = NULL;
    return posix_memalign(&p, 4096, n) ? NULL : p;
#endif
}

void gk3w_aligned_free(void *p)
{
#ifdef _WIN32
    _aligned_free(p);
#else
    free(p);
#endif
}

gk3w_boot_err gk3w_boot_check(gk3w_boot *bi)
{
    long n;
    if (gk3_bootimg_parse(bi->img, bi->len, GK3W_BOOT_MAX, &bi->b) || bi->b.total_size > bi->len)
        return GK3W_BOOT_UNPACK;
    /* 与 gk3-bootimg.py 相同的体检：只认 v2，三段都不空，内核是 PE，dtb 是 FDT */
    if (bi->b.version != 2 || !bi->b.kernel_size || !bi->b.ramdisk_size || !bi->b.dtb_size ||
        memcmp(bi->img + bi->b.kernel_off, "MZ", 2) || memcmp(bi->img + bi->b.dtb_off, "\xd0\x0d\xfe\xed", 4))
        return GK3W_BOOT_NOT_V2;
    n = gk3_bootimg_cmdline(&bi->b, bi->cmdline, sizeof bi->cmdline);
    if (n <= 0)
        return GK3W_BOOT_NO_CMDLINE;
    for (long i = 0; i < n; i++)
        if ((unsigned char)bi->cmdline[i] < 0x20 || (unsigned char)bi->cmdline[i] > 0x7e)
            return GK3W_BOOT_BAD_CMDLINE;
    return GK3W_BOOT_OK;
}

const char *gk3w_boot_strerror(gk3w_boot_err e)
{
    switch (e) {
    case GK3W_BOOT_OK: return "ok";
    case GK3W_BOOT_UNPACK: return "boot.img cannot be unpacked (disk not touched)";
    case GK3W_BOOT_NOT_V2: return "boot.img is not a v2 image with a dtb, or its kernel / dtb is malformed (disk not touched)";
    case GK3W_BOOT_NO_CMDLINE: return "boot.img has no kernel command line (disk not touched)";
    case GK3W_BOOT_BAD_CMDLINE: return "the boot.img command line has non-ASCII or control characters (disk not touched)";
    }
    return "?";
}

int gk3w_manifest_hash(const uint8_t *m, size_t len, const char *name, char out[65])
{
    size_t i = 0, nl = strlen(name);
    while (i < len) {
        size_t e = i;
        while (e < len && m[e] != '\n')
            e++;
        /* "<64 hex>  <name>"，或 sha256sum 二进制模式的 "<64 hex> *<name>" */
        if (e - i >= 66 + nl && m[i + 64] == ' ' && (m[i + 65] == ' ' || m[i + 65] == '*') && !memcmp(m + i + 66, name, nl) &&
            (e - i == 66 + nl || (e - i == 67 + nl && m[e - 1] == '\r'))) {
            for (int k = 0; k < 64; k++)
                out[k] = (char)(m[i + k] >= 'A' && m[i + k] <= 'F' ? m[i + k] + 32 : m[i + k]);
            out[64] = 0;
            return 0;
        }
        i = e + 1;
    }
    return -1;
}

int gk3w_image_run(const gk3w_reader *rd, int zst, uint64_t limit, const gk3w_sparse_sink *sink, int prog, const char **why,
                   uint32_t *blk_sz)
{
    ZSTD_DCtx *dc = NULL;
    uint8_t *in = NULL, *out = NULL, *sbuf = NULL;
    gk3w_sparse s;
    uint64_t read_total = 0;
    size_t r = 1;
    unsigned last_pct = 0;
    int rc = -1;
    long got;

    *why = "open";
    if (!rd->size)
        goto out;
    *why = "nomem";
    in = malloc(GK3W_IO_CHUNK);
    out = malloc(GK3W_IO_CHUNK);
    sbuf = gk3w_aligned_alloc(GK3W_IO_CHUNK);
    dc = zst ? ZSTD_createDCtx() : NULL;
    if (!in || !out || !sbuf || (zst && !dc))
        goto out;
    /* release.sh 用 -19 --long 压：解码时放宽窗口上限（同 zstd -dc --long=31） */
    if (dc)
        ZSTD_DCtx_setParameter(dc, ZSTD_d_windowLogMax, 31);
    if (gk3w_sparse_init(&s, limit, sbuf, GK3W_IO_CHUNK))
        goto out;
    if (!zst)
        r = 0;
    while ((got = rd->read(rd->ctx, in, GK3W_IO_CHUNK)) > 0) {
        ZSTD_inBuffer ib = {in, (size_t)got, 0};
        read_total += (uint64_t)got;
        if (!zst && gk3w_sparse_feed(&s, in, (size_t)got, sink)) {
            *why = s.err;
            goto out;
        }
        while (zst && ib.pos < ib.size) {
            ZSTD_outBuffer ob = {out, GK3W_IO_CHUNK, 0};
            r = ZSTD_decompressStream(dc, &ob, &ib);
            if (ZSTD_isError(r)) {
                *why = "zstd-corrupt";
                goto out;
            }
            if (gk3w_sparse_feed(&s, out, ob.pos, sink)) {
                *why = s.err;
                goto out;
            }
        }
        if (prog) {
            unsigned pct = 30 + (unsigned)(read_total * 40 / rd->size);
            if (pct != last_pct) {
                fprintf(stderr, "PROGRESS %u write-super done_mib=%llu total_mib=%llu\n", pct,
                        (unsigned long long)(s.out_off >> 20), (unsigned long long)(gk3w_sparse_total(&s) >> 20));
                fflush(stderr);
                last_pct = pct;
            }
        }
    }
    if (got < 0) {
        *why = "read";
        goto out;
    }
    /* 读到文件尾时帧必须正好结束（r == 0）；否则把解码器里剩下的吐完，还不结束就是下载不完整 */
    while (r) {
        ZSTD_inBuffer ib = {in, 0, 0};
        ZSTD_outBuffer ob = {out, GK3W_IO_CHUNK, 0};
        r = ZSTD_decompressStream(dc, &ob, &ib);
        if (ZSTD_isError(r) || !ob.pos) {
            *why = "zst-truncated";
            goto out;
        }
        if (gk3w_sparse_feed(&s, out, ob.pos, sink)) {
            *why = s.err;
            goto out;
        }
    }
    if (gk3w_sparse_finish(&s, sink)) {
        *why = s.err;
        goto out;
    }
    if (blk_sz)
        *blk_sz = s.blk_sz;
    rc = 0;
out:
    if (dc)
        ZSTD_freeDCtx(dc);
    free(in);
    free(out);
    gk3w_aligned_free(sbuf);
    return rc;
}
