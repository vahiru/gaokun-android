/* Linux：安装前检查要看的系统状态（只读）。
 *
 *   gk3l preflight     一行 PREFLIGHT：gk3l=<版本> manufacturer= model= model_ok=yes|no|unknown ac=yes|no
 *                      [battery=<百分比>] battery_min=<门槛>
 *
 * 与 Windows 版的 PREFLIGHT 同名的字段含义相同；LiveCD 只查机型与电量（能启动到 LiveCD 并运行安装器，root、UEFI 引导
 * 这些就必然满足）。判断规则在 ../policy.c，两个平台共用。
 *
 * 机型：/sys/class/dmi/id/product_name（要 CONFIG_DMIID）；读不到时退回内核日志里 "Hardware name: HUAWEI GK-W7X/…" 那一行，
 *   再读不到就是 model_ok=unknown（只提醒、不拦）。GK3_SKIP_MODEL_CHECK=1 时也报 unknown，另加 skipped=yes。
 * 电源：/sys/class/power_supply（测试可用 GK3_POWER_SUPPLY_DIR 换目录）。电池优先认本机 EC 驱动的 gaokun-ec-battery，
 *   没有就找 type 为 Battery 的；任何一个不是电池的电源报 online=1，或者电池自己在充电，都算接着电源。 */
#define _GNU_SOURCE
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/klog.h>

#include "../policy.h"
#include "../proto.h"
#include "lx.h"

/* 内核日志里的 "Hardware name: <厂商> <产品名>/<主板>, BIOS …" → 产品名 */
static void model_from_klog(char *out, size_t cap)
{
    int len = klogctl(10 /* SYSLOG_ACTION_SIZE_BUFFER */, NULL, 0);
    char *buf, *p, *e;
    out[0] = 0;
    if (len <= 0 || len > (64 << 20))
        return;
    buf = malloc((size_t)len + 1);
    if (!buf)
        return;
    len = klogctl(3 /* SYSLOG_ACTION_READ_ALL */, buf, len);
    if (len > 0) {
        buf[len] = 0;
        p = strstr(buf, "Hardware name: ");
        if (p) {
            p += 15;
            p = strchr(p, ' ');     /* 跳过厂商 */
            if (p) {
                p++;
                e = p + strcspn(p, "/,\n");
                if ((size_t)(e - p) < cap) {
                    memcpy(out, p, (size_t)(e - p));
                    out[e - p] = 0;
                }
            }
        }
    }
    free(buf);
}

static int is_type(const char *dir, const char *name, const char *type)
{
    char p[512], v[64];
    snprintf(p, sizeof p, "%s/%s/type", dir, name);
    return !gk3l_read_line(p, v, sizeof v) && !strcmp(v, type);
}

/* *ac：1 接着电源、0 没接；*battery：百分比，-1 不知道 */
static void read_power(int *ac, int *battery)
{
    const char *ps = getenv("GK3_POWER_SUPPLY_DIR");
    char bat[256] = "", p[512], v[64];
    uint64_t cap;
    DIR *dir;
    struct dirent *e;

    if (!ps || !*ps)
        ps = "/sys/class/power_supply";
    *ac = 0;
    *battery = -1;
    snprintf(p, sizeof p, "%s/gaokun-ec-battery/capacity", ps);
    if (!gk3l_read_u64(p, &cap))
        snprintf(bat, sizeof bat, "gaokun-ec-battery");
    dir = opendir(ps);
    if (dir) {
        while ((e = readdir(dir))) {
            if (e->d_name[0] == '.')
                continue;
            if (is_type(ps, e->d_name, "Battery")) {
                snprintf(p, sizeof p, "%s/%s/capacity", ps, e->d_name);
                if (!bat[0] && !gk3l_read_u64(p, &cap))
                    snprintf(bat, sizeof bat, "%s", e->d_name);
                continue;
            }
            snprintf(p, sizeof p, "%s/%s/online", ps, e->d_name);
            if (!gk3l_read_line(p, v, sizeof v) && !strcmp(v, "1"))
                *ac = 1;
        }
        closedir(dir);
    }
    if (!bat[0])
        return;
    snprintf(p, sizeof p, "%s/%s/status", ps, bat);
    if (!gk3l_read_line(p, v, sizeof v) && !strcmp(v, "Charging"))
        *ac = 1;
    snprintf(p, sizeof p, "%s/%s/capacity", ps, bat);
    if (!gk3l_read_u64(p, &cap) && cap <= 100)
        *battery = (int)cap;
}

int gk3l_cmd_preflight(int argc, char **argv)
{
    gk3w_line l = {0};
    char vendor[128] = "", model[128] = "";
    const char *skip = getenv("GK3_SKIP_MODEL_CHECK");
    int ac, battery;
    (void)argc;
    (void)argv;

    gk3l_read_line("/sys/class/dmi/id/sys_vendor", vendor, sizeof vendor);
    if (gk3l_read_line("/sys/class/dmi/id/product_name", model, sizeof model) || !model[0])
        model_from_klog(model, sizeof model);
    read_power(&ac, &battery);

    gk3w_line_begin(&l, "PREFLIGHT");
    gk3w_line_kv(&l, "gk3l", gk3l_version);
    gk3w_line_kv(&l, "manufacturer", vendor);
    gk3w_line_kv(&l, "model", model);
    if (skip && !strcmp(skip, "1")) {
        gk3w_line_kv(&l, "model_ok", "unknown");
        gk3w_line_kv(&l, "skipped", "yes");
    } else {
        gk3w_line_kv(&l, "model_ok", !model[0] ? "unknown" : gk3w_model_supported(model) ? "yes" : "no");
    }
    gk3w_line_kv(&l, "ac", ac ? "yes" : "no");
    if (battery >= 0)
        gk3w_line_u64(&l, "battery", (uint64_t)battery);
    gk3w_line_u64(&l, "battery_min", GK3W_BATTERY_MIN_PCT);
    if (gk3w_line_emit(&l, stdout)) {
        gk3w_line_free(&l);
        return 1;
    }
    gk3w_line_free(&l);
    return 0;
}
