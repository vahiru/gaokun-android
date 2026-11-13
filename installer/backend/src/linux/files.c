/* gk3l：挂载与文件（说明见 files.h）。 */
#define _GNU_SOURCE
#include "files.h"

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <linux/msdos_fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *rundir(void)
{
    const char *d = getenv("GK3_RUNDIR");
    return d && *d ? d : "/run/gaokun3";
}

int gk3l_mount_tmp(const char *dev, const char *fstype, int ro, char *dir, size_t cap)
{
    int err;
    if ((err = gk3l_mkdirs(rundir())))
        return err;
    if ((size_t)snprintf(dir, cap, "%s/mnt.XXXXXX", rundir()) >= cap)
        return ENAMETOOLONG;
    if (!mkdtemp(dir))
        return errno;
    /* noexec / nosuid / nodev：挂的是别人的分区，上面的东西一概不执行 */
    if (mount(dev, dir, fstype, MS_NOSUID | MS_NODEV | MS_NOEXEC | (ro ? MS_RDONLY : 0), NULL)) {
        err = errno;
        rmdir(dir);
        return err;
    }
    return 0;
}

int gk3l_umount_tmp(const char *dir)
{
    if (umount(dir))
        return errno;
    rmdir(dir);
    return 0;
}

int gk3l_read_file(const char *path, uint8_t **data, size_t *len, size_t max)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC), err;
    struct stat st;
    uint8_t *b;
    size_t got = 0;
    *data = NULL;
    *len = 0;
    if (fd < 0)
        return errno;
    if (fstat(fd, &st)) {
        err = errno;
        close(fd);
        return err;
    }
    if (!S_ISREG(st.st_mode)) {
        close(fd);
        return EISDIR;
    }
    if ((uint64_t)st.st_size > max) {
        close(fd);
        return EFBIG;
    }
    b = malloc(st.st_size ? (size_t)st.st_size : 1);
    if (!b) {
        close(fd);
        return ENOMEM;
    }
    while (got < (size_t)st.st_size) {
        ssize_t n = read(fd, b + got, (size_t)st.st_size - got);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            err = n < 0 ? errno : EIO;
            free(b);
            close(fd);
            return err;
        }
        got += (size_t)n;
    }
    close(fd);
    *data = b;
    *len = got;
    return 0;
}

int64_t gk3l_file_size(const char *path)
{
    struct stat st;
    return stat(path, &st) || !S_ISREG(st.st_mode) ? -1 : (int64_t)st.st_size;
}

int gk3l_exists(const char *path)
{
    struct stat st;
    return !lstat(path, &st);
}

int gk3l_is_dir(const char *path)
{
    struct stat st;
    return !stat(path, &st) && S_ISDIR(st.st_mode);
}

static int write_fd(int fd, const uint8_t *data, size_t len)
{
    while (len) {
        ssize_t n = write(fd, data, len);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return n < 0 ? errno : EIO;
        data += n;
        len -= (size_t)n;
    }
    return fsync(fd) ? errno : 0;
}

int gk3l_write_file(const char *path, const uint8_t *data, size_t len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644), err;
    if (fd < 0)
        return errno;
    err = write_fd(fd, data, len);
    if (close(fd) && !err)
        err = errno;
    return err;
}

static int fsync_dir_of(const char *path)
{
    char d[4096], *slash;
    int fd, err = 0;
    snprintf(d, sizeof d, "%s", path);
    slash = strrchr(d, '/');
    if (!slash)
        return 0;
    *slash = 0;
    fd = open(d[0] ? d : "/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return errno;
    if (fsync(fd))
        err = errno;
    close(fd);
    return err;
}

int gk3l_replace_file(const char *path, const uint8_t *data, size_t len)
{
    char tmp[4096];
    int err;
    uint32_t attrs = 0;
    int have_attrs = 0;
    if ((size_t)snprintf(tmp, sizeof tmp, "%s.tmp", path) >= sizeof tmp)
        return ENAMETOOLONG;
    {
        /* FAT 的属性（隐藏、系统…）：改名会换上 .tmp 的目录项，先把原来的抄过去 */
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            have_attrs = ioctl(fd, FAT_IOCTL_GET_ATTRIBUTES, &attrs) == 0;
            close(fd);
        }
    }
    err = gk3l_write_file(tmp, data, len);
    if (!err && have_attrs) {
        int fd = open(tmp, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            (void)ioctl(fd, FAT_IOCTL_SET_ATTRIBUTES, &attrs);
            close(fd);
        }
    }
    if (!err && rename(tmp, path))
        err = errno;
    if (!err)
        err = fsync_dir_of(path);
    if (err)
        unlink(tmp);
    return err;
}

int gk3l_mkdirs(const char *path)
{
    char p[4096];
    size_t n = strlen(path);
    if (n >= sizeof p)
        return ENAMETOOLONG;
    memcpy(p, path, n + 1);
    for (size_t i = 1; i <= n; i++) {
        if (p[i] != '/' && p[i] != 0)
            continue;
        p[i] = 0;
        if (mkdir(p, 0755) && errno != EEXIST)
            return errno;
        if (i < n)
            p[i] = '/';
    }
    return gk3l_is_dir(path) ? 0 : ENOTDIR;
}

static int rm_one(const char *p, const struct stat *st, int flag, struct FTW *f)
{
    (void)st;
    (void)f;
    return (flag == FTW_DP ? rmdir(p) : unlink(p)) ? errno : 0;
}

int gk3l_rm_rf(const char *path)
{
    int r = nftw(path, rm_one, 16, FTW_DEPTH | FTW_PHYS);
    return r < 0 ? errno : r;
}

int gk3l_file_same(const char *path, const uint8_t *data, size_t len)
{
    uint8_t *old;
    size_t olen;
    int same;
    if (gk3l_read_file(path, &old, &olen, len + 1))
        return 0;
    same = olen == len && !memcmp(old, data, len);
    free(old);
    return same;
}
