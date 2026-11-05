/*
 * gk3-fastbootd —— 统一启动入口的 fastboot 执行端（见 tools/gk3boot/README.md）。
 *
 * 盘上格式（GPT / BCB / BCAB / VAB / GK3 / boot.img）一律走 libgk3core，
 * 这里只有 fastboot 协议、sparse、LP 元数据只读解析、ESP 同步与传输。
 *
 * 线程模型：每种传输（USB FunctionFS、TCP）一个会话线程，命令执行在一把全局锁下串行 ——
 * 同一时刻只有一条命令在碰盘。
 */
#ifndef GK3_FBD_H
#define GK3_FBD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gk3core.h"

/* 所有 snprintf 都是有意截断的有界写（路径 / 提示串），gcc 的 -Wformat-truncation 在 -Werror 下全是误报 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif

#ifndef GK3FB_VERSION
#define GK3FB_VERSION "0.2.0-s7c"
#endif

#define FB_CMD_MAX   4096u   /* README.md "Transport and Framing" 1：命令 ≤ 4096 字节 */
#define FB_RESP_MAX  256u    /* 回应 ≤ 256 字节（含 4 字节前缀） */
#define FB_MSG_MAX   (FB_RESP_MAX - 4u)

/* ------------------------------------------------------------------ 退出码（与执行端 /init 的接口，README §13.3） */

/* 常驻模式（不带子命令）：守护进程自己【不】调 reboot(2)，把"接下来怎么办"用退出码交给 /init（PID 1，管生命周期与界面）。
 *   0  重启（fastboot reboot；reboot-bootloader / -fastboot / -recovery 在 gk3.dispatch=1 时已先写好 BCB，
 *      重启后由 gk3boot 的 BCB 分派送回执行端）—— /init：解绑 UDC → sync → reboot -f
 *   10 关机（shutdown / powerdown）                 —— /init：poweroff -f
 *   11 原地重起 fastboot（reboot-bootloader / -fastboot 而 gk3.dispatch 不是 1：gk3boot 下次不会消费 BCB，
 *      写了也白写还会堵住 init 的写入通道）       —— /init：解绑 → 重起守护进程 → 重绑（软重新枚举）
 *   12 原地切到菜单（reboot-recovery，同上条件）   —— /init：主菜单
 *   1  一个传输都起不来；2 用法错；其他 = 崩溃       —— /init：自动重起，60 秒内第 3 次就停下
 * 子命令（--wipe-data [--confirm] / --clear-bcb）：
 *   0 成功；3 免二次确认的条件不成立（/init 显示确认页）；4 守卫拒绝（合并中 / 更新待验证，BCB 已清、记了事件）；
 *   5 失败（没有目标盘、读写出错）；2 用法错。stdout 只打一两行英文（/init 原样显示）。 */
#define FB_EXIT_REBOOT   0
#define FB_EXIT_FATAL    1
#define FB_EXIT_USAGE    2
#define FB_EXIT_CONFIRM  3
#define FB_EXIT_REFUSED  4
#define FB_EXIT_FAILED   5
#define FB_EXIT_POWEROFF 10
#define FB_EXIT_RESTART  11
#define FB_EXIT_MENU     12

/* ------------------------------------------------------------------ 日志（log.c） */

void fb_log_init(bool to_kmsg, const char *file);
void fb_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* 环形缓冲里的日志按行回调（oem log 用），最多 max_lines 行（最新的那些）。 */
void fb_log_foreach(unsigned max_lines, void (*cb)(void *ctx, const char *line), void *ctx);
/* 状态文件（G.status_file，缺省 $GK3_RUN/fastbootd.status）：整份替换成一行（tmp + rename）。/init 把第一行显示在界面上，
 * 它的 mtime 变化也算"有活干"（空闲关机重新计时）。没有状态文件时什么也不做。 */
void fb_status(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ------------------------------------------------------------------ 传输（tcp.c / usb.c） */

typedef struct fb_transport fb_transport;
struct fb_transport {
    const char *name;                       /* "usb" / "tcp" */
    /* 等一个新会话（TCP：accept + FB01 握手；USB：端点就绪）。返回 0 成功。 */
    int (*open_session)(fb_transport *t);
    /* 读一条命令（一个 USB 传输 / 一条 TCP 消息），返回长度；<0 = 会话断开。 */
    long (*read_cmd)(fb_transport *t, char *buf, size_t max);
    /* 数据阶段：恰好读满 len 字节。返回 0 成功。 */
    int (*read_data)(fb_transport *t, void *buf, size_t len);
    /* 写一个回应包。返回 0 成功。 */
    int (*write)(fb_transport *t, const void *buf, size_t len);
    void (*close_session)(fb_transport *t);
    void *priv;
};

fb_transport *fb_tcp_new(int port);
/* ffs_dir：FunctionFS 挂载点（/init 建 gadget 时由 GK3_FFS 给，缺省 /dev/usb-ffs/fastboot）。 */
fb_transport *fb_usb_new(const char *udc, bool setup_gadget, const char *ffs_dir);

/* ------------------------------------------------------------------ 盘与白名单（disk.c） */

/* 对外暴露（getvar / flash / erase 认得的）只有前 5 个；misc 只供内部按偏移读写（fastboot-design §4.4-2）。 */
enum { FB_P_BOOT_A = 0, FB_P_BOOT_B, FB_P_SUPER, FB_P_USERDATA, FB_P_METADATA, FB_P_MISC, FB_P_N };
#define FB_P_EXPOSED 5

typedef struct {
    const char *name;
    uint32_t index;                 /* GPT 表项号（1 起，= Linux 的 pN） */
    uint64_t first_lba, last_lba;
    uint64_t off, size;             /* 盘内字节偏移 / 大小 */
    char partuuid[37];
} fb_part;

typedef struct {
    bool ok;                        /* 目标盘唯一、六个名字各恰好一个 —— false 时拒绝一切写入 */
    char err[400];                  /* !ok 时的原因 */
    char path[256];                 /* 整盘节点（或测试用的镜像文件） */
    char sysname[64];               /* /sys/block 下的名字（镜像文件时为空） */
    char model[64];
    int fd;
    bool is_blk;
    uint32_t bs;
    uint64_t nblocks;
    char disk_guid[37];
    uint32_t gpt_hdr_crc, gpt_tab_crc;   /* 启动时的主表头 / 表 CRC；每次写前重核（"写前核对"） */
    fb_part p[FB_P_N];
    /* 全部 GPT 表项的简表（找 ESP、oem device-info 用；只读） */
    uint32_t n_all;
    struct { uint32_t index; uint64_t first_lba, last_lba; char name[37]; char partuuid[37]; uint8_t type[16]; } all[128];
} fb_disk;

extern const char *const fb_part_names[FB_P_N];

typedef struct {
    const char *disk_override;      /* --disk：直接指定整盘节点或镜像文件（测试 / 开发） */
    const char *disks_filter;       /* --disks：扫描时只看这些（逗号分隔的 /dev/xxx；测试隔离用） */
    const char *want_misc_uuid;     /* gk3.disk = misc 的 PARTUUID（boot-entry-design §4.4.1） */
    const char *rundir;             /* 私有目录：mknod 出来的节点、ESP 挂载点 */
} fb_disk_opts;

/* 找目标盘并建白名单。失败时 d->ok=false、d->err 写明原因，但仍返回（getvar 还要能答）。 */
void fb_disk_open(fb_disk *d, const fb_disk_opts *o);
/* 写前核对：重读主 GPT，表头 / 表 CRC 必须与打开时一致。 */
int fb_disk_recheck(fb_disk *d, char *why, size_t why_len);
/* 白名单内读写。off/len 是分区内偏移；越界一律拒绝（返回 -1，errno=ERANGE）。 */
int fb_part_read(fb_disk *d, int pi, uint64_t off, void *buf, size_t len);
int fb_part_write(fb_disk *d, int pi, uint64_t off, const void *buf, size_t len);
/* 落盘 + 丢掉页缓存，使随后的读真的从介质来（读回核对用）。pi < 0 = 整块盘。 */
int fb_disk_sync_drop(fb_disk *d, int pi, uint64_t off, uint64_t len);
/* 尽力 discard（BLKDISCARD / 打洞）；不支持返回 1，失败 -1，成功 0。 */
int fb_part_discard(fb_disk *d, int pi, uint64_t off, uint64_t len);
/* 写一段零并读回核对。 */
int fb_part_zero_verify(fb_disk *d, int pi, uint64_t off, uint64_t len);
/* 按名字找对外分区（含 "boot" → boot_<slot>），找不到返回 -1。 */
int fb_part_lookup(const char *name, int cur_slot);
/* 给 PARTUUID / 分区号造一个私有节点（mknod），返回路径；整盘是镜像文件时返回 NULL。 */
const char *fb_disk_part_node(fb_disk *d, uint32_t index, const char *rundir, char *out, size_t out_len);

/* misc：只许 BCB（0–2 KiB）、BCAB（2048–2079）、GK3 记录（8 KiB 起 2 KiB，恢复出厂被拒时记事件）、
 * VAB（32 KiB 起 64 字节）四段，写后读回。 */
int fb_misc_read(fb_disk *d, void *buf64k);
int fb_misc_write(fb_disk *d, uint32_t off, const void *data, size_t len);

/* ------------------------------------------------------------------ sparse（sparse.c） */

#define FB_SPARSE_MAGIC 0xed26ff3au

typedef struct {
    uint32_t blk_sz, total_blks, total_chunks;
    uint64_t out_size;              /* total_blks * blk_sz */
} fb_sparse_hdr;

typedef struct {
    void *ctx;
    int (*raw)(void *ctx, uint64_t off, const void *data, size_t len);
    int (*fill)(void *ctx, uint64_t off, uint32_t pattern, uint64_t len);
} fb_sparse_ops;

bool fb_sparse_is(const void *img, size_t n);
/* 整份校验（不写盘）：越界、长度对不上、块数对不上、多余字节 → 返回原因串；通过返回 NULL。 */
const char *fb_sparse_check(const void *img, size_t n, uint64_t part_size, fb_sparse_hdr *h);
/* 校验通过后逐 chunk 回调；DONT_CARE 不回调（不碰那段盘）。返回 0 成功。 */
int fb_sparse_walk(const void *img, size_t n, const fb_sparse_ops *ops);

/* ------------------------------------------------------------------ LP 元数据（lp.c，只读） */

#define FB_LP_MAX_PARTS 64

typedef struct {
    uint32_t metadata_max_size, metadata_slot_count, logical_block_size;
    uint16_t major, minor;
    uint32_t flags;
    uint32_t n_parts;
    struct { char name[37]; uint32_t attrs; uint32_t num_extents; } parts[FB_LP_MAX_PARTS];
} fb_lp;

/* read(ctx, off, buf, len) 读 super 内偏移；slot = 元数据槽号。返回 NULL 成功，否则原因。
 * 校验：几何区魔数 + SHA-256、头魔数 / 版本 / header_size + SHA-256、表 SHA-256、表项越界。 */
const char *fb_lp_read(int (*rd)(void *ctx, uint64_t off, void *buf, size_t len), void *ctx,
                       uint32_t slot, fb_lp *out);
/* 槽 x（0/1）的元数据里有没有名字以 _<x> 结尾的分区 */
bool fb_lp_serves_slot(const fb_lp *lp, unsigned slot);

void fb_sha256(const void *data, size_t len, uint8_t out[32]);

/* ------------------------------------------------------------------ ESP（esp.c） */

typedef struct {
    const char *esp_dir_override;   /* --esp-dir：测试时直接用一个已有目录当 ESP 根（不挂载） */
    const char *want_esp_uuid;      /* gk3.esp = ESP 的 PARTUUID（gk3boot 自己从哪个 ESP 起的，S7c 传） */
    const char *rundir;
} fb_esp_opts;

typedef struct {
    bool mounted;
    char root[256];                 /* ESP 根目录 */
    char dev[256];
    char partuuid[37];
} fb_esp;

/* 找到并挂上（rw）ESP。err 写原因。 */
int fb_esp_open(fb_esp *e, fb_disk *d, const fb_esp_opts *o, char *err, size_t err_len);
void fb_esp_close(fb_esp *e);
/* 刷 boot_x 之后：把 kernel / ramdisk / dtb / cmdline.txt 写进该槽直连条目指向的 slot_x/，同步条目 options。
 * img 是完整 boot.img（已通过 SHA1(id) 校验）。info 回调把进度 / 结果发给主机。 */
int fb_esp_sync_slot(fb_esp *e, unsigned slot, const uint8_t *img, size_t img_len, const gk3_bootimg *b,
                     void (*info)(void *ctx, const char *msg), void *ctx, char *err, size_t err_len);
/* loader.conf 的 default 改成 *-android-<x>.conf（照 boot_control/EspSlot.cpp:SetEspDefaultSlot）。 */
int fb_esp_set_default(fb_esp *e, unsigned slot, char *err, size_t err_len);
/* 读 loader.conf 的 default 值（getvar gk3-esp-default）。 */
int fb_esp_get_default(fb_esp *e, char *out, size_t out_len);
/* slot_x/{Image,gaokun3.dtb,ramdisk.img} 存在且非空（set_active 守卫 ③）。 */
int fb_esp_slot_present(fb_esp *e, unsigned slot, char *err, size_t err_len);

/* ------------------------------------------------------------------ 协议与命令（proto.c / cmds.c / vars.c） */

typedef struct {
    /* 配置 */
    uint64_t max_download;
    const char *test_reboot;        /* --test-reboot=<文件>：重启类命令只把意图记进文件、不退出（离线测试） */
    const char *status_file;        /* 状态文件（fb_status）；NULL = 不写 */
    fb_disk_opts dopt;
    fb_esp_opts eopt;
    /* 来自 cmdline（boot-entry-design §4.4.1） */
    char why[32];
    int slot_hint;                  /* gk3.slot；-1 = 没给 */
    char bootver[64];
    char serial[64];
    bool dispatch;                  /* gk3.dispatch=1：拉起本执行端的 gk3boot 开着 BCB 分派 ⇒ 重启类命令可以写 BCB + 冷重启 */
    /* 运行期 */
    fb_disk disk;
    int cur_slot;                   /* current-slot：gk3.slot，set_active 之后跟着变（同上游 fastbootd） */
    bool flashed_boot[2];           /* 本会话刷过 boot_x（set_active 守卫 ④ 的豁免条件） */
    bool flashed_super;
    bool cancel_requested;          /* snapshot-update cancel 记下了，等整块刷 super 时收尾 */
    char entry_note[200];           /* 进入时对 BCB 做了什么（oem device-info） */
    uint8_t *dl;                    /* download 缓冲 */
    size_t dl_len;
} fb_state;

extern fb_state G;

typedef struct fb_ctx fb_ctx;       /* 一条命令的回应上下文 */
void fb_info(fb_ctx *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void fb_okay(fb_ctx *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void fb_fail(fb_ctx *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void fb_info_cb(void *c, const char *msg);   /* 给 esp.c 等的 INFO 回调 */

/* 一个会话：读命令、执行、回应，直到断开。返回 1 = 要求重启（已回 OKAY）。 */
int fb_serve(fb_transport *t);
/* 命令锁：重启前也要拿着它，免得另一个传输的会话正写到一半就被 reboot() 打断 */
void fb_cmd_lock(bool on);
/* 会话结束：丢掉 download 缓冲 */
void fb_session_end(void);
/* 执行一条命令（proto.c 调） */
void fb_dispatch(fb_ctx *c, char *cmd);
/* getvar（vars.c） */
void fb_cmd_getvar(fb_ctx *c, const char *arg);

/* cmds.c 对 proto.c 暴露的少量东西 */
/* BOOTLOADER / FASTBOOT / RECOVERY = 已写 BCB、冷重启；RESTART / MENU = 原地（gk3.dispatch 不是 1，或写不了 BCB） */
typedef enum {
    FB_RB_NONE = 0, FB_RB_REBOOT, FB_RB_BOOTLOADER, FB_RB_FASTBOOT, FB_RB_RECOVERY, FB_RB_POWEROFF,
    FB_RB_RESTART, FB_RB_MENU,
} fb_reboot_kind;
extern fb_reboot_kind fb_pending_reboot;
/* 会话收尾后执行重启类意图：sync → 退出进程（退出码见上）；--test-reboot 时只记意图、返回。 */
void fb_do_reboot(fb_reboot_kind k);
int fb_reboot_exit_code(fb_reboot_kind k);
/* 进入时处理 BCB（gk3.why）：bootloader / fastboot / recovery 类清掉，wipe 类原样留给 S7b。 */
void fb_entry(void);
/* 当前 VAB 有效状态（gk3_vab_effective，按 cur_slot） */
uint8_t fb_vab_status(void);
/* BCAB 读出（32 字节）；返回 gk3_bcab_validate 的结果 */
gk3_err fb_bcab_read(uint8_t bc[32]);
const char *fb_merge_name(uint8_t st);
/* 擦 userdata 或 metadata（§4.6.1 第 2、3 步；erase 命令与 --wipe-data 共用）。失败时已 fb_fail。返回 0 成功。 */
int fb_erase_part(fb_ctx *c, int pi);

/* 没有传输的"本地"回应上下文：子命令复用命令实现时用，INFO / OKAY / FAIL 只进日志（stderr），最后一条 FAIL 的
 * 文字可以取回来打到 stdout。 */
fb_ctx *fb_ctx_local(void);
const char *fb_ctx_last_fail(fb_ctx *c);

/* 子命令（sub.c）：/init 在停掉常驻实例之后调（同一块盘只有一个写者）。返回退出码。 */
int fb_sub_wipe(bool confirm);
int fb_sub_clear_bcb(void);

#endif
