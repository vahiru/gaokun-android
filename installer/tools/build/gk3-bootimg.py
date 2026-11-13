#!/usr/bin/env python3
# 把 Android boot 镜像（header v2）拆成 systemd-boot 要的几个普通文件。
#
#   python3 gk3-bootimg.py boot.img <输出目录>
#   → <输出目录>/{Image, ramdisk.img, gaokun3.dtb, cmdline.txt}
#
# 为什么要拆：这台机器是 UEFI + systemd-boot，它不认 Android boot 镜像，
# 只从 ESP 上加载普通文件。boot_a/boot_b 分区是唯一真相源，ESP 上这几个是
# 派生出来的副本（OTA 时由 vendor/bin/gaokun3-ota-postinstall.sh 刷新）。
#
# ⚠️ 这与设备侧的 device/huawei/gaokun3/bootimg/bootimg_extract.cpp 是同一件事的
#    两个实现：那边跑在 Android 里（include 树内 <bootimg.h>），这边跑在安装环境里。
#    两边必须产出【逐字节相同】的四个文件 —— 包括 cmdline.txt 的结尾换行
#    （bootimg_extract.cpp:176 是 fprintf("%s\n")）。改一边就要改另一边。
#
# ★ cmdline 从镜像头里取（BOARD_KERNEL_CMDLINE），不在安装器里另抄一份：
#   手抄的那份 2026-09-23 之前已经漂了（缺 himax disable_pressure=0，TODO B15）。
import os
import struct
import sys

HDR_V2_SIZE = 1660          # boot_img_hdr_v2：v1 的 1648 + dtb_size(4) + dtb_addr(8)


def die(msg):
    sys.stderr.write("!! gk3-bootimg: %s\n" % msg)
    sys.exit(1)


def main(img, out):
    with open(img, "rb") as f:
        hdr = f.read(HDR_V2_SIZE)
        if len(hdr) < HDR_V2_SIZE:
            die("文件太短，连 v2 头都不完整")
        if hdr[:8] != b"ANDROID!":
            die("不是 Android boot 镜像（魔数不对）")
        u32 = lambda off: struct.unpack_from("<I", hdr, off)[0]
        kernel_size, ramdisk_size, second_size = u32(8), u32(16), u32(24)
        page, ver = u32(36), u32(40)
        if ver != 2:
            die("只支持 header v2，这个是 v%d" % ver)
        if page == 0 or page > (1 << 20) or page & (page - 1):
            die("page_size 不合理：%d" % page)       # bootimg_extract.cpp:136
        recovery_dtbo_size, header_size, dtb_size = u32(1632), u32(1644), u32(1648)
        if header_size != HDR_V2_SIZE:
            die("header_size=%d，v2 应当是 %d" % (header_size, HDR_V2_SIZE))

        # 磁盘布局：header | kernel | ramdisk | second | recovery_dtbo | dtb，
        # 每段按 page_size 向上对齐（bootimg_extract.cpp:152-160）
        align = lambda x: (x + page - 1) // page * page
        off = page
        parts = []
        for size, name in ((kernel_size, "Image"), (ramdisk_size, "ramdisk.img")):
            parts.append((off, size, name))
            off += align(size)
        off += align(second_size) + align(recovery_dtbo_size)
        parts.append((off, dtb_size, "gaokun3.dtb"))

        os.makedirs(out, exist_ok=True)
        for start, size, name in parts:
            if size == 0:
                die("%s 在镜像里是空的" % name)
            f.seek(start)
            data = f.read(size)
            if len(data) != size:
                die("%s 读不全：%d / %d 字节（镜像被截断？）" % (name, len(data), size))
            # 体检：和 installer/tools/build/build-usb.sh 对输入做的检查同一套
            if name == "Image" and data[:2] != b"MZ":
                die("内核不是 PE/EFI 格式（开头不是 MZ）")
            if name == "gaokun3.dtb" and data[:4] != b"\xd0\x0d\xfe\xed":
                die("dtb 段开头不是 FDT 魔数 d00dfeed")
            with open(os.path.join(out, name), "wb") as o:
                o.write(data)
            sys.stderr.write("  %-12s %10d 字节\n" % (name, size))

        # cmdline[512] @64 + extra_cmdline[1024] @608，各自以 NUL 结尾 ——
        # 与 bootimg_extract.cpp:170-173 的 "%.*s%.*s" 拼法相同
        cstr = lambda b: b.split(b"\0", 1)[0].decode("ascii")
        cmdline = cstr(hdr[64:576]) + cstr(hdr[608:1632])
        if not cmdline.strip():
            die("镜像里没有内核命令行")
        with open(os.path.join(out, "cmdline.txt"), "w") as o:
            o.write(cmdline + "\n")
        sys.stderr.write("  %-12s %10d 字节\n" % ("cmdline.txt", len(cmdline) + 1))


if __name__ == "__main__":
    if len(sys.argv) != 3:
        die("用法：gk3-bootimg.py <boot.img> <输出目录>")
    main(sys.argv[1], sys.argv[2])
