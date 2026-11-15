/* 主机上的 ISO / FAT32 小工具：把 src/iso9660.c 与 src/fatw.c 接到普通文件上，给 test/test-media.sh 对照独立的工具用。
 *
 *   media_host iso-ls <iso>                                  每个条目一行：D <路径> 或 F <路径> <大小> <偏移>
 *   media_host fat-size <扇区> <路径>=<源文件>...             gk3w_fat_size_for 的结果（字节）
 *   media_host fat-mk <镜像> <字节> <扇区> <卷标> <路径>=<源文件>...
 *   media_host iso2fat <iso> <镜像> <字节> <扇区> <卷标> [<路径>=<源文件>...]   ISO 里的全部文件 + 另加的文件
 *
 * 镜像先截成给定的字节数再写；分区起点当作 1 MiB（BPB_HiddSec），时间戳固定。失败时打印原因、退出码 1。 */
#define _FILE_OFFSET_BITS 64
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/fatw.h"
#include "../src/iso9660.h"

#define MAX_FILES 4096

typedef struct {
    int fd;
    uint64_t base;      /* 内容在 fd 里的起点（ISO 里的文件不是 0） */
} source;

static int pread_all(int fd, void *buf, size_t len, uint64_t off)
{
    while (len) {
        ssize_t r = pread(fd, buf, len, (off_t)off);
        if (r <= 0)
            return -1;
        buf = (char *)buf + r;
        len -= (size_t)r;
        off += (uint64_t)r;
    }
    return 0;
}

static int iso_read(void *ctx, uint64_t off, void *buf, size_t len)
{
    return pread_all(*(int *)ctx, buf, len, off);
}

static int img_write(void *ctx, uint64_t off, const void *buf, size_t len)
{
    int fd = *(int *)ctx;
    while (len) {
        ssize_t w = pwrite(fd, buf, len, (off_t)off);
        if (w <= 0)
            return -1;
        buf = (const char *)buf + w;
        len -= (size_t)w;
        off += (uint64_t)w;
    }
    return 0;
}

static int file_read(void *ctx, const gk3w_fat_file *f, uint64_t off, void *buf, size_t len)
{
    const source *s = f->src;
    (void)ctx;
    return pread_all(s->fd, buf, len, s->base + off);
}

static gk3w_fat_file files[MAX_FILES];
static source sources[MAX_FILES];
static gk3w_iso_entry entries[MAX_FILES];
static size_t nfiles;

/* <路径>=<源文件> */
static int add_host_file(const char *arg)
{
    const char *eq = strchr(arg, '=');
    struct stat st;
    int fd;
    if (!eq || nfiles >= MAX_FILES)
        return -1;
    fd = open(eq + 1, O_RDONLY);
    if (fd < 0 || fstat(fd, &st))
        return -1;
    sources[nfiles] = (source){fd, 0};
    files[nfiles] = (gk3w_fat_file){strndup(arg, (size_t)(eq - arg)), (uint64_t)st.st_size, &sources[nfiles]};
    nfiles++;
    return 0;
}

static int list_iso(int fd, size_t *n)
{
    struct stat st;
    gk3w_iso_src src = {&fd, iso_read};
    gk3w_iso_err e;
    if (fstat(fd, &st))
        return -1;
    e = gk3w_iso_list(&src, (uint64_t)st.st_size, entries, MAX_FILES, n);
    if (e) {
        fprintf(stderr, "iso: %s\n", gk3w_iso_strerror(e));
        return -1;
    }
    return 0;
}

static int build(const char *img, const char *bytes, const char *sector, const char *label)
{
    int fd = open(img, O_RDWR | O_CREAT | O_TRUNC, 0644);
    gk3w_fat_params p = {0};
    gk3w_fat_io io = {&fd, img_write, file_read, NULL};
    gk3w_fat_err e;
    if (fd < 0)
        return -1;
    p.part_bytes = strtoull(bytes, NULL, 0);
    p.sector = (uint32_t)strtoul(sector, NULL, 0);
    p.hidden = (1u << 20) / p.sector;
    snprintf(p.label, sizeof p.label, "%s", label);
    p.volume_id = 0x12345678;
    gk3w_fat_stamp(2026, 11, 14, 12, 0, 0, &p.date, &p.time);
    if (ftruncate(fd, (off_t)p.part_bytes))
        return -1;
    e = gk3w_fat_build(&p, files, nfiles, &io);
    close(fd);
    if (e) {
        fprintf(stderr, "fat: %s\n", gk3w_fat_strerror(e));
        printf("FATERR %d\n", (int)e);
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "iso-ls")) {
        int fd = open(argv[2], O_RDONLY);
        size_t n;
        if (fd < 0 || list_iso(fd, &n))
            return 1;
        for (size_t i = 0; i < n; i++) {
            if (entries[i].is_dir)
                printf("D %s\n", entries[i].path);
            else
                printf("F %s %llu %llu\n", entries[i].path, (unsigned long long)entries[i].size, (unsigned long long)entries[i].offset);
        }
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "fat-size")) {
        uint64_t s;
        for (int i = 3; i < argc; i++)
            if (add_host_file(argv[i]))
                return 1;
        s = gk3w_fat_size_for(files, nfiles, (uint32_t)strtoul(argv[2], NULL, 0));
        printf("%llu\n", (unsigned long long)s);
        return s ? 0 : 1;
    }
    if (argc >= 6 && !strcmp(argv[1], "fat-mk")) {
        for (int i = 6; i < argc; i++)
            if (add_host_file(argv[i]))
                return 1;
        return build(argv[2], argv[3], argv[4], argv[5]) ? 1 : 0;
    }
    if (argc >= 7 && !strcmp(argv[1], "iso2fat")) {
        int fd = open(argv[2], O_RDONLY);
        size_t n;
        if (fd < 0 || list_iso(fd, &n))
            return 1;
        for (size_t i = 0; i < n; i++) {
            if (entries[i].is_dir)
                continue;
            sources[nfiles] = (source){fd, entries[i].offset};
            files[nfiles] = (gk3w_fat_file){entries[i].path, entries[i].size, &sources[nfiles]};
            nfiles++;
        }
        for (int i = 7; i < argc; i++)
            if (add_host_file(argv[i]))
                return 1;
        return build(argv[3], argv[4], argv[5], argv[6]) ? 1 : 0;
    }
    fprintf(stderr, "usage: media_host iso-ls|fat-size|fat-mk|iso2fat ...\n");
    return 2;
}
