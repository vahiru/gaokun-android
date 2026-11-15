/* FAT32 写入器（说明见 fatw.h）。 */
#include "fatw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "release.h"

#define NODE_MAX 4096
#define RSVD_MIN 32u                    /* Windows 格式化 FAT32 的保留扇区数 */
#define ALIGN_BYTES (1u << 20)          /* 数据区起点按 1 MiB 对齐（闪存的擦除块） */
#define CLUSTERS_MIN 65525u             /* 少于这么多簇就不是 FAT32（fatgen103 的判定） */
#define CLUSTERS_MAX 0x0FFFFFF5u
#define CHUNK (4u << 20)
#define FILE_MAX 0xFFFFFFFFull          /* 目录项里的文件大小是 32 位 */

typedef struct {
    char name[256];             /* 这一段的 UTF-8 名字 */
    uint16_t lname[255];        /* 同一个名字的 UTF-16 */
    int llen;
    int is_dir, parent;
    const gk3w_fat_file *file;
    uint64_t size;              /* 文件：内容字节；目录：目录项区的字节 */
    uint32_t first, nclus;      /* 起始簇（空文件是 0）、簇数 */
    uint8_t sname[11];          /* 8.3 短名（空格补齐） */
    int nlfn;                   /* 长文件名目录项的个数，0 = 只有短名 */
} node;

typedef struct {
    uint32_t sector, spc, cl;   /* 扇区字节、每簇扇区、每簇字节 */
    uint32_t total;             /* 总扇区 */
    uint32_t rsvd, fatsz;       /* 保留扇区、每份 FAT 的扇区 */
    uint32_t data;              /* 数据区起点（扇区） */
    uint32_t clusters;          /* 数据簇的个数 */
    uint32_t used;              /* 已分配的簇（目录与文件） */
} layout;

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* ── 名字 ─────────────────────────────────────────────────────────── */

/* UTF-8 → UTF-16（超出 BMP 的用代理对）；不合法的 UTF-8、超过 cap 个单元返回 -1 */
static int utf8_to_16(const char *s, uint16_t *out, int cap)
{
    const unsigned char *p = (const unsigned char *)s;
    int n = 0;
    while (*p) {
        uint32_t c;
        int k;
        if (*p < 0x80) {
            c = *p;
            k = 0;
        } else if ((*p & 0xE0) == 0xC0) {
            c = *p & 0x1F;
            k = 1;
        } else if ((*p & 0xF0) == 0xE0) {
            c = *p & 0x0F;
            k = 2;
        } else if ((*p & 0xF8) == 0xF0) {
            c = *p & 0x07;
            k = 3;
        } else {
            return -1;
        }
        p++;
        for (int i = 0; i < k; i++) {
            if ((*p & 0xC0) != 0x80)
                return -1;
            c = c << 6 | (*p++ & 0x3F);
        }
        if ((k == 1 && c < 0x80) || (k == 2 && c < 0x800) || (k == 3 && (c < 0x10000 || c > 0x10FFFF)) ||
            (c >= 0xD800 && c <= 0xDFFF))
            return -1;
        if (c >= 0x10000) {
            if (n + 2 > cap)
                return -1;
            c -= 0x10000;
            out[n++] = (uint16_t)(0xD800 | c >> 10);
            out[n++] = (uint16_t)(0xDC00 | (c & 0x3FF));
        } else {
            if (n + 1 > cap)
                return -1;
            out[n++] = (uint16_t)c;
        }
    }
    return n;
}

/* 长文件名里允许的名字：没有控制字符与 " * / : < > ? \ |，不以空格或点结尾（Windows 会把它们去掉），不是 "." ".." */
static int long_name_ok(const char *s)
{
    size_t len = strlen(s);
    if (!len || !strcmp(s, ".") || !strcmp(s, "..") || s[len - 1] == ' ' || s[len - 1] == '.')
        return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (*p < 0x20 || *p == 0x7F || strchr("\"*/:<>?\\|", *p))
            return 0;
    return 1;
}

static int short_char_ok(int c)
{
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c && strchr("!#$%&'()-@^_`{}~", c));
}

/* 名字本身就是合法的大写 8.3：不用长文件名 */
static int plain_83(const char *name, uint8_t out[11])
{
    const char *dot = strchr(name, '.');
    size_t bl = dot ? (size_t)(dot - name) : strlen(name), el = dot ? strlen(dot + 1) : 0;
    if (!bl || bl > 8 || el > 3 || (dot && (!el || strchr(dot + 1, '.'))))
        return 0;
    memset(out, ' ', 11);
    for (size_t i = 0; i < bl; i++) {
        if (!short_char_ok((unsigned char)name[i]))
            return 0;
        out[i] = (uint8_t)name[i];
    }
    for (size_t i = 0; i < el; i++) {
        if (!short_char_ok((unsigned char)dot[1 + i]))
            return 0;
        out[8 + i] = (uint8_t)dot[1 + i];
    }
    return 1;
}

/* 短名的基础（fatgen103 的 basis-name）：大写，短名里不允许的字符换成 '_'，去掉空格与点；扩展名取最后一个点之后的部分。
 * *lossy：转换丢了信息（换过字符、截断过），这时一定要加 ~N 尾巴 */
static void basis(const char *name, char base[9], char ext[4], int *lossy)
{
    const char *last = strrchr(name, '.');
    size_t b = 0, e = 0;
    *lossy = 0;
    if (last == name)
        last = NULL;
    for (const char *p = name; *p && p != last; p++) {
        int c = (unsigned char)*p;
        if (c == ' ' || c == '.') {
            *lossy = 1;
            continue;
        }
        if (c >= 'a' && c <= 'z')
            c -= 32;
        if (!short_char_ok(c)) {
            c = '_';
            *lossy = 1;
        }
        if (b < 8)
            base[b++] = (char)c;
        else
            *lossy = 1;
    }
    if (last)
        for (const char *p = last + 1; *p; p++) {
            int c = (unsigned char)*p;
            if (c == ' ') {
                *lossy = 1;
                continue;
            }
            if (c >= 'a' && c <= 'z')
                c -= 32;
            if (!short_char_ok(c)) {
                c = '_';
                *lossy = 1;
            }
            if (e < 3)
                ext[e++] = (char)c;
            else
                *lossy = 1;
        }
    if (!b) {
        base[b++] = '_';
        *lossy = 1;
    }
    base[b] = 0;
    ext[e] = 0;
}

/* 同一目录里别的条目已经用了这个短名，或者别的条目的长文件名按 8.3 读正好是它：vfat 查找时短名也算数，
 * 这样的短名会让那个文件按自己的名字打不开 */
static int sname_taken(const node *nd, int nn, int self, int parent, const uint8_t s[11])
{
    for (int i = 1; i < nn; i++) {
        char up[256];
        uint8_t o[11];
        size_t k;
        if (i == self || nd[i].parent != parent)
            continue;
        if (nd[i].nlfn >= 0 && !memcmp(nd[i].sname, s, 11))
            return 1;
        for (k = 0; nd[i].name[k] && k + 1 < sizeof up; k++)
            up[k] = (char)(nd[i].name[k] >= 'a' && nd[i].name[k] <= 'z' ? nd[i].name[k] - 32 : nd[i].name[k]);
        up[k] = 0;
        if (plain_83(up, o) && !memcmp(o, s, 11))
            return 1;
    }
    return 0;
}

/* 给第 k 个节点定短名与长文件名目录项个数（同一目录里先定的节点不会再变） */
static int name_node(node *nd, int nn, int k)
{
    node *x = &nd[k];
    char base[9], ext[4];
    int lossy;
    uint8_t s[11];
    x->nlfn = -1;   /* 还没定：sname_taken 跳过它 */
    if (plain_83(x->name, s) && !sname_taken(nd, nn, k, x->parent, s)) {
        memcpy(x->sname, s, 11);
        x->nlfn = 0;
        return 0;
    }
    basis(x->name, base, ext, &lossy);
    if (!lossy) {
        memset(s, ' ', 11);
        memcpy(s, base, strlen(base));
        memcpy(s + 8, ext, strlen(ext));
        if (!sname_taken(nd, nn, k, x->parent, s))
            goto done;
    }
    for (unsigned long t = 1; t < 1000000; t++) {
        char tail[9];
        int tl = snprintf(tail, sizeof tail, "~%lu", t);
        size_t keep = strlen(base);
        if (keep > (size_t)(8 - tl))
            keep = (size_t)(8 - tl);
        memset(s, ' ', 11);
        memcpy(s, base, keep);
        memcpy(s + keep, tail, (size_t)tl);
        memcpy(s + 8, ext, strlen(ext));
        if (!sname_taken(nd, nn, k, x->parent, s))
            goto done;
    }
    return -1;
done:
    memcpy(x->sname, s, 11);
    x->nlfn = (x->llen + 12) / 13;
    return 0;
}

/* 名字比较：ASCII 不分大小写（FAT 的长文件名不分大小写），其余逐字节 */
static int same_name(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        int ca = (unsigned char)*a, cb = (unsigned char)*b;
        if (ca >= 'a' && ca <= 'z')
            ca -= 32;
        if (cb >= 'a' && cb <= 'z')
            cb -= 32;
        if (ca != cb)
            return 0;
    }
    return *a == *b;
}

static int find_child(const node *nd, int nn, int parent, const char *name)
{
    for (int i = 1; i < nn; i++)
        if (nd[i].parent == parent && same_name(nd[i].name, name))
            return i;
    return -1;
}

static int add_node(node *nd, int *nn, int parent, const char *name, size_t len, int is_dir)
{
    node *x;
    if (*nn >= NODE_MAX || len >= sizeof x->name)
        return -1;
    x = &nd[*nn];
    memset(x, 0, sizeof *x);
    memcpy(x->name, name, len);
    x->name[len] = 0;
    if (!long_name_ok(x->name) || (x->llen = utf8_to_16(x->name, x->lname, 255)) <= 0)
        return -1;
    x->is_dir = is_dir;
    x->parent = parent;
    return (*nn)++;
}

/* 按路径建树：根是 0 号节点；中间的目录按需要建；重名（不分大小写）、文件与目录同名都算错 */
static int build_tree(const gk3w_fat_file *files, size_t n, node *nd, int *nn)
{
    memset(&nd[0], 0, sizeof nd[0]);
    nd[0].is_dir = 1;
    nd[0].parent = -1;
    *nn = 1;
    for (size_t i = 0; i < n; i++) {
        const char *p = files[i].path;
        int parent = 0;
        if (!p || !*p || files[i].size > FILE_MAX)
            return -1;
        for (;;) {
            const char *slash = strchr(p, '/');
            size_t len = slash ? (size_t)(slash - p) : strlen(p);
            char comp[256];
            int c;
            if (!len || len >= sizeof comp)
                return -1;
            memcpy(comp, p, len);
            comp[len] = 0;
            c = find_child(nd, *nn, parent, comp);
            if (slash) {
                if (c < 0)
                    c = add_node(nd, nn, parent, comp, len, 1);
                if (c < 0 || !nd[c].is_dir)
                    return -1;
                parent = c;
                p = slash + 1;
            } else {
                if (c >= 0)
                    return -1;
                c = add_node(nd, nn, parent, comp, len, 0);
                if (c < 0)
                    return -1;
                nd[c].file = &files[i];
                nd[c].size = files[i].size;
                break;
            }
        }
    }
    for (int k = 1; k < *nn; k++)
        if (name_node(nd, *nn, k))
            return -1;
    return 0;
}

/* ── 布局 ─────────────────────────────────────────────────────────── */

static int make_layout(uint64_t part_bytes, uint32_t sector, uint32_t cl, layout *L)
{
    uint64_t ts = part_bytes / sector, data, clus;
    uint32_t align = ALIGN_BYTES / sector, rsvd = RSVD_MIN, fatsz = 1;
    memset(L, 0, sizeof *L);
    if (ts > 0xFFFFFFFFu || cl < sector || cl % sector || cl / sector > 128 || ts <= rsvd + 2u)
        return -1;
    L->sector = sector;
    L->cl = cl;
    L->spc = cl / sector;
    L->total = (uint32_t)ts;
    /* 每份 FAT 的大小：簇数取决于 FAT 占掉多少，从小往大试到够用为止 */
    for (int it = 0; it < 64; it++) {
        uint32_t need;
        if (rsvd + 2ull * fatsz >= ts)
            return -1;
        clus = (ts - rsvd - 2ull * fatsz) / L->spc;
        need = (uint32_t)(((clus + 2) * 4 + sector - 1) / sector);
        if (need <= fatsz)
            break;
        fatsz = need;
    }
    data = (rsvd + 2ull * fatsz + align - 1) / align * align;
    if (data >= ts)
        return -1;
    L->fatsz = fatsz;
    L->data = (uint32_t)data;
    L->rsvd = (uint32_t)(data - 2ull * fatsz);
    clus = (ts - data) / L->spc;
    L->clusters = clus > CLUSTERS_MAX ? CLUSTERS_MAX + 1 : (uint32_t)clus;
    return 0;
}

/* Windows 格式化 FAT32 的默认簇大小 */
static uint32_t default_cluster(uint64_t bytes)
{
    if (bytes <= (256ull << 20))
        return 512;
    if (bytes <= (8ull << 30))
        return 4096;
    if (bytes <= (16ull << 30))
        return 8192;
    if (bytes <= (32ull << 30))
        return 16384;
    return 32768;
}

static int pick_layout(uint64_t part_bytes, uint32_t sector, layout *L)
{
    uint32_t cl = default_cluster(part_bytes);
    if (cl < sector)
        cl = sector;
    for (; cl >= sector; cl /= 2) {
        if (make_layout(part_bytes, sector, cl, L))
            continue;
        if (L->clusters > CLUSTERS_MAX)
            return -1;
        if (L->clusters >= CLUSTERS_MIN)
            return 0;
    }
    return -1;
}

/* 目录项区的字节：根目录多一个卷标项，子目录多 "." ".." */
static uint64_t dir_bytes(const node *nd, int nn, int d)
{
    uint64_t e = d == 0 ? 1 : 2;
    for (int i = 1; i < nn; i++)
        if (nd[i].parent == d)
            e += (uint64_t)nd[i].nlfn + 1;
    return e * 32;
}

/* 分配簇：根目录在簇 2，其余目录接着放，再放文件；每个对象占连续的簇 */
static gk3w_fat_err allocate(node *nd, int nn, layout *L)
{
    uint64_t next = 2;
    for (int pass = 0; pass < 2; pass++)
        for (int k = 0; k < nn; k++) {
            node *x = &nd[k];
            uint64_t bytes, c;
            if (x->is_dir != !pass)
                continue;
            bytes = x->is_dir ? dir_bytes(nd, nn, k) : x->size;
            if (x->is_dir)
                x->size = bytes;
            c = (bytes + L->cl - 1) / L->cl;
            if (x->is_dir && !c)
                c = 1;
            x->first = c ? (uint32_t)next : 0;
            x->nclus = (uint32_t)c;
            next += c;
            if (next - 2 > L->clusters)
                return GK3W_FAT_NO_SPACE;
        }
    L->used = (uint32_t)(next - 2);
    return GK3W_FAT_OK;
}

static gk3w_fat_err plan(const gk3w_fat_file *files, size_t n, uint64_t part_bytes, uint32_t sector, node *nd, int *nn, layout *L)
{
    if ((sector != 512 && sector != 4096) || build_tree(files, n, nd, nn))
        return GK3W_FAT_ARG;
    if (pick_layout(part_bytes, sector, L))
        return GK3W_FAT_TOO_SMALL;
    return allocate(nd, *nn, L);
}

uint64_t gk3w_fat_size_for(const gk3w_fat_file *files, size_t n, uint32_t sector)
{
    node *nd = calloc(NODE_MAX, sizeof *nd);
    uint64_t sum = 0, size;
    int nn;
    layout L;
    if (!nd)
        return 0;
    for (size_t i = 0; i < n; i++)
        sum += files[i].size;
    /* 从内容大小加一点余量起，放不下就加大再试：簇大小随分区大小变 */
    size = (sum + (16u << 20) + ALIGN_BYTES - 1) / ALIGN_BYTES * ALIGN_BYTES;
    for (int it = 0; it < 256; it++) {
        gk3w_fat_err e = plan(files, n, size, sector, nd, &nn, &L);
        if (e == GK3W_FAT_OK)
            break;
        if (e == GK3W_FAT_ARG) {
            size = 0;
            break;
        }
        size += (size / 64 + ALIGN_BYTES - 1) / ALIGN_BYTES * ALIGN_BYTES;
    }
    free(nd);
    return size;
}

/* ── 写 ───────────────────────────────────────────────────────────── */

static void put_entry(uint8_t *e, const uint8_t s[11], uint8_t attr, uint32_t clus, uint32_t size, const gk3w_fat_params *p)
{
    memset(e, 0, 32);
    memcpy(e, s, 11);
    e[11] = attr;
    put16(e + 14, p->time);
    put16(e + 16, p->date);
    put16(e + 18, p->date);
    put16(e + 20, clus >> 16);
    put16(e + 22, p->time);
    put16(e + 24, p->date);
    put16(e + 26, clus & 0xFFFF);
    put32(e + 28, size);
}

static uint8_t lfn_sum(const uint8_t s[11])
{
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
        sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + s[i]);
    return sum;
}

static void put_lfn(uint8_t *e, int ord, int last, const node *x, uint8_t sum)
{
    static const int pos[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    memset(e, 0, 32);
    e[0] = (uint8_t)(ord | (last ? 0x40 : 0));
    e[11] = 0x0F;
    e[13] = sum;
    for (int i = 0; i < 13; i++) {
        int k = (ord - 1) * 13 + i;
        put16(e + pos[i], k < x->llen ? x->lname[k] : k == x->llen ? 0x0000 : 0xFFFF);
    }
}

static int write_all(const gk3w_fat_io *io, uint64_t off, const uint8_t *buf, uint64_t len)
{
    while (len) {
        size_t k = len > CHUNK ? CHUNK : (size_t)len;
        if (io->write(io->ctx, off, buf, k))
            return -1;
        off += k;
        buf += k;
        len -= k;
    }
    return 0;
}

static uint64_t clus_off(const layout *L, uint32_t c)
{
    return ((uint64_t)L->data + (uint64_t)(c - 2) * L->spc) * L->sector;
}

static void boot_sector(uint8_t *b, const layout *L, const gk3w_fat_params *p)
{
    char lab[12];
    memset(b, 0, L->sector);
    b[0] = 0xEB;
    b[1] = 0x58;
    b[2] = 0x90;
    memcpy(b + 3, "MSWIN4.1", 8);
    put16(b + 11, L->sector);
    b[13] = (uint8_t)L->spc;
    put16(b + 14, L->rsvd);
    b[16] = 2;
    b[21] = 0xF8;
    put16(b + 24, 63);
    put16(b + 26, 255);
    put32(b + 28, p->hidden);
    put32(b + 32, L->total);
    put32(b + 36, L->fatsz);
    put32(b + 44, 2);
    put16(b + 48, 1);
    put16(b + 50, 6);
    b[64] = 0x80;
    b[66] = 0x29;
    put32(b + 67, p->volume_id);
    snprintf(lab, sizeof lab, "%-11s", p->label[0] ? p->label : "NO NAME");
    memcpy(b + 71, lab, 11);
    memcpy(b + 82, "FAT32   ", 8);
    /* 引导代码：INT 18h（交给固件试下一个启动设备），之后原地循环 */
    b[90] = 0xCD;
    b[91] = 0x18;
    b[92] = 0xEB;
    b[93] = 0xFE;
    b[510] = 0x55;
    b[511] = 0xAA;
}

static void fsinfo_sector(uint8_t *b, const layout *L)
{
    memset(b, 0, L->sector);
    put32(b, 0x41615252);
    put32(b + 484, 0x61417272);
    put32(b + 488, L->clusters - L->used);
    put32(b + 492, L->used + 2 <= L->clusters + 1 ? L->used + 2 : 0xFFFFFFFF);
    put32(b + 508, 0xAA550000);
}

static gk3w_fat_err write_dirs(const node *nd, int nn, const layout *L, const gk3w_fat_params *p, const gk3w_fat_io *io)
{
    for (int d = 0; d < nn; d++) {
        const node *x = &nd[d];
        uint64_t len = (uint64_t)x->nclus * L->cl;
        uint8_t *buf, *e;
        int rc;
        if (!x->is_dir)
            continue;
        buf = gk3w_aligned_alloc((size_t)len);
        if (!buf)
            return GK3W_FAT_NOMEM;
        memset(buf, 0, (size_t)len);
        e = buf;
        if (d == 0) {
            uint8_t lab[11];
            char s[12];
            snprintf(s, sizeof s, "%-11s", p->label[0] ? p->label : "NO NAME");
            memcpy(lab, s, 11);
            put_entry(e, lab, 0x08, 0, 0, p);
            e += 32;
        } else {
            static const uint8_t dot[11] = {'.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
            static const uint8_t dotdot[11] = {'.', '.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
            put_entry(e, dot, 0x10, x->first, 0, p);
            put_entry(e + 32, dotdot, 0x10, x->parent == 0 ? 0 : nd[x->parent].first, 0, p);
            e += 64;
        }
        for (int k = 1; k < nn; k++) {
            const node *c = &nd[k];
            if (c->parent != d)
                continue;
            for (int ord = c->nlfn; ord >= 1; ord--) {
                put_lfn(e, ord, ord == c->nlfn, c, lfn_sum(c->sname));
                e += 32;
            }
            put_entry(e, c->sname, c->is_dir ? 0x10 : 0x20, c->first, c->is_dir ? 0 : (uint32_t)c->size, p);
            e += 32;
        }
        rc = write_all(io, clus_off(L, x->first), buf, len);
        gk3w_aligned_free(buf);
        if (rc)
            return GK3W_FAT_WRITE;
    }
    return GK3W_FAT_OK;
}

static gk3w_fat_err write_files(const node *nd, int nn, const layout *L, const gk3w_fat_io *io)
{
    uint8_t *buf = gk3w_aligned_alloc(CHUNK);
    uint64_t total = 0, done = 0;
    if (!buf)
        return GK3W_FAT_NOMEM;
    for (int k = 0; k < nn; k++)
        if (!nd[k].is_dir)
            total += nd[k].size;
    for (int k = 0; k < nn; k++) {
        const node *x = &nd[k];
        uint64_t off = 0, at;
        if (x->is_dir || !x->size)
            continue;
        at = clus_off(L, x->first);
        while (off < x->size) {
            size_t want = x->size - off > CHUNK ? CHUNK : (size_t)(x->size - off);
            size_t pad = (want + L->sector - 1) / L->sector * L->sector;
            if (io->read(io->ctx, x->file, off, buf, want)) {
                gk3w_aligned_free(buf);
                return GK3W_FAT_READ;
            }
            memset(buf + want, 0, pad - want);
            if (io->write(io->ctx, at + off, buf, pad)) {
                gk3w_aligned_free(buf);
                return GK3W_FAT_WRITE;
            }
            off += want;
            done += want;
            if (io->progress)
                io->progress(io->ctx, done, total);
        }
    }
    gk3w_aligned_free(buf);
    return GK3W_FAT_OK;
}

gk3w_fat_err gk3w_fat_build(const gk3w_fat_params *p, const gk3w_fat_file *files, size_t n, const gk3w_fat_io *io)
{
    node *nd = calloc(NODE_MAX, sizeof *nd);
    layout L;
    gk3w_fat_err e;
    uint8_t *fat = NULL, *rs = NULL;
    uint64_t fat_len, rs_len;
    int nn;
    if (!nd)
        return GK3W_FAT_NOMEM;
    for (const char *c = p->label; *c; c++)
        if (!short_char_ok((unsigned char)*c) && *c != ' ') {
            free(nd);
            return GK3W_FAT_ARG;
        }
    if (strlen(p->label) > 11) {
        free(nd);
        return GK3W_FAT_ARG;
    }
    e = plan(files, n, p->part_bytes, p->sector, nd, &nn, &L);
    if (e)
        goto out;
    fat_len = (uint64_t)L.fatsz * L.sector;
    rs_len = (uint64_t)L.rsvd * L.sector;
    fat = gk3w_aligned_alloc((size_t)fat_len);
    rs = gk3w_aligned_alloc((size_t)rs_len);
    if (!fat || !rs) {
        e = GK3W_FAT_NOMEM;
        goto out;
    }
    /* 先把保留区清零：分区里原有的文件系统签名作废，写到一半断掉时也认不出一个"格式化好了"的卷 */
    memset(rs, 0, (size_t)rs_len);
    if (write_all(io, 0, rs, rs_len)) {
        e = GK3W_FAT_WRITE;
        goto out;
    }
    if ((e = write_files(nd, nn, &L, io)) || (e = write_dirs(nd, nn, &L, p, io)))
        goto out;
    memset(fat, 0, (size_t)fat_len);
    put32(fat, 0x0FFFFFF8);
    put32(fat + 4, 0x0FFFFFFF);
    for (int k = 0; k < nn; k++)
        for (uint32_t i = 0; i < nd[k].nclus; i++) {
            uint32_t c = nd[k].first + i;
            put32(fat + 4ull * c, i + 1 < nd[k].nclus ? c + 1 : 0x0FFFFFFF);
        }
    if (write_all(io, rs_len, fat, fat_len) || write_all(io, rs_len + fat_len, fat, fat_len)) {
        e = GK3W_FAT_WRITE;
        goto out;
    }
    /* 最后写引导扇区与 FSInfo（各有一份备份在 6、7 扇区）：有了它们，这个卷才算格式化好了 */
    boot_sector(rs, &L, p);
    fsinfo_sector(rs + L.sector, &L);
    memcpy(rs + 6ull * L.sector, rs, 2ull * L.sector);
    if (write_all(io, 0, rs, rs_len))
        e = GK3W_FAT_WRITE;
out:
    gk3w_aligned_free(fat);
    gk3w_aligned_free(rs);
    free(nd);
    return e;
}

const char *gk3w_fat_strerror(gk3w_fat_err e)
{
    switch (e) {
    case GK3W_FAT_OK: return "ok";
    case GK3W_FAT_ARG: return "invalid FAT parameters or file names";
    case GK3W_FAT_TOO_SMALL: return "the partition is too small for FAT32";
    case GK3W_FAT_NO_SPACE: return "the files do not fit into the partition";
    case GK3W_FAT_NOMEM: return "out of memory";
    case GK3W_FAT_WRITE: return "writing the file system failed";
    case GK3W_FAT_READ: return "reading a source file failed";
    }
    return "unknown FAT error";
}

void gk3w_fat_stamp(int year, int mon, int day, int hour, int min, int sec, uint16_t *date, uint16_t *time)
{
    if (year < 1980)
        year = 1980;
    if (year > 2107)
        year = 2107;
    *date = (uint16_t)((year - 1980) << 9 | mon << 5 | day);
    *time = (uint16_t)(hour << 11 | min << 5 | sec / 2);
}
