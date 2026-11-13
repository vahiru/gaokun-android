/* GPT 的写（说明见 gptw.h）。 */
#include "gptw.h"

#include <string.h>

#include "gk3core.h"

const char *gk3w_gptw_strerror(gk3w_gptw_err e)
{
    switch (e) {
    case GK3W_GPTW_OK: return "ok";
    case GK3W_GPTW_EINVAL: return "invalid";
    case GK3W_GPTW_FULL: return "full";
    case GK3W_GPTW_RANGE: return "range";
    case GK3W_GPTW_OVERLAP: return "overlap";
    case GK3W_GPTW_LAYOUT: return "layout";
    }
    return "?";
}

static int hexv(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

int gk3w_guid_parse(const char *s, uint8_t out[16])
{
    /* 文本里的字节顺序 → 盘上位置：前三段小端（mixed-endian），后两段原样 */
    static const uint8_t pos[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
    int k = 0;
    if (strlen(s) != 36)
        return -1;
    for (int i = 0; i < 36;) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (s[i] != '-')
                return -1;
            i++;
            continue;
        }
        if (hexv(s[i]) < 0 || hexv(s[i + 1]) < 0)
            return -1;
        out[pos[k++]] = (uint8_t)(hexv(s[i]) * 16 + hexv(s[i + 1]));
        i += 2;
    }
    return 0;
}

void gk3w_guid_v4(uint8_t g[16])
{
    g[7] = (uint8_t)((g[7] & 0x0F) | 0x40);   /* time_hi_and_version 的高 4 位（盘上第 7 字节，小端） */
    g[8] = (uint8_t)((g[8] & 0x3F) | 0x80);   /* variant 10xx */
}

static void seal_header(uint8_t *hdr)
{
    uint32_t hs = gk3_le32(hdr + 12);
    gk3_put_le32(hdr + 16, 0);
    gk3_put_le32(hdr + 16, gk3_crc32(0, hdr, hs));
}

gk3w_gptw_err gk3w_gpt_init(uint32_t bs, uint64_t total_blocks, const uint8_t disk_guid[16], uint8_t *pmbr, uint8_t *hdr,
                            uint8_t *entries)
{
    uint64_t tb, first_usable, bak_entries;
    if ((bs != 512 && bs != 4096) || !total_blocks)
        return GK3W_GPTW_EINVAL;
    tb = (GK3W_GPT_ENTRIES * GK3W_GPT_ENTRY_SIZE + bs - 1) / bs;
    first_usable = 2 + tb;
    if (total_blocks < 2 * first_usable + 2)
        return GK3W_GPTW_EINVAL;
    bak_entries = total_blocks - 1 - tb;

    /* 保护性 MBR：一个 0xEE 分区从 LBA 1 盖到盘尾（超过 32 位时记 0xFFFFFFFF），CHS 照惯例填 0xFFFFFF */
    memset(pmbr, 0, bs);
    pmbr[446 + 1] = 0x00;
    pmbr[446 + 2] = 0x02;
    pmbr[446 + 3] = 0x00;
    pmbr[446 + 4] = 0xEE;
    pmbr[446 + 5] = 0xFF;
    pmbr[446 + 6] = 0xFF;
    pmbr[446 + 7] = 0xFF;
    gk3_put_le32(pmbr + 446 + 8, 1);
    gk3_put_le32(pmbr + 446 + 12, total_blocks - 1 > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)(total_blocks - 1));
    pmbr[510] = 0x55;
    pmbr[511] = 0xAA;

    memset(entries, 0, GK3W_GPT_ENTRIES * GK3W_GPT_ENTRY_SIZE);
    memset(hdr, 0, bs);
    memcpy(hdr, "EFI PART", 8);
    gk3_put_le32(hdr + 8, 0x00010000u);
    gk3_put_le32(hdr + 12, GK3W_GPT_HDR_SIZE);
    gk3_put_le64(hdr + 24, 1);
    gk3_put_le64(hdr + 32, total_blocks - 1);
    gk3_put_le64(hdr + 40, first_usable);
    gk3_put_le64(hdr + 48, bak_entries - 1);
    memcpy(hdr + 56, disk_guid, 16);
    gk3_put_le64(hdr + 72, 2);
    gk3_put_le32(hdr + 80, GK3W_GPT_ENTRIES);
    gk3_put_le32(hdr + 84, GK3W_GPT_ENTRY_SIZE);
    gk3_put_le32(hdr + 88, gk3_crc32(0, entries, GK3W_GPT_ENTRIES * GK3W_GPT_ENTRY_SIZE));
    seal_header(hdr);
    return GK3W_GPTW_OK;
}

static int is_empty(const uint8_t *e)
{
    for (int i = 0; i < 16; i++)
        if (e[i])
            return 0;
    return 1;
}

gk3w_gptw_err gk3w_gpt_add(uint8_t *hdr, uint32_t bs, uint8_t *entries, size_t entries_len, const gk3w_gpt_new *np, size_t n,
                           uint32_t *idx)
{
    gk3_gpt g;
    uint32_t slots[GK3W_GPT_ENTRIES];
    size_t found = 0;
    if (gk3_gpt_parse_mem(hdr, bs, entries, entries_len, &g) != GK3_OK)
        return GK3W_GPTW_EINVAL;
    for (size_t i = 0; i < n; i++) {
        size_t nl = strlen(np[i].name);
        if (nl > GK3_GPT_NAME_CHARS || is_empty(np[i].type))
            return GK3W_GPTW_EINVAL;
        for (size_t k = 0; k < nl; k++)
            if (np[i].name[k] < 0x20 || np[i].name[k] > 0x7e)
                return GK3W_GPTW_EINVAL;
        if (np[i].first > np[i].last || np[i].first < g.first_usable || np[i].last > g.last_usable)
            return GK3W_GPTW_RANGE;
        for (size_t j = 0; j < i; j++)
            if (np[i].first <= np[j].last && np[j].first <= np[i].last)
                return GK3W_GPTW_OVERLAP;
        for (uint32_t k = 0; k < g.num_entries; k++) {
            gk3_gpt_part p;
            if (gk3_gpt_get(&g, k, &p) != GK3_OK)
                continue;
            if (np[i].first <= p.last_lba && p.first_lba <= np[i].last)
                return GK3W_GPTW_OVERLAP;
        }
    }
    for (uint32_t k = 0; k < g.num_entries && found < n && found < GK3W_GPT_ENTRIES; k++)
        if (is_empty(entries + (size_t)k * g.entry_size))
            slots[found++] = k;
    if (found < n)
        return GK3W_GPTW_FULL;

    for (size_t i = 0; i < n; i++) {
        uint8_t *e = entries + (size_t)slots[i] * g.entry_size;
        size_t nl = strlen(np[i].name);
        memset(e, 0, g.entry_size);
        memcpy(e, np[i].type, 16);
        memcpy(e + 16, np[i].uuid, 16);
        gk3_put_le64(e + 32, np[i].first);
        gk3_put_le64(e + 40, np[i].last);
        gk3_put_le64(e + 48, np[i].attrs);
        for (size_t k = 0; k < nl; k++)
            gk3_put_le16(e + 56 + 2 * k, (uint16_t)np[i].name[k]);
        if (idx)
            idx[i] = slots[i] + 1;
    }
    gk3_put_le32(hdr + 88, gk3_crc32(0, entries, (size_t)g.num_entries * g.entry_size));
    seal_header(hdr);
    return GK3W_GPTW_OK;
}

gk3w_gptw_err gk3w_gpt_remove(uint8_t *hdr, uint32_t bs, uint8_t *entries, size_t entries_len, uint32_t idx)
{
    gk3_gpt g;
    if (gk3_gpt_parse_mem(hdr, bs, entries, entries_len, &g) != GK3_OK)
        return GK3W_GPTW_EINVAL;
    if (!idx || idx > g.num_entries || is_empty(entries + (size_t)(idx - 1) * g.entry_size))
        return GK3W_GPTW_RANGE;
    memset(entries + (size_t)(idx - 1) * g.entry_size, 0, g.entry_size);
    gk3_put_le32(hdr + 88, gk3_crc32(0, entries, (size_t)g.num_entries * g.entry_size));
    seal_header(hdr);
    return GK3W_GPTW_OK;
}

gk3w_gptw_err gk3w_gpt_make_backup(const uint8_t *hdr, uint32_t bs, uint8_t *bak_hdr, uint64_t *entries_lba)
{
    gk3_gpt g;
    uint64_t tb;
    uint32_t hs;
    if (gk3_gpt_parse_header(hdr, bs, &g) != GK3_OK)
        return GK3W_GPTW_EINVAL;
    tb = ((uint64_t)g.num_entries * g.entry_size + bs - 1) / bs;
    if (g.alt_lba <= tb || g.alt_lba - tb <= g.last_usable)
        return GK3W_GPTW_LAYOUT;
    hs = gk3_le32(hdr + 12);
    memset(bak_hdr, 0, bs);
    memcpy(bak_hdr, hdr, hs);
    gk3_put_le64(bak_hdr + 24, g.alt_lba);
    gk3_put_le64(bak_hdr + 32, g.my_lba);
    gk3_put_le64(bak_hdr + 72, g.alt_lba - tb);
    seal_header(bak_hdr);
    *entries_lba = g.alt_lba - tb;
    return GK3W_GPTW_OK;
}
