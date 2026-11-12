/* Windows 侧共用的磁盘、卷、文件与参数工具。 */
#ifndef GK3W_DISK_H
#define GK3W_DISK_H

#include <windows.h>
#include <winioctl.h>

#include <stddef.h>
#include <stdint.h>

#include "../blkey.h"
#include "../proto.h"

/* ── 参数 ── */
const wchar_t *gk3w_arg(int argc, wchar_t **argv, const wchar_t *key);
/* 不带值的开关参数在不在 */
int gk3w_flag(int argc, wchar_t **argv, const wchar_t *key);
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
/* Windows 目录所在卷的那块盘；找不到返回 -1 */
long gk3w_system_disk_number(void);
/* 以读写方式打开一块 VHD（先查类型，真实磁盘连写句柄都不拿）。失败已报 ERR，返回 -1。 */
int gk3w_disk_open_vhd(DWORD num, gk3w_disk *d, gk3w_touched t);
/* real = 0：同 gk3w_disk_open_vhd。real = 1（--real-disk）：只认 Windows 所在的那块系统盘（VHD 也不认）；
 * 机型与安装前检查由调用方先用 gk3w_sys_gate 把关。 */
int gk3w_disk_open(DWORD num, int real, gk3w_disk *d, gk3w_touched t);
void gk3w_disk_close(gk3w_disk *d);
int gk3w_disk_read_layout(gk3w_disk *d);
/* 以 lay 的前 count 个表项设置分区表，并让分区管理器重读。失败已报 ERR，返回 -1。 */
int gk3w_disk_set_layout(gk3w_disk *d, DRIVE_LAYOUT_INFORMATION_EX *lay, DWORD count, gk3w_touched t);
PARTITION_INFORMATION_EX *gk3w_disk_find_part(gk3w_disk *d, const GUID *id);
/* 分区表备份：GPT 主表区（LBA 0 到 usable 起点）与备份表区（usable 终点之后到盘尾）原样存进
 * <dir>\gpt-before-<tag>-disk<N>-<时间>-head<扇区数>-tail<扇区数>.bin，写完读回比对。失败已报 ERR（touched 照 t），返回 -1。 */
int gk3w_gpt_backup(gk3w_disk *d, const wchar_t *dir, const wchar_t *tag, gk3w_touched t);
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
/* 先写 <path>.tmp、读回核对，再改名覆盖 <path>（保留 <path> 原来的属性）：中途失败或掉电时 <path> 要么是旧的、要么是
 * 完整的新文件。失败时删掉 .tmp；返回 -1，GetLastError() 是失败那一步的错误码。 */
int gk3w_replace_file_verify(const wchar_t *path, const uint8_t *data, size_t len);
/* 逐级建目录（path 是目录本身）；已存在不算错 */
int gk3w_mkdirs(const wchar_t *path, size_t root_len);
int gk3w_exists(const wchar_t *path);

/* ── 系统状态（sys.c） ── */
typedef struct {
    char manufacturer[64], model[64];
    int secure_boot;            /* 1 开、0 关、-1 读不到 */
    int fast_startup;           /* HiberbootEnabled 为 1（休眠关着时暂不生效，打开休眠就回来，所以不管休眠） */
    int hibernate;              /* Control\Power\HibernateEnabled：1 开、0 关、-1 读不到 */
    int hiberfil;               /* 系统盘上有 hiberfil.sys */
    int bitlocker;              /* GK3W_BL_STATE_* */
    int bitlocker_ids_known;    /* 下面的 ID 查到了（0 个也算查到） */
    char bitlocker_key_ids[128]; /* 恢复密钥的 ID（前 8 位，逗号分隔）：用户据此在 Microsoft 账户里找对应的密钥 */
    int ac;                     /* 1 插着电源、0 没插、-1 不知道 */
    int battery;                /* 电量百分比，-1 不知道 */
    wchar_t system_drive;
} gk3w_sys;
void gk3w_sys_read(gk3w_sys *s);
int gk3w_model_ok(const gk3w_sys *s);
/* 同上，现读注册表（不读其余系统状态） */
int gk3w_model_ok_now(void);
/* 系统卷的 BitLocker 保护开着（或状态不明）：改动启动链之前要校验恢复密钥、暂停保护 */
int gk3w_sys_bitlocker_on(const gk3w_sys *s);
/* 写真实磁盘之前的把关；拒绝时已报 ERR（touched=no），返回 -1。
 * GK3W_GATE_APPLY：机型、Secure Boot、快速启动、BitLocker、电量。GK3W_GATE_UNINSTALL：机型、BitLocker、电量
 * （卸载之后不再有 Android 写 ESP，Secure Boot 与快速启动无关）。
 * bitlocker_key：用户输入的恢复密钥（NULL = 没给）；BitLocker 保护开着时必须给，并与系统卷的恢复密钥一致。 */
typedef enum { GK3W_GATE_APPLY, GK3W_GATE_UNINSTALL } gk3w_gate_mode;
int gk3w_sys_gate(const gk3w_sys *s, const char *bitlocker_key, gk3w_gate_mode mode);

/* ── BitLocker（bitlocker.c） ── */
typedef enum {
    GK3W_BL_OK = 0,
    GK3W_BL_FORMAT,             /* 不是 48 位数字，或某一组校验不过 */
    GK3W_BL_WRONG,              /* 与卷上的恢复密钥都不一致 */
    GK3W_BL_NO_PASSWORD,        /* 卷上没有恢复密钥（数字密码保护器） */
    GK3W_BL_QUERY,              /* WMI 查不了，无法校验 */
} gk3w_bl_result;
/* 保护状态（ProtectionStatus：0 关、1 开、2 未知；WMI 不可用 -1）与 *conversion（ConversionStatus 0–5，查不到 -1）。
 * 保护开着或未知时顺带查恢复密钥的 ID（逗号分隔，各取前 8 位），查到了 *ids_known = 1 */
int gk3w_bitlocker_status(wchar_t drive, char *ids, size_t cap, int *ids_known, int *conversion);
/* 用户输入的恢复密钥与卷上的是否一致；GK3W_BL_FORMAT 时 *bad_group 是校验不过的那一组（1–8），位数不对为 0 */
gk3w_bl_result gk3w_bitlocker_verify(wchar_t drive, const char *key, int *bad_group);
/* 暂停卷的 BitLocker 保护，Windows 再启动 reboots 次后自动恢复。成功 0，失败 -1（已写日志） */
int gk3w_bitlocker_suspend(wchar_t drive, unsigned reboots);
/* 校验失败的 ERR（bitlocker-key-format / -key-wrong / no-recovery-key / unverifiable）。返回 1 */
int gk3w_bitlocker_err(gk3w_bl_result r, int bad_group, gk3w_touched t);
/* 从标准输入读一行（恢复密钥），去掉行尾。读不到或空行返回 -1 */
int gk3w_read_key_stdin(char *buf, size_t cap);
/* 删 systemd-boot 的 EFI 变量（LoaderEntryDefault 等）：0 = 删了、本来就没有，或 ours 不认这个值而留着；
 * -1 = 读不到 / 删不掉（已记日志）。ours 为 NULL 时不看值、一律删。 */
int gk3w_loader_var_delete(const wchar_t *name, int (*ours)(const char *value));

/* ── 散列与随机数 ── */
int gk3w_sha256(const void *data, size_t len, uint8_t out[32]);
typedef struct gk3w_sha256_ctx gk3w_sha256_ctx;
gk3w_sha256_ctx *gk3w_sha256_begin(void);
int gk3w_sha256_update(gk3w_sha256_ctx *c, const void *data, size_t len);
int gk3w_sha256_end(gk3w_sha256_ctx *c, uint8_t out[32]);
/* 整个文件的 sha256（小写 hex）与长度；0 = 成功，-1 = 打不开 / 读失败 / CNG 出错 */
int gk3w_file_sha256(const wchar_t *path, char out[65], uint64_t *size);
void gk3w_hex(const uint8_t *b, size_t n, char *out);
int gk3w_random(void *buf, size_t n);

#endif
