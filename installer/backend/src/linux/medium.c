/* gk3l：安装介质（说明见 lx.h）。 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "disk.h"
#include "lx.h"

#define MEDIA "/media/gk3"

typedef struct {
    char disk[64];
    uint64_t start, size;       /* sysfs 的 512 字节单位 */
    int ram, ok;
} medium_rec;

static const char *rec_path(void)
{
    const char *p = getenv("GK3_MEDIUM_FILE");
    return p && *p ? p : "/run/gaokun3/medium";
}

static void read_rec(medium_rec *m)
{
    FILE *f;
    char line[256];
    int have = 0;
    memset(m, 0, sizeof *m);
    f = fopen(rec_path(), "re");
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        char *v = strchr(line, '='), *end = NULL;
        if (!v)
            continue;
        *v++ = 0;
        v[strcspn(v, "\r\n")] = 0;
        if (!strcmp(line, "disk") && *v && strlen(v) < sizeof m->disk && !strchr(v, '/')) {
            strcpy(m->disk, v);
            have |= 1;
        } else if (!strcmp(line, "start") && *v >= '0' && *v <= '9') {
            m->start = strtoull(v, &end, 10);
            have |= *end ? 0 : 2;
        } else if (!strcmp(line, "size") && *v >= '0' && *v <= '9') {
            m->size = strtoull(v, &end, 10);
            have |= *end ? 0 : 4;
        } else if (!strcmp(line, "ram")) {
            m->ram = !strcmp(v, "yes");
        }
    }
    fclose(f);
    m->ok = have == 7 && m->size;
}

/* 盘上起点、大小是这样的分区现在是哪个（sysfs）；整盘清空时分区被重新加进内核，分区号可能变 */
static int find_kpart(const char *disk, uint64_t start, uint64_t size, gk3l_kpart *out)
{
    static gk3l_kpart kp[256];
    size_t n = gk3l_kernel_parts(disk, kp, 256);
    for (size_t i = 0; i < n; i++)
        if (kp[i].start512 == start && kp[i].size512 == size) {
            *out = kp[i];
            return 0;
        }
    return -1;
}

static int rec_part(const medium_rec *m, gk3l_kpart *out)
{
    return find_kpart(m->disk, m->start, m->size, out);
}

int gk3l_medium_devno(char *out, size_t cap)
{
    medium_rec m;
    const char *dir = getenv("GK3_MEDIUM_DIR");
    read_rec(&m);
    if (m.ok && m.ram) {
        gk3l_kpart k;
        if (rec_part(&m, &k) || strlen(k.devno) >= cap)
            return -1;
        strcpy(out, k.devno);
        return 0;
    }
    return gk3l_devno_at(dir && *dir ? dir : MEDIA, out, cap);
}

int gk3l_medium_ram(void)
{
    medium_rec m;
    read_rec(&m);
    return m.ok && m.ram;
}
