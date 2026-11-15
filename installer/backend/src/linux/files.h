/* gk3l：挂载与文件（ESP 上的与本地的）。 */
#ifndef GK3L_FILES_H
#define GK3L_FILES_H

#include <stddef.h>
#include <stdint.h>

/* 在 GK3_RUNDIR（默认 /run/gaokun3）下建一个临时挂载点并把 dev 以 fstype 挂上去；ro = 1 只读。
 * 成功时 dir 是挂载点（≥ 64 字节）。返回 0，失败返回 errno。 */
int gk3l_mount_tmp(const char *dev, const char *fstype, int ro, char *dir, size_t cap);
/* 卸下并删掉挂载点。返回 0，失败返回 errno（挂载点留着） */
int gk3l_umount_tmp(const char *dir);

/* 整个读进内存（malloc，调用方 free）。不存在返回 ENOENT；超过 max 字节返回 EFBIG；其他失败返回 errno */
int gk3l_read_file(const char *path, uint8_t **data, size_t *len, size_t max);
/* 文件大小；不存在返回 -1 */
int64_t gk3l_file_size(const char *path);
int gk3l_exists(const char *path);
int gk3l_is_dir(const char *path);
/* 写一个文件（截断重写）并 fsync。返回 0，失败返回 errno */
int gk3l_write_file(const char *path, const uint8_t *data, size_t len);
/* 先写 <path>.tmp 并 fsync，再改名覆盖 <path>、fsync 目录：中途失败或掉电时 <path> 要么是旧的、要么是完整的新文件。
 * FAT 上沿用 <path> 原来的属性（隐藏、系统）。失败时删掉 .tmp，返回 errno */
int gk3l_replace_file(const char *path, const uint8_t *data, size_t len);
/* 逐级建目录（path 是目录本身）；已存在不算错。返回 0 或 errno */
int gk3l_mkdirs(const char *path);
/* 删一个目录及其下的一切。返回 0 或 errno */
int gk3l_rm_rf(const char *path);
/* 与 data 逐字节相同（文件不存在算不同） */
int gk3l_file_same(const char *path, const uint8_t *data, size_t len);

#endif
