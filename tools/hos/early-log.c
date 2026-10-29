/* SPDX-License-Identifier: Apache-2.0
 * Experimental first-stage logger. Runs before Android init; no ESP access.
 * misc layout: bootloader_message/bootloader_message.h:25-36 reserves 0..64 KiB.
 * Header slots at 64 KiB and +512; circular data starts at 68 KiB.
 * Build with a fixed HOS_MISC_UUID. Never accepts a device path from cmdline.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <linux/fs.h>
#ifndef HOS_MISC_UUID
#error HOS_MISC_UUID must be fixed at build time
#endif
#define DISK_SIZE (4U * 1024 * 1024)
#define HEADER_OFFSET 65536U
#define DATA_OFFSET 69632U
#define CAPACITY (DISK_SIZE - DATA_OFFSET)
#ifdef HOS_NAMESPACE_TEST
/* Only the separately built namespace test binary substitutes kernel cmdline. */
#define CMDLINE_PATH "/hos-test-cmdline"
#else
#define CMDLINE_PATH "/hos-proc/cmdline"
#endif
struct header {
    char magic[8];
    uint32_t version, header_size;
    uint64_t capacity, total, generation;
    uint32_t crc, reserved;
    uint64_t start_ns, dropped;
};
_Static_assert(sizeof(struct header) == 64 && offsetof(struct header, crc) == 40,
               "unexpected header ABI");
static uint64_t now_ns(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return 0;
    return (uint64_t)t.tv_sec * 1000000000ULL + t.tv_nsec;
}
static uint32_t crc32(const void *p, size_t n) {
    const unsigned char *b = p;
    uint32_t c = ~0U;
    for (size_t i = 0; i < n; i++) {
        c ^= b[i];
        for (int j = 0; j < 8; j++) c = (c >> 1) ^ (0xedb88320U & (0U - (c & 1)));
    }
    return ~c;
}
static int write_at(int fd, const void *buf, size_t n, off_t offset) {
    const char *p = buf;
    while (n) {
        ssize_t r = pwrite(fd, p, n, offset);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        p += r; n -= (size_t)r; offset += r;
    }
    return 0;
}
static int commit(int fd, struct header *h) {
    h->generation++;
    h->crc = 0;
    h->crc = crc32(h, sizeof(*h));
    if (write_at(fd, h, sizeof(*h), HEADER_OFFSET + (h->generation % 2) * 512)) return -1;
    return fdatasync(fd);
}
static int append(int fd, struct header *h, const void *buf, size_t n) {
    const char *p = buf;
    while (n) {
        size_t pos = h->total % CAPACITY;
        size_t take = CAPACITY - pos;
        if (take > n) take = n;
        if (write_at(fd, p, take, DATA_OFFSET + pos)) return -1;
        h->total += take; p += take; n -= take;
    }
    return commit(fd, h);
}
static void init_header(struct header *h) {
    memset(h, 0, sizeof(*h));
    memcpy(h->magic, "GK3HLOG1", 8);
    h->version = 1; h->header_size = sizeof(*h); h->capacity = CAPACITY;
    h->start_ns = now_ns();
}
static int read_text(const char *path, char *buf, size_t size) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, size - 1);
    close(fd);
    if (n < 0 || (size_t)n == size - 1) return -1;
    buf[n] = 0;
    return 0;
}
static int command_guard(char *text) {
    int super = 0, marker = 0;
    for (char *p = strtok(text, " \t\r\n"); p; p = strtok(NULL, " \t\r\n")) {
        if (!strncmp(p, "androidboot.force_super_partition=", 34)) {
            if (strcmp(p, "androidboot.force_super_partition=super_hos") || super++) return -1;
        }
        if (!strncmp(p, "androidboot.hos_earlylog=", 25)) {
            if (strcmp(p, "androidboot.hos_earlylog=misc-ring-v1") || marker++) return -1;
        }
    }
    return super == 1 && marker == 1 ? 0 : -1;
}
static int find_target(dev_t *target) {
    DIR *d = opendir("/hos-sys/class/block");
    if (!d) return -1;
    int matches = 0, invalid = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char path[512], text[4096];
        snprintf(path, sizeof(path), "/hos-sys/class/block/%s/uevent", e->d_name);
        if (read_text(path, text, sizeof(text))) continue;
        char name[128] = "", uuid[128] = "";
        unsigned major_id = 0, minor_id = 0;
        for (char *p = strtok(text, "\n"); p; p = strtok(NULL, "\n")) {
            if (!strncmp(p, "PARTNAME=", 9)) snprintf(name, sizeof(name), "%s", p + 9);
            if (!strncmp(p, "PARTUUID=", 9)) snprintf(uuid, sizeof(uuid), "%s", p + 9);
            if (!strncmp(p, "MAJOR=", 6)) major_id = (unsigned)strtoul(p + 6, NULL, 10);
            if (!strncmp(p, "MINOR=", 6)) minor_id = (unsigned)strtoul(p + 6, NULL, 10);
        }
        if (!strcmp(name, "misc_hos") || !strcasecmp(uuid, HOS_MISC_UUID)) {
            matches++;
            if (strcmp(name, "misc_hos") || strcasecmp(uuid, HOS_MISC_UUID) || !major_id) invalid = 1;
            *target = makedev(major_id, minor_id);
        }
    }
    closedir(d);
    return matches == 1 && !invalid ? 0 : -1;
}
static int capture(int output, int input, int ready, int regular_test) {
    struct header h;
    init_header(&h);
    /* Reset both old headers, never touching bootloader/system space. */
    unsigned char zero[1024] = {0};
    if (write_at(output, zero, sizeof(zero), HEADER_OFFSET) || commit(output, &h)) return -1;
    char buffer[65536];
    int notified = 0;
    const uint64_t deadline = now_ns() + 180ULL * 1000000000ULL;
    for (;;) {
        if (now_ns() >= deadline) return notified ? commit(output, &h) : -1;
        ssize_t n = read(input, buffer, sizeof(buffer));
        if (n > 0) {
            if (append(output, &h, buffer, (size_t)n)) return -1;
            continue;
        }
        if (n == 0 && regular_test) return 0;
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EPIPE) { h.dropped++; continue; }
        if (n < 0 && errno != EAGAIN) return -1;
        if (!notified) {
            if (write(ready, "R", 1) != 1) return -1;
            close(ready); notified = 1;
        }
        if (now_ns() >= deadline) return commit(output, &h);
        struct pollfd p = {.fd = input, .events = POLLIN};
        int r = poll(&p, 1, 100);
        if (r < 0 && errno != EINTR) return -1;
    }
}
static void halt(const char *reason) {
    dprintf(STDERR_FILENO, "HOS early logger stopped: %s (%s)\n", reason, strerror(errno));
    /* A missing/incorrect logger target must not silently start the experiment. */
    for (;;) pause();
}
int main(int argc, char **argv) {
    /* Host/Ubuntu tests only accept regular files and cannot run as PID 1. */
    if (getpid() != 1) {
        if (argc == 3 && !strcmp(argv[1], "--check-cmdline")) {
            char text[8192];
            return read_text(argv[2], text, sizeof(text)) || command_guard(text) ? 1 : 0;
        }
        if (argc == 4 && !strcmp(argv[1], "--capture-test")) {
            int out = open(argv[2], O_RDWR | O_CLOEXEC | O_NOFOLLOW);
            int in = open(argv[3], O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            struct stat a, b;
            if (out < 0 || in < 0 || fstat(out, &a) || fstat(in, &b) ||
                !S_ISREG(a.st_mode) || !S_ISREG(b.st_mode) || a.st_size != DISK_SIZE ||
                (a.st_dev == b.st_dev && a.st_ino == b.st_ino)) return 2;
            int r = capture(out, in, -1, 1);
            close(in); close(out);
            return r ? 3 : 0;
        }
        fprintf(stderr, "PID 1 required (or explicit regular-file test mode)\n");
        return 2;
    }
    if (mkdir("/hos-proc", 0700) || mkdir("/hos-sys", 0700) || mkdir("/hos-dev", 0700)) halt("mkdir");
    if (mount("proc", "/hos-proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, "") ||
        mount("sysfs", "/hos-sys", "sysfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, "") ||
        mount("tmpfs", "/hos-dev", "tmpfs", MS_NOSUID | MS_NOEXEC, "mode=0700")) halt("mount");
    char text[8192];
    if (read_text(CMDLINE_PATH, text, sizeof(text)) || command_guard(text)) halt("cmdline guard");
    dev_t target = 0;
    int found = 0;
    for (int i = 0; i < 300; i++) {
        if (!find_target(&target)) { found = 1; break; }
        usleep(100000);
    }
    if (!found) halt("isolated PARTNAME/PARTUUID not found uniquely");
    /* /dev/kmsg is major 1, minor 11; drivers/char/mem.c, memdevs[11]. */
    if (mknod("/hos-dev/misc_hos", S_IFBLK | 0600, target) ||
        mknod("/hos-dev/kmsg", S_IFCHR | 0600, makedev(1, 11))) halt("mknod");
    int out = open("/hos-dev/misc_hos", O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    uint64_t size = 0;
    if (out < 0 || ioctl(out, BLKGETSIZE64, &size) || size != DISK_SIZE) halt("block size guard");
    int input = open("/hos-dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    int pipefd[2];
    if (input < 0 || pipe2(pipefd, O_CLOEXEC)) halt("kmsg/pipe");
    pid_t child = fork();
    if (child < 0) halt("fork");
    if (!child) {
        close(pipefd[0]);
        int r = capture(out, input, pipefd[1], 0);
        close(input); close(out);
        _exit(r ? 1 : 0);
    }
    close(pipefd[1]); close(input); close(out);
    struct pollfd p = {.fd = pipefd[0], .events = POLLIN};
    char ready;
    if (poll(&p, 1, 30000) != 1 || read(pipefd[0], &ready, 1) != 1 || ready != 'R') halt("logger not ready");
    close(pipefd[0]);
    if (umount2("/hos-proc", MNT_DETACH) || umount2("/hos-sys", MNT_DETACH) ||
        umount2("/hos-dev", MNT_DETACH)) halt("detach mounts");
    if (rmdir("/hos-proc") || rmdir("/hos-sys") || rmdir("/hos-dev")) halt("remove mountpoints");
    argv[0] = "/init";
    execv("/init.hos.real", argv);
    halt("exec Android init");
    return 1;
}
