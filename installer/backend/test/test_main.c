/* 主机单测：协议编码、空闲区计算（与平台无关的部分）。make test 用 ASan/UBSan 跑。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/layout.h"
#include "../src/proto.h"

static int g_fail, g_run;

#define CHECK(c)                                                              \
    do {                                                                      \
        g_run++;                                                              \
        if (!(c)) {                                                           \
            g_fail++;                                                         \
            fprintf(stderr, "%s:%d: 失败：%s\n", __FILE__, __LINE__, #c);     \
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

int main(void)
{
    test_enc();
    test_line();
    test_free_factory();
    test_free_middle();
    test_free_edges();
    printf("%d 项检查，失败 %d 项\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}
