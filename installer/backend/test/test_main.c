/* 主机单测：协议编码、空闲区计算、启动项名字与分区内容的识别（与平台无关的部分）。make test 用 ASan/UBSan 跑。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/esp.h"
#include "../src/layout.h"
#include "../src/proto.h"

static int g_fail, g_run;

#define CHECK(c)                                                              \
    do {                                                                      \
        g_run++;                                                              \
        if (!(c)) {                                                           \
            g_fail++;                                                         \
            fprintf(stderr, "%s:%d: FAIL: %s\n", __FILE__, __LINE__, #c);     \
        }                                                                     \
    } while (0)

static void test_enc(void)
{
    char out[64];
    CHECK(gk3w_enc("Basic data partition", out, sizeof out) == 24);
    CHECK(!strcmp(out, "Basic%20data%20partition"));
    gk3w_enc("100%", out, sizeof out);
    CHECK(!strcmp(out, "100%25"));
    gk3w_enc("a\nb\rc\td", out, sizeof out);
    CHECK(!strcmp(out, "a%0Ab%0Dc%09d"));
    gk3w_enc("数据", out, sizeof out);   /* UTF-8 原样透过 */
    CHECK(!strcmp(out, "数据"));
    gk3w_enc("k=v", out, sizeof out);    /* 前端按第一个 = 切键名，值里的 = 不用编码 */
    CHECK(!strcmp(out, "k=v"));
    /* 截断时仍以 NUL 结尾，返回值是完整长度 */
    CHECK(gk3w_enc("a b", out, 3) == 5);
    CHECK(!strcmp(out, "a%"));
}

static void test_line(void)
{
    gk3w_line l = {0};
    char buf[256] = "";
    FILE *f = tmpfile();
    CHECK(f != NULL);
    if (!f)
        return;
    gk3w_line_begin(&l, "PART");
    gk3w_line_kv(&l, "name", "EFI system partition");
    gk3w_line_u64(&l, "start", 2048);
    gk3w_line_hex64(&l, "attrs", 0x8000000000000001ull);
    CHECK(gk3w_line_emit(&l, f) == 0);
    rewind(f);
    CHECK(fgets(buf, sizeof buf, f) != NULL);
    CHECK(!strcmp(buf, "PART name=EFI%20system%20partition start=2048 attrs=0x8000000000000001\n"));
    fclose(f);
    gk3w_line_free(&l);
}

/* 出厂布局（installer/live/testdata/factory/probe.txt 的内置盘；usable 区间按 512 GB 盘 1000215216 扇区算） */
static const gk3w_span factory[] = {
    {2048, 616447},          {616448, 649215},         {649216, 252307455},     {252307456, 958269439},
    {958269440, 960366591},  {960366592, 998115327},   {998115328, 1000214494},
};
#define UFIRST 34
#define ULAST (1000215216ull - 34)

static void test_free_factory(void)
{
    gk3w_span out[8];
    size_t n = 99;
    /* 开头 34..2047 与结尾那段都不到 16 MiB：一段都不报 */
    CHECK(gk3w_free_spans(UFIRST, ULAST, factory, 7, 512, 16u << 20, out, 8, &n) == GK3W_LAYOUT_OK);
    CHECK(n == 0);
    /* 门槛设成 0：两段缝隙都报出来 */
    CHECK(gk3w_free_spans(UFIRST, ULAST, factory, 7, 512, 0, out, 8, &n) == GK3W_LAYOUT_OK);
    CHECK(n == 2);
    CHECK(out[0].first == 34 && out[0].last == 2047);
    CHECK(out[1].first == 1000214495 && out[1].last == ULAST);
}

static void test_free_middle(void)
{
    /* Data 缩小 40 GiB：空闲区夹在 Data 与 WINPE 之间；输入顺序打乱，结果按起点排序 */
    gk3w_span parts[7];
    gk3w_span out[4];
    size_t n = 0;
    memcpy(parts, factory, sizeof parts);
    parts[3].last -= 40ull * 2048 * 1024;
    gk3w_span t = parts[0];
    parts[0] = parts[5];
    parts[5] = t;
    CHECK(gk3w_free_spans(UFIRST, ULAST, parts, 7, 512, 16u << 20, out, 4, &n) == GK3W_LAYOUT_OK);
    CHECK(n == 1);
    CHECK(out[0].first == parts[3].last + 1 && out[0].last == 958269439);
    CHECK(gk3w_span_bytes(out[0], 512) == 40ull << 30);
}

static void test_free_edges(void)
{
    gk3w_span out[4];
    size_t n = 0;
    gk3w_span bad[2] = {{100, 50}, {200, 300}};
    gk3w_span overlap[2] = {{100, 200}, {150, 300}};
    gk3w_span outside[1] = {{10, 2000}};
    gk3w_span full[1] = {{34, 999}};
    gk3w_span tail[1] = {{500, 999}};

    /* 空盘：整个 usable 区间 */
    CHECK(gk3w_free_spans(34, 999, NULL, 0, 512, 0, out, 4, &n) == GK3W_LAYOUT_OK);
    CHECK(n == 1 && out[0].first == 34 && out[0].last == 999);
    CHECK(gk3w_free_spans(34, 999, bad, 2, 512, 0, out, 4, &n) == GK3W_LAYOUT_EBADPART);
    CHECK(gk3w_free_spans(34, 999, overlap, 2, 512, 0, out, 4, &n) == GK3W_LAYOUT_EOVERLAP);
    CHECK(gk3w_free_spans(34, 999, outside, 1, 512, 0, out, 4, &n) == GK3W_LAYOUT_EBADPART);
    CHECK(gk3w_free_spans(34, 999, full, 1, 512, 0, out, 4, &n) == GK3W_LAYOUT_OK && n == 0);
    CHECK(gk3w_free_spans(34, 999, tail, 1, 512, 0, out, 4, &n) == GK3W_LAYOUT_OK);
    CHECK(n == 1 && out[0].first == 34 && out[0].last == 499);
    CHECK(gk3w_free_spans(999, 34, NULL, 0, 512, 0, out, 4, &n) == GK3W_LAYOUT_EINVAL);
    CHECK(gk3w_free_spans(34, 999, NULL, 0, 0, 0, out, 4, &n) == GK3W_LAYOUT_EINVAL);
    /* cap 不够：n 报全数，只写前 cap 段 */
    {
        gk3w_span three[2] = {{100, 199}, {300, 399}};
        CHECK(gk3w_free_spans(34, 999, three, 2, 512, 0, out, 1, &n) == GK3W_LAYOUT_OK);
        CHECK(n == 3 && out[0].first == 34 && out[0].last == 99);
    }
    /* usable 区间顶到 UINT64_MAX 也不溢出 */
    {
        gk3w_span top[1] = {{UINT64_MAX - 10, UINT64_MAX}};
        CHECK(gk3w_free_spans(UINT64_MAX - 100, UINT64_MAX, top, 1, 512, 0, out, 4, &n) == GK3W_LAYOUT_OK);
        CHECK(n == 1 && out[0].last == UINT64_MAX - 11);
    }
    CHECK(gk3w_span_bytes((gk3w_span){0, UINT64_MAX}, 512) == UINT64_MAX);
    CHECK(gk3w_span_bytes((gk3w_span){5, 4}, 512) == 0);
}

static void test_entry_names(void)
{
    static const char *const ours[] = {
        "0123456789abcdef0123456789abcdef-android-a.conf", "0123456789ABCDEF0123456789abcdef-Android-B.conf",
        "0123456789abcdef0123456789abcdef-rescue.conf", "0123456789abcdef0123456789abcdef-recovery-b.conf",
        "0123456789abcdef0123456789abcdef-android-a+3.conf", "0123456789abcdef0123456789abcdef-android-a+0-3.conf",
        "0123456789abcdef0123456789abcdef-android-b.conf.disabled", "gk3boot-android-a.conf", "gk3-windows.conf",
        "gk3prev-android-b+2-1.conf", "gaokun3-live.conf", "gaokun3-live-2.conf", "GAOKUN3-LIVE-10.CONF",
    };
    static const char *const theirs[] = {
        "arch.conf", "0123456789abcdef0123456789abcdef-6.12.0.conf", "0123456789abcdef0123456789abcde-android-a.conf",
        "0123456789abcdef0123456789abcdeg-android-a.conf", "0123456789abcdef0123456789abcdef-android-c.conf",
        "0123456789abcdef0123456789abcdef-android-a", "gaokun3-live-x.conf", "gaokun3-livecd.conf", "gk3.efi",
        "0123456789abcdef0123456789abcdef-android-a+.conf", "0123456789abcdef0123456789abcdef-android-a+1-.conf",
        "", ".conf", "fedora-android-a.conf",
    };
    for (size_t i = 0; i < sizeof ours / sizeof ours[0]; i++)
        CHECK(gk3w_our_entry_name(ours[i]));
    for (size_t i = 0; i < sizeof theirs / sizeof theirs[0]; i++)
        CHECK(!gk3w_our_entry_name(theirs[i]));
    CHECK(gk3w_our_var_value("auto-windows") && gk3w_our_var_value("AUTO-Windows") && gk3w_our_var_value("gk3-windows.conf"));
    CHECK(gk3w_our_var_value("*-android-a.conf") && gk3w_our_var_value("0123456789abcdef0123456789abcdef-android-b.conf"));
    CHECK(gk3w_our_var_value("gk3boot-tools.conf"));
    CHECK(!gk3w_our_var_value("arch.conf") && !gk3w_our_var_value("auto-windows ") && !gk3w_our_var_value("") &&
          !gk3w_our_var_value("x/-android-a.conf") && !gk3w_our_var_value("@saved"));
}

/* 拼一段版本资源：每个 key 后面补 pad 个 0 字节再接值 */
static size_t put_vs(uint8_t *b, const char *key, const char *val, int pad)
{
    size_t n = 0;
    for (const char *p = key;; p++) {
        b[n++] = (uint8_t)*p, b[n++] = 0;
        if (!*p)
            break;
    }
    for (int i = 0; i < pad; i++)
        b[n++] = 0;
    for (const char *p = val;; p++) {
        b[n++] = (uint8_t)*p, b[n++] = 0;
        if (!*p)
            break;
    }
    return n;
}

static void test_windows_boot_manager(void)
{
    static uint8_t b[512];
    size_t n;
    memset(b, 0x5a, sizeof b);
    n = 16 + put_vs(b + 16, "CompanyName", "Microsoft Corporation", 0);
    n += put_vs(b + n, "FileDescription", "Boot Manager", 2);
    CHECK(gk3w_is_windows_boot_manager(b, n));
    CHECK(!gk3w_is_windows_boot_manager(b, n - 2));                 /* 值的结尾 NUL 被截掉 */
    memset(b, 0x5a, sizeof b);
    n = put_vs(b, "CompanyName", "Microsoft Corporation", 2);
    n += put_vs(b + n, "FileDescription", "Windows Boot Manager", 0);   /* 不是 bootmgfw 的描述 */
    CHECK(!gk3w_is_windows_boot_manager(b, n));
    memset(b, 0x5a, sizeof b);
    n = put_vs(b, "CompanyName", "Someone Else", 0);
    n += put_vs(b + n, "FileDescription", "Boot Manager", 0);
    CHECK(!gk3w_is_windows_boot_manager(b, n));
    memset(b, 0x5a, sizeof b);
    n = put_vs(b, "CompanyName", "Microsoft Corporation", 4);         /* 补 4 个字节不是合法的对齐 */
    n += put_vs(b + n, "FileDescription", "Boot Manager", 0);
    CHECK(!gk3w_is_windows_boot_manager(b, n));
    CHECK(!gk3w_is_windows_boot_manager(b, 0));
    {
        static const char sd[] = "xx#### LoaderInfo: systemd-boot 257.13-1~deb13u1 ####yy";
        CHECK(gk3w_is_systemd_boot((const uint8_t *)sd, sizeof sd - 1));
        CHECK(!gk3w_is_systemd_boot((const uint8_t *)sd, 30));       /* 标记被截断 */
        CHECK(!gk3w_is_systemd_boot((const uint8_t *)"#### LoaderInfo: systemd-stub 257 ####", 38));
    }
}

static void test_fallback_decide(void)
{
    /* 假文件：W = Windows 启动管理器（当前版本），W0 = 较旧的 Windows 启动管理器，SD = systemd-boot，
     * SD2 = 另一版 systemd-boot，G = 别的系统的引导程序（GRUB） */
    static uint8_t W[256], W0[256], SD[64], SD2[64], G[32];
    size_t n;
    memset(W, 1, sizeof W);
    n = put_vs(W, "CompanyName", "Microsoft Corporation", 0);
    put_vs(W + n, "FileDescription", "Boot Manager", 2);
    memcpy(W0, W, sizeof W);
    W0[255] = 2;
    memset(SD, 3, sizeof SD);
    memcpy(SD + 4, "#### LoaderInfo: systemd-boot 257 ####", 38);
    memcpy(SD2, SD, sizeof SD);
    SD2[63] = 4;
    memset(G, 5, sizeof G);
#define FB(c, w, b) gk3w_fallback_decide(c, (c) ? sizeof(c) : 0, w, (w) ? sizeof(w) : 0, b, (b) ? sizeof(b) : 0)
    static const uint8_t *const N = NULL;
    /* 正常装过：回落路径是 systemd-boot */
    CHECK(FB(SD, W, W) == GK3W_FB_USE_W);           /* 备份是装机那天的 Windows */
    CHECK(FB(SD, W, W0) == GK3W_FB_USE_W);          /* Windows 之后更新过：用当前的，不用旧拷贝 */
    CHECK(FB(SD, N, W0) == GK3W_FB_USE_BAK);        /* 没有 bootmgfw.efi：用备份 */
    CHECK(FB(SD, W, G) == GK3W_FB_RESTORE_ORIGINAL);/* 装之前是 GRUB：换回它 */
    CHECK(FB(SD, N, G) == GK3W_FB_RESTORE_ORIGINAL);
    CHECK(FB(SD, W, N) == GK3W_FB_USE_W);           /* 装之前那里没有文件 */
    CHECK(FB(SD, N, N) == GK3W_FB_NO_WINDOWS);
    CHECK(FB(SD, W, SD2) == GK3W_FB_USE_W);         /* 备份是我们早先的 systemd-boot：当作没有 */
    CHECK(FB(SD, N, SD2) == GK3W_FB_NO_WINDOWS);
    /* 装完之后别的系统换了回落路径 */
    CHECK(FB(G, W, W) == GK3W_FB_LEAVE_OTHER);
    CHECK(FB(G, W, SD2) == GK3W_FB_LEAVE_OTHER);
    CHECK(FB(G, W, N) == GK3W_FB_UNTOUCHED);         /* 没有备份：我们没动过回落路径 */
    CHECK(FB(G, N, N) == GK3W_FB_UNTOUCHED);
    /* apply 留了备份、还没写回落路径就中断：cur 就是备份 */
    CHECK(FB(G, W, G) == GK3W_FB_RESTORE_ORIGINAL); /* 内容相同，调用方不会再写 */
    CHECK(FB(W, W, W) == GK3W_FB_USE_W);
    /* 重跑：第 1 步已经做完 */
    CHECK(FB(W, W, W0) == GK3W_FB_USE_W);           /* 已是 W，调用方不会再写 */
    CHECK(FB(W, W, N) == GK3W_FB_UNTOUCHED);        /* 备份也删了 */
    CHECK(FB(W0, W, N) == GK3W_FB_UNTOUCHED);       /* 旧的 Windows 启动管理器：不是我们放的，不动 */
    /* 回落路径不存在 */
    CHECK(FB(N, W, N) == GK3W_FB_UNTOUCHED);
    CHECK(FB(N, W, W0) == GK3W_FB_USE_W);
    CHECK(FB(N, W, G) == GK3W_FB_RESTORE_ORIGINAL);
    CHECK(FB(N, N, N) == GK3W_FB_UNTOUCHED);
#undef FB
}

static void test_content_kind(void)
{
    static uint8_t b[8192];
    memset(b, 0, sizeof b);
    CHECK(gk3w_content_kind_of(b, sizeof b) == GK3W_KIND_EMPTY);
    memcpy(b + 3, "NTFS    ", 8);
    CHECK(gk3w_content_kind_of(b, sizeof b) == GK3W_KIND_NTFS);
    memcpy(b + 3, "-FVE-FS-", 8);
    CHECK(gk3w_content_kind_of(b, sizeof b) == GK3W_KIND_BITLOCKER);
    memcpy(b + 3, "EXFAT   ", 8);
    CHECK(gk3w_content_kind_of(b, sizeof b) == GK3W_KIND_EXFAT);
    memset(b, 0, sizeof b);
    memcpy(b + 82, "FAT32   ", 8);
    CHECK(gk3w_content_kind_of(b, sizeof b) == GK3W_KIND_FAT);
    memset(b, 0, sizeof b);
    memcpy(b + 54, "FAT16   ", 8);
    CHECK(gk3w_content_kind_of(b, sizeof b) == GK3W_KIND_FAT);
    memset(b, 0, sizeof b);
    memcpy(b, "ANDROID!", 8);
    CHECK(gk3w_content_kind_of(b, sizeof b) == GK3W_KIND_ANDROID_BOOT);
    memset(b, 0, sizeof b);
    b[1080] = 0x53;
    b[1081] = 0xEF;
    CHECK(gk3w_content_kind_of(b, sizeof b) == GK3W_KIND_EXT4);
    CHECK(gk3w_content_kind_of(b, 1081) == GK3W_KIND_UNKNOWN);
    memset(b, 0, sizeof b);
    b[4096] = 0x67, b[4097] = 0x44, b[4098] = 0x6c, b[4099] = 0x61;
    CHECK(gk3w_content_kind_of(b, sizeof b) == GK3W_KIND_LP);
    b[4099] = 0;
    CHECK(gk3w_content_kind_of(b, sizeof b) == GK3W_KIND_UNKNOWN);
    CHECK(!strcmp(gk3w_content_kind_name(GK3W_KIND_LP), "lp") && !strcmp(gk3w_content_kind_name((gk3w_content_kind)99), "unknown"));
}

int main(void)
{
    test_entry_names();
    test_content_kind();
    test_windows_boot_manager();
    test_fallback_decide();
    test_enc();
    test_line();
    test_free_factory();
    test_free_middle();
    test_free_edges();
    printf("%d checks, %d failed\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}
