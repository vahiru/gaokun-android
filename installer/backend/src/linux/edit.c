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
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include "../gptw.h"
#include "../proto.h"
#include "../release.h"
#include "disk.h"
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
    gk3w_progress(50, "part-delete");
    if (gk3w_gpt_remove(d.hdr, d.bs, d.ent, (size_t)d.g.num_entries * d.g.entry_size, (uint32_t)idx) != GK3W_GPTW_OK) {
        gk3l_err(T, "gpt-layout", "internal error: cannot remove the entry from the partition table (disk not touched)", 0);
        goto out;
    }
    T = GK3W_TOUCHED_YES;
    w = gk3l_gpt_write(&d, d.hdr, d.ent, chk);
    if (w) {
        gk3l_err_kv(T, w == -1 ? "disk-write" : "disk-verify", "part", "gpt", w == -1 ? "writing the partition table failed (the backup is listed above)" : "the partition table did not read back identically (the backup is listed above)", errno);
        goto out;
    }
    if ((err = gk3l_kernel_del(&d, idx))) {
        gk3l_err_kv(T, "kernel-del", "part", part, "the partition is gone from the partition table, but the kernel would not drop it; reboot", err);
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
