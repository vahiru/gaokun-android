/* 主机辅助工具：用与 gk3w 相同的 zstd 解码 + sparse 展开代码，把 super.img.zst 展开到一个普通文件，
 * 与 scripts/live/gk3-unsparse.py（zstd -dc | python3 gk3-unsparse.py）的结果逐字节对拍。
 *
 *   build/host/unsparse <super.img.zst> <输出文件>
 *
 * DONT_CARE 区间按 gk3-unsparse.py 的语义跳过（文件上留成空洞），最后把文件截到展开后的总长。 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/sparse.h"
#include "zstd.h"

static int fd_out;

static int w(void *ctx, uint64_t off, const uint8_t *d, size_t len)
{
    (void)ctx;
    while (len) {
        ssize_t n = pwrite(fd_out, d, len, (off_t)off);
        if (n <= 0)
            return -1;
        d += n;
        len -= (size_t)n;
        off += (uint64_t)n;
    }
    return 0;
}

static int f(void *ctx, uint64_t off, uint64_t len, uint32_t pat)
{
    static uint8_t buf[1 << 20];
    for (size_t i = 0; i < sizeof buf; i += 4)
        memcpy(buf + i, &pat, 4);
    while (len) {
        size_t n = len < sizeof buf ? (size_t)len : sizeof buf;
        if (w(ctx, off, buf, n))
            return -1;
        off += n;
        len -= n;
    }
    return 0;
}

int main(int argc, char **argv)
{
    static uint8_t in[1 << 20], out[1 << 20], sbuf[4 << 20] __attribute__((aligned(4096)));
    FILE *fi;
    ZSTD_DCtx *dc;
    gk3w_sparse s;
    gk3w_sparse_sink sink = {NULL, w, f};
    size_t r = 1;
    int eof = 0;

    if (argc != 3)
        return fprintf(stderr, "用法：unsparse <super.img.zst> <输出文件>\n"), 2;
    fi = fopen(argv[1], "rb");
    fd_out = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (!fi || fd_out < 0)
        return fprintf(stderr, "打不开：%s\n", strerror(errno)), 2;
    dc = ZSTD_createDCtx();
    /* release.sh 用 -19 --long 压：解码时放宽窗口上限（同 installer-lib.sh 的 zstd -dc --long=31） */
    ZSTD_DCtx_setParameter(dc, ZSTD_d_windowLogMax, 31);
    gk3w_sparse_init(&s, UINT64_MAX, sbuf, sizeof sbuf);
    while (!eof) {
        size_t got = fread(in, 1, sizeof in, fi);
        ZSTD_inBuffer ib = {in, got, 0};
        if (got < sizeof in)
            eof = 1;
        while (ib.pos < ib.size) {
            ZSTD_outBuffer ob = {out, sizeof out, 0};
            r = ZSTD_decompressStream(dc, &ob, &ib);
            if (ZSTD_isError(r))
                return fprintf(stderr, "zstd：%s\n", ZSTD_getErrorName(r)), 1;
            if (gk3w_sparse_feed(&s, out, ob.pos, &sink))
                return fprintf(stderr, "sparse：%s\n", s.err), 1;
        }
    }
    /* 输入读完但帧没结束（r != 0）= 下载不完整 */
    for (;;) {
        ZSTD_inBuffer ib = {in, 0, 0};
        ZSTD_outBuffer ob = {out, sizeof out, 0};
        if (r == 0)
            break;
        r = ZSTD_decompressStream(dc, &ob, &ib);
        if (ZSTD_isError(r) || ob.pos == 0) {
            fprintf(stderr, "zstd：输入提前结束\n");
            return 1;
        }
        if (gk3w_sparse_feed(&s, out, ob.pos, &sink))
            return fprintf(stderr, "sparse：%s\n", s.err), 1;
    }
    if (gk3w_sparse_finish(&s, &sink))
        return fprintf(stderr, "sparse：%s\n", s.err), 1;
    if (ftruncate(fd_out, (off_t)gk3w_sparse_total(&s)) || close(fd_out))
        return fprintf(stderr, "截断 / 关闭失败：%s\n", strerror(errno)), 1;
    printf("展开 %llu 字节\n", (unsigned long long)gk3w_sparse_total(&s));
    ZSTD_freeDCtx(dc);
    fclose(fi);
    return 0;
}
