/* gk3l 的参数、报错、sysfs 与外部命令工具（说明见 lx.h）。 */
#define _GNU_SOURCE
#include "lx.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* ── 参数 ── */

const char *gk3l_arg(int argc, char **argv, const char *key)
{
    for (int i = 0; i + 1 < argc; i++)
        if (!strcmp(argv[i], key))
            return argv[i + 1];
    return NULL;
}

int gk3l_flag(int argc, char **argv, const char *key)
{
    for (int i = 0; i < argc; i++)
        if (!strcmp(argv[i], key))
            return 1;
    return 0;
}

int gk3l_arg_u64(int argc, char **argv, const char *key, uint64_t *out)
{
    const char *v = gk3l_arg(argc, argv, key);
    char *end = NULL;
    if (!v || *v < '0' || *v > '9')
        return -1;
    errno = 0;
    *out = strtoull(v, &end, 10);
    return (errno || !end || *end) ? -1 : 0;
}

/* ── 失败与日志 ── */

int gk3l_err_kv(gk3w_touched t, const char *code, const char *k, const char *v, const char *human, int err)
{
    gk3w_line l = {0};
    gk3w_line_begin(&l, "ERR");
    gk3w_line_kv(&l, "code", code);
    if (k)
        gk3w_line_kv(&l, k, v);
    if (err > 0)
        gk3w_line_u64(&l, "errno", (uint64_t)err);
    gk3w_fail(&l, t, human);
    gk3w_line_free(&l);
    return 1;
}

int gk3l_err_kv2(gk3w_touched t, const char *code, const char *k1, const char *v1, const char *k2, const char *v2, const char *human,
                 int err)
{
    gk3w_line l = {0};
    gk3w_line_begin(&l, "ERR");
    gk3w_line_kv(&l, "code", code);
    gk3w_line_kv(&l, k1, v1);
    if (k2)
        gk3w_line_kv(&l, k2, v2);
    if (err > 0)
        gk3w_line_u64(&l, "errno", (uint64_t)err);
    gk3w_fail(&l, t, human);
    gk3w_line_free(&l);
    return 1;
}

int gk3l_err(gk3w_touched t, const char *code, const char *human, int err)
{
    return gk3l_err_kv(t, code, NULL, NULL, human, err);
}

void gk3l_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

/* ── 小文件 ── */

int gk3l_read_line(const char *path, char *out, size_t cap)
{
    FILE *f;
    size_t n;
    if (!cap)
        return -1;
    out[0] = 0;
    f = fopen(path, "re");
    if (!f)
        return -1;
    if (!fgets(out, (int)(cap > 0x7fffffff ? 0x7fffffff : cap), f)) {
        out[0] = 0;
        fclose(f);
        return -1;
    }
    fclose(f);
    n = strlen(out);
    while (n && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' ' || out[n - 1] == '\t'))
        out[--n] = 0;
    return 0;
}

int gk3l_read_u64(const char *path, uint64_t *out)
{
    char buf[32], *end = NULL;
    if (gk3l_read_line(path, buf, sizeof buf) || buf[0] < '0' || buf[0] > '9')
        return -1;
    errno = 0;
    *out = strtoull(buf, &end, 10);
    return (errno || !end || *end) ? -1 : 0;
}

/* ── 外部命令 ── */

int gk3l_run(char *const argv[], char *out, size_t cap)
{
    int pfd[2] = {-1, -1}, st = 0;
    size_t got = 0;
    pid_t pid;

    if (out && cap)
        out[0] = 0;
    fflush(stdout);
    fflush(stderr);
    if (out && pipe2(pfd, O_CLOEXEC))
        return -1;
    pid = fork();
    if (pid < 0) {
        if (out) {
            close(pfd[0]);
            close(pfd[1]);
        }
        return -1;
    }
    if (pid == 0) {
        int nul = open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (nul >= 0)
            dup2(nul, 0);
        /* 子进程的 stdout 绝不能进我们的 stdout（那是协议记录）：要收就接管道，不收就并进 stderr（进安装日志） */
        dup2(out ? pfd[1] : 2, 1);
        execvp(argv[0], argv);
        _exit(127);
    }
    if (out) {
        char sink[512];
        close(pfd[1]);
        for (;;) {
            ssize_t n;
            if (got + 1 < cap)
                n = read(pfd[0], out + got, cap - 1 - got);
            else
                n = read(pfd[0], sink, sizeof sink);   /* 超出部分读掉丢弃，别让子进程卡在写管道上 */
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                break;
            if (got + 1 < cap)
                got += (size_t)n;
        }
        close(pfd[0]);
        if (cap)
            out[got] = 0;
    }
    while (waitpid(pid, &st, 0) < 0)
        if (errno != EINTR)
            return -1;
    if (WIFEXITED(st))
        return WEXITSTATUS(st) == 127 ? -1 : WEXITSTATUS(st);
    if (WIFSIGNALED(st))
        return 128 + WTERMSIG(st);
    return -1;
}

int gk3l_run_quiet(char *const argv[])
{
    int nul = open("/dev/null", O_WRONLY | O_CLOEXEC), saved, r;
    if (nul < 0)
        return gk3l_run(argv, NULL, 0);
    fflush(stderr);
    saved = dup(2);
    dup2(nul, 2);
    close(nul);
    r = gk3l_run(argv, NULL, 0);
    dup2(saved, 2);
    close(saved);
    return r;
}

/* ── 安装介质 ── */

/* mountinfo 里的路径把空格、制表符、换行、反斜杠写成 \ooo（八进制） */
static void unescape_octal(char *s)
{
    char *w = s;
    for (char *r = s; *r; r++) {
        if (r[0] == '\\' && r[1] >= '0' && r[1] <= '3' && r[2] >= '0' && r[2] <= '7' && r[3] >= '0' && r[3] <= '7') {
            *w++ = (char)((r[1] - '0') * 64 + (r[2] - '0') * 8 + (r[3] - '0'));
            r += 3;
        } else {
            *w++ = *r;
        }
    }
    *w = 0;
}

int gk3l_mount_of(const char *devno, char *mnt_out, size_t cap)
{
    FILE *f = fopen("/proc/self/mountinfo", "re");
    char *line = NULL;
    size_t lcap = 0;
    int found = -1;
    if (!f)
        return -1;
    while (getline(&line, &lcap, f) > 0) {
        char dn[32], mnt[4096];
        if (sscanf(line, "%*s %*s %31s %*s %4095s", dn, mnt) != 2 || strcmp(dn, devno))
            continue;
        unescape_octal(mnt);
        if (strlen(mnt) < cap) {
            strcpy(mnt_out, mnt);
            found = 0;
            break;
        }
    }
    free(line);
    fclose(f);
    return found;
}

int gk3l_devno_at(const char *dir, char *out, size_t cap)
{
    FILE *f;
    char *line = NULL;
    size_t lcap = 0;
    int found = -1;

    f = fopen("/proc/self/mountinfo", "re");
    if (!f)
        return -1;
    /* 36 35 98:0 /mnt1 /mnt2 rw,noatime master:1 - ext3 /dev/root rw,errors=continue
     * 同一挂载点挂了几层时以最后一行（最上层）为准 */
    while (getline(&line, &lcap, f) > 0) {
        char devno[32], mnt[4096];
        if (sscanf(line, "%*s %*s %31s %*s %4095s", devno, mnt) != 2)
            continue;
        unescape_octal(mnt);
        if (strcmp(mnt, dir))
            continue;
        if (strlen(devno) < cap) {
            strcpy(out, devno);
            found = 0;
        }
    }
    free(line);
    fclose(f);
    return found;
}
