/* Exercise only a disposable child: system UID writes a cross-UID migration.
 * Compare with/without CAP_SYS_NICE. Requires isolated HyperOS and root.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <linux/capability.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc != 2 || (strcmp(argv[1], "nice") && strcmp(argv[1], "no-nice"))) return 2;
    char cmdline[4096] = {0};
    int fd = open("/proc/cmdline", O_RDONLY);
    if (fd < 0) return 90;
    ssize_t len = read(fd, cmdline, sizeof(cmdline)-1);
    close(fd);
    if (len < 0 || !strstr(cmdline, "androidboot.force_super_partition=super_hos") || getuid()) return 90;
    int ready[2];
    if (pipe(ready)) return 3;
    pid_t child = fork();
    if (child < 0) return 3;
    if (!child) {
        close(ready[0]);
        if (setgroups(0, NULL) || setgid(2000) || setuid(2000)) _exit(4);
        char ok = 'Y';
        if (write(ready[1], &ok, 1) != 1) _exit(4);
        close(ready[1]);
        sleep(15); /* Bounded lifetime even if the parent fails. */
        _exit(0);
    }
    close(ready[1]);
    char ok = 0;
    ssize_t got = read(ready[0], &ok, 1);
    close(ready[0]);
    if (got != 1 || ok != 'Y') return 4;
    if (prctl(PR_SET_KEEPCAPS, 1L) || setgroups(0, NULL) || setgid(1000) || setuid(1000)) return 5;
    struct __user_cap_header_struct header = {_LINUX_CAPABILITY_VERSION_3, 0};
    struct __user_cap_data_struct caps[2] = {{0}, {0}};
    unsigned int mask = 1U << CAP_KILL;
    if (!strcmp(argv[1], "nice")) mask |= 1U << CAP_SYS_NICE;
    caps[0].effective = caps[0].permitted = mask;
    if (syscall(SYS_capset, &header, caps)) return 5;
    /* Open AFTER dropping UID: cgroup v1 checks the opener's credentials. */
    fd = open("/dev/cpuctl/foreground/cgroup.procs", O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        printf("OPEN FAILED uid=%u errno=%d\n", getuid(), errno);
        kill(child, SIGTERM);
        waitpid(child, NULL, 0);
        return 6;
    }
    char pid[32];
    int n = snprintf(pid, sizeof(pid), "%d", child);
    ssize_t written = write(fd, pid, n);
    int err = written < 0 ? errno : 0;
    close(fd);
    printf("mode=%s writer_uid=%u child_uid=2000 cap_eff=%x written=%zd errno=%d\n",
           argv[1], getuid(), mask, written, err);
    kill(child, SIGTERM);
    waitpid(child, NULL, 0);
    return written == n ? 0 : (err == EACCES ? 10 : 11);
}
