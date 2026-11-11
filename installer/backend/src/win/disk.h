/* Windows 侧共用的磁盘、卷、文件与参数工具。 */
#ifndef GK3W_DISK_H
#define GK3W_DISK_H

#include <windows.h>
#include <winioctl.h>

#include <stddef.h>
#include <stdint.h>

#include "../proto.h"

/* ── 参数 ── */
const wchar_t *gk3w_arg(int argc, wchar_t **argv, const wchar_t *key);
/* 十进制非负整数；没有或格式不对返回 -1 */
int gk3w_arg_u64(int argc, wchar_t **argv, const wchar_t *key, uint64_t *out);

/* ── 失败：ERR code=<code> [k=v] [winerr=] [touched=]，再一行 `!! human`。返回 1（命令的退出码）。 ── */
int gk3w_err(gk3w_touched t, const char *code, const char *human, DWORD winerr);
/* 带一个额外字段 */
int gk3w_err_kv(gk3w_touched t, const char *code, const char *k, const char *v, const char *human, DWORD winerr);
/* stderr 上的日志行（给人看，进安装日志） */
void gk3w_log(const char *fmt, ...);

/* ── 字符串 ── */
/* UTF-8 → UTF-16（out 以 NUL 结尾）；失败返回 -1 */
int gk3w_u2w(const char *s, wchar_t *out, size_t cap);

/* ── 磁盘 ── */
typedef struct {
    HANDLE h;
    DWORD num;
    uint32_t bs;                /* 逻辑扇区字节数 */
    uint64_t size;              /* 磁盘字节数 */
    DRIVE_LAYOUT_INFORMATION_EX *lay;
    DWORD laysz;
} gk3w_disk;

/* 这块盘是不是 VHD 虚拟盘（只查不报） */
int gk3w_disk_is_vhd(DWORD num);
/* 以读写方式打开一块 VHD（原型只对 VHD 动手：先查类型，真实磁盘连写句柄都不拿）。失败已报 ERR，返回 -1。 */
int gk3w_disk_open_vhd(DWORD num, gk3w_disk *d, gk3w_touched t);
void gk3w_disk_close(gk3w_disk *d);
int gk3w_disk_read_layout(gk3w_disk *d);
/* 以 lay 的前 count 个表项设置分区表，并让分区管理器重读。失败已报 ERR，返回 -1。 */
int gk3w_disk_set_layout(gk3w_disk *d, DRIVE_LAYOUT_INFORMATION_EX *lay, DWORD count, gk3w_touched t);
PARTITION_INFORMATION_EX *gk3w_disk_find_part(gk3w_disk *d, const GUID *id);
/* usable 区间（含两端，逻辑扇区） */
void gk3w_disk_usable(const gk3w_disk *d, uint64_t *first, uint64_t *last);

/* 按字节偏移读写；off、len 与 buf 地址都要按扇区对齐（句柄是无缓冲的） */
int gk3w_pread(HANDLE h, void *buf, DWORD len, uint64_t off);
int gk3w_pwrite(HANDLE h, const void *buf, DWORD len, uint64_t off);
/* 写后立即读回比对（chk 与 buf 同样大小、同样对齐）。0 成功；-1 写失败；-2 读回不一致。 */
int gk3w_write_verify(HANDLE h, const void *buf, DWORD len, uint64_t off, void *chk);

/* ── 卷 ── */
/* 找起点在 (disk, offset) 的卷，写出 \\?\Volume{…}\ 路径。找不到返回 -1。 */
int gk3w_find_volume(DWORD disk, uint64_t offset, wchar_t *out, size_t cap);

/* ── 文件（ESP 上的与本地的） ── */
/* 整个读进内存（malloc，调用方 free）。超过 max 字节算失败。 */
int gk3w_read_file(const wchar_t *path, uint8_t **data, size_t *len, size_t max);
/* 写一个文件（CREATE_ALWAYS + 落到介质），再读回比对 */
int gk3w_write_file_verify(const wchar_t *path, const uint8_t *data, size_t len);
/* 逐级建目录（path 是目录本身）；已存在不算错 */
int gk3w_mkdirs(const wchar_t *path, size_t root_len);
int gk3w_exists(const wchar_t *path);

/* ── 散列与随机数 ── */
int gk3w_sha256(const void *data, size_t len, uint8_t out[32]);
typedef struct gk3w_sha256_ctx gk3w_sha256_ctx;
gk3w_sha256_ctx *gk3w_sha256_begin(void);
int gk3w_sha256_update(gk3w_sha256_ctx *c, const void *data, size_t len);
int gk3w_sha256_end(gk3w_sha256_ctx *c, uint8_t out[32]);
void gk3w_hex(const uint8_t *b, size_t n, char *out);
int gk3w_random(void *buf, size_t n);

#endif
