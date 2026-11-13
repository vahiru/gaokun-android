/* 安装文件：boot.img 的体检、发版校验清单（install-artifacts.sha256）、super 等 sparse 镜像（可带 zstd）的流式展开。
 * Windows 与 Linux 共用，平台只提供"读文件"与"写到盘上"两个回调。 */
#ifndef GK3W_RELEASE_H
#define GK3W_RELEASE_H

#include <stddef.h>
#include <stdint.h>

#include "gk3core.h"
#include "sparse.h"

#define GK3W_BOOT_MAX (64u << 20)      /* boot.img 的上限：与 boot 分区同大 */
#define GK3W_IO_CHUNK (4u << 20)

typedef struct {
    uint8_t *img;               /* 整个 boot.img（调用方读进来，malloc） */
    size_t len;
    gk3_bootimg b;
    char cmdline[GK3_BOOT_ARGS_SIZE + GK3_BOOT_EXTRA_ARGS_SIZE + 1];
} gk3w_boot;

typedef enum {
    GK3W_BOOT_OK = 0,
    GK3W_BOOT_UNPACK,           /* 头解析不了，或声明的长度超出文件 */
    GK3W_BOOT_NOT_V2,           /* 不是带 dtb 的 v2，或内核不是 PE、dtb 不是 FDT（与 gk3-bootimg.py 相同的体检） */
    GK3W_BOOT_NO_CMDLINE,       /* 没有内核命令行 */
    GK3W_BOOT_BAD_CMDLINE,      /* 命令行里有非 ASCII 或控制字符 */
} gk3w_boot_err;

/* 体检 bi->img[0, bi->len)，填好 bi->b 与 bi->cmdline */
gk3w_boot_err gk3w_boot_check(gk3w_boot *bi);
/* 给日志与 `!!` 的英文说明（末尾带"disk not touched"） */
const char *gk3w_boot_strerror(gk3w_boot_err e);

/* install-artifacts.sha256 里 name 那一行的 sha256（小写 hex）；没有返回 -1 */
int gk3w_manifest_hash(const uint8_t *m, size_t len, const char *name, char out[65]);

typedef struct {
    void *ctx;
    /* 读最多 cap 字节；返回读到的字节数，0 = 文件尾，< 0 = 读错 */
    long (*read)(void *ctx, uint8_t *buf, size_t cap);
    uint64_t size;              /* 文件总长（进度用，必须非零） */
} gk3w_reader;

/* 把 sparse 镜像（zst = 1 时先 zstd 解码）展开写到 sink；sink 为 NULL 时只校验（动盘之前的完整试解）。
 * limit：展开后不得超过的字节数。prog = 1 时按读入的压缩字节在 stderr 上报 `PROGRESS 30→70 write-super`。
 * 返回 0 成功；失败时 *why 是原因（英文代码，进 ERR 的 why=）。*blk_sz（可为 NULL）是镜像的块大小。 */
int gk3w_image_run(const gk3w_reader *r, int zst, uint64_t limit, const gk3w_sparse_sink *sink, int prog, const char **why,
                   uint32_t *blk_sz);

/* 按 4096 对齐分配（无缓冲 I/O 的缓冲区）与释放 */
void *gk3w_aligned_alloc(size_t n);
void gk3w_aligned_free(void *p);

#endif
