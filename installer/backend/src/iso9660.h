/* ISO 9660 的只读解析：读 Joliet 目录树（文件名是 UCS-2 大端），列出其中的目录与文件。
 * 用途：Windows 安装器制作安装介质时从 LiveCD 的混合 ISO 里取文件（installer/tools/build/build-iso.sh 用 xorriso
 * -J -joliet-long 生成 Joliet 树）。只认单区段的文件（ISO 9660 里小于 4 GiB 的文件只有一个区段），
 * 多区段文件、交错文件一律算格式不对。格式见 ECMA-119 第 8、9 节与 Joliet 规范（补充卷描述符的转义序列）。 */
#ifndef GK3W_ISO9660_H
#define GK3W_ISO9660_H

#include <stddef.h>
#include <stdint.h>

#define GK3W_ISO_SECTOR 2048u
#define GK3W_ISO_PATH_MAX 512

typedef struct {
    void *ctx;
    /* 从 ISO 的 off 处读 len 字节到 buf；返回 0 成功 */
    int (*read)(void *ctx, uint64_t off, void *buf, size_t len);
} gk3w_iso_src;

typedef struct {
    char path[GK3W_ISO_PATH_MAX];   /* 相对根目录、用 / 分隔的 UTF-8 路径，例如 "gaokun3/live.squashfs" */
    uint64_t offset;                /* 内容在 ISO 里的字节偏移（目录是目录记录所在的位置） */
    uint64_t size;
    int is_dir;
} gk3w_iso_entry;

typedef enum {
    GK3W_ISO_OK = 0,
    GK3W_ISO_IO,                /* 读不了 */
    GK3W_ISO_NOT_ISO,           /* 第 16 扇区起找不到主卷描述符 */
    GK3W_ISO_NO_JOLIET,         /* 没有 Joliet 补充卷描述符 */
    GK3W_ISO_BAD,               /* 目录记录越界、名字不合法、多区段或交错文件、层数过深 */
    GK3W_ISO_TOO_MANY,          /* 条目多于调用方给的容量 */
} gk3w_iso_err;

/* 列出 Joliet 树里的全部条目：父目录总在它里面的条目之前；out 最多 cap 个，*n 返回个数。
 * iso_size 是 ISO 文件的字节数，所有区段都必须落在它之内。 */
gk3w_iso_err gk3w_iso_list(const gk3w_iso_src *src, uint64_t iso_size, gk3w_iso_entry *out, size_t cap, size_t *n);
/* 给日志与 `!!` 的英文说明 */
const char *gk3w_iso_strerror(gk3w_iso_err e);

#endif
