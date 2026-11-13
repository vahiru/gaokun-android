/* cmdline：用实机 golden 核对"boot.img 头 + slot_suffix = 今天直连条目开出来的 /proc/cmdline"。 */
#include "t.h"

void test_cmdline(void)
{
    size_t len, hl;
    unsigned char *pc = t_vector("proc-cmdline-20261005.txt", &len);
    unsigned char *h = t_vector("boot-1791053208-hdr.bin", &hl);
    gk3_bootimg b;
    char base[2048], out[2048], want[2048], *p;
    uint16_t u[8];

    if (!pc || !h)
        return;
    CHECK_EQ(gk3_bootimg_parse(h, hl, 0, &b), GK3_OK);
    CHECK(gk3_bootimg_cmdline(&b, base, sizeof(base)) > 0, "取头里的 cmdline");

    /* 实机 /proc/cmdline = "initrd=\<mid>\android\slot_a\ramdisk.img " + cmdline.txt + " androidboot.slot_suffix=_a"
     * （systemd-boot 在 options 前加 initrd=，§4.3.1 说入口不加它）。去掉 initrd= 那个 token 应与入口的输出逐字节相同。 */
    memcpy(want, pc, len);
    want[len] = 0;
    while (len && (want[len - 1] == '\n' || want[len - 1] == '\r'))
        want[--len] = 0;
    CHECK(strncmp(want, "initrd=", 7) == 0, "实机 cmdline 以 initrd= 开头");
    p = strchr(want, ' ');
    {
        gk3_android_args a = {0, NULL, NULL, NULL, NULL, NULL, NULL};
        CHECK_EQ(gk3_cmdline_android(base, &a, out, sizeof(out)), GK3_OK);
        CHECK_STR(out, p ? p + 1 : "");
    }
    /* 入口的完整追加（§4.3.1 的顺序） */
    {
        gk3_android_args a = {1, "gk3boot-1.0.0", "fallback", "gk3boot-android-b+2-1.conf", NULL, NULL, NULL};
        char *tail;
        CHECK_EQ(gk3_cmdline_android(base, &a, out, sizeof(out)), GK3_OK);
        tail = strstr(out, " androidboot.slot_suffix=");
        CHECK(tail != NULL, "有 slot_suffix");
        if (tail)
            CHECK_STR(tail, " androidboot.slot_suffix=_b androidboot.bootloader=gk3boot-1.0.0"
                            " androidboot.gk3boot.event=fallback androidboot.gk3boot.entry=gk3boot-android-b+2-1.conf");
        CHECK(strncmp(out, base, strlen(base)) == 0, "前面就是头里的 cmdline 原样");
    }
    /* 观察模式（E4）：mode 排在最后；base 里混进来的旧 gk3boot.* 键被换掉 */
    {
        gk3_android_args a = {0, "gk3boot-0.1.0-e4.g5c13441", "none", "gk3boot-e4.conf", "observe", NULL, NULL};
        char *tail;
        CHECK_EQ(gk3_cmdline_android(base, &a, out, sizeof(out)), GK3_OK);
        tail = strstr(out, " androidboot.slot_suffix=");
        if (tail)
            CHECK_STR(tail, " androidboot.slot_suffix=_a androidboot.bootloader=gk3boot-0.1.0-e4.g5c13441"
                            " androidboot.gk3boot.event=none androidboot.gk3boot.entry=gk3boot-e4.conf"
                            " androidboot.gk3boot.mode=observe");
        CHECK_EQ(gk3_cmdline_android("x androidboot.gk3boot.mode=action y", &a, out, sizeof(out)), GK3_OK);
        CHECK(strstr(out, "mode=action") == NULL, "旧的 mode 被去掉");
        a.mode = "ob serve";
        CHECK_EQ(gk3_cmdline_android(base, &a, out, sizeof(out)), GK3_EINVAL);
    }
    /* 动作模式（S5 后续）：streak 排在 mode 之后；旧的 streak 被换掉；值照样过滤 */
    {
        gk3_android_args a = {1, "gk3boot-0.2.0", "fallback", "gk3boot-android-b+2-1.conf", "action", "4", NULL};
        char *tail;
        CHECK_EQ(gk3_cmdline_android("x androidboot.gk3boot.streak=9 y", &a, out, sizeof(out)), GK3_OK);
        tail = strstr(out, " androidboot.slot_suffix=");
        CHECK(tail != NULL && strncmp(out, "x y ", 4) == 0, "旧的 streak 被去掉");
        if (tail)
            CHECK_STR(tail, " androidboot.slot_suffix=_b androidboot.bootloader=gk3boot-0.2.0"
                            " androidboot.gk3boot.event=fallback androidboot.gk3boot.entry=gk3boot-android-b+2-1.conf"
                            " androidboot.gk3boot.mode=action androidboot.gk3boot.streak=4");
        a.streak = "4 5";
        CHECK_EQ(gk3_cmdline_android(base, &a, out, sizeof(out)), GK3_EINVAL);
    }
    /* S15：分派开着时多一项 androidboot.gk3boot.dispatch=1（排最后；旧的被换掉）；不给就不出现 */
    {
        gk3_android_args a = {0, "gk3boot-0.3.0", "none", "gk3boot-android-a.conf", "action", "1", "1"};
        char *tail;
        CHECK_EQ(gk3_cmdline_android("x androidboot.gk3boot.dispatch=0 y", &a, out, sizeof(out)), GK3_OK);
        tail = strstr(out, " androidboot.slot_suffix=");
        CHECK(tail != NULL && strncmp(out, "x y ", 4) == 0, "旧的 dispatch 被去掉");
        if (tail)
            CHECK_STR(tail, " androidboot.slot_suffix=_a androidboot.bootloader=gk3boot-0.3.0"
                            " androidboot.gk3boot.event=none androidboot.gk3boot.entry=gk3boot-android-a.conf"
                            " androidboot.gk3boot.mode=action androidboot.gk3boot.streak=1 androidboot.gk3boot.dispatch=1");
        a.dispatch = "1 2";
        CHECK_EQ(gk3_cmdline_android(base, &a, out, sizeof(out)), GK3_EINVAL);
    }
    /* base 里已有的同名键被替换，不会出现两个 */
    {
        gk3_android_args a = {0, NULL, NULL, NULL, NULL, NULL, NULL};
        CHECK_EQ(gk3_cmdline_android("a=1  androidboot.slot_suffix=_b\tb=2 androidboot.slot_suffixes=x", &a, out, sizeof(out)), GK3_OK);
        CHECK_STR(out, "a=1 b=2 androidboot.slot_suffixes=x androidboot.slot_suffix=_a");
        CHECK_EQ(gk3_cmdline_android("foo=\"a b\" bar", &a, out, sizeof(out)), GK3_OK);
        CHECK_STR(out, "foo=\"a b\" bar androidboot.slot_suffix=_a");
        CHECK_EQ(gk3_cmdline_android("", &a, out, sizeof(out)), GK3_OK);
        CHECK_STR(out, "androidboot.slot_suffix=_a");
        CHECK_EQ(gk3_cmdline_android(base, &a, out, 100), GK3_ENOSPC);
        a.slot = 2;
        CHECK_EQ(gk3_cmdline_android(base, &a, out, sizeof(out)), GK3_EINVAL);
    }
    /* 值里不准有空白 / 引号（不给注入机会） */
    {
        gk3_android_args a = {0, "gk3boot 1.0", NULL, NULL, NULL, NULL, NULL};
        CHECK_EQ(gk3_cmdline_android(base, &a, out, sizeof(out)), GK3_EINVAL);
        a.bootloader = NULL;
        a.entry = "x\"y";
        CHECK_EQ(gk3_cmdline_android(base, &a, out, sizeof(out)), GK3_EINVAL);
        a.entry = "";
        CHECK_EQ(gk3_cmdline_android(base, &a, out, sizeof(out)), GK3_EINVAL);
    }

    /* 执行端（§4.4.1）：规则同 LiveCD 启动项的过滤，再去 deferred_probe_timeout */
    {
        gk3_fastboot_args f = {"bootloader", 0, "1.0.0", "53912ab2-33ac-49d2-a099-94fed8664a26", NULL, false, false};
        CHECK_EQ(gk3_cmdline_fastboot(base, &f, out, sizeof(out)), GK3_OK);
        CHECK_STR(out, "iommu.passthrough=0 iommu.strict=0 printk.devkmsg=on console=tty0 clk_ignore_unused "
                       "pd_ignore_unused arm64.nopauth efi=noruntime fbcon=rotate:1 "
                       "usbhid.quirks=0x12d1:0x10b8:0x20000000 himax_hx83121a_spi.disable_pressure=0 "
                       "panic=10 gk3.mode=fastboot gk3.why=bootloader gk3.slot=a gk3.bootver=1.0.0 "
                       "gk3.disk=53912ab2-33ac-49d2-a099-94fed8664a26");
        CHECK_EQ(gk3_cmdline_fastboot("panic=5 gk3.mode=x initrd=y", &f, out, sizeof(out)), GK3_OK);
        CHECK_STR(out, "initrd=y panic=10 gk3.mode=fastboot gk3.why=bootloader gk3.slot=a gk3.bootver=1.0.0 "
                       "gk3.disk=53912ab2-33ac-49d2-a099-94fed8664a26");
        /* S7c：gk3.esp / gk3.dispatch / gk3.fbtcp 只在给了时追加；base 里的同名 gk3.* 一律先去掉（不让 boot.img 冒充入口） */
        f.esp = "6d7ac5a1-8b3e-4f24-9a52-0c0ffee00001";
        f.dispatch = true;
        f.fbtcp = true;
        CHECK_EQ(gk3_cmdline_fastboot("console=tty0 gk3.fbtcp=1 gk3.dispatch=0 gk3.esp=zz", &f, out, sizeof(out)), GK3_OK);
        CHECK_STR(out, "console=tty0 panic=10 gk3.mode=fastboot gk3.why=bootloader gk3.slot=a gk3.bootver=1.0.0 "
                       "gk3.disk=53912ab2-33ac-49d2-a099-94fed8664a26 gk3.esp=6d7ac5a1-8b3e-4f24-9a52-0c0ffee00001 "
                       "gk3.dispatch=1 gk3.fbtcp=1");
        f.esp = "a b";
        CHECK_EQ(gk3_cmdline_fastboot(base, &f, out, sizeof(out)), GK3_EINVAL);
        f.esp = NULL;
        f.dispatch = f.fbtcp = false;
        f.why = "wipe now";
        CHECK_EQ(gk3_cmdline_fastboot(base, &f, out, sizeof(out)), GK3_EINVAL);
    }

    /* UCS-2 LoadOptions */
    CHECK_EQ(gk3_ascii_to_ucs2("ab", u, 3), GK3_OK);
    CHECK(u[0] == 'a' && u[1] == 'b' && u[2] == 0, "ucs2");
    CHECK_EQ(gk3_ascii_to_ucs2("abc", u, 3), GK3_ENOSPC);
    CHECK_EQ(gk3_ascii_to_ucs2("a\xe9", u, 8), GK3_EINVAL);
    free(pc);
    free(h);
}
