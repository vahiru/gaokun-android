/* gk3-fbi：执行端 initramfs（fastboot.img）里 /init 的小帮手（见 tools/gk3boot/README.md）。
 * 静态 aarch64（musl），链接 libgk3core，只读，不写任何盘。
 *
 *   gk3-fbi keyd                 常驻：读所有 /dev/input/event*，按键 → 一行一个词写到 stdout：
 *                                up / down / ok / back。原始事件（设备名 + 键码）记到 stderr，
 *                                真机键码就从这份日志里抄。
 *                                每 2 秒重扫一次 /dev/input（键盘盖是 USB HID，可能后插）。
 *   gk3-fbi find   <PARTUUID>    在所有盘的主 GPT 里找这个 PARTUUID（= gk3.disk，misc 的），
 *                                打印 "<分区节点> <整盘节点>"；不唯一 / 找不到 → 非零退出。
 *   gk3-fbi font <PSF2> <tty>    给这个 VT 装控制台字体（Terminus 32x16）与它的 Unicode 映射。
 *   gk3-fbi status <PARTUUID>    英文状态（给 tty1 的界面直接打印）：目标盘、六个名字是否唯一、
 *                                BCB、两个槽、VAB 合并状态、GK3 记录。
 *
 * 输出一律英文 ASCII：Linux VT 的字体画不了 CJK。
 * 只读：清 BCB、擦数据、写槽都是 gk3-fastbootd 的事（接口见 tools/gk3boot/README.md §13）。
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "gk3core.h"

/* ------------------------------------------------------------------ 按键 */

#define MAXDEV 32
#define BITS_PER_LONG (sizeof(long) * 8)
#define NLONGS(x) (((x) + BITS_PER_LONG - 1) / BITS_PER_LONG)
#define TEST_BIT(b, a) ((a[(b) / BITS_PER_LONG] >> ((b) % BITS_PER_LONG)) & 1)

struct dev {
    int fd;
    char path[64];
    char name[80];
};

static struct dev devs[MAXDEV];
static int ndev;

/* 键码 → 菜单动作。音量上 / 下 + 电源是平板姿态的主通道；
 * 键盘盖的方向键 / 回车 / Esc 作备选。 */
static const char *map_key(unsigned code)
{
    switch (code) {
    case KEY_VOLUMEUP:
    case KEY_UP:
    case KEY_PAGEUP:
        return "up";
    case KEY_VOLUMEDOWN:
    case KEY_DOWN:
    case KEY_PAGEDOWN:
    case KEY_TAB:
        return "down";
    case KEY_POWER:
    case KEY_ENTER:
    case KEY_KPENTER:
    case KEY_SPACE:
        return "ok";
    case KEY_ESC:
    case KEY_BACKSPACE:
    case KEY_LEFT:
        return "back";
    }
    return NULL;
}

static const char *key_name(unsigned code)
{
    switch (code) {
    case KEY_VOLUMEUP: return "KEY_VOLUMEUP";
    case KEY_VOLUMEDOWN: return "KEY_VOLUMEDOWN";
    case KEY_POWER: return "KEY_POWER";
    case KEY_UP: return "KEY_UP";
    case KEY_DOWN: return "KEY_DOWN";
    case KEY_LEFT: return "KEY_LEFT";
    case KEY_RIGHT: return "KEY_RIGHT";
    case KEY_ENTER: return "KEY_ENTER";
    case KEY_KPENTER: return "KEY_KPENTER";
    case KEY_SPACE: return "KEY_SPACE";
    case KEY_ESC: return "KEY_ESC";
    case KEY_BACKSPACE: return "KEY_BACKSPACE";
    case KEY_TAB: return "KEY_TAB";
    case KEY_PAGEUP: return "KEY_PAGEUP";
    case KEY_PAGEDOWN: return "KEY_PAGEDOWN";
    }
    return "?";
}

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static int known(const char *path)
{
    for (int i = 0; i < ndev; i++)
        if (!strcmp(devs[i].path, path))
            return 1;
    return 0;
}

/* 只留下"能报我们关心的键"的设备：触摸屏、传感器之类不进 poll，免得空转。 */
static void rescan(void)
{
    DIR *d = opendir("/dev/input");
    struct dirent *e;
    if (!d)
        return;
    /* 已经不在的设备先忘掉（含"没有菜单键、fd 已关"的那些）：eventN 会被后插的设备复用 */
    for (int i = 0; i < ndev;) {
        struct stat st;
        if (stat(devs[i].path, &st)) {
            if (devs[i].fd >= 0)
                close(devs[i].fd);
            devs[i] = devs[--ndev];
        } else {
            i++;
        }
    }
    while ((e = readdir(d)) != NULL && ndev < MAXDEV) {
        char path[64];
        unsigned long keys[NLONGS(KEY_CNT)];
        int fd, any = 0;
        if (strncmp(e->d_name, "event", 5))
            continue;
        snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
        if (known(path))
            continue;
        fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;
        memset(keys, 0, sizeof(keys));
        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) >= 0) {
            static const unsigned want[] = {KEY_VOLUMEUP, KEY_VOLUMEDOWN, KEY_POWER, KEY_UP, KEY_DOWN, KEY_ENTER};
            for (size_t k = 0; k < sizeof(want) / sizeof(want[0]); k++)
                if (TEST_BIT(want[k], keys))
                    any = 1;
        }
        devs[ndev].fd = fd;
        snprintf(devs[ndev].path, sizeof(devs[ndev].path), "%s", path);
        devs[ndev].name[0] = 0;
        ioctl(fd, EVIOCGNAME(sizeof(devs[ndev].name) - 1), devs[ndev].name);
        fprintf(stderr, "keyd: %s \"%s\" %s\n", path, devs[ndev].name, any ? "watching" : "no menu keys, ignored");
        if (!any) {
            /* 记着它（不再重复打开），但 fd 关掉、不进 poll */
            close(fd);
            devs[ndev].fd = -1;
        }
        ndev++;
    }
    closedir(d);
}

static int keyd(void)
{
    double last_scan = 0;
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    for (;;) {
        struct pollfd p[MAXDEV];
        int map[MAXDEV], n = 0, r;
        if (now_s() - last_scan >= 2.0) {
            rescan();
            last_scan = now_s();
        }
        for (int i = 0; i < ndev; i++)
            if (devs[i].fd >= 0) {
                p[n].fd = devs[i].fd;
                p[n].events = POLLIN;
                p[n].revents = 0;
                map[n++] = i;
            }
        r = poll(p, (nfds_t)n, 2000);
        if (r < 0 && errno != EINTR)
            return 1;
        for (int j = 0; r > 0 && j < n; j++) {
            struct dev *dv = &devs[map[j]];
            struct input_event ev[16];
            ssize_t got;
            if (!p[j].revents)
                continue;
            got = read(dv->fd, ev, sizeof(ev));
            if (got < 0 && errno != EAGAIN && errno != EINTR) {
                /* 设备拔了（ENODEV）：丢掉，下次重扫时它的路径要能被重新认领 */
                fprintf(stderr, "keyd: %s gone (%s)\n", dv->path, strerror(errno));
                close(dv->fd);
                *dv = devs[--ndev];
                break; /* map[] 已失效，重来一轮 */
            }
            for (ssize_t k = 0; got > 0 && k < got / (ssize_t)sizeof(ev[0]); k++) {
                const char *act;
                if (ev[k].type != EV_KEY)
                    continue;
                act = map_key(ev[k].code);
                /* 原始事件全记：真机键码要从这里抄 */
                fprintf(stderr, "keyd: %s \"%s\" code=%u (%s) value=%d -> %s\n", dv->path, dv->name, ev[k].code,
                        key_name(ev[k].code), ev[k].value, act ? act : "-");
                /* 按下算一次；按住自动重复只给上下移动（电源键按住不能连发"确认"） */
                if (!act || !(ev[k].value == 1 || (ev[k].value == 2 && (!strcmp(act, "up") || !strcmp(act, "down")))))
                    continue;
                if (printf("%s\n", act) < 0 || fflush(stdout) == EOF)
                    return 0; /* 读的一方没了 */
            }
        }
    }
}

/* ------------------------------------------------------------------ 盘 */

static int lower_eq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
    return *a == *b;
}

static int read_sys(const char *path, char *buf, size_t n)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    if (!fgets(buf, (int)n, f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    buf[strcspn(buf, "\n")] = 0;
    return 0;
}

struct hit {
    char disk[32];      /* nvme0n1 */
    char part[48];      /* nvme0n1p4 */
    uint32_t bs;
    uint64_t first, last;
    int six_err;        /* 六个名字唯一性 */
    const char *six_bad;
};

/* 读整盘的主 GPT。成功返回 malloc 出来的缓冲区（hdr 块 + 表项），g 指向其中 */
static uint8_t *read_gpt(const char *disk, uint32_t *bs_out, gk3_gpt *g)
{
    char p[96], v[32];
    uint32_t bs = 512;
    uint8_t *hdr = NULL, *ent = NULL;
    size_t elen;
    int fd;
    snprintf(p, sizeof(p), "/sys/block/%s/queue/logical_block_size", disk);
    if (!read_sys(p, v, sizeof(v)))
        bs = (uint32_t)strtoul(v, NULL, 10);
    if (bs != 512 && bs != 4096)
        return NULL;
    snprintf(p, sizeof(p), "/dev/%s", disk);
    fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return NULL;
    hdr = malloc(bs);
    if (!hdr || pread(fd, hdr, bs, bs) != (ssize_t)bs || gk3_gpt_parse_header(hdr, bs, g))
        goto fail;
    elen = (size_t)g->num_entries * g->entry_size;
    if (elen == 0 || elen > (1u << 20))
        goto fail;
    ent = malloc(bs + elen);
    if (!ent)
        goto fail;
    memcpy(ent, hdr, bs);
    if (pread(fd, ent + bs, elen, (off_t)(g->entries_lba * bs)) != (ssize_t)elen)
        goto fail;
    if (gk3_gpt_parse_mem(ent, bs, ent + bs, elen, g))
        goto fail;
    free(hdr);
    close(fd);
    *bs_out = bs;
    return ent;
fail:
    free(hdr);
    free(ent);
    close(fd);
    return NULL;
}

/* sysfs 里按分区号找分区节点名（不拼 "p"：nvme0n1p4 / sda4 / vda4 的规则不一样，sysfs 说了算） */
static int part_node(const char *disk, uint32_t index, char *out, size_t n)
{
    char p[128], v[16];
    DIR *d;
    struct dirent *e;
    snprintf(p, sizeof(p), "/sys/block/%s", disk);
    d = opendir(p);
    if (!d)
        return -1;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, disk, strlen(disk)))
            continue;
        snprintf(p, sizeof(p), "/sys/block/%s/%s/partition", disk, e->d_name);
        if (!read_sys(p, v, sizeof(v)) && strtoul(v, NULL, 10) == index) {
            snprintf(out, n, "%s", e->d_name);
            closedir(d);
            return 0;
        }
    }
    closedir(d);
    return -1;
}

/* 返回命中次数（>1 = 不唯一，不能用：设计稿 §4.4.1 "仍要校验唯一性"） */
static int find_partuuid(const char *uuid, struct hit *h)
{
    static const char *const six[] = {"misc", "boot_a", "boot_b", "super", "userdata", "metadata"};
    DIR *d = opendir("/sys/block");
    struct dirent *e;
    int hits = 0;
    if (!d)
        return 0;
    while ((e = readdir(d)) != NULL) {
        gk3_gpt g;
        uint32_t bs;
        uint8_t *buf;
        if (e->d_name[0] == '.' || !strncmp(e->d_name, "loop", 4) || !strncmp(e->d_name, "ram", 3) ||
            !strncmp(e->d_name, "zram", 4) || !strncmp(e->d_name, "dm-", 3))
            continue;
        buf = read_gpt(e->d_name, &bs, &g);
        if (!buf)
            continue;
        for (uint32_t i = 0; i < g.num_entries; i++) {
            gk3_gpt_part p;
            char s[37];
            if (gk3_gpt_get(&g, i, &p))
                continue;
            gk3_guid_str(p.part_guid, s);
            if (!lower_eq(s, uuid))
                continue;
            if (hits++ == 0) {
                snprintf(h->disk, sizeof(h->disk), "%s", e->d_name);
                if (part_node(e->d_name, p.index, h->part, sizeof(h->part)))
                    snprintf(h->part, sizeof(h->part), "?");
                h->bs = bs;
                h->first = p.first_lba;
                h->last = p.last_lba;
                h->six_bad = NULL;
                h->six_err = gk3_gpt_require_unique(&g, six, 6, &h->six_bad);
            }
        }
        free(buf);
    }
    closedir(d);
    return hits;
}

static int cmd_find(const char *uuid)
{
    struct hit h;
    int n = find_partuuid(uuid, &h);
    if (n != 1) {
        fprintf(stderr, "gk3-fbi: PARTUUID %s found %d times\n", uuid, n);
        return n ? 3 : 2;
    }
    printf("/dev/%s /dev/%s\n", h.part, h.disk);
    return 0;
}

static const char *merge_name(uint8_t m)
{
    switch (m) {
    case GK3_MERGE_NONE: return "none";
    case GK3_MERGE_UNKNOWN: return "unknown";
    case GK3_MERGE_SNAPSHOTTED: return "snapshotted (update pending)";
    case GK3_MERGE_MERGING: return "MERGING";
    case GK3_MERGE_CANCELLED: return "cancelled";
    }
    return "?";
}

static int cmd_status(const char *uuid)
{
    struct hit h;
    uint8_t m[GK3_MISC_READ_SIZE];
    int n = find_partuuid(uuid, &h), fd;
    gk3_bcb_info bi;
    gk3_vab v;
    gk3_err e;
    char p[64];

    if (n == 0) {
        printf("Disk:    misc partition %s not found\n", uuid);
        return 2;
    }
    if (n > 1) {
        printf("Disk:    PARTUUID %s appears %d times - refusing to guess\n", uuid, n);
        return 3;
    }
    printf("Disk:    /dev/%s (misc = /dev/%s)\n", h.disk, h.part);
    if (h.six_err)
        printf("Layout:  PROBLEM - partition \"%s\": %s\n", h.six_bad ? h.six_bad : "?", gk3_strerror(h.six_err));
    else
        printf("Layout:  ok (misc boot_a boot_b super userdata metadata, each exactly once)\n");
    if ((h.last - h.first + 1) * h.bs < GK3_MISC_READ_SIZE) {
        printf("Misc:    partition smaller than 64 KiB\n");
        return 4;
    }
    snprintf(p, sizeof(p), "/dev/%s", h.part);
    fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0 || pread(fd, m, sizeof(m), 0) != (ssize_t)sizeof(m)) {
        printf("Misc:    cannot read %s: %s\n", p, strerror(errno));
        if (fd >= 0)
            close(fd);
        return 4;
    }
    close(fd);

    gk3_bcb_classify(m, &bi);
    printf("BCB:     %s\n", gk3_bcb_kind_name(bi.kind));
    e = gk3_bcab_validate(m + GK3_MISC_BCAB_OFF);
    if (e) {
        printf("Slots:   boot control block invalid (%s)\n", gk3_strerror(e));
    } else {
        unsigned active = 0;
        gk3_slot_info s[2];
        for (unsigned i = 0; i < 2; i++)
            gk3_bcab_get_slot(m + GK3_MISC_BCAB_OFF, i, &s[i]);
        active = s[1].priority > s[0].priority ? 1 : 0;
        for (unsigned i = 0; i < 2; i++)
            printf("Slot _%c: %-10s priority=%u tries=%u%s%s%s\n", 'a' + i,
                   gk3_slot_bootable(&s[i]) ? "bootable" : "UNBOOTABLE", s[i].priority, s[i].tries,
                   s[i].successful ? " successful" : "", s[i].verity_corrupted ? " verity-corrupted" : "",
                   i == active ? "  <- active" : "");
    }
    gk3_vab_parse(m + GK3_MISC_SYSTEM_OFF, &v);
    if (v.valid)
        printf("Update:  merge status %s%s\n", merge_name(v.merge_status),
               v.merge_status == GK3_MERGE_MERGING ? " (wipe and slot switch refused)" : "");
    else
        printf("Update:  no virtual A/B message\n");
    if (!gk3_rec_validate(m + GK3_MISC_GK3_OFF))
        printf("Record:  boot streak %u, dispatch count %u%s\n", gk3_rec_boot_streak(m + GK3_MISC_GK3_OFF),
               gk3_rec_dispatch_count(m + GK3_MISC_GK3_OFF), gk3_rec_migrated(m + GK3_MISC_GK3_OFF) ? ", migrated" : "");
    else
        printf("Record:  none\n");
    return 0;
}

/* ------------------------------------------------------------------ 控制台字体 */

/* 把 PSF2 字体装到一个 VT 上：KDFONTOP(KD_FONT_OP_SET) 装字形 + PIO_UNIMAPCLR/PIO_UNIMAP 装 Unicode 映射。
 * 为什么不用 busybox loadfont：Debian 的 busybox-static 不认 Uni2-TerminusBold32x16（实测报
 * "bad length or unsupported font type"）。
 * 字形数据的排法：KD_FONT_OP_SET 的每个字形占 32 行 × ceil(width/8) 字节（vpitch 固定 32）；
 * PSF2 的 32 点高字体每个字形正好 32 行，所以 PSF2 的字形区原样就是内核要的格式 —— 高度不是 32 的字体这里拒绝。
 * Unicode 表不装的话，内核沿用旧映射（原 8x16 字体的 CP437 顺序），字形顺序不同的字体就会画错字。 */
static int cmd_font(const char *psf, const char *tty)
{
    FILE *f = fopen(psf, "rb");
    uint8_t *d;
    long n;
    uint32_t hsz, flags, cnt, csz, h, w;
    struct console_font_op op;
    struct unipair *up;
    struct unimapdesc ud;
    struct unimapinit ui;
    size_t nup = 0, cap;
    int fd;

    if (!f) {
        perror(psf);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    d = malloc((size_t)n);
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "font: read %s failed\n", psf);
        return 2;
    }
    fclose(f);
    if (n < 32 || gk3_le32(d) != 0x864ab572u) {
        fprintf(stderr, "font: %s is not PSF2\n", psf);
        return 2;
    }
    hsz = gk3_le32(d + 8);
    flags = gk3_le32(d + 12);
    cnt = gk3_le32(d + 16);
    csz = gk3_le32(d + 20);
    h = gk3_le32(d + 24);
    w = gk3_le32(d + 28);
    if (h != 32 || w == 0 || w > 32 || cnt == 0 || cnt > 512 || csz != ((w + 7) / 8) * h ||
        (uint64_t)hsz + (uint64_t)cnt * csz > (uint64_t)n) {
        fprintf(stderr, "font: unsupported PSF2 (%ux%u, %u glyphs, %u bytes each)\n", w, h, cnt, csz);
        return 2;
    }
    fd = open(tty, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "font: open %s: %s\n", tty, strerror(errno));
        return 3;
    }
    memset(&op, 0, sizeof(op));
    op.op = KD_FONT_OP_SET;
    op.width = w;
    op.height = h;
    op.charcount = cnt;
    op.data = d + hsz;
    if (ioctl(fd, KDFONTOP, &op)) {
        fprintf(stderr, "font: KDFONTOP on %s: %s\n", tty, strerror(errno));
        close(fd);
        return 3;
    }
    if (flags & 1) {
        /* Unicode 表：每个字形一串 UTF-8 码点，0xFE 之后是组合序列（跳过），0xFF 结束这个字形 */
        const uint8_t *p = d + hsz + (size_t)cnt * csz, *end = d + n;
        cap = 4096;
        up = malloc(cap * sizeof(*up));
        for (uint32_t g = 0; g < cnt && p < end && up; g++) {
            int seq = 0;
            while (p < end && *p != 0xFF) {
                uint32_t c;
                int k;
                if (*p == 0xFE) {
                    seq = 1;
                    p++;
                    continue;
                }
                if (*p < 0x80) {
                    c = *p;
                    k = 1;
                } else if ((*p & 0xE0) == 0xC0) {
                    c = *p & 0x1F;
                    k = 2;
                } else if ((*p & 0xF0) == 0xE0) {
                    c = *p & 0x0F;
                    k = 3;
                } else {
                    c = *p & 0x07;
                    k = 4;
                }
                if (p + k > end)
                    break;
                for (int j = 1; j < k; j++)
                    c = (c << 6) | (p[j] & 0x3F);
                p += k;
                if (seq || c > 0xFFFF)
                    continue;
                if (nup == cap) {
                    cap *= 2;
                    up = realloc(up, cap * sizeof(*up));
                    if (!up)
                        break;
                }
                up[nup].unicode = (unsigned short)c;
                up[nup].fontpos = (unsigned short)g;
                nup++;
            }
            p++;
        }
        if (up && nup) {
            memset(&ui, 0, sizeof(ui));
            ud.entry_ct = (unsigned short)nup;
            ud.entries = up;
            if (ioctl(fd, PIO_UNIMAPCLR, &ui) || ioctl(fd, PIO_UNIMAP, &ud))
                fprintf(stderr, "font: unicode map on %s: %s (glyphs loaded anyway)\n", tty, strerror(errno));
        }
        free(up);
    }
    close(fd);
    printf("%ux%u, %u glyphs, %zu unicode entries\n", w, h, cnt, nup);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 4 && !strcmp(argv[1], "font"))
        return cmd_font(argv[2], argv[3]);
    if (argc >= 2 && !strcmp(argv[1], "keyd"))
        return keyd();
    if (argc >= 3 && !strcmp(argv[1], "find"))
        return cmd_find(argv[2]);
    if (argc >= 3 && !strcmp(argv[1], "status"))
        return cmd_status(argv[2]);
    fprintf(stderr, "usage: gk3-fbi keyd | find <PARTUUID> | status <PARTUUID> | font <psf2> <tty>\n");
    return 2;
}
