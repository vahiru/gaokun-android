/* SPDX-License-Identifier: MIT */
/* AOSP libsparse（system/core/libsparse/include/sparse/sparse.h）的一个子集：只有 e2fsprogs 的 lib/ext2fs/sparse_io.c
 * 用到的那几个函数，签名与 AOSP 相同。mke2fs -E android_sparse 生成新文件系统时只用写的一半；读（import）的几个函数
 * 一律返回失败，sparse_io.c 会退回按参数新建。
 *
 * 为什么不直接编 AOSP 的 libsparse：它是 Apache-2.0，而 mke2fs 是 GPLv2，两者不兼容；这份是自己写的，用 MIT 许可。 */
#ifndef GK3_SPARSE_SHIM_H
#define GK3_SPARSE_SHIM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct sparse_file;

struct sparse_file *sparse_file_new(unsigned int block_size, int64_t len);
void sparse_file_destroy(struct sparse_file *s);
/* data 在 sparse_file_write 之前必须一直有效（不拷贝）。len 是块大小的整数倍。 */
int sparse_file_add_data(struct sparse_file *s, void *data, uint64_t len, unsigned int block);
/* 只支持 gz = false、sparse = true、crc = false（sparse_io.c 就是这样调的）。 */
int sparse_file_write(struct sparse_file *s, int fd, bool gz, bool sparse, bool crc);

/* 读的一半：不支持 */
struct sparse_file *sparse_file_import(int fd, bool verbose, bool crc);
unsigned int sparse_file_block_size(struct sparse_file *s);
int64_t sparse_file_len(struct sparse_file *s, bool sparse, bool crc);
int sparse_file_foreach_chunk(struct sparse_file *s, bool sparse, bool crc,
                              int (*write)(void *priv, const void *data, size_t len, unsigned int block,
                                           unsigned int nr_blocks),
                              void *priv);

#endif
