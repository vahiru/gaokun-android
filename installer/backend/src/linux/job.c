/* Linux：写盘进程脱离界面运行。
 *
 * 问题：图形安装器起的子进程全在 gk3-installer.service 的 cgroup 里。界面一崩，服务停下，systemd 按默认的
 *   KillMode=control-group 把整个 cgroup 杀掉 —— 正在写 super 的那个进程一起死，留下写了一半的盘。
 * 做法：写盘的那一次调用放进一个独立的临时单元（systemd-run），输出写进文件、结束时写状态文件；界面只是"跟着读"。
 *   界面死了，单元照样跑完；界面重新起来，按 id 接着跟（status 找得到它）。
 *
 *   gk3l job run <命令> [参数…]     = start + follow：stdout / stderr / 退出码与直接调用逐行相同，只在 stderr 多一行
 *                                     JOB 记录（id 记进日志，界面重新起来时按它接着跟）
 *   gk3l job start <命令> [参数…]   起单元，立刻返回。stdout：JOB id=… state=running mode=systemd-run|setsid unit=…
 *   gk3l job follow <id>            转发这个 job 的输出（从头），等它结束，退出码 = job 的退出码；
 *                                     没写状态就没了（被杀 / 机器出错）⇒ ERR job-lost touched=yes，退出码 125
 *   gk3l job status [id]            每个 job 一行：JOB id=… cmd=… state=running|done|lost rc=… mode=… unit=…
 *   gk3l job body <目录> <命令> …   单元里跑的那段（内部用）
 *
 * job 目录 $GK3_RUNDIR/jobs/<id>/：cmd、started、mode、unit、pid、out、err、rc（写完才有）、start-error。
 * 单元里套一层 systemd-inhibit：写盘期间不许关机、睡眠、按电源键与合盖关机；锁跟着写盘进程活，不跟着界面活。
 * 不是 systemd 当 PID 1 的环境（容器）退回 setsid：只脱离进程组与终端。判据同 sd_booted(3)：/run/systemd/system/ 在不在。
 * job 的输出只在内存里（LiveCD 不往介质写，见 lx.h），需要时由另存日志（logs.c）带走。
 * 测试用：GK3_SYSTEMD_RUN=<假的 systemd-run>（同时当作"systemd 在跑"），GK3_JOB_POLL=<秒>。 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../proto.h"
#include "files.h"
#include "lx.h"

#define INHIBIT_WHAT "shutdown:sleep:handle-power-key:handle-suspend-key:handle-hibernate-key:handle-lid-switch"

static void jobdir(char *out, size_t cap)
{
    const char *run = getenv("GK3_RUNDIR");
    snprintf(out, cap, "%s/jobs", run && *run ? run : "/run/gaokun3");
}

static int put(const char *dir, const char *name, const char *text)
{
    char p[4200];
    if ((size_t)snprintf(p, sizeof p, "%s/%s", dir, name) >= sizeof p)
        return ENAMETOOLONG;
    return gk3l_write_file(p, (const uint8_t *)text, strlen(text));
}

static int get(const char *dir, const char *name, char *out, size_t cap)
{
    char p[4200];
    if ((size_t)snprintf(p, sizeof p, "%s/%s", dir, name) >= sizeof p)
        return -1;
    return gk3l_read_line(p, out, cap);
}

/* 在 PATH 里找可执行文件 */
static int which(const char *t, char *out, size_t cap)
{
    const char *path = getenv("PATH");
    char buf[4096], p[4200], *save = NULL;
    if (strchr(t, '/')) {
        snprintf(out, cap, "%s", t);
        return access(t, X_OK);
    }
    snprintf(buf, sizeof buf, "%s", path && *path ? path : "/usr/sbin:/usr/bin:/sbin:/bin");
    for (char *d = strtok_r(buf, ":", &save); d; d = strtok_r(NULL, ":", &save)) {
        if ((size_t)snprintf(p, sizeof p, "%s/%s", d, t) >= sizeof p)
            continue;
        if (!access(p, X_OK))
            return (size_t)snprintf(out, cap, "%s", p) >= cap ? -1 : 0;
    }
    return -1;
}

/* running / done / lost */
static const char *job_state(const char *d)
{
    uint64_t pid, started;
    char p[4200];
    snprintf(p, sizeof p, "%s/rc", d);
    if (gk3l_exists(p))
        return "done";
    snprintf(p, sizeof p, "%s/start-error", d);
    if (gk3l_exists(p))
        return "lost";
    snprintf(p, sizeof p, "%s/pid", d);
    if (gk3l_read_u64(p, &pid)) {
        /* 刚起、还没写 pid：给 30 秒，过了还没有就算没起来 */
        snprintf(p, sizeof p, "%s/started", d);
        return !gk3l_read_u64(p, &started) && (uint64_t)time(NULL) - started < 30 ? "running" : "lost";
    }
    if (pid && !kill((pid_t)pid, 0))
        return "running";
    snprintf(p, sizeof p, "%s/rc", d);
    return gk3l_exists(p) ? "done" : "lost";   /* 进程刚好在这两步之间写完 */
}

/* 单元里跑的那段：stdout / stderr 换成文件，pid 记下来，跑命令，写退出码 */
static int job_body(int argc, char **argv)
{
    const char *d = argc >= 2 ? argv[0] : NULL;
    char p[4200], pid[32], rc[16];
    int fo, fe, r;
    if (!d)
        return 2;
    snprintf(p, sizeof p, "%s/out", d);
    fo = open(p, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    snprintf(p, sizeof p, "%s/err", d);
    fe = open(p, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fo < 0 || fe < 0)
        return 2;
    fflush(stdout);
    fflush(stderr);
    dup2(fo, 1);
    dup2(fe, 2);
    close(fo);
    close(fe);
    snprintf(pid, sizeof pid, "%d\n", (int)getpid());
    put(d, "pid", pid);
    r = gk3l_dispatch(argc - 1, argv + 1);
    fflush(stdout);
    fflush(stderr);
    snprintf(rc, sizeof rc, "%d\n", r);
    /* 先写 .tmp 再改名：跟读的人看见 rc 时它一定是完整的 */
    snprintf(p, sizeof p, "%s/rc.tmp", d);
    if (!gk3l_write_file(p, (const uint8_t *)rc, strlen(rc))) {
        char q[4200];
        snprintf(q, sizeof q, "%s/rc", d);
        rename(p, q);
    }
    return r;
}

static int inhibit_ok(char *path, size_t cap)
{
    char *av[] = {path, "--what=" INHIBIT_WHAT, "--who=gaokun3 installer", "--why=probe", "true", NULL};
    if (which("systemd-inhibit", path, cap))
        return 0;
    return gk3l_run_quiet(av) == 0;
}

/* 起单元；成功时 id 填好，返回 0 */
static int job_start(int argc, char **argv, char *id, size_t idcap)
{
    char dir[4096], d[4400], self[4096], sr[4096], inh[4096], cwd[4096], buf[256], unit[128] = "-";
    const char *srenv = getenv("GK3_SYSTEMD_RUN");
    char *av[256];
    int a = 0, use_inh, systemd;
    uint8_t rnd[4] = {0};
    ssize_t n;
    time_t now = time(NULL);
    struct tm tmv;
    extern char **environ;

    if (argc < 1)
        return gk3l_err(GK3W_TOUCHED_NA, "usage", "usage: job start|run <command> [args...]", 0), -1;
    n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n <= 0)
        return gk3l_err(GK3W_TOUCHED_NO, "job-start", "cannot find the gk3l executable", errno), -1;
    self[n] = 0;
    jobdir(dir, sizeof dir);
    localtime_r(&now, &tmv);
    strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", &tmv);
    if (getentropy(rnd, sizeof rnd)) {
        rnd[0] = (uint8_t)getpid();
    }
    snprintf(id, idcap, "%.32s-%d-%02x%02x", buf, (int)getpid(), rnd[0], rnd[1]);
    snprintf(d, sizeof d, "%s/%s", dir, id);
    if (gk3l_mkdirs(d))
        return gk3l_err_kv(GK3W_TOUCHED_NO, "job-start", "dir", d, "cannot create the job directory", errno), -1;
    {
        char cmd[1024] = "";
        for (int i = 0; i < argc && strlen(cmd) + strlen(argv[i]) + 2 < sizeof cmd; i++) {
            if (i)
                strcat(cmd, " ");
            strcat(cmd, argv[i]);
        }
        strcat(cmd, "\n");
        put(d, "cmd", cmd);
    }
    snprintf(buf, sizeof buf, "%lld\n", (long long)now);
    put(d, "started", buf);
    if (!getcwd(cwd, sizeof cwd))
        snprintf(cwd, sizeof cwd, "/");
    use_inh = inhibit_ok(inh, sizeof inh);
    if (!use_inh)
        gk3l_log("warning: cannot take a systemd-inhibit lock (is logind running?); continuing. The power key and lid are still ignored by logind.conf.d, and the sleep targets are masked");
    systemd = srenv && *srenv ? !which(srenv, sr, sizeof sr) : (gk3l_is_dir("/run/systemd/system") && !which("systemd-run", sr, sizeof sr));
    if (systemd) {
        static char envs[64][512];
        int ne = 0;
        snprintf(unit, sizeof unit, "gk3-job-%.100s", id);
        av[a++] = sr;
        av[a++] = "--quiet";
        av[a++] = "--collect";
        av[a++] = "--service-type=exec";
        static char ua[160], da[200], wa[4200];
        snprintf(ua, sizeof ua, "--unit=%s", unit);
        snprintf(da, sizeof da, "--description=gaokun3 installer: %s", argv[0]);
        snprintf(wa, sizeof wa, "--working-directory=%s", cwd);
        av[a++] = ua;
        av[a++] = da;
        av[a++] = wa;
        /* 单元拿不到调用者的环境：GK3_* 一律带过去 */
        for (char **e = environ; *e && ne < 64; e++)
            if (!strncmp(*e, "GK3_", 4) && strlen(*e) < 500 && strchr(*e, '=')) {
                snprintf(envs[ne], sizeof envs[ne], "--setenv=%s", *e);
                av[a++] = envs[ne++];
            }
    }
    if (use_inh) {
        static char why[200];
        snprintf(why, sizeof why, "--why=writing the disk (%s)", argv[0]);
        av[a++] = inh;
        av[a++] = "--what=" INHIBIT_WHAT;
        av[a++] = "--who=gaokun3 installer";
        av[a++] = why;
        av[a++] = "--mode=block";
    }
    av[a++] = self;
    av[a++] = "job";
    av[a++] = "body";
    av[a++] = d;
    for (int i = 0; i < argc && a < 250; i++)
        av[a++] = argv[i];
    av[a] = NULL;
    if (systemd) {
        if (gk3l_run(av, NULL, 0) != 0) {
            put(d, "start-error", "systemd-run failed\n");
            return gk3l_err_kv(GK3W_TOUCHED_NO, "job-start", "unit", unit, "systemd-run cannot start the disk-writing unit", 0), -1;
        }
        put(d, "mode", "systemd-run\n");
    } else {
        /* 两次 fork + setsid：写盘进程不是我们的子进程，也不在我们的进程组里 */
        pid_t p;
        gk3l_log("warning: not a systemd system (or no systemd-run): the disk-writing process is only detached with setsid; it is still killed if the whole cgroup of the installer is");
        fflush(stdout);
        fflush(stderr);
        p = fork();
        if (p < 0)
            return gk3l_err(GK3W_TOUCHED_NO, "job-start", "fork failed", errno), -1;
        if (p == 0) {
            int nul = open("/dev/null", O_RDWR);
            setsid();
            if (fork())
                _exit(0);
            if (nul >= 0) {
                dup2(nul, 0);
                dup2(nul, 1);
                dup2(nul, 2);
            }
            execv(av[0], av);
            _exit(127);
        }
        while (waitpid(p, NULL, 0) < 0 && errno == EINTR)
            ;
        put(d, "mode", "setsid\n");
    }
    {
        char u[160];
        snprintf(u, sizeof u, "%s\n", unit);
        put(d, "unit", u);
    }
    return 0;
}

static void job_line(const char *id, const char *state, const char *mode, const char *unit, const char *cmd, const char *rc, FILE *fp)
{
    gk3w_line l = {0};
    gk3w_line_begin(&l, "JOB");
    gk3w_line_kv(&l, "id", id);
    if (cmd)
        gk3w_line_kv(&l, "cmd", cmd);
    gk3w_line_kv(&l, "state", state);
    if (rc)
        gk3w_line_kv(&l, "rc", rc);
    gk3w_line_kv(&l, "mode", mode);
    gk3w_line_kv(&l, "unit", unit);
    (void)gk3w_line_emit(&l, fp);
    gk3w_line_free(&l);
}

/* 跟着读：只转发完整的行（写盘进程写到一半的那行等它写完），stdout 对 stdout、stderr 对 stderr */
static int job_follow(const char *id)
{
    char dir[4096], d[4400];
    uint64_t off[2] = {0, 0};
    const char *pollenv = getenv("GK3_JOB_POLL");
    useconds_t poll_us = (useconds_t)((pollenv && *pollenv ? atof(pollenv) : 0.3) * 1e6);
    jobdir(dir, sizeof dir);
    snprintf(d, sizeof d, "%s/%s", dir, id);
    {
        char p[4500];
        snprintf(p, sizeof p, "%s/cmd", d);
        if (strchr(id, '/') || !gk3l_exists(p))
            return gk3l_err_kv(GK3W_TOUCHED_NA, "job-missing", "id", id, "no such job", 0);
    }
    for (;;) {
        const char *st = job_state(d);
        int final = strcmp(st, "running") != 0;
        for (int k = 0; k < 2; k++) {
            char p[4500];
            FILE *f;
            uint8_t *b = NULL;
            size_t n = 0;
            snprintf(p, sizeof p, "%s/%s", d, k ? "err" : "out");
            f = fopen(p, "re");
            if (!f)
                continue;
            if (!fseeko(f, (off_t)off[k], SEEK_SET)) {
                size_t cap = 1 << 16;
                b = malloc(cap);
                while (b) {
                    size_t r = fread(b + n, 1, cap - n, f);
                    n += r;
                    if (n < cap)
                        break;
                    {
                        uint8_t *nb = realloc(b, cap * 2);
                        if (!nb)
                            break;
                        b = nb;
                        cap *= 2;
                    }
                }
            }
            fclose(f);
            if (b && n) {
                size_t cut = n;
                if (!final) {
                    while (cut && b[cut - 1] != '\n')
                        cut--;
                }
                if (cut) {
                    FILE *o = k ? stderr : stdout;
                    fwrite(b, 1, cut, o);
                    if (final && b[cut - 1] != '\n')
                        fputc('\n', o);
                    fflush(o);
                    off[k] += cut;
                }
            }
            free(b);
        }
        if (!strcmp(st, "done")) {
            uint64_t rc = 1;
            char p[4500];
            snprintf(p, sizeof p, "%s/rc", d);
            gk3l_read_u64(p, &rc);
            return (int)rc;
        }
        if (!strcmp(st, "lost")) {
            gk3w_line l = {0};
            gk3w_line_begin(&l, "ERR");
            gk3w_line_kv(&l, "code", "job-lost");
            gk3w_line_kv(&l, "id", id);
            gk3w_fail(&l, GK3W_TOUCHED_YES, "the disk-writing process ended without writing its status (killed? machine error?); the disk may be half written");
            gk3w_line_free(&l);
            return 125;
        }
        usleep(poll_us);
    }
}

static int job_status(const char *only)
{
    char dir[4096];
    DIR *dd;
    struct dirent *e;
    jobdir(dir, sizeof dir);
    dd = opendir(dir);
    if (!dd)
        return 0;
    while ((e = readdir(dd))) {
        char d[4400], cmd[1024] = "", mode[32] = "-", unit[160] = "-", rc[16] = "-", p[4500];
        const char *st;
        if (e->d_name[0] == '.' || (only && strcmp(only, e->d_name)))
            continue;
        snprintf(d, sizeof d, "%s/%s", dir, e->d_name);
        snprintf(p, sizeof p, "%s/cmd", d);
        if (!gk3l_exists(p))
            continue;
        get(d, "cmd", cmd, sizeof cmd);
        get(d, "mode", mode, sizeof mode);
        get(d, "unit", unit, sizeof unit);
        get(d, "rc", rc, sizeof rc);
        st = job_state(d);
        job_line(e->d_name, st, mode, unit, cmd, rc, stdout);
    }
    closedir(dd);
    return 0;
}

int gk3l_cmd_job(int argc, char **argv)
{
    char id[128], d[4400], mode[32] = "-", unit[160] = "-", dir[4096];
    if (argc < 1)
        return gk3l_err(GK3W_TOUCHED_NA, "usage", "usage: job run|start|follow|status ...", 0);
    if (!strcmp(argv[0], "body"))
        return job_body(argc - 1, argv + 1);
    if (!strcmp(argv[0], "status"))
        return job_status(argc >= 2 ? argv[1] : NULL);
    if (!strcmp(argv[0], "follow")) {
        if (argc < 2)
            return gk3l_err(GK3W_TOUCHED_NA, "usage", "usage: job follow <id>", 0);
        return job_follow(argv[1]);
    }
    if (strcmp(argv[0], "start") && strcmp(argv[0], "run"))
        return gk3l_err(GK3W_TOUCHED_NA, "usage", "usage: job run|start|follow|status ...", 0);
    if (job_start(argc - 1, argv + 1, id, sizeof id))
        return 1;
    jobdir(dir, sizeof dir);
    snprintf(d, sizeof d, "%s/%s", dir, id);
    get(d, "mode", mode, sizeof mode);
    get(d, "unit", unit, sizeof unit);
    /* run：JOB 记录打在 stderr 上，不混进 stdout 的记录流 */
    job_line(id, "running", mode, unit, NULL, NULL, strcmp(argv[0], "run") ? stdout : stderr);
    if (!strcmp(argv[0], "start"))
        return 0;
    return job_follow(id);
}
