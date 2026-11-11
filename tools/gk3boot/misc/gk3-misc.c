/* gk3-misc：libgk3core 的 CLI（设计稿 §4.1）。
 *
 *   gk3-misc dump    <misc 镜像（≥64 KiB，dd if=/dev/block/by-name/misc bs=4096 count=16）>
 *   gk3-misc select  <misc 镜像> [hint a|b]     入口在这份 misc 上会怎么选（只算，不写）
 *   gk3-misc gpt     <盘头镜像（LBA 0–33）> [块大小]
 *   gk3-misc bootimg <boot.img>                 解析头、复算 SHA1(id)、打印 cmdline
 *
 * S10（安装器）：
 *   gk3-misc init <misc 分区或镜像> [--slot a|b] [--default windows|android|none]
 *     安装器清零 misc 之后调它（scripts/live/installer-lib.sh 的 gk3_apply；设计稿 §4.7 "misc"）。只写三处、其余字节不碰：
 *       0      BCB 2048 字节清零（迁移标记的前提：标记的意思是"这之前的 BCB 都已清掉"）
 *       2048   bootloader_control = gk3_bcab_init_install：目标槽 priority 15 / tries 6 / 未成功，另一槽 0 / 0
 *              （新装机器的另一槽没有 system，LP 里那一槽是陈旧元数据 —— C′ §2.6）
 *       8192   GK3 记录 v1：gk3_rec_init + gk3_rec_migrate(全零 BCB, GK3_DISPATCH_VER)（= 入口首跑迁移在空 BCB 上做的那一步，
 *              事件环里一条 migrated）；--default windows|android 时再放一个 set_default 请求（设计稿 §4.9.3、U12：
 *              安装器不直接写 LoaderEntryDefault，由入口第一次在动作模式下运行时按 core/src/dual.c 的同一套规则写，理由见
 *              installer-lib.sh 的"默认启动哪个系统"一节）。
 *     写完 fsync，再【绕过页缓存】读回 64 KiB 逐字节比对（块设备上 O_DIRECT；不支持时退回普通读并说一声），
 *     并用 gk3_bcab_validate / gk3_rec_validate 复核。stdout 一行
 *       MISCINIT slot=a default=none bcab=<32 字节 hex> rec_crc=<8 hex> rest=zero|nonzero direct=yes|no
 *     rest：2080–8192 与 10240–65536 是不是全零（安装器先整块清零，所以应为 zero；只报告不判死）。
 *     退出码：0 成功；1 写 / 读回 / 复核失败；2 用法 / 打不开 / 小于 64 KiB。
 *
 * S15（双系统）的三个【写】子命令，只改镜像文件里 misc+8 KiB 的 GK3 记录（记录无效时报错、不建）——
 * 开发时在 dd 出来的镜像上模拟 Android 侧，QEMU 夹具与上机前的离线演练用：
 *   gk3-misc mark-poweroff <misc 镜像> <sys.powerctl 的值>   = vendor rc on shutdown 那一步（设计稿 §4.9.3）
 *   gk3-misc set-next      <misc 镜像> windows|sdboot-menu|none
 *   gk3-misc set-default   <misc 镜像> windows|android|none
 * 设备上 on shutdown 跑的不是这个 CLI，而是 boot HAL 二进制的 --gk3-mark-poweroff（同一个库函数
 * gk3_rec_mark_poweroff；理由见 device/huawei/gaokun3/boot_control/Gk3Boot.cpp 顶部）。
 */
#define _GNU_SOURCE   /* O_DIRECT（Linux）；macOS 上没有它，下面按 #ifdef 退回 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "gk3core.h"

static unsigned char *slurp(const char *p, size_t *n)
{
    FILE *f = fopen(p, "rb");
    unsigned char *b;
    long l;
    if (!f) {
        perror(p);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    l = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc(l ? (size_t)l : 1);
    if (fread(b, 1, (size_t)l, f) != (size_t)l) {
        perror(p);
        exit(2);
    }
    fclose(f);
    *n = (size_t)l;
    return b;
}

static void printable(const char *label, const unsigned char *p, size_t n)
{
    printf("%s\"", label);
    for (size_t i = 0; i < n && p[i]; i++)
        if (p[i] == '\n')
            printf("\\n");
        else if (p[i] >= 0x20 && p[i] < 0x7f)
            putchar(p[i]);
        else
            printf("\\x%02x", p[i]);
    printf("\"\n");
}

static int dump(const unsigned char *m, size_t n)
{
    gk3_bcb_info bi;
    gk3_vab v;
    gk3_err e;
    if (n < GK3_MISC_READ_SIZE) {
        fprintf(stderr, "misc 镜像不足 64 KiB\n");
        return 2;
    }
    gk3_bcb_classify(m, &bi);
    printf("BCB      kind=%s%s\n", gk3_bcb_kind_name(bi.kind), gk3_is_zero(m, 2048) ? "（2 KiB 全零）" : "");
    printable("         command=", m, 32);
    printable("         recovery=", m + 64, 768);

    e = gk3_bcab_validate(m + 2048);
    printf("BCAB     %s  suffix=", e ? gk3_strerror(e) : "有效");
    printable("", m + 2048, 4);
    printf("         nb_slot=%u recovery_tries=%u merge_status=%u crc(盘上字节)=%02x %02x %02x %02x\n",
           gk3_bcab_nb_slot(m + 2048), gk3_bcab_recovery_tries(m + 2048), gk3_bcab_merge_status(m + 2048),
           m[2076], m[2077], m[2078], m[2079]);
    for (unsigned i = 0; i < 2; i++) {
        gk3_slot_info s;
        gk3_bcab_get_slot(m + 2048, i, &s);
        printf("         _%c priority=%u tries=%u successful=%u verity_corrupted=%u → %s\n", 'a' + i, s.priority,
               s.tries, s.successful, s.verity_corrupted, gk3_slot_bootable(&s) ? "可启动" : "不可启动");
    }

    e = gk3_rec_validate(m + GK3_MISC_GK3_OFF);
    if (e) {
        printf("GK3      无记录（%s）%s\n", gk3_strerror(e),
               gk3_is_zero(m + GK3_MISC_GK3_OFF, GK3_REC_SIZE) ? "，8 KiB 处 2 KiB 全零" : "，⚠️ 8 KiB 处有非零内容");
    } else {
        const unsigned char *r = m + GK3_MISC_GK3_OFF;
        gk3_event ev[GK3_EV_N];
        uint32_t k = gk3_rec_events(r, ev, GK3_EV_N);
        uint8_t ns;
        gk3_next_kind nk = gk3_rec_next(r, &ns);
        char cmd[33];
        gk3_rec_migrated_command(r, cmd);
        printf("GK3      有效  migrated=%u boot_streak=%u next=%u/%u dispatch_count=%u migrated_command=\"%s\"\n",
               gk3_rec_migrated(r), gk3_rec_boot_streak(r), nk, ns, gk3_rec_dispatch_count(r), cmd);
        {
            static const char *const os[] = {"unknown", "android", "windows"}, *const sd[] = {"none", "windows", "android"};
            printf("         双系统：default_os=%s set_default=%s clean_poweroff=%u\n", os[gk3_rec_default_os(r)],
                   sd[gk3_rec_set_default_req(r)], gk3_rec_clean_poweroff(r));
        }
        for (uint32_t i = 0; i < k && i < GK3_EV_N; i++)
            printf("         #%u %s slot=%u aux=%u%s\n", ev[i].seq, gk3_ev_name((gk3_ev_code)ev[i].code), ev[i].slot,
                   ev[i].aux, ev[i].flags & GK3_EVF_NOTIFIED ? "（已通知）" : "");
    }

    gk3_vab_parse(m + GK3_MISC_SYSTEM_OFF, &v);
    printf("VAB      %s version=%u magic=%08x merge_status=%u source_slot=%u\n", v.valid ? "有效" : "无效",
           v.version, v.magic, v.merge_status, v.source_slot);
    /* GK3 记录（8 KiB 处 2 KiB）不算"其他" */
    printf("其他     2K+32..8K、10K..32K %s\n",
           gk3_is_zero(m + 2080, GK3_MISC_GK3_OFF - 2080) &&
                   gk3_is_zero(m + GK3_MISC_GK3_OFF + GK3_REC_SIZE, 32768 - GK3_MISC_GK3_OFF - GK3_REC_SIZE)
               ? "全零"
               : "有非零内容");
    return 0;
}

static int sel(unsigned char *m, unsigned hint)
{
    gk3_sel s;
    gk3_vab v;
    uint8_t bc[32];
    static const char *k[] = {"boot", "bcab_invalid", "noslot", "merging"};
    gk3_vab_parse(m + GK3_MISC_SYSTEM_OFF, &v);
    memcpy(bc, m + 2048, 32);
    gk3_select_slot(bc, hint, v.valid ? v.merge_status : GK3_MERGE_UNKNOWN, &s);
    printf("决定 %s：slot=_%c active=_%c fallback=%u 扣 tries=%u（%u→%u）%s%s\n", k[s.kind], 'a' + s.slot,
           'a' + s.active, s.fallback, s.decremented, s.tries_before, s.tries_after,
           s.kind == GK3_SEL_BCAB_INVALID ? " bcab=" : "", s.kind == GK3_SEL_BCAB_INVALID ? gk3_strerror(s.bcab_err) : "");
    if (s.decremented)
        printf("（入口会把 BCAB 写回 misc+2048，这里没写）\n");
    return 0;
}

static int gpt(const unsigned char *d, size_t n, unsigned bs)
{
    gk3_gpt g;
    gk3_gpt_part p;
    gk3_err e;
    char gs[37];
    static const char *const six[] = {"misc", "boot_a", "boot_b", "super", "userdata", "metadata"};
    const char *bad = NULL;
    if (n < 2 * bs) {
        fprintf(stderr, "太短\n");
        return 2;
    }
    e = gk3_gpt_parse_header(d + bs, bs, &g);
    if (!e && g.entries_lba * bs + (size_t)g.num_entries * g.entry_size > n)
        e = GK3_ENOSPC;
    if (!e)
        e = gk3_gpt_parse_mem(d + bs, bs, d + g.entries_lba * bs, n - g.entries_lba * bs, &g);
    if (e) {
        printf("GPT 无效：%s\n", gk3_strerror(e));
        return 1;
    }
    gk3_guid_str(g.disk_guid, gs);
    printf("GPT  disk=%s  表项 %u×%u @LBA %llu  usable %llu–%llu  已用 %u\n", gs, g.num_entries, g.entry_size,
           (unsigned long long)g.entries_lba, (unsigned long long)g.first_usable, (unsigned long long)g.last_usable,
           gk3_gpt_count(&g));
    for (uint32_t i = 0; i < g.num_entries; i++) {
        if (gk3_gpt_get(&g, i, &p))
            continue;
        gk3_guid_str(p.part_guid, gs);
        printf("  p%-3u %-14s %12llu %12llu  attrs=%016llx  %s\n", p.index, p.name, (unsigned long long)p.first_lba,
               (unsigned long long)p.last_lba, (unsigned long long)p.attrs, gs);
    }
    e = gk3_gpt_require_unique(&g, six, 6, &bad);
    printf("六个名字各恰好一次：%s%s%s\n", e ? gk3_strerror(e) : "是", e ? " —— " : "", e ? bad : "");
    return e ? 1 : 0;
}

static int bootimg(const unsigned char *d, size_t n)
{
    gk3_bootimg b;
    uint8_t got[20];
    char cmd[2048];
    gk3_err e = gk3_bootimg_parse(d, n, 0, &b);
    if (e) {
        printf("boot.img 无效：%s\n", gk3_strerror(e));
        return 1;
    }
    printf("boot.img v%u page=%u kernel=%u ramdisk=%u second=%u recovery_dtbo=%u dtb=%u total=%llu\n", b.version,
           b.page_size, b.kernel_size, b.ramdisk_size, b.second_size, b.recovery_dtbo_size, b.dtb_size,
           (unsigned long long)b.total_size);
    e = gk3_bootimg_verify_id(&b, d, n, got);
    printf("id ");
    for (int i = 0; i < 20; i++)
        printf("%02x", b.id[i]);
    printf("  复算 %s\n", e == GK3_OK ? "一致" : gk3_strerror(e));
    if (gk3_bootimg_cmdline(&b, cmd, sizeof(cmd)) >= 0)
        printf("cmdline \"%s\"\n", cmd);
    return e ? 1 : 0;
}

/* S15：改镜像里的 GK3 记录（只写 8 KiB 处那 2 KiB，其余字节不碰） */
static int edit(const char *path, unsigned char *m, size_t n, const char *cmd, const char *arg)
{
    unsigned char *r = m + GK3_MISC_GK3_OFF;
    FILE *f;
    if (n < GK3_MISC_GK3_OFF + GK3_REC_SIZE || gk3_rec_validate(r) != GK3_OK) {
        fprintf(stderr, "%s：8 KiB 处没有有效的 GK3 记录（入口还没在动作模式下跑过？）—— 不建\n", path);
        return 1;
    }
    if (!strcmp(cmd, "mark-poweroff")) {
        if (!gk3_rec_mark_poweroff(r, arg)) {
            printf("不写：sys.powerctl=\"%s\"，实际默认%s Windows\n", arg,
                   gk3_rec_effective_default_windows(r) ? "是" : "不是");
            return 0;
        }
    } else if (!strcmp(cmd, "set-next")) {
        if (!strcmp(arg, "windows"))
            gk3_rec_set_next(r, GK3_NEXT_WINDOWS, 0);
        else if (!strcmp(arg, "sdboot-menu"))
            gk3_rec_set_next(r, GK3_NEXT_SDBOOT_MENU, 0);
        else if (!strcmp(arg, "none"))
            gk3_rec_set_next(r, GK3_NEXT_NONE, 0);
        else
            return fprintf(stderr, "set-next windows|sdboot-menu|none\n"), 2;
    } else {
        if (!strcmp(arg, "windows"))
            gk3_rec_put_set_default_req(r, GK3_SETDEF_WINDOWS);
        else if (!strcmp(arg, "android"))
            gk3_rec_put_set_default_req(r, GK3_SETDEF_ANDROID);
        else if (!strcmp(arg, "none"))
            gk3_rec_put_set_default_req(r, GK3_SETDEF_NONE);
        else
            return fprintf(stderr, "set-default windows|android|none\n"), 2;
    }
    gk3_rec_seal(r);
    if (!(f = fopen(path, "r+b")) || fseek(f, GK3_MISC_GK3_OFF, SEEK_SET) ||
        fwrite(r, 1, GK3_REC_SIZE, f) != GK3_REC_SIZE || fclose(f)) {
        perror(path);
        return 1;
    }
    printf("已写：%s %s\n", cmd, arg);
    return 0;
}

/* S10：安装器初始化 misc（文件头的说明）。不走 slurp：目标多半是块设备，只读写前 64 KiB。 */
static int full_pwrite(int fd, const void *b, size_t n, off_t off)
{
    const unsigned char *p = b;
    while (n) {
        ssize_t w = pwrite(fd, p, n, off);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return -1;
        p += w, n -= (size_t)w, off += w;
    }
    return 0;
}

static int full_pread(int fd, void *b, size_t n, off_t off)
{
    unsigned char *p = b;
    while (n) {
        ssize_t r = pread(fd, p, n, off);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return -1;
        p += r, n -= (size_t)r, off += r;
    }
    return 0;
}

static int init_misc(int argc, char **argv)
{
    const char *path = argv[2];
    unsigned slot = 0;
    gk3_setdef def = GK3_SETDEF_NONE;
    static const char *const defname[] = {"none", "windows", "android"};
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--slot") && i + 1 < argc && (!strcmp(argv[i + 1], "a") || !strcmp(argv[i + 1], "b")))
            slot = argv[++i][0] == 'b';
        else if (!strcmp(argv[i], "--default") && i + 1 < argc) {
            const char *v = argv[++i];
            if (!strcmp(v, "windows"))
                def = GK3_SETDEF_WINDOWS;
            else if (!strcmp(v, "android"))
                def = GK3_SETDEF_ANDROID;
            else if (strcmp(v, "none"))
                return fprintf(stderr, "--default windows|android|none\n"), 2;
        } else
            return fprintf(stderr, "用法：gk3-misc init <misc 分区或镜像> [--slot a|b] [--default windows|android|none]\n"), 2;
    }

    int fd = open(path, O_RDWR);
    if (fd < 0)
        return fprintf(stderr, "%s：%s\n", path, strerror(errno)), 2;
    off_t size = lseek(fd, 0, SEEK_END);
    if (size < (off_t)GK3_MISC_READ_SIZE) {
        fprintf(stderr, "%s：只有 %lld 字节，misc 至少要 64 KiB（入口一次读 0–64 KiB）\n", path, (long long)size);
        close(fd);
        return 2;
    }

    /* 0–2080：BCB 清零 + BCAB；8192–10240：GK3 记录（组装规则在 libgk3core，Windows 安装器用同一个函数） */
    unsigned char head[GK3_MISC_INIT_HEAD_SIZE], rec[GK3_REC_SIZE];
    if (gk3_misc_build_install(head, rec, slot, def)) {
        fprintf(stderr, "内部错误：造出来的 BCAB / GK3 记录自己都不合法\n");
        close(fd);
        return 1;
    }
    if (full_pwrite(fd, head, sizeof(head), 0) || full_pwrite(fd, rec, sizeof(rec), GK3_MISC_GK3_OFF) || fsync(fd)) {
        fprintf(stderr, "%s：写失败：%s\n", path, strerror(errno));
        close(fd);
        return 1;
    }
    close(fd);

    /* 读回：绕过页缓存读的才是盘上的字节（同 HAL 的 gk3_blk_write_bytes_verify、安装器的 gk3__verify_on） */
    unsigned char *m = NULL;
    int direct = 0;
    if (posix_memalign((void **)&m, 4096, GK3_MISC_READ_SIZE))
        return fprintf(stderr, "内存不够\n"), 1;
#ifdef O_DIRECT
    fd = open(path, O_RDONLY | O_DIRECT);
    if (fd >= 0) {
        if (full_pread(fd, m, GK3_MISC_READ_SIZE, 0) == 0)
            direct = 1;
        else
            close(fd), fd = -1;
    }
#else
    fd = -1;
#endif
    if (!direct) {
        if (fd >= 0)
            close(fd);
        fd = open(path, O_RDONLY);
        if (fd < 0 || full_pread(fd, m, GK3_MISC_READ_SIZE, 0)) {
            fprintf(stderr, "%s：读回失败：%s\n", path, strerror(errno));
            if (fd >= 0)
                close(fd);
            free(m);
            return 1;
        }
        fprintf(stderr, "（%s 不支持 O_DIRECT，读回走的是页缓存）\n", path);
    }
    close(fd);
    int bad = memcmp(m, head, sizeof(head)) || memcmp(m + GK3_MISC_GK3_OFF, rec, sizeof(rec)) ||
              gk3_bcab_validate(m + GK3_MISC_BCAB_OFF) || gk3_rec_validate(m + GK3_MISC_GK3_OFF);
    int rest = gk3_is_zero(m + sizeof(head), GK3_MISC_GK3_OFF - sizeof(head)) &&
               gk3_is_zero(m + GK3_MISC_GK3_OFF + GK3_REC_SIZE, GK3_MISC_READ_SIZE - GK3_MISC_GK3_OFF - GK3_REC_SIZE);
    if (bad) {
        fprintf(stderr, "%s：读回来的 BCB / BCAB / GK3 记录与写下去的不一致\n", path);
        free(m);
        return 1;
    }
    printf("MISCINIT slot=%c default=%s bcab=", 'a' + slot, defname[def]);
    for (unsigned i = 0; i < GK3_MISC_BCAB_SIZE; i++)
        printf("%02x", m[GK3_MISC_BCAB_OFF + i]);
    printf(" rec_crc=%02x%02x%02x%02x rest=%s direct=%s\n", m[GK3_MISC_GK3_OFF + GK3_REC_SIZE - 1],
           m[GK3_MISC_GK3_OFF + GK3_REC_SIZE - 2], m[GK3_MISC_GK3_OFF + GK3_REC_SIZE - 3],
           m[GK3_MISC_GK3_OFF + GK3_REC_SIZE - 4], rest ? "zero" : "nonzero", direct ? "yes" : "no");
    free(m);
    return 0;
}

int main(int argc, char **argv)
{
    size_t n;
    unsigned char *d;
    if (argc < 3) {
        fprintf(stderr, "用法：gk3-misc dump|select|gpt|bootimg|init|mark-poweroff|set-next|set-default <文件> [参数]\n");
        return 2;
    }
    if (!strcmp(argv[1], "init"))
        return init_misc(argc, argv);
    d = slurp(argv[2], &n);
    if (!strcmp(argv[1], "dump"))
        return dump(d, n);
    if (!strcmp(argv[1], "select"))
        return n < GK3_MISC_READ_SIZE ? 2 : sel(d, argc > 3 && argv[3][0] == 'b');
    if (!strcmp(argv[1], "gpt"))
        return gpt(d, n, argc > 3 ? (unsigned)atoi(argv[3]) : 512);
    if (!strcmp(argv[1], "bootimg"))
        return bootimg(d, n);
    if (!strcmp(argv[1], "mark-poweroff") || !strcmp(argv[1], "set-next") || !strcmp(argv[1], "set-default")) {
        if (argc < 4)
            return fprintf(stderr, "用法：gk3-misc %s <misc 镜像> <值>\n", argv[1]), 2;
        return edit(argv[2], d, n, argv[1], argv[3]);
    }
    fprintf(stderr, "不认识的子命令 %s\n", argv[1]);
    return 2;
}
