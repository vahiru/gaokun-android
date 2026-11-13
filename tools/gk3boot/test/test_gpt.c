/* GPT：实机主表（2026-10-05 只读 dd 出的 LBA 0–33）+ 在它上面造的坏表 + 按安装器布局生成的 4K 扇区盘。
 * 测试侧重算 CRC 用 zlib，不用被测的 gk3_crc32。 */
#include <zlib.h>
#include "t.h"

#define BS 512
static const char *const six[] = {"misc", "boot_a", "boot_b", "super", "userdata", "metadata"};

static void reseal(unsigned char *disk, unsigned bs)
{
    unsigned char *h = disk + bs;
    uint32_t n = gk3_le32(h + 80), es = gk3_le32(h + 84);
    uint64_t el = gk3_le64(h + 72);
    if (el >= 2 && (uint64_t)n * es <= 16384 && el * bs + (uint64_t)n * es <= 34 * 4096)   /* 坏头里的天文数字不去算 */
        gk3_put_le32(h + 88, (uint32_t)crc32(0L, disk + el * bs, n * es));
    gk3_put_le32(h + 16, 0);
    gk3_put_le32(h + 16, (uint32_t)crc32(0L, h, gk3_le32(h + 12)));
}

static void set_name(unsigned char *ent, const char *name)
{
    memset(ent + 56, 0, 72);
    for (int i = 0; name[i] && i < 36; i++)
        gk3_put_le16(ent + 56 + 2 * i, (uint8_t)name[i]);
}

static gk3_err parse(const unsigned char *disk, unsigned bs, gk3_gpt *g)
{
    gk3_err e = gk3_gpt_parse_header(disk + bs, bs, g);
    if (e)
        return e;
    return gk3_gpt_parse_mem(disk + bs, bs, disk + g->entries_lba * bs,
                             (size_t)g->num_entries * g->entry_size, g);
}

/* 按安装器的命名（安装器写的 PARTLABEL；hw-inventory §8quinquies）生成一张盘头 */
static unsigned char *build(unsigned bs, uint64_t nblocks, size_t *len)
{
    static const struct { const char *name; uint64_t first_mib, size_mib; } parts[] = {
        {"esp", 1, 300}, {"userdata", 301, 9000}, {"ubunturescue", 9301, 1000},
        {"misc", 10301, 4}, {"boot_a", 10305, 64}, {"boot_b", 10369, 64},
        {"super", 10433, 12288}, {"metadata", 22721, 32},
    };
    size_t entries_blocks = 128 * 128 / bs, n = 2 + entries_blocks;
    unsigned char *d = calloc(n, bs), *h = d + bs;
    uint8_t type_linux[16] = {0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47,
                              0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4};
    memcpy(h, "EFI PART", 8);
    gk3_put_le32(h + 8, 0x00010000);
    gk3_put_le32(h + 12, 92);
    gk3_put_le64(h + 24, 1);
    gk3_put_le64(h + 32, nblocks - 1);
    gk3_put_le64(h + 40, 2 + entries_blocks);
    gk3_put_le64(h + 48, nblocks - 2 - entries_blocks);
    memset(h + 56, 0x5a, 16);
    gk3_put_le64(h + 72, 2);
    gk3_put_le32(h + 80, 128);
    gk3_put_le32(h + 84, 128);
    for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        unsigned char *e = d + 2 * bs + i * 128;
        uint64_t per_mib = (1u << 20) / bs;
        memcpy(e, type_linux, 16);
        memset(e + 16, (int)(i + 1), 16);
        gk3_put_le64(e + 32, parts[i].first_mib * per_mib);
        gk3_put_le64(e + 40, (parts[i].first_mib + parts[i].size_mib) * per_mib - 1);
        set_name(e, parts[i].name);
    }
    reseal(d, bs);
    *len = n * bs;
    return d;
}

static void real_disk(void)
{
    size_t len;
    unsigned char *d = t_vector("gpt-primary-20261005.bin", &len), *m;
    gk3_gpt g;
    gk3_gpt_part p;
    char guid[37];
    const char *bad = NULL;
    if (!d)
        return;
    CHECK_EQ(len, 34 * BS);
    CHECK_EQ(parse(d, BS, &g), GK3_OK);
    CHECK_EQ(g.num_entries, 128);
    CHECK_EQ(g.entry_size, 128);
    CHECK_EQ(g.first_usable, 34);
    CHECK_EQ(g.last_usable, 1000215182);
    CHECK_EQ(g.alt_lba, 1000215215);
    CHECK_EQ(gk3_gpt_count(&g), 8);
    gk3_guid_str(g.disk_guid, guid);
    CHECK_STR(guid, "e6c13d1a-e678-468a-b6b6-b79f19efb0e5");
    CHECK_EQ(gk3_gpt_require_unique(&g, six, 6, &bad), GK3_OK);

    /* 实机：misc 就坐在 GPT 表后面的 LBA 34–2047（1007 KiB） */
    CHECK_EQ(gk3_gpt_find(&g, "misc", &p), GK3_OK);
    CHECK_EQ(p.index, 4);
    CHECK_EQ(p.first_lba, 34);
    CHECK_EQ(p.last_lba, 2047);
    gk3_guid_str(p.part_guid, guid);
    CHECK_STR(guid, "53912ab2-33ac-49d2-a099-94fed8664a26");
    gk3_guid_str(p.type_guid, guid);
    CHECK_STR(guid, "0fc63daf-8483-4772-8e79-3d69d8477de4");
    CHECK_EQ(gk3_gpt_find(&g, "boot_a", &p), GK3_OK);
    CHECK_EQ(p.first_lba, 814344192);
    CHECK_EQ(p.attrs, 1ull << 54);   /* 实机上有 ABL 的 PART_ATT_SUCCESS_BIT（谁写的待查）；入口不看属性位 */
    CHECK_EQ(gk3_gpt_find(&g, "boot_b", &p), GK3_OK);
    CHECK_EQ(p.attrs, 0);
    CHECK_EQ(gk3_gpt_find(&g, "esp", &p), GK3_OK);
    CHECK_EQ(p.index, 1);
    CHECK_EQ(gk3_gpt_find(&g, "metadata", &p), GK3_OK);
    CHECK_EQ(p.index, 10);
    CHECK_EQ(gk3_gpt_find(&g, "recovery", &p), GK3_ENOENT);
    CHECK_EQ(gk3_gpt_find(&g, "Misc", &p), GK3_ENOENT);       /* 大小写敏感 */
    CHECK_EQ(gk3_gpt_find(&g, "mis", &p), GK3_ENOENT);        /* 不做前缀匹配 */
    CHECK_EQ(gk3_gpt_get(&g, 6, &p), GK3_ENOENT);             /* p7 已删 */
    CHECK_EQ(gk3_gpt_get(&g, 128, &p), GK3_ERANGE);

    /* —— 在实机表上造坏表 —— */
    m = malloc(len);
#define FRESH() memcpy(m, d, len)
    FRESH();
    set_name(m + 2 * BS + 5 * 128, "boot_a");                 /* boot_b 改名成 boot_a → 重名 */
    reseal(m, BS);
    CHECK_EQ(parse(m, BS, &g), GK3_OK);
    CHECK_EQ(gk3_gpt_find(&g, "boot_a", &p), GK3_EDUP);
    CHECK_EQ(gk3_gpt_require_unique(&g, six, 6, &bad), GK3_EDUP);
    CHECK(bad && strcmp(bad, "boot_a") == 0, "bad = %s", bad ? bad : "(null)");

    FRESH();
    set_name(m + 2 * BS + 3 * 128, "misc2");                  /* misc 不见了 */
    reseal(m, BS);
    CHECK_EQ(parse(m, BS, &g), GK3_OK);
    CHECK_EQ(gk3_gpt_require_unique(&g, six, 6, &bad), GK3_ENOENT);
    CHECK(bad && strcmp(bad, "misc") == 0, "bad = %s", bad ? bad : "(null)");

    FRESH();
    m[2 * BS + 3 * 128 + 56] = 'M';                           /* 改表项不重算 → 表 CRC 坏 */
    CHECK_EQ(parse(m, BS, &g), GK3_ECRC);

    FRESH();
    m[BS + 60] ^= 1;                                          /* 改头不重算 → 头 CRC 坏 */
    CHECK_EQ(parse(m, BS, &g), GK3_ECRC);

    FRESH();
    m[BS] = 'X';
    CHECK_EQ(parse(m, BS, &g), GK3_EMAGIC);

    FRESH();
    gk3_put_le32(m + BS + 8, 0x00020000);
    reseal(m, BS);
    CHECK_EQ(parse(m, BS, &g), GK3_EVERSION);

    FRESH();
    gk3_put_le64(m + BS + 24, 1000215215);                    /* 头说自己在盘尾 = 备份表头 */
    reseal(m, BS);
    CHECK_EQ(parse(m, BS, &g), GK3_ERANGE);

    FRESH();
    gk3_put_le32(m + BS + 84, 100);                           /* 表项大小不是 128·2^n */
    reseal(m, BS);
    CHECK_EQ(gk3_gpt_parse_header(m + BS, BS, &g), GK3_ERANGE);

    FRESH();
    gk3_put_le32(m + BS + 80, 0x01000000);                    /* 天文数字的表项数 */
    reseal(m, BS);
    CHECK_EQ(gk3_gpt_parse_header(m + BS, BS, &g), GK3_ERANGE);

    FRESH();
    gk3_put_le64(m + BS + 40, 20);                            /* usable 区压在表上 */
    reseal(m, BS);
    CHECK_EQ(gk3_gpt_parse_header(m + BS, BS, &g), GK3_ERANGE);

    FRESH();
    gk3_put_le32(m + BS + 12, 600);                           /* header_size 超出 */
    reseal(m, BS);
    CHECK_EQ(gk3_gpt_parse_header(m + BS, BS, &g), GK3_ERANGE);

    FRESH();
    gk3_put_le64(m + 2 * BS + 3 * 128 + 32, 0);               /* misc 起点 0：在 usable 区外 */
    reseal(m, BS);
    CHECK_EQ(parse(m, BS, &g), GK3_OK);
    CHECK_EQ(gk3_gpt_find(&g, "misc", &p), GK3_ERANGE);

    FRESH();
    gk3_put_le16(m + 2 * BS + 3 * 128 + 56 + 8, 0xe9);        /* "misc" + U+00E9：非 ASCII 名字不参与查找 */
    reseal(m, BS);
    CHECK_EQ(parse(m, BS, &g), GK3_OK);
    CHECK_EQ(gk3_gpt_find(&g, "misc", &p), GK3_ENOENT);
    CHECK_EQ(gk3_gpt_get(&g, 3, &p), GK3_OK);
    CHECK_STR(p.name, "");

    FRESH();
    set_name(m + 2 * BS + 3 * 128, "abcdefghijklmnopqrstuvwxyz0123456789");  /* 36 字符、没有 NUL */
    reseal(m, BS);
    CHECK_EQ(parse(m, BS, &g), GK3_OK);
    CHECK_EQ(gk3_gpt_find(&g, "abcdefghijklmnopqrstuvwxyz0123456789", &p), GK3_OK);
#undef FRESH
    free(m);
    free(d);
}

static void generated(unsigned bs)
{
    size_t len;
    unsigned char *d = build(bs, 488386ull * (1u << 20) / bs, &len);
    t_memdev md = {d, len, bs, -1, -1, 0};
    gk3_blk dev;
    gk3_gpt g;
    gk3_gpt_part p;
    unsigned char hdr[4096], *ent = malloc(16384 + bs);
    t_memdev_bind(&md, &dev);
    CHECK_EQ(gk3_gpt_read(&dev, &g, hdr, ent, 16384), GK3_ERANGE);       /* 设备比表说的小 */
    dev.num_blocks = 488386ull * (1u << 20) / bs;                         /* 只造了盘头，大小按整盘报 */
    CHECK_EQ(gk3_gpt_read(&dev, &g, hdr, ent, 16384), GK3_OK);
    CHECK_EQ(g.block_size, bs);
    CHECK_EQ(g.first_usable, 2 + 16384 / bs);
    CHECK_EQ(gk3_gpt_require_unique(&g, six, 6, NULL), GK3_OK);
    CHECK_EQ(gk3_gpt_find(&g, "super", &p), GK3_OK);
    CHECK_EQ(p.first_lba, 10433ull * (1u << 20) / bs);
    CHECK_EQ(gk3_gpt_read(&dev, &g, hdr, ent, 16384 - bs), GK3_ENOSPC);   /* 缓冲区不够 */
    free(ent);
    free(d);
}

void test_gpt(void)
{
    real_disk();
    generated(512);
    generated(4096);
}
