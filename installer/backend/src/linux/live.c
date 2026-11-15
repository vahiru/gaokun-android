/* gk3l：安装器分区（说明见 live.h）。
 *
 *   gk3l remove-installer --disk /dev/X
 *   gk3l apply … --remove-installer    装完之后的最后一步，做同样的事
 *
 * 删掉 X 上的安装器分区；userdata 紧挨在它前面时把空出来的空间并进 userdata（GPT 表项、内核的分区、ext4 一起扩）。
 *   1. 核对：分区表是 gpt 且与内核一致；安装器分区在这块盘上、读进了内存、没在用；这块盘有且只有一个 ESP、没有挂着。
 *   2. 备份分区表（与安装时的一样只放在内存里，见 lx.h）。
 *   3. 内核放掉安装器分区（在用时拒绝）→ 主表去掉它、userdata 的表项改大 → 写主表与备份表并读回核对。
 *   4. 内核里的 userdata 改大（BLKPG_RESIZE_PARTITION，不行就去掉再加）→ e2fsck -f -p → resize2fs。
 *      文件系统没扩成只提醒（NOTE code=userdata-fs-not-grown）：分区比文件系统大不影响使用。
 *   5. ESP 上只为启动安装器分区而写的东西删掉：旧版的 Windows 脚本把 LiveCD 的内核放在
 *      EFI/gaokun3/、启动项是 loader/entries/gaokun3-live*.conf。安装器分区没了，它们只会启动失败（XBOOTLDR 的
 *      GK3LIVE 自带启动项，ESP 上没有这些）。
 *   6. ESP 上没有 Android 的启动项时（只写入过本机磁盘的安装介质、还没装 Android），把 Windows 安装器为它对 ESP 做的
 *      改动撤回，规则同卸载（src/win/uninstall.c 开头的 1–2）：ESP 上有别的系统（loader/entries 里有不是我们写的启动项，
 *      或 EFI/Linux 下有 UKI）时只把 loader.conf 的 .before-gaokun3 换回来；否则回落路径按 gk3w_fallback_decide 换回
 *      （写 .tmp 再改名、读回核对），删掉 loader/ 与 EFI/systemd/，最后删 .before-gaokun3。有 Android 时不动（那是
 *      Android 的启动链）。Windows 在休眠时不改（同 apply 的 esp-windows-hibernated）。LiveCD 暂停不了 BitLocker：
 *      回落路径换回之后 Windows 可能要恢复密钥（风险确认页写明）。
 *      第 5、6 步失败都只提醒：安装器分区已经删了，留下的至多是一个只有 Windows 的开机菜单。第 6 步没有做成时另报
 *      NOTE code=esp-kept why=hibernated|foreign|unreadable|no-windows|write-failed（界面据此如实说明）。
 * stdout：BACKUP；REMOVED part= name= size_mib= grow=yes|no userdata_mib= esp=。
 * 进度代码：PROGRESS 92 remove-installer（apply 里）/ 10、50、80、100（命令）。 */
#define _GNU_SOURCE
#include "live.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "../esp.h"
#include "../proto.h"
#include "../release.h"
#include "files.h"
#include "lx.h"
#include "sysstate.h"

#define ESP_TYPE "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"

#define XBOOTLDR_TYPE "BC13C2FF-59E6-4262-A352-B275FD6F7172"

int gk3l_live_like(const char *type, const char *fs, const char *label, const char *name)
{
    if (strcmp(fs, "vfat") || !strcmp(type, ESP_TYPE))
        return 0;
    return !strcmp(type, XBOOTLDR_TYPE) || !strcasecmp(label, "GK3LIVE") || !strcmp(name, "GK3LIVE");
}

int gk3l_live_find(const gk3l_disk *d, gk3l_live *lv)
{
    char devno[32], type[37], fs[16], label[256], node[96];
    const gk3l_kpart *k = NULL;
    gk3_gpt_part q;
    uint64_t f = d->bs / 512;
    memset(lv, 0, sizeof *lv);
    if (strcmp(d->table, "gpt") || !gk3l_disk_internal(d->name) || !gk3l_medium_ram() || gk3l_medium_devno(devno, sizeof devno))
        return 0;
    for (size_t i = 0; i < d->nk; i++)
        if (!strcmp(d->kp[i].devno, devno))
            k = &d->kp[i];
    if (!k || !k->num || k->num > d->g.num_entries || gk3_gpt_get(&d->g, (uint32_t)k->num - 1, &q) != GK3_OK)
        return 0;
    /* 内核里的那个分区与表项要是同一段 */
    if (q.first_lba * f != k->start512 || (q.last_lba - q.first_lba + 1) * f != k->size512)
        return 0;
    gk3l_gpt_name_utf8(&d->g, q.index - 1, lv->name, sizeof lv->name);
    gk3l_guid_upper(q.type_guid, type);
    snprintf(node, sizeof node, "/dev/%s", k->name);
    gk3l_blkid(node, fs, sizeof fs, label, sizeof label);
    if (!gk3l_live_like(type, fs, label, lv->name)) {
        memset(lv, 0, sizeof *lv);
        return 0;
    }
    lv->idx = q.index;
    lv->first = q.first_lba;
    lv->last = q.last_lba;
    memcpy(lv->raw, d->ent + (size_t)(q.index - 1) * d->g.entry_size, GK3W_GPT_ENTRY_SIZE);
    lv->k = *k;
    return 1;
}

uint64_t gk3l_live_grow_to(const gk3w_span *parts, size_t n, gk3w_span ud, gk3w_span lv, uint64_t last_usable, uint32_t bs)
{
    uint64_t limit = last_usable, per_mib = (1u << 20) / bs, size;
    if (ud.last >= lv.first || ud.first > ud.last || lv.last > last_usable)
        return 0;
    for (size_t i = 0; i < n; i++) {
        if ((parts[i].first == ud.first && parts[i].last == ud.last) || (parts[i].first == lv.first && parts[i].last == lv.last))
            continue;
        /* 中间夹着别的分区 */
        if (parts[i].last > ud.last && parts[i].first < lv.first)
            return 0;
        if (parts[i].first > lv.last && parts[i].first - 1 < limit)
            limit = parts[i].first - 1;
    }
    size = (limit - ud.first + 1) / per_mib * per_mib;
    return ud.first + size - 1 > ud.last ? ud.first + size - 1 : 0;
}

void gk3l_live_preview(const gk3l_disk *d, const gk3l_live *lv, const gk3w_span *parts, size_t n, gk3w_span ud, FILE *fp)
{
    gk3w_line l = {0};
    uint64_t per_mib = (1u << 20) / d->bs;
    uint64_t to = gk3l_live_grow_to(parts, n, ud, (gk3w_span){lv->first, lv->last}, d->g.last_usable, d->bs);
    char node[96];
    snprintf(node, sizeof node, "/dev/%s", lv->k.name);
    gk3w_line_begin(&l, "PLAN");
    gk3w_line_kv(&l, "op", "rmlive");
    gk3w_line_kv(&l, "part", node);
    gk3w_line_kv(&l, "name", lv->name);
    gk3w_line_u64(&l, "start", lv->first);
    gk3w_line_u64(&l, "end", lv->last);
    gk3w_line_u64(&l, "size_mib", (lv->last - lv->first + 1) / per_mib);
    gk3w_line_kv(&l, "merge", to ? "yes" : "no");
    gk3w_line_u64(&l, "userdata_mib", ((to ? to : ud.last) - ud.first + 1) / per_mib);
    (void)gk3w_line_emit(&l, fp);
    gk3w_line_free(&l);
}

/* 动盘之前的失败：apply 的最后一步里只提醒（安装本身已经成了），单独的命令里报 ERR。k2 可为 NULL */
static int refuse2(int as_note, const char *code, const char *k, const char *v, const char *k2, const char *v2, const char *human, int err)
{
    if (as_note) {
        gk3w_line l = {0};
        gk3l_log("the installer partition is kept: %s", human);
        gk3w_line_begin(&l, "NOTE");
        gk3w_line_kv(&l, "code", "installer-kept");
        gk3w_line_kv(&l, "why", code);
        if (k)
            gk3w_line_kv(&l, k, v);
        if (k2)
            gk3w_line_kv(&l, k2, v2);
        (void)gk3w_line_emit(&l, stdout);
        gk3w_line_free(&l);
        return 1;
    }
    if (k)
        gk3l_err_kv2(GK3W_TOUCHED_NO, code, k, v, k2, v2, human, err);
    else
        gk3l_err(GK3W_TOUCHED_NO, code, human, err);
    return -1;
}

static int refuse(int as_note, const char *code, const char *k, const char *v, const char *human, int err)
{
    return refuse2(as_note, code, k, v, NULL, NULL, human, err);
}

static int have_tool(const char *t)
{
    char p[4096];
    const char *path = getenv("PATH");
    char *dup, *save = NULL;
    int found = 0;
    if (!path)
        path = "/usr/sbin:/usr/bin:/sbin:/bin";
    dup = strdup(path);
    for (char *dir = dup ? strtok_r(dup, ":", &save) : NULL; dir && !found; dir = strtok_r(NULL, ":", &save)) {
        snprintf(p, sizeof p, "%s/%s", dir, t);
        found = access(p, X_OK) == 0;
    }
    free(dup);
    return found;
}

/* ext4 扩到分区大小。返回 0；没扩成返回 -1（已报 NOTE） */
static int grow_fs(const char *node)
{
    char fs[16], label[256];
    const char *why = NULL;
    int r;
    gk3l_blkid(node, fs, sizeof fs, label, sizeof label);
    if (strcmp(fs, "ext4")) {
        why = fs[0] ? "not-ext4" : "no-fs";
    } else if (!have_tool("e2fsck") || !have_tool("resize2fs")) {
        why = "tool-missing";
    } else {
        /* -p：只修不用问的问题；0 = 没问题，1 = 修好了，更大的数要人来处理 —— 那时不扩 */
        char *ck[] = {"e2fsck", "-f", "-p", (char *)node, NULL};
        char *rs[] = {"resize2fs", (char *)node, NULL};
        r = gk3l_run(ck, NULL, 0);
        if (r != 0 && r != 1)
            why = "fsck";
        else if (gk3l_run(rs, NULL, 0))
            why = "resize2fs";
    }
    if (why) {
        gk3w_line l = {0};
        gk3l_log("warning: the userdata partition was enlarged, but its file system was not (%s); it keeps its old size", why);
        gk3w_line_begin(&l, "NOTE");
        gk3w_line_kv(&l, "code", "userdata-fs-not-grown");
        gk3w_line_kv(&l, "why", why);
        (void)gk3w_line_emit(&l, stdout);
        gk3w_line_free(&l);
        return -1;
    }
    gk3l_log("userdata: file system enlarged to fill the partition (e2fsck -f -p, resize2fs)");
    return 0;
}

/* loader/entries/gaokun3-live.conf 或 gaokun3-live-<数字>.conf（不分大小写：FAT） */
static int is_live_entry(const char *n)
{
    const char *p = "gaokun3-live";
    size_t i;
    for (i = 0; p[i]; i++)
        if (!n[i] || (n[i] | 0x20) != p[i])
            return 0;
    n += i;
    if (*n == '-') {
        if (n[1] < '0' || n[1] > '9')
            return 0;
        for (n++; *n >= '0' && *n <= '9'; n++)
            ;
    }
    return !strcasecmp(n, ".conf");
}

/* 第 5 步：删不掉只提醒（安装器分区已经删了，这些条目启动时会失败、但不碍事） */
static void esp_drop_live_entries(const char *esp_node)
{
    char mnt[4096], p[4400];
    DIR *dd;
    struct dirent *e;
    int n = 0;
    if (gk3l_mount_tmp(esp_node, "vfat", 0, mnt, sizeof mnt)) {
        gk3l_log("warning: cannot mount the EFI system partition to remove the old LiveCD boot entries");
        return;
    }
    snprintf(p, sizeof p, "%s/loader/entries", mnt);
    if ((dd = opendir(p))) {
        while ((e = readdir(dd))) {
            if (!is_live_entry(e->d_name))
                continue;
            snprintf(p, sizeof p, "%s/loader/entries/%s", mnt, e->d_name);
            if (unlink(p))
                gk3l_log("warning: cannot delete the old LiveCD boot entry %s", e->d_name);
            else
                n++;
        }
        closedir(dd);
    }
    snprintf(p, sizeof p, "%s/EFI/gaokun3", mnt);
    if (gk3l_is_dir(p)) {
        if (gk3l_rm_rf(p))
            gk3l_log("warning: cannot delete EFI/gaokun3 (the old LiveCD kernel)");
        else
            n++;
    }
    sync();
    if (gk3l_umount_tmp(mnt))
        gk3l_log("warning: cannot unmount the EFI system partition");
    if (n)
        gk3l_log("ESP: removed the old LiveCD boot entry and kernel (they only started the installer partition)");
}

static int ends_with_ci(const char *n, const char *suf)
{
    size_t a = strlen(n), b = strlen(suf);
    return a >= b && !strcasecmp(n + a - b, suf);
}

/* ESP 上还有我们为启动安装器分区留下的东西：回落路径是 systemd-boot，或 loader.conf 是我们写的 */
static int esp_has_ours(const char *mnt)
{
    char p[4400];
    uint8_t *b = NULL;
    size_t n = 0;
    int ours = 0;
    snprintf(p, sizeof p, "%s/EFI/BOOT/BOOTAA64.EFI", mnt);
    if (!gk3l_read_file(p, &b, &n, 64u << 20))
        ours = gk3w_is_systemd_boot(b, n);
    free(b);
    b = NULL;
    snprintf(p, sizeof p, "%s/loader/loader.conf", mnt);
    if (!ours && !gk3l_read_file(p, &b, &n, 1u << 20))
        ours = gk3w_is_our_loader_conf(b, n);
    free(b);
    return ours;
}

/* 第 6 步没有做成：ESP 上还留着我们的东西时报给界面（没有就不用说：本来就是原样） */
static void note_esp_kept(const char *mnt, const char *why)
{
    gk3w_line l = {0};
    if (!esp_has_ours(mnt))
        return;
    gk3w_line_begin(&l, "NOTE");
    gk3w_line_kv(&l, "code", "esp-kept");
    gk3w_line_kv(&l, "why", why);
    (void)gk3w_line_emit(&l, stdout);
    gk3w_line_free(&l);
}

/* 第 6 步（说明见文件头）。读不到的文件按"不存在"处理之前先确认确实不存在：读错误时整步放弃 */
static void esp_restore_no_android(const char *esp_node, const char *disk)
{
    char mnt[4096], p[4400], q[4500];   /* q 有时是 p 加后缀 */
    DIR *dd;
    struct dirent *e;
    int android = 0, foreign = 0, keep_bak = 0, err = 0;
    uint8_t *cur = NULL, *w = NULL, *bak = NULL;
    const uint8_t *target = NULL;
    size_t cur_len = 0, w_len = 0, bak_len = 0, target_len = 0;
    static const char *const rels[3] = {"EFI/BOOT/BOOTAA64.EFI", "EFI/Microsoft/Boot/bootmgfw.efi", "EFI/BOOT/BOOTAA64.EFI.before-gaokun3"};
    uint8_t **bufs[3] = {&cur, &w, &bak};
    size_t *lens[3] = {&cur_len, &w_len, &bak_len};
    if (gk3l_mount_tmp(esp_node, "vfat", 0, mnt, sizeof mnt)) {
        gk3l_log("warning: cannot mount the EFI system partition to restore it");
        return;
    }
    snprintf(p, sizeof p, "%s/loader/entries", mnt);
    if ((dd = opendir(p))) {
        while ((e = readdir(dd))) {
            if (e->d_name[0] == '.' || !ends_with_ci(e->d_name, ".conf"))
                continue;
            if (strcasestr(e->d_name, "-android-"))
                android = 1;
            else if (!gk3w_our_entry_name(e->d_name))
                foreign++;
        }
        closedir(dd);
    }
    snprintf(p, sizeof p, "%s/EFI/Linux", mnt);
    if ((dd = opendir(p))) {
        while ((e = readdir(dd)))
            if (e->d_name[0] != '.' && ends_with_ci(e->d_name, ".efi"))
                foreign++;
        closedir(dd);
    }
    if (android)
        goto done;   /* 那是 Android 的启动链 */
    {
        gk3l_win ws;
        gk3l_win_state(disk, &ws);
        if (ws.hibernated == 1) {
            gk3l_log("warning: Windows is hibernated or in Fast Startup (%s): the EFI system partition is left as it is", ws.hib_part);
            note_esp_kept(mnt, "hibernated");
            goto done;
        }
        if (ws.hibernated < 0)
            gk3l_log("warning: cannot tell whether Windows is hibernated; make sure Windows was shut down with Fast Startup off");
    }
    if (foreign) {
        snprintf(p, sizeof p, "%s/loader/loader.conf", mnt);
        snprintf(q, sizeof q, "%s.before-gaokun3", p);
        if (gk3l_exists(q)) {
            if (rename(q, p))
                gk3l_log("warning: cannot put loader/loader.conf.before-gaokun3 back (%s)", strerror(errno));
            else
                gk3l_log("ESP: loader/loader.conf restored from its .before-gaokun3 copy");
        }
        gk3l_log("ESP is shared with another system: the fallback EFI/BOOT/BOOTAA64.EFI, loader/ and EFI/systemd stay as they are");
        note_esp_kept(mnt, "foreign");
        goto done;
    }
    for (int k = 0; k < 3; k++) {
        snprintf(p, sizeof p, "%s/%s", mnt, rels[k]);
        err = gk3l_read_file(p, bufs[k], lens[k], 64u << 20);
        if (err == ENOENT)
            err = 0;
        if (err) {
            gk3l_log("warning: cannot read %s on the EFI system partition (%s); the ESP is left as it is", rels[k], strerror(err));
            note_esp_kept(mnt, "unreadable");
            goto done;
        }
    }
    switch (gk3w_fallback_decide(cur, cur_len, w, w_len, bak, bak_len)) {
    case GK3W_FB_LEAVE_OTHER:
        keep_bak = !gk3w_fallback_bak_disposable(bak, bak_len, w, w_len);
        gk3l_log("EFI/BOOT/BOOTAA64.EFI holds another system's boot loader: left as it is");
        break;
    case GK3W_FB_RESTORE_ORIGINAL:
    case GK3W_FB_USE_BAK:
        target = bak, target_len = bak_len;
        break;
    case GK3W_FB_USE_W:
        target = w, target_len = w_len;
        break;
    case GK3W_FB_NO_WINDOWS:
        gk3l_log("warning: EFI/BOOT/BOOTAA64.EFI is systemd-boot and there is neither the Windows Boot Manager nor a usable backup to put back; the ESP is left as it is");
        note_esp_kept(mnt, "no-windows");
        goto done;
    case GK3W_FB_UNTOUCHED:
        break;
    }
    snprintf(p, sizeof p, "%s/EFI/BOOT/BOOTAA64.EFI", mnt);
    if (target && !(cur && cur_len == target_len && !memcmp(cur, target, target_len))) {
        snprintf(q, sizeof q, "%s/EFI/BOOT", mnt);
        if ((err = gk3l_mkdirs(q)) || (err = gk3l_replace_file(p, target, target_len)) || !gk3l_file_same(p, target, target_len)) {
            gk3l_log("warning: restoring EFI/BOOT/BOOTAA64.EFI failed (%s); the rest of the ESP is left as it is", err ? strerror(err) : "read-back mismatch");
            note_esp_kept(mnt, "write-failed");
            goto done;
        }
        gk3l_log("ESP: EFI/BOOT/BOOTAA64.EFI restored (%s)", target == w ? "the Windows Boot Manager" : "the file it held before");
    }
    /* systemd-boot 的配置与副本、中断留下的临时文件；最后才删备份（前面失败时它还在，重跑仍有还原的来源） */
    snprintf(q, sizeof q, "%s/loader", mnt);
    if (gk3l_is_dir(q) && gk3l_rm_rf(q))
        err = 1;
    snprintf(q, sizeof q, "%s/EFI/systemd", mnt);
    if (gk3l_is_dir(q) && gk3l_rm_rf(q))
        err = 1;
    snprintf(q, sizeof q, "%s.tmp", p);
    unlink(q);
    snprintf(q, sizeof q, "%s.before-gaokun3.tmp", p);
    unlink(q);
    snprintf(q, sizeof q, "%s.before-gaokun3", p);
    if (!err && !keep_bak && gk3l_exists(q) && unlink(q))
        err = 1;
    if (err)
        gk3l_log("warning: some of loader/, EFI/systemd or EFI/BOOT/BOOTAA64.EFI.before-gaokun3 could not be removed from the ESP");
    else
        gk3l_log("ESP: restored to how it was before the installer was written to this disk (no Android on it)");
done:
    free(cur);
    free(w);
    free(bak);
    sync();
    if (gk3l_umount_tmp(mnt))
        gk3l_log("warning: cannot unmount the EFI system partition");
}

int gk3l_live_remove(gk3l_disk *d, int as_note)
{
    static gk3w_span parts[1024];
    gk3l_live lv;
    gk3_gpt_part q, ud = {0};
    uint32_t esp_idx = 0;
    size_t n = 0, elen;
    int n_esp = 0, n_ud = 0, err, w, grow = 0;
    uint64_t new_last = 0, per_mib = (1u << 20) / d->bs;
    char esp_node[96] = "", ud_node[96] = "", what[400], live_node[96];
    uint8_t *hdr = NULL, *ent = NULL, *chk = NULL;
    int rc = -1;

    if (as_note)
        gk3w_progress(92, "remove-installer");
    else
        gk3w_progress(10, "remove-installer");
    if (gk3l_disk_read_table(d))
        return refuse(as_note, "nomem", NULL, NULL, "out of memory (disk not touched)", ENOMEM);
    if (strcmp(d->table, "gpt"))
        return refuse(as_note, "disk-not-gpt", "table", d->table, "the disk has no usable GPT partition table (disk not touched)", 0);
    if (!gk3l_disk_kernel_ok(d))
        return refuse(as_note, "kernel-stale", NULL, NULL, "the kernel's view of the partitions differs from the partition table on disk (disk not touched)", 0);
    if (!gk3l_live_find(d, &lv))
        return refuse(as_note, "live-missing", "disk", d->path, "the installer partition is not on this disk, this is not an internal disk, or the installer was not loaded into memory from it (disk not touched)", 0);
    snprintf(live_node, sizeof live_node, "/dev/%s", lv.k.name);
    if (gk3l_part_in_use(d->name, &lv.k, what, sizeof what))
        return refuse(as_note, "live-busy", "mounts", what, "the installer partition is in use (disk not touched)", 0);

    for (uint32_t i = 0; i < d->g.num_entries; i++) {
        char type[37], nm[GK3_GPT_NAME_CHARS * 4 + 1];
        if (gk3_gpt_get(&d->g, i, &q) != GK3_OK)
            continue;
        if (n >= sizeof parts / sizeof parts[0])
            return refuse(as_note, "gpt-layout", NULL, NULL, "the partition table has too many entries (disk not touched)", 0);
        parts[n++] = (gk3w_span){q.first_lba, q.last_lba};
        gk3l_guid_upper(q.type_guid, type);
        if (!strcmp(type, ESP_TYPE)) {
            n_esp++;
            esp_idx = q.index;
        }
        gk3l_gpt_name_utf8(&d->g, i, nm, sizeof nm);
        if (!strcmp(nm, "userdata")) {
            n_ud++;
            ud = q;
        }
    }
    if (n_esp != 1)
        return refuse(as_note, n_esp ? "esp-ambiguous" : "esp-missing", NULL, NULL,
                      n_esp ? "this disk has more than one EFI system partition (disk not touched)" : "this disk has no EFI system partition (disk not touched)", 0);
    {
        const gk3l_kpart *k = gk3l_kernel_find(d->kp, d->nk, esp_idx);
        char where[4096];
        if (!k)
            return refuse(as_note, "kernel-stale", NULL, NULL, "the kernel does not know the EFI system partition (disk not touched)", 0);
        snprintf(esp_node, sizeof esp_node, "/dev/%s", k->name);
        if (!gk3l_mount_of(k->devno, where, sizeof where))
            return refuse(as_note, "esp-busy", "mnt", where, "the EFI system partition is mounted; unmount it first (disk not touched)", 0);
    }
    /* userdata：名字恰好是 userdata 的那一个、没在用、紧挨在安装器分区前面 */
    if (n_ud == 1) {
        const gk3l_kpart *k = gk3l_kernel_find(d->kp, d->nk, ud.index);
        new_last = gk3l_live_grow_to(parts, n, (gk3w_span){ud.first_lba, ud.last_lba}, (gk3w_span){lv.first, lv.last}, d->g.last_usable, d->bs);
        if (new_last && k && !gk3l_part_in_use(d->name, k, what, sizeof what)) {
            grow = 1;
            snprintf(ud_node, sizeof ud_node, "/dev/%s", k->name);
        }
    }
    if (!grow)
        gk3l_log("userdata does not directly precede the installer partition (or is in use): the space becomes unallocated");

    if (d->g.alt_lba >= d->blocks)
        return refuse(as_note, "gpt-layout", NULL, NULL, "the backup partition table header lies beyond the end of the disk (disk not touched)", 0);
    /* 备份写不成时 gk3l_gpt_backup 已报 ERR gpt-backup；装完之后那一步里另报 NOTE，安装照样算成功 */
    if (gk3l_gpt_backup(d, NULL, "rmlive", d->g.first_usable, d->blocks - (d->g.last_usable + 1)))
        return as_note ? refuse(as_note, "gpt-backup", NULL, NULL, "writing the partition table backup failed (disk not touched)", 0) : -1;

    /* 新表在副本上改好再动盘 */
    elen = (size_t)d->g.num_entries * d->g.entry_size;
    hdr = gk3w_aligned_alloc(4096);
    ent = gk3w_aligned_alloc((elen + 4095) / 4096 * 4096);
    chk = gk3w_aligned_alloc(GK3L_ENTRIES_MAX);
    if (!hdr || !ent || !chk) {
        rc = refuse(as_note, "nomem", NULL, NULL, "out of memory (disk not touched)", ENOMEM);
        goto out;
    }
    memcpy(hdr, d->hdr, d->bs);
    memcpy(ent, d->ent, elen);
    if (gk3w_gpt_remove(hdr, d->bs, ent, elen, lv.idx) != GK3W_GPTW_OK ||
        (grow && gk3w_gpt_set_last(hdr, d->bs, ent, elen, ud.index, new_last) != GK3W_GPTW_OK)) {
        rc = refuse(as_note, "gpt-layout", NULL, NULL, "internal error: cannot change the partition table (disk not touched)", 0);
        goto out;
    }
    if (!as_note)
        gk3w_progress(50, "remove-installer");
    /* 内核先放掉安装器分区（在用时拒绝，这是最终的判据）；放掉了再改分区表，改不成就加回去 */
    if ((err = gk3l_kernel_del(d, lv.idx))) {
        rc = refuse(as_note, "live-busy", "part", live_node, "the kernel would not release the installer partition: it is in use (disk not touched)", err);
        goto out;
    }

    /* ══ 从这里起动盘 ══ */
    w = gk3l_gpt_write(d, hdr, ent, chk);
    if (w) {
        int e = errno;
        if (gk3l_kernel_add(d, lv.idx, lv.first, lv.last))
            gk3l_log("warning: could not give the installer partition back to the kernel; reboot before using this disk");
        gk3l_err_kv(GK3W_TOUCHED_YES, w == -1 ? "disk-write" : "disk-verify", "part", "gpt",
                    w == -1 ? "writing the partition table failed (the backup is listed above)" : "the partition table did not read back identically (the backup is listed above)", e);
        goto out;
    }
    if (gk3l_disk_read_table(d) || strcmp(d->table, "gpt") || gk3_gpt_get(&d->g, lv.idx - 1, &q) != GK3_ENOENT ||
        (grow && (gk3_gpt_get(&d->g, ud.index - 1, &q) != GK3_OK || q.last_lba != new_last))) {
        gk3l_err(GK3W_TOUCHED_YES, "verify-layout", "after removing the installer partition, the partition table is not as expected", 0);
        goto out;
    }
    gk3l_log("installer partition %s (%s) removed from the partition table and the kernel", live_node, lv.name);
    esp_drop_live_entries(esp_node);
    esp_restore_no_android(esp_node, d->name);
    if (grow) {
        if (!as_note)
            gk3w_progress(80, "remove-installer");
        err = gk3l_kernel_resize(d, ud.index, ud.first_lba, new_last);
        if (err) {
            /* 老内核没有 BLKPG_RESIZE_PARTITION：去掉再按新大小加回去（没在用，见上） */
            err = gk3l_kernel_del(d, ud.index);
            if (!err)
                err = gk3l_kernel_add(d, ud.index, ud.first_lba, new_last);
        }
        if (err || gk3l_wait_node(d, ud.index, ud_node, sizeof ud_node)) {
            gk3l_err_kv(GK3W_TOUCHED_YES, "kernel-add", "name", "userdata", "the kernel did not take the new size of userdata; reboot before using this disk", err);
            goto out;
        }
        gk3l_flush_node(ud_node);
        gk3l_log("userdata: partition enlarged from %llu MiB to %llu MiB", (unsigned long long)((ud.last_lba - ud.first_lba + 1) / per_mib),
                 (unsigned long long)((new_last - ud.first_lba + 1) / per_mib));
        (void)grow_fs(ud_node);
    }
    {
        gk3w_line l = {0};
        gk3w_line_begin(&l, "REMOVED");
        gk3w_line_kv(&l, "part", live_node);
        gk3w_line_kv(&l, "name", lv.name);
        gk3w_line_u64(&l, "size_mib", (lv.last - lv.first + 1) / per_mib);
        gk3w_line_kv(&l, "grow", grow ? "yes" : "no");
        gk3w_line_u64(&l, "userdata_mib", n_ud == 1 ? ((grow ? new_last : ud.last_lba) - ud.first_lba + 1) / per_mib : 0);
        gk3w_line_kv(&l, "esp", esp_node);
        (void)gk3w_line_emit(&l, stdout);
        gk3w_line_free(&l);
    }
    rc = 0;
out:
    gk3w_aligned_free(hdr);
    gk3w_aligned_free(ent);
    gk3w_aligned_free(chk);
    return rc;
}

int gk3l_cmd_remove_installer(int argc, char **argv)
{
    static gk3l_disk d;
    const char *disk = gk3l_arg(argc, argv, "--disk");
    char name[64];
    int err, rc = 1;
    if (!disk || gk3l_whole_disk_name(disk, name, sizeof name))
        return gk3l_err(GK3W_TOUCHED_NO, "usage", "usage: remove-installer --disk /dev/X", 0);
    if (gk3l_sys_gate())
        return 1;
    err = gk3l_disk_open(&d, name, 1);
    if (err)
        return gk3l_err_kv(GK3W_TOUCHED_NO, err == EACCES || err == EPERM ? "not-root" : "disk-open", "disk", disk, "cannot open the disk for writing", err);
    if (gk3l_disk_lock(&d)) {
        gk3l_err(GK3W_TOUCHED_NO, "disk-busy", "another program holds the disk (udev or another installer); try again (disk not touched)", errno);
    } else if (gk3l_live_remove(&d, 0) == 0) {
        gk3w_progress(100, "done");
        rc = 0;
    }
    gk3l_disk_close(&d);
    return rc;
}
