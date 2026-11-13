/* 主机单测：sparse 流式展开、分区方案、ESP 文本与空间检查。make test 用 ASan/UBSan 跑。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/esp.h"
#include "../src/plan.h"
#include "../src/sparse.h"

static int g_fail, g_run;

#define CHECK(c)                                                              \
    do {                                                                      \
        g_run++;                                                              \
        if (!(c)) {                                                           \
            g_fail++;                                                         \
            fprintf(stderr, "%s:%d: FAIL: %s\n", __FILE__, __LINE__, #c);     \
        }                                                                     \
    } while (0)

/* ── sparse ─────────────────────────────────────────────────────────── */

typedef struct {
    uint8_t *b;
    size_t n, cap;
} vec;

static void put(vec *v, const void *p, size_t n)
{
    if (v->n + n > v->cap) {
        v->cap = (v->n + n) * 2;
        v->b = realloc(v->b, v->cap);
    }
    memcpy(v->b + v->n, p, n);
    v->n += n;
}
static void put16(vec *v, uint16_t x) { uint8_t b[2] = {(uint8_t)x, (uint8_t)(x >> 8)}; put(v, b, 2); }
static void put32(vec *v, uint32_t x)
{
    uint8_t b[4] = {(uint8_t)x, (uint8_t)(x >> 8), (uint8_t)(x >> 16), (uint8_t)(x >> 24)};
    put(v, b, 4);
}

static void file_hdr(vec *v, uint32_t blk, uint32_t blocks, uint32_t chunks, uint16_t fhs, uint16_t chs)
{
    put32(v, GK3W_SPARSE_MAGIC);
    put16(v, 1);
    put16(v, 0);
    put16(v, fhs);
    put16(v, chs);
    put32(v, blk);
    put32(v, blocks);
    put32(v, chunks);
    put32(v, 0);
    for (int i = 28; i < fhs; i++)
        put(v, "\0", 1);
}

static void chunk(vec *v, uint16_t type, uint32_t blocks, uint32_t data_len, uint16_t chs)
{
    put16(v, type);
    put16(v, 0);
    put32(v, blocks);
    put32(v, chs + data_len);
    for (int i = 12; i < chs; i++)
        put(v, "\0", 1);
}

typedef struct {
    uint8_t *img;
    size_t size;
    int writes, fills;
    int unaligned;
} memsink;

static int ms_write(void *ctx, uint64_t off, const uint8_t *d, size_t len)
{
    memsink *m = ctx;
    if (off % 512 || len % 512)
        m->unaligned++;
    if (off + len > m->size)
        return -1;
    memcpy(m->img + off, d, len);
    m->writes++;
    return 0;
}

static int ms_fill(void *ctx, uint64_t off, uint64_t len, uint32_t pat)
{
    memsink *m = ctx;
    if (off % 512 || len % 512)
        m->unaligned++;
    if (off + len > m->size)
        return -1;
    for (uint64_t i = 0; i < len; i += 4)
        memcpy(m->img + off + i, &pat, 4);   /* 主机是小端 */
    m->fills++;
    return 0;
}

/* blk 4096：RAW 2 块 | FILL 1 块 | DONT_CARE 2 块 | CRC32 | RAW 1 块；总 6 块 */
static vec sample(uint16_t fhs, uint16_t chs, uint8_t *expect, size_t exp_size)
{
    vec v = {0};
    uint8_t blk[4096];
    file_hdr(&v, 4096, 6, 5, fhs, chs);
    chunk(&v, 0xCAC1, 2, 8192, chs);
    for (int k = 0; k < 2; k++) {
        for (int i = 0; i < 4096; i++)
            blk[i] = (uint8_t)(i * 7 + k);
        put(&v, blk, 4096);
        memcpy(expect + k * 4096, blk, 4096);
    }
    chunk(&v, 0xCAC2, 1, 4, chs);
    put32(&v, 0x11223344);
    for (int i = 0; i < 4096; i += 4)
        memcpy(expect + 8192 + i, "\x44\x33\x22\x11", 4);
    chunk(&v, 0xCAC3, 2, 0, chs);   /* DONT_CARE：expect 那两块保持 0xEE */
    chunk(&v, 0xCAC4, 0, 4, chs);
    put32(&v, 0xdeadbeef);
    chunk(&v, 0xCAC1, 1, 4096, chs);
    for (int i = 0; i < 4096; i++)
        blk[i] = (uint8_t)(255 - i);
    put(&v, blk, 4096);
    memcpy(expect + 5 * 4096, blk, 4096);
    (void)exp_size;
    return v;
}

static int run_sparse(const vec *v, size_t piece, size_t bufcap, memsink *m, gk3w_sparse *s)
{
    static uint8_t buf[16384] __attribute__((aligned(4096)));
    if (gk3w_sparse_init(s, m->size, buf, bufcap))
        return -1;
    gk3w_sparse_sink sink = {m, ms_write, ms_fill};
    for (size_t off = 0; off < v->n; off += piece) {
        size_t n = v->n - off < piece ? v->n - off : piece;
        if (gk3w_sparse_feed(s, v->b + off, n, &sink))
            return -1;
    }
    return gk3w_sparse_finish(s, &sink);
}

static void test_sparse_ok(void)
{
    static const size_t pieces[] = {1, 3, 12, 4096, 5000, 1 << 20};
    uint16_t hdr_sizes[][2] = {{28, 12}, {32, 16}};
    for (size_t h = 0; h < 2; h++) {
        for (size_t p = 0; p < sizeof pieces / sizeof pieces[0]; p++) {
            uint8_t expect[6 * 4096], img[6 * 4096];
            memset(expect, 0xEE, sizeof expect);
            memset(img, 0xEE, sizeof img);
            vec v = sample(hdr_sizes[h][0], hdr_sizes[h][1], expect, sizeof expect);
            memsink m = {img, sizeof img, 0, 0, 0};
            gk3w_sparse s;
            /* 攒写缓冲 4096：两块的 RAW 会分两次写出；16384：攒成一次 */
            int rc = run_sparse(&v, pieces[p], p % 2 ? 4096 : 16384, &m, &s);
            CHECK(rc == 0);
            if (rc)
                fprintf(stderr, "  err=%s piece=%zu\n", s.err, pieces[p]);
            CHECK(!memcmp(img, expect, sizeof img));
            CHECK(m.unaligned == 0);
            CHECK(m.fills == 1);
            CHECK(gk3w_sparse_total(&s) == 6 * 4096);
            free(v.b);
        }
    }
}

static void test_sparse_bad(void)
{
    uint8_t expect[6 * 4096], img[6 * 4096];
    memsink m = {img, sizeof img, 0, 0, 0};
    gk3w_sparse s;
    vec v;

    /* 截断：最后一个字节不给 */
    v = sample(28, 12, expect, sizeof expect);
    v.n--;
    CHECK(run_sparse(&v, 4096, 4096, &m, &s) == -1 && !strcmp(s.err, "truncated"));
    v.n++;
    /* 尾部多一个字节 */
    put(&v, "x", 1);
    CHECK(run_sparse(&v, 4096, 4096, &m, &s) == -1 && !strcmp(s.err, "trailing-data"));
    free(v.b);
    /* 魔数不对 */
    v = sample(28, 12, expect, sizeof expect);
    v.b[0] ^= 1;
    CHECK(run_sparse(&v, 7, 4096, &m, &s) == -1 && !strcmp(s.err, "bad-magic"));
    free(v.b);
    /* 目标比展开后小 */
    v = sample(28, 12, expect, sizeof expect);
    m.size = 5 * 4096;
    CHECK(run_sparse(&v, 4096, 4096, &m, &s) == -1 && !strcmp(s.err, "too-big"));
    m.size = sizeof img;
    free(v.b);
    /* RAW 的 total_sz 与块数不符 */
    v = (vec){0};
    file_hdr(&v, 4096, 1, 1, 28, 12);
    chunk(&v, 0xCAC1, 1, 4000, 12);
    CHECK(run_sparse(&v, 4096, 4096, &m, &s) == -1 && !strcmp(s.err, "bad-raw-size"));
    free(v.b);
    /* chunk 的块数合计超过头里的 total_blks */
    v = (vec){0};
    file_hdr(&v, 4096, 1, 1, 28, 12);
    chunk(&v, 0xCAC3, 2, 0, 12);
    CHECK(run_sparse(&v, 4096, 4096, &m, &s) == -1 && !strcmp(s.err, "too-many-blocks"));
    free(v.b);
    /* 块数合计不够 total_blks */
    v = (vec){0};
    file_hdr(&v, 4096, 3, 1, 28, 12);
    chunk(&v, 0xCAC3, 2, 0, 12);
    CHECK(run_sparse(&v, 4096, 4096, &m, &s) == -1 && !strcmp(s.err, "block-count"));
    free(v.b);
    /* 不认识的 chunk 类型 */
    v = (vec){0};
    file_hdr(&v, 4096, 1, 1, 28, 12);
    chunk(&v, 0xCAC9, 1, 0, 12);
    CHECK(run_sparse(&v, 4096, 4096, &m, &s) == -1 && !strcmp(s.err, "bad-chunk-type"));
    free(v.b);
    /* 块大小不是 512 的倍数 */
    v = (vec){0};
    file_hdr(&v, 1000, 1, 1, 28, 12);
    CHECK(run_sparse(&v, 4096, 4096, &m, &s) == -1 && !strcmp(s.err, "bad-blk-sz"));
    free(v.b);
    /* 回调失败要传出来 */
    {
        uint8_t ex2[6 * 4096];
        memsink small = {img, 4096, 0, 0, 0};   /* limit 够大但写入越界 → 回调失败 */
        static uint8_t buf[4096] __attribute__((aligned(4096)));
        gk3w_sparse_sink sink = {&small, ms_write, ms_fill};
        v = sample(28, 12, ex2, sizeof ex2);
        CHECK(gk3w_sparse_init(&s, 1 << 20, buf, sizeof buf) == 0);
        CHECK(gk3w_sparse_feed(&s, v.b, v.n, &sink) == -1 && !strcmp(s.err, "write"));
        free(v.b);
    }
    /* 只校验、不写（sink = NULL） */
    {
        static uint8_t buf[4096] __attribute__((aligned(4096)));
        uint8_t ex3[6 * 4096];
        v = sample(28, 12, ex3, sizeof ex3);
        CHECK(gk3w_sparse_init(&s, 1 << 20, buf, sizeof buf) == 0);
        CHECK(gk3w_sparse_feed(&s, v.b, v.n, NULL) == 0);
        CHECK(gk3w_sparse_finish(&s, NULL) == 0);
        free(v.b);
    }
    CHECK(gk3w_sparse_init(&s, 1, img, 1000) == -1);
}

/* ── 分区方案 ───────────────────────────────────────────────────────── */

static void test_plan_fixture(void)
{
    /* installer/live/testdata/windows-setup-shrunk：probe 的 FREE 与 plan-along-rescue-no.txt（shell 版 gk3_plan 录的） */
    static const struct {
        const char *name;
        uint64_t first, last, mib;
    } want[] = {
        {"misc", 789448704, 789456895, 4},          {"metadata", 789456896, 789522431, 32},
        {"boot_a", 789522432, 789653503, 64},       {"boot_b", 789653504, 789784575, 64},
        {"super", 789784576, 814950399, 12288},     {"userdata", 814950400, 957220863, 69468},
    };
    gk3w_plan_req r = {512, {789448704, 957220863}, 0, 0, 0};
    gk3w_plan p;
    CHECK(gk3w_plan_alongside(&r, &p) == GK3W_PLAN_OK);
    CHECK(p.n == 6);
    for (size_t i = 0; i < 6 && i < p.n; i++) {
        CHECK(!strcmp(p.parts[i].name, want[i].name));
        CHECK(p.parts[i].first == want[i].first && p.parts[i].last == want[i].last && p.parts[i].mib == want[i].mib);
    }
    CHECK(p.avail_mib == 81920 && p.fixed_mib == 12452 && p.userdata_mib == 69468);

    /* 带救援分区（plan-along-rescue-yes.txt：gk3rescue 排在 super 之后、userdata 之前） */
    r.rescue = 1;
    CHECK(gk3w_plan_alongside(&r, &p) == GK3W_PLAN_OK);
    CHECK(p.n == 7 && !strcmp(p.parts[5].name, "gk3rescue") && p.parts[5].first == 814950400 &&
          p.parts[5].last == 817047551);
    CHECK(p.parts[6].first == 817047552 && p.parts[6].last == 957220863 && p.userdata_mib == 68444);
}

static void test_plan_edges(void)
{
    gk3w_plan p;
    gk3w_plan_req r = {512, {1000, 1000 + (20644ull << 11)}, 0, 0, 0};
    /* 起点 1000 向上对齐到 2048，丢掉的不到 1 MiB 让可用只剩 20643 MiB，差 1 MiB */
    CHECK(gk3w_plan_alongside(&r, &p) == GK3W_PLAN_ENOSPACE && p.avail_mib == 20643);
    r.region.last += 2048;
    CHECK(gk3w_plan_alongside(&r, &p) == GK3W_PLAN_OK);
    CHECK(p.parts[0].first == 2048 && p.avail_mib == 20644);
    /* 差 1 MiB 不够 */
    r.region.last = 2048 + (20644ull << 11) - 2;
    CHECK(gk3w_plan_alongside(&r, &p) == GK3W_PLAN_ENOSPACE && p.need_mib == 20644 && p.avail_mib == 20643);
    /* 正好够 */
    r.region.last = 2048 + (20644ull << 11) - 1;
    CHECK(gk3w_plan_alongside(&r, &p) == GK3W_PLAN_OK && p.userdata_mib == 8192);
    CHECK(p.parts[p.n - 1].last == r.region.last);
    /* 指定 userdata */
    r.region.last = 2048 + (40000ull << 11) - 1;
    r.userdata_mib = 8191;
    CHECK(gk3w_plan_alongside(&r, &p) == GK3W_PLAN_EUD_SMALL);
    r.userdata_mib = 40000;
    CHECK(gk3w_plan_alongside(&r, &p) == GK3W_PLAN_EUD_BIG);
    r.userdata_mib = 10000;
    CHECK(gk3w_plan_alongside(&r, &p) == GK3W_PLAN_OK && p.parts[p.n - 1].mib == 10000);
    r.userdata_mib = 0;
    /* 4Kn：1 MiB = 256 扇区 */
    {
        gk3w_plan_req q = {4096, {6, 6 + (20645ull << 8)}, 0, 0, 0};
        CHECK(gk3w_plan_alongside(&q, &p) == GK3W_PLAN_OK);
        CHECK(p.parts[0].first == 256 && p.parts[0].last == 256 + 4 * 256 - 1);
        CHECK(p.parts[4].mib == 12288 && p.parts[4].last - p.parts[4].first + 1 == 12288ull * 256);
    }
    /* 参数不对、区间顶到 UINT64_MAX 不溢出 */
    r.sector = 1000;
    CHECK(gk3w_plan_alongside(&r, &p) == GK3W_PLAN_EINVAL);
    r.sector = 512;
    r.region = (gk3w_span){5, 4};
    CHECK(gk3w_plan_alongside(&r, &p) == GK3W_PLAN_EINVAL);
    r.region = (gk3w_span){UINT64_MAX - 10, UINT64_MAX};
    CHECK(gk3w_plan_alongside(&r, &p) == GK3W_PLAN_EINVAL);
    r.region = (gk3w_span){UINT64_MAX - (40000ull << 11), UINT64_MAX};
    CHECK(gk3w_plan_alongside(&r, &p) == GK3W_PLAN_OK && p.parts[p.n - 1].last <= UINT64_MAX);
}

/* ── ESP ────────────────────────────────────────────────────────────── */

static void test_esp(void)
{
    char out[1024];
    const char *mid = "0123456789abcdef0123456789abcdef";
    const char *want =
        "title      crDroid 16.0 (gaokun3) \xe2\x80\x94 slot _b\n"
        "version    gaokun3-slot-b\n"
        "sort-key   zandroidb\n"
        "options    console=tty0 quiet androidboot.slot_suffix=_b\n"
        "linux      /0123456789abcdef0123456789abcdef/android/slot_b/Image\n"
        "devicetree /0123456789abcdef0123456789abcdef/android/slot_b/gaokun3.dtb\n"
        "initrd     /0123456789abcdef0123456789abcdef/android/slot_b/ramdisk.img\n";
    CHECK(gk3w_esp_android_entry(out, sizeof out, mid, 'b', "console=tty0 quiet") == (int)strlen(want));
    CHECK(!strcmp(out, want));
    CHECK(gk3w_esp_android_entry(out, 20, mid, 'a', "x") == -1);
    CHECK(gk3w_esp_android_entry(out, sizeof out, mid, 'c', "x") == -1);
    CHECK(gk3w_esp_android_entry(out, sizeof out, "0123456789ABCDEF0123456789abcdef", 'a', "x") == -1);
    CHECK(gk3w_is_machine_id(mid) && !gk3w_is_machine_id("0123") && !gk3w_is_machine_id(""));
    CHECK(!strcmp(gk3w_loader_conf, "timeout 5\nconsole-mode keep\neditor no\ndefault *-android-a.conf\n"));

    /* 空间检查：出厂 300 MiB 的 ESP，Windows 占了约 100 MiB */
    gk3w_esp_space sp = {300, 199 * 1024, 2 * 1024, 46 * 1024, 1, 0};
    CHECK(gk3w_esp_space_check(&sp) == GK3W_ESP_OK);
    sp.esp_size_mib = 100;
    CHECK(gk3w_esp_space_check(&sp) == GK3W_ESP_TOO_SMALL);
    sp.esp_size_mib = 300;
    sp.need_kib = 200 * 1024;
    CHECK(gk3w_esp_space_check(&sp) == GK3W_ESP_FULL);
    /* 装完剩下的 + 一个槽 ≤ 56 MiB：OTA 放不下 */
    sp = (gk3w_esp_space){300, 100 * 1024, 90 * 1024, 46 * 1024, 0, 0};
    CHECK(gk3w_esp_space_check(&sp) == GK3W_ESP_OTA_ROOM);
    /* 有 Windows：扣掉一个槽和一版入口后不到 32 MiB */
    sp = (gk3w_esp_space){300, 160 * 1024, 80 * 1024, 46 * 1024, 1, 0};
    CHECK(gk3w_esp_space_check(&sp) == GK3W_ESP_WIN_RESERVE);
    sp.windows = 0;
    CHECK(gk3w_esp_space_check(&sp) == GK3W_ESP_OK);
    /* 空闲不到 150 MiB */
    sp = (gk3w_esp_space){300, 149 * 1024, 1024, 10 * 1024, 0, 0};
    CHECK(gk3w_esp_space_check(&sp) == GK3W_ESP_FULL_MIN && !strcmp(gk3w_esp_check_code(GK3W_ESP_FULL_MIN), "esp-full"));
}

static void test_plan_whole_reinstall(void)
{
    /* 整盘清空：64 GiB 的 512 盘，新表的 usable 区 [34, total-34]，ESP 从 2048 起 300 MiB，userdata 吃掉剩下的 */
    uint64_t total = 64ull << 21;
    gk3w_plan_req r = {512, {34, total - 34}, 0, 0, 1};
    gk3w_plan p;
    gk3w_reinstall ri;
    gk3w_part_ref parts[8];
    CHECK(gk3w_plan_alongside(&r, &p) == GK3W_PLAN_OK);
    CHECK(p.n == 7 && !strcmp(p.parts[0].name, "esp") && p.parts[0].first == 2048 && p.parts[0].mib == 300);
    CHECK(!strcmp(p.parts[1].name, "misc") && p.parts[1].first == 2048 + 300 * 2048);
    CHECK(!strcmp(p.parts[6].name, "userdata") && p.parts[6].last <= total - 34);
    /* 与 installer-lib.sh 相同：盘按整 MiB 算，首尾各让出 1 MiB */
    CHECK(p.avail_mib == (64u << 10) - 2 && p.fixed_mib == 300 + 4 + 32 + 128 + 12288);
    CHECK(p.userdata_mib == p.avail_mib - p.fixed_mib);

    /* 重新安装：按名字认出一套，misc 只有 1007 KiB 也行 */
    memset(parts, 0, sizeof parts);
    {
        static const char *const nm[] = {"esp", "misc", "boot_a", "boot_b", "metadata", "super", "userdata", "x"};
        static const uint64_t sz[] = {300 * 2048, 2014, 64 * 2048, 64 * 2048, 32 * 2048, 12288 * 2048, 9000 * 2048, 2048};
        uint64_t cur = 34;
        for (int i = 0; i < 8; i++) {
            snprintf(parts[i].name, sizeof parts[i].name, "%s", nm[i]);
            parts[i].idx = (uint32_t)i + 1;
            parts[i].first = cur;
            parts[i].last = cur + sz[i] - 1;
            cur += sz[i];
        }
    }
    CHECK(gk3w_plan_reinstall(512, parts, 8, 0, &ri) == GK3W_PLAN_OK);
    CHECK(ri.p[0].idx == 2 && ri.kib[0] == 1007 && ri.act[0] == GK3W_REUSE_WRITE);
    CHECK(ri.act[1] == GK3W_REUSE_FORMAT && ri.act[5] == GK3W_REUSE_FORMAT && ri.userdata_mib == 9000);
    CHECK(ri.fixed_mib == 0 + 32 + 64 + 64 + 12288);
    CHECK(gk3w_plan_reinstall(512, parts, 8, 1, &ri) == GK3W_PLAN_OK && ri.act[1] == GK3W_REUSE_KEEP && ri.act[5] == GK3W_REUSE_KEEP);
    /* 太小、缺、重名 */
    parts[5].last = parts[5].first + 12287 * 2048 - 1;
    CHECK(gk3w_plan_reinstall(512, parts, 8, 0, &ri) == GK3W_PLAN_SMALL && !strcmp(ri.bad, "super") && ri.need_kib == 12288 * 1024);
    parts[5].last = parts[5].first + 12288 * 2048 - 1;
    snprintf(parts[7].name, sizeof parts[7].name, "boot_a");
    CHECK(gk3w_plan_reinstall(512, parts, 8, 0, &ri) == GK3W_PLAN_DUP && !strcmp(ri.bad, "boot_a"));
    snprintf(parts[7].name, sizeof parts[7].name, "x");
    snprintf(parts[4].name, sizeof parts[4].name, "meta");
    snprintf(parts[6].name, sizeof parts[6].name, "data");
    CHECK(gk3w_plan_reinstall(512, parts, 8, 0, &ri) == GK3W_PLAN_MISSING && !strcmp(ri.bad, "metadata,userdata"));

    /* 重新安装的 ESP：100 MiB 的也不拦，空闲下限 16 MiB */
    {
        gk3w_esp_space sp = {100, 17 * 1024, 1024, 60 * 1024, 0, 1};
        CHECK(gk3w_esp_space_check(&sp) == GK3W_ESP_OK);
        sp.free_kib = 15 * 1024;
        CHECK(gk3w_esp_space_check(&sp) == GK3W_ESP_FULL_MIN);
        sp.reinstall = 0;
        sp.free_kib = 149 * 1024;
        CHECK(gk3w_esp_space_check(&sp) == GK3W_ESP_TOO_SMALL);
    }
}

int main(void)
{
    test_plan_whole_reinstall();
    test_sparse_ok();
    test_sparse_bad();
    test_plan_fixture();
    test_plan_edges();
    test_esp();
    printf("stage 2: %d checks, %d failed\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}
