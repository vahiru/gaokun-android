/* Linux：删除一个分区（LiveCD 安装方式页的"删除分区"：删掉 Windows 恢复分区、华为一键恢复分区这类"磁盘管理"删不了的，
 * 腾出空间）。每次删一个、立即执行，前端事先单独确认。
 *
 *   gk3l delete --part /dev/X [--backup-dir DIR]
 *
 * 成功时 stdout 一行 RESULT op=delete part=…。进度：10 gpt-backup → 50 part-delete → 100 done。
 * 不删：ESP（删了它，盘上所有系统都无法启动；要重建 ESP 用整盘清空）、在用的分区（挂着、交换分区、上面叠着别的设备，
 * 首先是安装器所在的那个）。动手之前把分区表备份到安装介质。
 * 只清掉分区表里的那一项，不抹分区里的数据：分区表备份还原回去，数据就还在（Windows 的删除也是这样）。 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include "../gptw.h"
#include "../proto.h"
#include "../release.h"
#include "disk.h"
#include "files.h"
#include "lx.h"

#define ESP_TYPE "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"

int gk3l_cmd_delete(int argc, char **argv)
{
    const char *part = gk3l_arg(argc, argv, "--part");
    static gk3l_disk d;
    char p[256], real[4096], dname[64], *slash, medium[32] = "", what[400];
    struct stat st;
    uint64_t idx;
    const gk3l_kpart *k;
    gk3_gpt_part q;
    uint8_t *chk = NULL;
    gk3w_touched T = GK3W_TOUCHED_NO;
    int rc = 1, err, w;

    if (!part)
        return gk3l_err(T, "usage", "usage: delete --part /dev/X [--backup-dir DIR]", 0);
    /* 分区节点 → 整块盘、分区号（按 sysfs 找，不按名字猜） */
    if (stat(part, &st) || !S_ISBLK(st.st_mode))
        return gk3l_err_kv(T, "not-block", "dev", part, "not a block device", 0);
    snprintf(p, sizeof p, "/sys/dev/block/%u:%u/partition", major(st.st_rdev), minor(st.st_rdev));
    if (gk3l_read_u64(p, &idx))
        return gk3l_err_kv(T, "part-unknown", "part", part, "not a partition", 0);
    snprintf(p, sizeof p, "/sys/dev/block/%u:%u/..", major(st.st_rdev), minor(st.st_rdev));
    if (!realpath(p, real) || !(slash = strrchr(real, '/')) || strlen(slash + 1) >= sizeof dname)
        return gk3l_err_kv(T, "part-unknown", "part", part, "cannot tell which disk the partition belongs to", 0);
    snprintf(dname, sizeof dname, "%s", slash + 1);

    err = gk3l_disk_open(&d, dname, 1);
    if (err)
        return gk3l_err_kv(T, err == EACCES || err == EPERM ? "not-root" : "disk-open", "disk", dname, "cannot open the disk for writing", err);
    chk = gk3w_aligned_alloc(GK3L_ENTRIES_MAX);
    if (!chk) {
        gk3l_err(T, "nomem", "out of memory", ENOMEM);
        goto out;
    }
    if (gk3l_disk_lock(&d)) {
        gk3l_err(T, "disk-busy", "another program holds the disk (udev or another installer); try again (disk not touched)", errno);
        goto out;
    }
    if (gk3l_disk_read_table(&d)) {
        gk3l_err(T, "nomem", "out of memory", ENOMEM);
        goto out;
    }
    if (strcmp(d.table, "gpt")) {
        gk3l_err_kv(T, !strcmp(d.table, "mbr") ? "mbr-disk" : !strcmp(d.table, "unreadable") ? "disk-unreadable" : "disk-not-gpt", "table", d.table,
                    "the disk has no usable GPT partition table; only a whole-disk wipe can change it (disk not touched)", 0);
        goto out;
    }
    if (!gk3l_disk_kernel_ok(&d)) {
        gk3l_err(T, "kernel-stale", "the kernel's view of the partitions differs from the partition table on disk; reboot and try again (disk not touched)", 0);
        goto out;
    }
    k = gk3l_kernel_find(d.kp, d.nk, idx);
    if (!k || idx > d.g.num_entries || gk3_gpt_get(&d.g, (uint32_t)idx - 1, &q) != GK3_OK) {
        gk3l_err_kv(T, "part-unknown", "part", part, "the partition is not in the partition table (disk not touched)", 0);
        goto out;
    }
    {
        char type[37];
        gk3l_guid_upper(q.type_guid, type);
        if (!strcmp(type, ESP_TYPE)) {
            gk3l_err(T, "edit-esp", "this is the EFI system partition: without it no system on this disk can start (to rebuild it, wipe the whole disk) (disk not touched)", 0);
            goto out;
        }
    }
    gk3l_medium_devno(medium, sizeof medium);
    if ((medium[0] && !strcmp(k->devno, medium)) || gk3l_part_in_use(d.name, k, what, sizeof what)) {
        gk3l_err_kv(T, "edit-mounted", "part", part, "the partition is in use (is the installer running from it?) (disk not touched)", 0);
        goto out;
    }

    if (d.g.alt_lba >= d.blocks) {
        gk3l_err(T, "gpt-layout", "the backup partition table header lies beyond the end of the disk (disk not touched)", 0);
        goto out;
    }
    gk3w_progress(10, "gpt-backup");
    if (gk3l_gpt_backup(&d, gk3l_arg(argc, argv, "--backup-dir"), "delete", d.g.first_usable, d.blocks - (d.g.last_usable + 1)))
        goto out;
    {
        /* 界面按 part= 说"正在删除 …" */
        char enc[400];
        gk3w_enc(part, enc, sizeof enc);
        fprintf(stderr, "PROGRESS 50 part-delete part=%s\n", enc);
        fflush(stderr);
    }
    if (gk3w_gpt_remove(d.hdr, d.bs, d.ent, (size_t)d.g.num_entries * d.g.entry_size, (uint32_t)idx) != GK3W_GPTW_OK) {
        gk3l_err(T, "gpt-layout", "internal error: cannot remove the entry from the partition table (disk not touched)", 0);
        goto out;
    }
    /* 先让内核放掉这个分区：它在用（挂着、交换分区、被独占）时内核拒绝（EBUSY），这是最终的判据。
     * 放掉了再改分区表；改不成就把它加回去 */
    if ((err = gk3l_kernel_del(&d, idx))) {
        gk3l_err_kv(T, "edit-mounted", "part", part, "the kernel would not release the partition: it is in use (disk not touched)", err);
        goto out;
    }
    T = GK3W_TOUCHED_YES;
    w = gk3l_gpt_write(&d, d.hdr, d.ent, chk);
    if (w) {
        int e = errno;
        if (gk3l_kernel_add(&d, (uint32_t)idx, q.first_lba, q.last_lba))
            gk3l_log("warning: could not give the partition back to the kernel; reboot before using this disk");
        gk3l_err_kv(T, w == -1 ? "disk-write" : "disk-verify", "part", "gpt", w == -1 ? "writing the partition table failed (the backup is listed above)" : "the partition table did not read back identically (the backup is listed above)", e);
        goto out;
    }
    if (gk3l_disk_read_table(&d) || strcmp(d.table, "gpt") || gk3_gpt_get(&d.g, (uint32_t)idx - 1, &q) != GK3_ENOENT || !gk3l_disk_kernel_ok(&d)) {
        gk3l_err(T, "verify-layout", "after deleting, the partition table or the kernel's view is not as expected", 0);
        goto out;
    }
    gk3w_progress(100, "done");
    {
        gk3w_line l = {0};
        gk3w_line_begin(&l, "RESULT");
        gk3w_line_kv(&l, "op", "delete");
        gk3w_line_kv(&l, "part", part);
        rc = gk3w_line_emit(&l, stdout) ? 1 : 0;
        gk3w_line_free(&l);
    }
out:
    gk3w_aligned_free(chk);
    gk3l_disk_close(&d);
    return rc;
}

/* gk3l restore-table --file <备份> [--disk /dev/X]
 *
 * 把 gk3l 动盘之前存的分区表备份（gpt-before-<tag>-<盘>-<时间>-head<N>-tail<M>.bin：盘头 N 个扇区 + 盘尾 M 个扇区，原样）
 * 写回原位，逐段读回核对，再让内核重读分区表；有分区在用、重读不了时提示重启。盘名取自文件名，--disk 可以另指。
 * 文件大小、盘的扇区大小与盘尾位置对不上就拒绝：备份不是这块盘的。成功时 stdout 一行 RESULT op=restore-table disk=… reread=yes|no */
int gk3l_cmd_restore_table(int argc, char **argv)
{
    const char *file = gk3l_arg(argc, argv, "--file"), *disk = gk3l_arg(argc, argv, "--disk"), *base, *h;
    static gk3l_disk d;
    unsigned long long head = 0, tail = 0;
    char name[64] = "";
    uint8_t *data = NULL, *buf = NULL, *chk = NULL;
    size_t len = 0;
    gk3w_touched T = GK3W_TOUCHED_NO;
    int rc = 1, err, w = 0, reread;

    if (!file)
        return gk3l_err(T, "usage", "usage: restore-table --file gpt-before-...-headN-tailM.bin [--disk /dev/X]", 0);
    base = strrchr(file, '/') ? strrchr(file, '/') + 1 : file;
    h = strstr(base, "-head");
    if (strncmp(base, "gpt-before-", 11) || !h || sscanf(h, "-head%llu-tail%llu.bin", &head, &tail) != 2 || !head || !tail)
        return gk3l_err_kv(T, "usage", "file", file, "not a partition table backup made by this installer (gpt-before-...-headN-tailM.bin)", 0);
    if (disk) {
        if (gk3l_whole_disk_name(disk, name, sizeof name))
            return gk3l_err_kv(T, "disk-open", "disk", disk, "--disk is not a whole disk", 0);
    } else {
        /* gpt-before-<tag>-<盘>-<年月日>-<时分秒>-head…：盘名是 tag 之后、日期之前的那一段 */
        const char *a = strchr(base + 11, '-'), *b;
        b = a ? strchr(a + 1, '-') : NULL;
        if (!a || !b || (size_t)(b - a - 1) >= sizeof name || b == a + 1)
            return gk3l_err_kv(T, "usage", "file", file, "cannot tell the disk from the file name; pass --disk", 0);
        memcpy(name, a + 1, (size_t)(b - a - 1));
        name[b - a - 1] = 0;
    }
    if ((err = gk3l_read_file(file, &data, &len, 8u << 20)))
        return gk3l_err_kv(T, "restore-read", "file", file, "cannot read the backup file", err);
    err = gk3l_disk_open(&d, name, 1);
    if (err) {
        free(data);
        return gk3l_err_kv(T, err == EACCES || err == EPERM ? "not-root" : "disk-open", "disk", name, "cannot open the disk for writing", err);
    }
    /* 备份必须是这块盘的：长度 = (head + tail) × 扇区大小；GPT 的备份表头（在盘尾那段的最后一个扇区）指着这块盘的最后一个扇区 */
    if ((uint64_t)len != (head + tail) * d.bs || head + tail > d.blocks) {
        gk3l_err_kv(T, "restore-mismatch", "disk", d.path, "the backup does not match this disk (sector size or length differ) (disk not touched)", 0);
        goto out;
    }
    if (!memcmp(data + len - d.bs, "EFI PART", 8) && gk3_le64(data + len - d.bs + 24) != d.blocks - 1) {
        gk3l_err_kv(T, "restore-mismatch", "disk", d.path, "the backup was made on a disk of a different size (disk not touched)", 0);
        goto out;
    }
    if (gk3l_disk_lock(&d)) {
        gk3l_err(T, "disk-busy", "another program holds the disk; try again (disk not touched)", errno);
        goto out;
    }
    buf = gk3w_aligned_alloc(len);
    chk = gk3w_aligned_alloc(len);
    if (!buf || !chk) {
        gk3l_err(T, "nomem", "out of memory", ENOMEM);
        goto out;
    }
    memcpy(buf, data, len);
    T = GK3W_TOUCHED_YES;
    /* 先写盘尾（备份表）再写盘头（主表）：中途断电时主表还是现在的，固件与内核照它走 */
    w = gk3l_write_verify(&d, buf + head * d.bs, (size_t)(tail * d.bs), (d.blocks - tail) * d.bs, chk);
    if (!w)
        w = gk3l_write_verify(&d, buf, (size_t)(head * d.bs), 0, chk);
    if (w) {
        gk3l_err_kv(T, w == -1 ? "disk-write" : "disk-verify", "part", "gpt", w == -1 ? "writing the partition table back failed" : "the restored partition table did not read back identically", errno);
        goto out;
    }
    /* 让内核重读整张表；有分区在用（挂着）时内核拒绝，要重启 */
    reread = ioctl(d.fd, BLKRRPART) == 0;
    if (!reread)
        gk3l_log("the partition table is restored on the disk, but the kernel cannot re-read it while a partition is in use; reboot");
    {
        gk3w_line l = {0};
        gk3w_line_begin(&l, "RESULT");
        gk3w_line_kv(&l, "op", "restore-table");
        gk3w_line_kv(&l, "disk", d.path);
        gk3w_line_kv(&l, "reread", reread ? "yes" : "no");
        rc = gk3w_line_emit(&l, stdout) ? 1 : 0;
        gk3w_line_free(&l);
    }
out:
    free(data);
    gk3w_aligned_free(buf);
    gk3w_aligned_free(chk);
    gk3l_disk_close(&d);
    return rc;
}
