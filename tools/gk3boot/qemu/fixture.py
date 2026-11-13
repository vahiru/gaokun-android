#!/usr/bin/env python3
"""gk3boot QEMU 夹具：造盘、做快照、比快照、写 AAVMF 变量（设计稿 §5 S3）。

在 scripts/gk3boot/gk3boot-build.Dockerfile 的容器里跑（要 mkfs.vfat / mtools / virt-fw-vars）。

  fixture.py mkdisk   --out DIR [--bootimg 完整 boot.img] [--variant normal|broken|espfull]
                      [--gk3boot-options '...'] [--gk3boot-entry NAME] [--loader-default PATTERN]
                      [--boot-a IMG] [--boot-b IMG] [--misc FILE] [--corrupt-a] [--corrupt-b]
  fixture.py snapshot DISK MANIFEST OUT.json
  fixture.py diff     BEFORE.json AFTER.json [--allow-misc] [--new-logs-out FILE]
                      只允许 ESP 上多出 \\EFI\\gk3boot\\{probe\\log-*,log\\boot-*}.txt 与条目计数改名；
                      --allow-misc：misc 区域的变化交给 check_misc.py 逐字节判（gk3boot 动作模式会写它）
  fixture.py vars     IN.fd OUT.fd [--oneshot NAME] [--set NAME=VALUE ...] [--del NAME ...]
                      往 AAVMF 变量库里写 systemd-boot 厂商 GUID 下的字符串变量（与 boot-oneshot.sh 同一格式）；
                      --oneshot X 等于 --set LoaderEntryOneShot=X；--del 删掉（S15：LoaderEntryDefault 等）
  fixture.py misc-set DISK MANIFEST [--streak N] [--next windows|sdboot-menu|none] [--set-default windows|android|none]
                      [--poweroff] [--default-os unknown|android|windows]
                      改盘上 misc+8 KiB 的 GK3 记录（没有有效记录就先建一份空的）、重算 CRC —— 在两次 QEMU 之间冒充
                      Android 侧（HAL 开机完成清 streak、Parts 的请求、on shutdown 的 mark-poweroff；S15）
  fixture.py vars-get IN.fd NAME                  打印 systemd-boot 厂商 GUID 下变量 NAME 的值（UTF-16 解码）或 "(absent)"
  fixture.py esp-get  DISK MANIFEST PATH OUT      从盘上的 ESP 取一个文件
  fixture.py misc-get DISK MANIFEST OUT           取 misc 分区的前 64 KiB（gk3boot 读的那一段）

gk3boot（S5）的 QEMU 测试另用这几样（qemu/run-boot-tests.sh）：
  fixture.py initramfs  --init ELF --marker STR --out FILE      最小 initramfs（newc cpio + gzip，同 Android ramdisk 的压缩）
  fixture.py fdt-mark   IN.dtb OUT.dtb NAME VALUE                给根节点加一个字符串属性（证明内核用的是我们装的 dtb）
  fixture.py mkbootimg  --kernel K --ramdisk R --dtb D --cmdline S --out F   header v2（与本机 BoardConfig 同版本、page 2048）
  fixture.py misc       --variant V --out FILE                  从实机 misc 向量改出一份（变体见 cmd_misc）

盘的样子照实机抄（tools/gk3boot/test/vectors/gpt-primary-20261005.bin）：分区号、名字、类型 GUID、
PARTUUID、磁盘 GUID、boot_a 的属性位 bit 54 都与实机一致；misc 也照实机坐在 LBA 34–2047，
内容是实机只读读出的 64 KiB（misc-20261005-1791053208.bin）。其余分区按比例缩小。
ESP 的目录结构照安装器（installer/backend/src/linux/apply.c 的 write_esp）：systemd-boot 257.13 + loader.conf +
<mid>-android-{a,b}.conf 直连条目 + <mid>/android/slot_{a,b}/；"Image" 换成 gk3-fake-android.efi。
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import uuid
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
GK3 = os.path.dirname(HERE)
VEC = os.path.join(GK3, "test", "vectors")
GPT_VEC = os.path.join(VEC, "gpt-primary-20261005.bin")
MISC_VEC = os.path.join(VEC, "misc-20261005-1791053208.bin")
CMDLINE_VEC = os.path.join(VEC, "proc-cmdline-20261005.txt")
EFI_BUILD = os.path.join(GK3, "build", "efi")
SDBOOT = "/usr/lib/systemd/boot/efi/systemd-bootaa64.efi"
MID = "8a29534fa802480d9fbb71aa18c01d7b"      # 实机 ESP 上的 machine-id 目录（proc-cmdline 向量里的 initrd= 路径）
SECTOR = 512
MiB = 1024 * 1024 // SECTOR                    # 扇区数

# 夹具里的分区大小（扇区）。esp 与实机同大小（614400 扇区 = 300 MiB）；boot_x 与实机同为 64 MiB；其余缩小。
LAYOUT = [  # (实机分区号, 名字, 扇区数)；misc（p4）单独放在 LBA 34
    (1, "esp", 614400),
    (2, "userdata", 16 * MiB),
    (8, "super", 16 * MiB),
    (5, "boot_a", 131072),
    (6, "boot_b", 131072),
    (10, "metadata", 16 * MiB),
    (3, "ubunturescue", 8 * MiB),
]


def die(msg):
    print("✗ " + msg, file=sys.stderr)
    sys.exit(1)


def sha256(b):
    return hashlib.sha256(b).hexdigest()


def run(cmd, **kw):
    env = dict(os.environ, MTOOLS_SKIP_CHECK="1")
    r = subprocess.run(cmd, env=env, capture_output=True, text=True, **kw)
    if r.returncode:
        die("%s → %d\n%s%s" % (" ".join(cmd), r.returncode, r.stdout, r.stderr))
    return r.stdout


# ---------------------------------------------------------------- 实机 GPT 向量

def real_gpt():
    d = open(GPT_VEC, "rb").read()
    hdr = d[512:1024]
    disk_guid = hdr[56:72]
    parts = {}
    for i in range(128):
        e = d[1024 + i * 128:1024 + (i + 1) * 128]
        if e[:16] == b"\0" * 16:
            continue
        name = e[56:128].decode("utf-16-le").rstrip("\0")
        first, last, attrs = struct.unpack_from("<QQQ", e, 32)
        parts[name] = dict(index=i + 1, type=e[:16], guid=e[16:32], attrs=attrs, real_first=first, real_last=last)
    return disk_guid, parts


def gpt_entry(type_guid, part_guid, first, last, attrs, name):
    n = name.encode("utf-16-le")
    return type_guid + part_guid + struct.pack("<QQQ", first, last, attrs) + n + b"\0" * (72 - len(n))


def gpt_header(my, alt, first_usable, last_usable, entries_lba, disk_guid, entries_crc):
    h = bytearray(92)
    struct.pack_into("<8sIII", h, 0, b"EFI PART", 0x00010000, 92, 0)
    struct.pack_into("<QQQQ", h, 24, my, alt, first_usable, last_usable)
    h[56:72] = disk_guid
    struct.pack_into("<QIII", h, 72, entries_lba, 128, 128, entries_crc)
    struct.pack_into("<I", h, 16, zlib.crc32(bytes(h)) & 0xffffffff)
    return bytes(h) + b"\0" * (SECTOR - 92)


# ---------------------------------------------------------------- 夹具里的文件

def mkbootimg(kernel, ramdisk, dtb, cmdline, name=b""):
    """header v2 boot.img（page 2048），id 按 mkbootimg 算。cmdline 超过 511 字节的部分按 mkbootimg 的规矩
    原样切进 extra_cmdline（头 @608[1024]），中间不加空格 —— 与 gk3_bootimg_cmdline 的拼法对应。"""
    page = 2048
    if len(cmdline) > 511 + 1023:
        die("cmdline 太长（%d）" % len(cmdline))
    s = hashlib.sha1()
    for blob in (kernel, ramdisk, b"", b"", dtb):
        s.update(blob)
        s.update(struct.pack("<I", len(blob)))
    h = bytearray(page)
    h[0:8] = b"ANDROID!"
    struct.pack_into("<IIIIIIIII", h, 8, len(kernel), 0x8000, len(ramdisk), 0x1000000, 0, 0, 0x100, page, 2)
    h[48:48 + min(len(name), 16)] = name[:16]
    h[64:64 + min(len(cmdline), 511)] = cmdline[:511]
    h[608:608 + len(cmdline[511:])] = cmdline[511:]
    h[576:596] = s.digest()
    struct.pack_into("<IQI", h, 1632, 0, 0, 1660)
    struct.pack_into("<IQ", h, 1648, len(dtb), 0x2000000)

    def pad(b):
        return b + b"\0" * (-len(b) % page)
    return bytes(h) + pad(kernel) + pad(ramdisk) + pad(dtb), s.hexdigest()


def synth_bootimg(tag):
    """合成一份 header v2 boot.img，kernel 以 MZ…zimg 开头（zboot 的样子，但不是真 PE —— LoadImage 会拒）。"""
    kernel = b"MZ\0\0zimg" + hashlib.sha256(tag.encode()).digest() * 100
    ramdisk = b"\x1f\x8b" + bytes(range(256)) * 20
    dtb = bytes.fromhex("d00dfeed") + b"\0" * 60
    cmdline = b"console=tty0 androidboot.hardware=gaokun3 gk3fixture=" + tag.encode()
    return mkbootimg(kernel, ramdisk, dtb, cmdline, tag.encode())


def bootimg_id(img):
    return img[576:596].hex()


def minimal_fdt():
    """最小合法 FDT（空根节点）：systemd-boot 的 devicetree_install 只查大小，夹具里的 'Image' 也不读它。"""
    struct_blk = struct.pack(">II", 1, 0) + struct.pack(">I", 2) + struct.pack(">I", 9)   # BEGIN_NODE "" / END_NODE / END
    rsv = b"\0" * 16
    off_rsv = 40
    off_struct = off_rsv + len(rsv)
    off_str = off_struct + len(struct_blk)
    total = off_str
    hdr = struct.pack(">IIIIIIIIII", 0xd00dfeed, total, off_struct, off_str, off_rsv, 17, 16, 0, 0, len(struct_blk))
    return hdr + rsv + struct_blk


GK3BOOT_ENTRY = "gk3boot-e4.conf"


COUNTER = re.compile(r"\+\d+(-\d+)?(?=\.conf$)")


def esp_tree(stage, variant, gk3boot_options=None, gk3boot_entry=GK3BOOT_ENTRY, loader_default="*-android-a.conf",
             fastboot_img=None, windows=False, gk3_windows_entry=False):
    """在 stage 目录下摆出 ESP 的内容。返回条目文件名等信息。
    gk3boot_options 不为 None 时摆 gk3boot（S5）的条目，否则摆探针（S4）的条目。
    gk3boot_entry：条目文件名。gk3boot-e4.conf（E4 的样子：非默认、经 OneShot 进入）；
    gk3boot-android-<x>[+N].conf（E5 的样子：sort-key 0gk3 排在直连条目 zandroid<x> 前面，loader.conf 的
    default "*-android-<x>.conf" 先命中它 —— 设计稿 §4.2；systemd-boot 去掉计数后比 id，boot.c:1340-1376）。"""
    def put(rel, data):
        p = os.path.join(stage, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(data)

    sd = open(SDBOOT, "rb").read()
    put("EFI/BOOT/BOOTAA64.EFI", sd)
    put("EFI/systemd/systemd-bootaa64.efi", sd)
    # 夹具把菜单等待缩到 2 秒，别的照抄
    put("loader/loader.conf", ("timeout 2\nconsole-mode keep\neditor no\ndefault %s\n" % loader_default).encode())
    cmd = open(CMDLINE_VEC).read().split()
    base = " ".join(t for t in cmd if not t.startswith("initrd=") and not t.startswith("androidboot.slot_suffix="))
    fake = open(os.path.join(EFI_BUILD, "gk3-fake-android.efi"), "rb").read()
    if windows:
        # S15 双系统：ESP 上有 Windows 的启动管理器（假的：打一行 GK3-FAKE-WINDOWS 后关机）。systemd-boot 据它生成
        # auto-windows（boot.c:2146-2148，只看文件能不能 Open，:1978-1982）；BCD 没有，标题用缺省的 "Windows Boot Manager"
        put("EFI/Microsoft/Boot/bootmgfw.efi", open(os.path.join(EFI_BUILD, "gk3-fake-windows.efi"), "rb").read())
        if gk3_windows_entry:
            # U22：安装器自写的 type1 条目（id = 文件名 gk3-windows.conf）
            put("loader/entries/gk3-windows.conf", (
                "title      Windows\n"
                "sort-key   0gk3w\n"
                "efi        /EFI/Microsoft/Boot/bootmgfw.efi\n").encode())
    for slot in "ab":
        put("%s/android/slot_%s/Image" % (MID, slot), fake)
        put("%s/android/slot_%s/gaokun3.dtb" % (MID, slot), minimal_fdt())
        put("%s/android/slot_%s/ramdisk.img" % (MID, slot), b"\x1f\x8b" + b"\0" * 4094)
        # 与 installer/backend/src/esp.c 的 gk3w_esp_android_entry 同一格式（title 里的 "—" 也照抄）
        put("loader/entries/%s-android-%s.conf" % (MID, slot), (
            "title      crDroid 16.0 (gaokun3) — slot _%s\n"
            "version    gaokun3-slot-%s\n"
            "sort-key   zandroid%s\n"
            "options    %s androidboot.slot_suffix=_%s\n"
            "linux      /%s/android/slot_%s/Image\n"
            "devicetree /%s/android/slot_%s/gaokun3.dtb\n"
            "initrd     /%s/android/slot_%s/ramdisk.img\n"
            % (slot, slot, slot, base, slot, MID, slot, MID, slot, MID, slot)).encode())
    if gk3boot_options is not None:
        # 与 README §10 的上机步骤同一份条目：文件名不匹配 *-android-*.conf、不带计数、title 只用 ASCII（§4.13）
        put("EFI/gk3boot/e4/gk3boot.efi", open(os.path.join(EFI_BUILD, "gk3boot.efi"), "rb").read())
        if fastboot_img:
            # 执行端 initramfs（S7c）：与 gk3boot.efi 同目录（设计稿 §4.1）
            put("EFI/gk3boot/e4/fastboot.img", open(fastboot_img, "rb").read())
        as_default = gk3boot_entry.startswith("gk3boot-android-")
        put("loader/entries/" + gk3boot_entry, (
            "title      %s\n"
            "sort-key   %s\n"
            "efi        /EFI/gk3boot/e4/gk3boot.efi\n"
            "options    %s\n" % ("gk3boot (default entry)" if as_default else "gk3boot E4 (observe)",
                                  "0gk3" if as_default else "zzgk3boot", gk3boot_options)).encode())
        dslot = loader_default[-6]
        return dict(oneshot=None if as_default else COUNTER.sub("", gk3boot_entry), gk3boot_entry=gk3boot_entry,
                    default_entry="%s-android-%s.conf" % (MID, dslot), loader_default=loader_default,
                    gk3boot_options=gk3boot_options)
    probe = os.path.join(EFI_BUILD, "gk3probe.efi")
    if not os.path.exists(probe):
        return dict(oneshot=None, default_entry="%s-android-a.conf" % MID)
    put("EFI/gk3boot/probe/gk3probe.efi", open(probe, "rb").read())
    # 夹具里带启动计数（+3），顺带验证 LoaderBootCountPath；OneShot 写不带计数的 id "gk3probe.conf"
    # （systemd-boot 去掉计数后比 id，boot.c:1340-1376）。上机步骤用不带计数的文件名，见 README §9。
    put("loader/entries/gk3probe+3.conf", (
        "title      gk3probe (E3) — 只读探针\n"
        "sort-key   zzgk3probe\n"
        "efi        /EFI/gk3boot/probe/gk3probe.efi\n"
        "options    gk3probe.hold=1 gk3probe.keyscan=3000\n").encode())
    return dict(oneshot="gk3probe.conf", default_entry="%s-android-a.conf" % MID)


def make_fat(stage, sectors, out, fill=False):
    with open(out, "wb") as f:
        f.truncate(sectors * SECTOR)
    run(["mkfs.vfat", "-F", "32", "-S", str(SECTOR), "-n", "ESP", out])
    for top in sorted(os.listdir(stage)):
        run(["mcopy", "-s", "-o", "-i", out, os.path.join(stage, top), "::/"])
    if fill:
        # espfull 变体：用一个文件把剩余空间正好占满 —— 探针建目录 / 写日志会 VOLUME_FULL，测"写盘失败也不能挂"
        free = int(re.search(r"([\d ]+) bytes free", run(["mdir", "-i", out, "::/"])).group(1).replace(" ", ""))
        filler = out + ".filler"
        with open(filler, "wb") as f:
            f.truncate(free)
        run(["mcopy", "-o", "-i", out, filler, "::/filler.bin"])
        os.unlink(filler)
        left = int(re.search(r"([\d ]+) bytes free", run(["mdir", "-i", out, "::/"])).group(1).replace(" ", ""))
        if left:
            die("ESP 没填满（还剩 %d 字节）" % left)


# ---------------------------------------------------------------- mkdisk

def cmd_mkdisk(a):
    os.makedirs(a.out, exist_ok=True)
    if not os.path.exists(os.path.join(EFI_BUILD, "gk3-fake-android.efi")):
        die("先 make -C tools/gk3boot/efi（缺 gk3-fake-android.efi）")
    disk_guid, real = real_gpt()
    for n in ("misc", "boot_a", "boot_b", "super", "userdata", "metadata", "esp"):
        if n not in real:
            die("实机 GPT 向量里没有 %s" % n)

    # 分区排布
    lba = 2048
    parts = []
    misc = real["misc"]
    parts.append(dict(name="misc", index=misc["index"], first=34, last=2047))
    for idx, name, sz in LAYOUT:
        parts.append(dict(name=name, index=idx, first=lba, last=lba + sz - 1))
        lba += sz
    last_usable = lba + 2048 - 1
    total = last_usable + 1 + 33
    disk_path = os.path.join(a.out, "disk.img")

    # broken 变体：多一个同名 boot_b（p7，实机上的空项），再把 super 改名 —— 探针应记录 EDUP / ENOENT 然后照常跑完
    entries_by_index = {}
    for p in parts:
        r = real[p["name"]]
        entries_by_index[p["index"]] = gpt_entry(r["type"], r["guid"], p["first"], p["last"], r["attrs"], p["name"])
    if a.variant == "broken":
        sup = next(p for p in parts if p["name"] == "super")
        r = real["super"]
        entries_by_index[8] = gpt_entry(r["type"], r["guid"], sup["first"], sup["last"], 0, "super_renamed")
        b = next(p for p in parts if p["name"] == "boot_b")
        entries_by_index[7] = gpt_entry(real["boot_b"]["type"], uuid.uuid4().bytes_le, b["first"], b["last"], 0, "boot_b")
    table = b"".join(entries_by_index.get(i + 1, b"\0" * 128) for i in range(128))
    tcrc = zlib.crc32(table) & 0xffffffff

    with open(disk_path, "wb") as f:
        f.truncate(total * SECTOR)
        # 保护 MBR
        mbr = bytearray(SECTOR)
        struct.pack_into("<BBBBBBBBII", mbr, 446, 0, 0, 2, 0, 0xEE, 0xFF, 0xFF, 0xFF, 1, min(total - 1, 0xffffffff))
        mbr[510:512] = b"\x55\xaa"
        f.seek(0)
        f.write(mbr)
        f.write(gpt_header(1, total - 1, 34, last_usable, 2, disk_guid, tcrc))
        f.write(table)
        f.seek((total - 33) * SECTOR)
        f.write(table)
        f.write(gpt_header(total - 1, 1, 34, last_usable, total - 33, disk_guid, tcrc))

        def at(name):
            return next(p for p in parts if p["name"] == name)

        # misc：实机 64 KiB（或 --misc 给的改过的一份）
        f.seek(at("misc")["first"] * SECTOR)
        f.write(open(a.misc or MISC_VEC, "rb").read())
        # boot_a：完整的真 boot.img（有就用），否则合成；boot_b：总是合成（两份内容不同，各验各的）
        # --boot-a / --boot-b：gk3boot 测试用的现成镜像（Debian 内核 + 测试 initramfs 重新打的包）
        if a.boot_a:
            ba = open(a.boot_a, "rb").read()
            src_a = os.path.basename(a.boot_a) + " (%d bytes)" % len(ba)
        elif a.bootimg:
            ba = open(a.bootimg, "rb").read()
            src_a = os.path.basename(a.bootimg) + " (real, %d bytes)" % len(ba)
        else:
            ba, _ = synth_bootimg("fixture-a")
            src_a = "synthetic"
        if a.corrupt_a:
            # 坏一个 kernel 字节、头里的 id 不动 → SHA1(id) 对不上（§4.3.3 的 boot_corrupt）
            ba = bytearray(ba)
            ba[2048 + 4096] ^= 0xff
            ba = bytes(ba)
            src_a += " [kernel byte 4096 flipped]"
        bb = open(a.boot_b, "rb").read() if a.boot_b else synth_bootimg("fixture-b")[0]
        if a.corrupt_b:
            bb = bytearray(bb)
            bb[2048 + 4096] ^= 0xff
            bb = bytes(bb)
        for name, img in (("boot_a", ba), ("boot_b", bb)):
            p = at(name)
            if len(img) > (p["last"] - p["first"] + 1) * SECTOR:
                die("%s 放不下 %d 字节" % (name, len(img)))
            f.seek(p["first"] * SECTOR)
            f.write(img)
        # super / userdata / metadata：写一点可辨认的内容，快照比对时才有意义
        for name in ("super", "userdata", "metadata", "ubunturescue"):
            p = at(name)
            f.seek(p["first"] * SECTOR)
            f.write(("gk3fixture %s\n" % name).encode() * 64)

        # ESP
        stage = tempfile.mkdtemp()
        info = esp_tree(stage, a.variant, a.gk3boot_options, a.gk3boot_entry, a.loader_default, a.fastboot_img,
                        a.windows, a.gk3_windows_entry)
        esp = at("esp")
        fat = os.path.join(a.out, "esp.tmp")
        make_fat(stage, esp["last"] - esp["first"] + 1, fat, fill=a.variant == "espfull")
        shutil.rmtree(stage)
        f.seek(esp["first"] * SECTOR)
        with open(fat, "rb") as g:
            shutil.copyfileobj(g, f, 4 * 1024 * 1024)
        os.unlink(fat)

    manifest = dict(
        disk=disk_path, sector=SECTOR, total_sectors=total, variant=a.variant, parts=parts,
        boot_a_id=bootimg_id(ba), boot_b_id=bootimg_id(bb), boot_a_source=src_a, mid=MID,
        disk_guid=str(uuid.UUID(bytes_le=disk_guid)),
        esp_partuuid=str(uuid.UUID(bytes_le=real["esp"]["guid"])),
        misc_partuuid=str(uuid.UUID(bytes_le=real["misc"]["guid"])), **info)
    with open(os.path.join(a.out, "manifest.json"), "w") as g:
        json.dump(manifest, g, indent=1)
    print("✓ 夹具盘 %s（%d MiB，变体 %s，boot_a=%s）" % (disk_path, total * SECTOR >> 20, a.variant, src_a))


# ---------------------------------------------------------------- 快照与比较

def esp_extract(disk, m, outdir):
    esp = next(p for p in m["parts"] if p["name"] == "esp")
    fat = os.path.join(outdir, ".esp.img")
    with open(disk, "rb") as f, open(fat, "wb") as g:
        f.seek(esp["first"] * SECTOR)
        left = (esp["last"] - esp["first"] + 1) * SECTOR
        while left:
            b = f.read(min(left, 4 * 1024 * 1024))
            if not b:
                break
            g.write(b)
            left -= len(b)
    # 先只读查一遍 FAT：固件 / 被测程序把 FAT 写坏了（例如目录项指回根目录、成环）时，下面的 mcopy -s
    # 会无限递归、把容器的盘写满 —— 2026-10-05 gk3boot espfull 场景真遇到过（gk3efi.c gk3_log_open_seq 的注释）
    r = subprocess.run(["fsck.fat", "-n", fat], capture_output=True, text=True)
    if r.returncode:
        os.unlink(fat)
        die("ESP 的 FAT 不一致（fsck.fat -n → %d）：\n%s" % (r.returncode, (r.stdout + r.stderr)[-1500:]))
    tree = os.path.join(outdir, "esp")
    os.makedirs(tree, exist_ok=True)
    run(["mcopy", "-s", "-n", "-i", fat, "::/*", tree])
    os.unlink(fat)
    return tree


def cmd_snapshot(a):
    m = json.load(open(a.manifest))
    snap = dict(regions={}, esp={})
    with open(a.disk, "rb") as f:
        def region(name, first, n):
            f.seek(first * SECTOR)
            h = hashlib.sha256()
            left = n * SECTOR
            while left:
                b = f.read(min(left, 4 * 1024 * 1024))
                if not b:
                    break
                h.update(b)
                left -= len(b)
            snap["regions"][name] = h.hexdigest()
        region("gpt-primary", 0, 34)
        region("gpt-backup", m["total_sectors"] - 33, 33)
        for p in m["parts"]:
            if p["name"] != "esp":
                region(p["name"], p["first"], p["last"] - p["first"] + 1)
    tmp = tempfile.mkdtemp()
    tree = esp_extract(a.disk, m, tmp)
    for root, _, files in os.walk(tree):
        for fn in files:
            p = os.path.join(root, fn)
            snap["esp"][os.path.relpath(p, tree)] = sha256(open(p, "rb").read())
    shutil.rmtree(tmp)
    json.dump(snap, open(a.out, "w"), indent=1, sort_keys=True)


def cmd_diff(a):
    b, c = json.load(open(a.before)), json.load(open(a.after))
    bad = []
    for k, v in b["regions"].items():
        if c["regions"].get(k) != v:
            if k == "misc" and a.allow_misc:
                print("  misc 变了（交给 check_misc.py 逐字节判）")
                continue
            if k in (a.allow_parts or "").split(","):
                print("  %s 变了（--allow-parts，另行判内容）" % k)
                continue
            bad.append("块区域 %s 变了" % k)
    added = sorted(set(c["esp"]) - set(b["esp"]))
    removed = sorted(set(b["esp"]) - set(c["esp"]))
    changed = sorted(k for k in set(b["esp"]) & set(c["esp"]) if b["esp"][k] != c["esp"][k])
    logs = [x for x in added if (x.startswith("EFI/gk3boot/probe/log-") or x.startswith("EFI/gk3boot/log/boot-"))
            and x.endswith(".txt")]
    # systemd-boot 自己做的计数改名：loader/entries/gk3probe+N[-M].conf → 另一个计数（内容不变）
    counted = ("loader/entries/gk3probe+", "loader/entries/gk3boot-e4+", "loader/entries/gk3boot-android-")
    ren_from = [x for x in removed if x.startswith(counted)]
    ren_to = [x for x in added if x.startswith(counted)]
    for x in added:
        if x not in logs and x not in ren_to:
            bad.append("ESP 上多了 %s" % x)
    for x in removed:
        if x not in ren_from:
            bad.append("ESP 上少了 %s" % x)
    for x in changed:
        bad.append("ESP 上 %s 的内容变了" % x)
    if len(ren_from) != len(ren_to) or any(b["esp"][f] != c["esp"][t] for f, t in zip(ren_from, ren_to)):
        bad.append("条目改名前后内容不同或数量不对：%s → %s" % (ren_from, ren_to))
    if a.new_logs_out:
        with open(a.new_logs_out, "w") as g:
            g.write("".join(x + "\n" for x in logs))
    for x in bad:
        print("✗ " + x)
    print("  新日志：%s" % (", ".join(logs) or "（无）"))
    if ren_from:
        print("  systemd-boot 计数改名：%s → %s" % (", ".join(ren_from), ", ".join(ren_to)))
    if bad:
        sys.exit(1)
    print("✓ 盘上只多了自己的日志（%sboot_a / boot_b / super / userdata / metadata / GPT 逐字节未变）"
          % ("" if a.allow_misc else "misc / "))


# ---------------------------------------------------------------- AAVMF 变量

def cmd_vars(a):
    # 属性 NV|BS|RT = 7，UTF-16LE + 双 NUL（同 scripts/boot-oneshot.sh）
    sets = list(a.set or [])
    if a.oneshot:
        sets.append("LoaderEntryOneShot=" + a.oneshot)
    vs = []
    for kv in sets:
        k, v = kv.split("=", 1)
        vs.append(dict(name=k, guid="4a67b082-0a4c-41cf-b6c7-440b29bb8c4f", attr=7,
                       data=(v.encode("utf-16-le") + b"\0\0").hex()))
    cmd = ["virt-fw-vars", "-i", a.input, "-o", a.out]
    for k in a.delete or []:
        cmd += ["--delete", k]
    jf = a.out + ".json"
    if vs:
        json.dump(dict(version=2, variables=vs), open(jf, "w"))
        cmd += ["--set-json", jf]
    run(cmd)
    if vs:
        os.unlink(jf)
    print("✓ %s：%s%s" % (os.path.basename(a.out), " ".join(sets), "".join(" 删 " + k for k in a.delete or [])))


def rec_seal(r):
    struct.pack_into("<I", r, 2044, zlib.crc32(bytes(r[:2044])) & 0xffffffff)


def rec_valid(r):
    magic, ver, size = struct.unpack_from("<IHH", r, 0)
    return magic == 0x52334B47 and ver == 1 and size == 2048 and \
        struct.unpack_from("<I", r, 2044)[0] == zlib.crc32(bytes(r[:2044])) & 0xffffffff


def cmd_misc_set(a):
    """S15：GK3 记录的 Android 侧写者在 QEMU 里没有（夹具的"Android"是测试 initramfs）—— 这里照 README §5 / §16 的布局
    用 Python 独立写：boot_streak @20、next_kind @21、flags bit2 @8、set_default @360、default_os @361、CRC @2044"""
    m = json.load(open(a.manifest))
    p = next(x for x in m["parts"] if x["name"] == "misc")
    with open(a.disk, "r+b") as f:
        f.seek(p["first"] * SECTOR + 8192)
        r = bytearray(f.read(2048))
        if not rec_valid(r):
            r = bytearray(gk3_record())
        if a.streak is not None:
            r[20] = a.streak
        if a.next:
            r[21] = {"none": 0, "sdboot-menu": 1, "windows": 3}[a.next]
            r[22] = 0
        if a.set_default:
            r[360] = dict(none=0, windows=1, android=2)[a.set_default]
        if a.default_os:
            r[361] = dict(unknown=0, android=1, windows=2)[a.default_os]
        if a.poweroff:
            flags = struct.unpack_from("<I", r, 8)[0] | 4
            struct.pack_into("<I", r, 8, flags)
        rec_seal(r)
        f.seek(p["first"] * SECTOR + 8192)
        f.write(bytes(r))
    print("✓ misc GK3 记录：streak=%u next=%u flags=0x%x set_default=%u default_os=%u" % (
        r[20], r[21], struct.unpack_from("<I", r, 8)[0], r[360], r[361]))


def cmd_vars_get(a):
    """systemd-boot 厂商 GUID 下的变量（virt-fw-vars --output-json 导出后找）"""
    with tempfile.TemporaryDirectory() as t:
        jf = os.path.join(t, "v.json")
        run(["virt-fw-vars", "-i", a.input, "--output-json", jf])
        j = json.load(open(jf))
    for v in j.get("variables", []):
        if v.get("name") == a.name and v.get("guid", "").lower() == "4a67b082-0a4c-41cf-b6c7-440b29bb8c4f":
            print(bytes.fromhex(v["data"]).decode("utf-16-le", "replace").rstrip("\0"))
            return
    print("(absent)")


def cmd_part_get(a):
    m = json.load(open(a.manifest))
    p = next(x for x in m["parts"] if x["name"] == a.name)
    with open(a.disk, "rb") as f:
        f.seek(p["first"] * SECTOR)
        data = f.read((p["last"] - p["first"] + 1) * SECTOR)
    open(a.out, "wb").write(data)


def cmd_misc_get(a):
    m = json.load(open(a.manifest))
    p = next(x for x in m["parts"] if x["name"] == "misc")
    with open(a.disk, "rb") as f:
        f.seek(p["first"] * SECTOR)
        open(a.out, "wb").write(f.read(65536))


def cmd_esp_get(a):
    m = json.load(open(a.manifest))
    tmp = tempfile.mkdtemp()
    tree = esp_extract(a.disk, m, tmp)
    src = os.path.join(tree, a.path)
    if not os.path.exists(src):
        shutil.rmtree(tmp)
        die("ESP 上没有 %s" % a.path)
    shutil.copy(src, a.out)
    shutil.rmtree(tmp)


# ---------------------------------------------------------------- gk3boot（S5）测试用的产物

def cpio_newc(entries):
    """entries: (name, mode, data, rdev_major, rdev_minor)。newc 格式（"070701"，内核 init/initramfs.c）。"""
    out = bytearray()
    ino = 1
    for name, mode, data, rmaj, rmin in entries + [("TRAILER!!!", 0, b"", 0, 0)]:
        nm = name.encode() + b"\0"
        hdr = "070701" + "".join("%08X" % v for v in (
            ino, mode, 0, 0, 2 if mode & 0o040000 else 1, 0, len(data), 0, 0, rmaj, rmin, len(nm), 0))
        out += hdr.encode() + nm
        out += b"\0" * (-len(out) % 4)
        out += data
        out += b"\0" * (-len(out) % 4)
        ino += 1
    return bytes(out)


def cmd_initramfs(a):
    import gzip
    init = open(a.init, "rb").read()
    if init[:4] != b"\x7fELF":
        die("%s 不是 ELF" % a.init)
    ents = [
        ("dev", 0o040755, b"", 0, 0), ("dev/console", 0o020600, b"", 5, 1),
        ("proc", 0o040755, b"", 0, 0), ("sys", 0o040755, b"", 0, 0),
        ("init", 0o100755, init, 0, 0), ("gk3-initrd-marker", 0o100644, a.marker.encode() + b"\n", 0, 0),
    ]
    data = gzip.compress(cpio_newc(ents), mtime=0)
    open(a.out, "wb").write(data)
    print("✓ initramfs %s（%d 字节，marker %s）" % (a.out, len(data), a.marker))


def fdt_add_root_prop(dtb, name, value):
    """给根节点开头插一个字符串属性。只支持 libfdt 的标准布局（头 / rsvmap / struct / strings 依次排）。"""
    magic, total, off_struct, off_str, off_rsv, ver, _, _, size_str, size_struct = struct.unpack_from(">10I", dtb, 0)
    if magic != 0xd00dfeed or ver < 17 or not (off_rsv < off_struct < off_str):
        die("不认识的 FDT 布局")
    if struct.unpack_from(">I", dtb, off_struct)[0] != 1 or dtb[off_struct + 4] != 0:
        die("struct 块不以根节点开头")
    ins_at = off_struct + 8                      # BEGIN_NODE + 根节点名 ""（补齐到 4 字节）
    strings = dtb[off_str:off_str + size_str]
    nameoff = len(strings)
    val = value.encode() + b"\0"
    prop = struct.pack(">III", 3, len(val), nameoff) + val + b"\0" * (-len(val) % 4)
    new_struct = dtb[off_struct:ins_at] + prop + dtb[ins_at:off_struct + size_struct]
    new_strings = strings + name.encode() + b"\0"
    head = bytearray(dtb[:off_struct])
    new_off_str = off_struct + len(new_struct)
    total = new_off_str + len(new_strings)
    struct.pack_into(">I", head, 4, total)
    struct.pack_into(">I", head, 12, new_off_str)
    struct.pack_into(">I", head, 32, len(new_strings))
    struct.pack_into(">I", head, 36, len(new_struct))
    return bytes(head) + new_struct + new_strings


EXEC_HOOK = r"""# S7c 端到端的测试钩子（只在测试 overlay 里有）：/init 解析完 cmdline 之后 source
for m in %(mods)s; do insmod /lib/modules-test/$m 2>/dev/null || echo "[test-hook] insmod $m failed" > /dev/console; done
GK3_UDC=dummy_udc.0
GK3_TRACE=1
GK3_QUIET=0
# 夹具的盘是 NVMe（模块，异步探测）：等它出来再往下走 —— 真机的 nvme 是内建的，/init 跑起来时早就在了
i=0; while [ ! -e /sys/block/nvme0n1 ] && [ $i -lt 100 ]; do sleep 0.1; i=$((i + 1)); done
echo "[test-hook] modules loaded, disk $(ls /sys/block | tr '
' ' ')" > /dev/console
# 宿主 fastboot 走 TCP（gk3.fbtcp=1）：QEMU user 网络的固定地址（qemu_run.py --net-fwd）
busybox ip link set lo up 2>/dev/null
busybox ip link set eth0 up 2>/dev/null && busybox ip addr add 10.0.2.15/24 dev eth0 2>/dev/null
echo "[test-hook] net: $(busybox ip -o -4 addr show 2>/dev/null | tr '
' ' ')" > /dev/console
# 守护进程的日志抄到串口（判定用）
( while [ ! -f /run/gk3/fastbootd.log ]; do sleep 0.2; done; tail -n +1 -f /run/gk3/fastbootd.log | sed 's/^/[fbd] /' > /dev/console ) &
"""


def cmd_exec_initrd(a):
    """执行端的 initrd（S7c 端到端）：fastboot.img 原样 + 一段测试 overlay（gzip cpio 直接拼，内核按顺序解；
    与 initramfs/test/qemu_fbi.py 同一种做法）。overlay 只多出 test-hook 与测试内核的模块，不替换被测的任何文件。"""
    order = [m.strip() for m in open(os.path.join(a.modules, "order")) if m.strip()]
    mods = [m for m in order if not m.startswith(("drm", "virtio-gpu", "virtio_gpu", "virtio_dma_buf"))]
    ents = [("etc", 0o040755, b"", 0, 0), ("etc/gk3-fbi", 0o040755, b"", 0, 0),
            ("etc/gk3-fbi/test-hook", 0o100644, (EXEC_HOOK % dict(mods=" ".join(mods))).encode(), 0, 0),
            ("lib", 0o040755, b"", 0, 0), ("lib/modules-test", 0o040755, b"", 0, 0)]
    for m in mods:
        ents.append(("lib/modules-test/" + m, 0o100644, open(os.path.join(a.modules, m), "rb").read(), 0, 0))
    import gzip
    with open(a.out, "wb") as f:
        f.write(open(a.img, "rb").read())
        f.write(gzip.compress(cpio_newc(ents), mtime=0))
    print("✓ 执行端 initrd %s（fastboot.img %d 字节 + overlay：%d 个模块）" % (a.out, os.path.getsize(a.img), len(mods)))


def cmd_fdt_mark(a):
    d = fdt_add_root_prop(open(a.input, "rb").read(), a.name, a.value)
    open(a.out, "wb").write(d)
    print("✓ %s：根节点加 %s = \"%s\"（%d 字节）" % (a.out, a.name, a.value, len(d)))


def cmd_mkbootimg(a):
    img, sha = mkbootimg(open(a.kernel, "rb").read(), open(a.ramdisk, "rb").read(), open(a.dtb, "rb").read(),
                         a.cmdline.encode(), a.name.encode())
    open(a.out, "wb").write(img)
    print("✓ boot.img %s（%d 字节，id %s，cmdline %d 字节%s）" % (
        a.out, len(img), sha, len(a.cmdline), "，切进了 extra_cmdline" if len(a.cmdline) > 511 else ""))


def bcab_slot(prio, tries, ok):
    return prio | (tries << 4) | (int(ok) << 7)


def gk3_record(migrated=False, streak=0, ok_streak=0):
    """GK3 记录 v1（README §5 的布局）：magic / version / size / flags，boot_streak 在 20、ok_streak 在 27，CRC32 在 2044。"""
    r = bytearray(2048)
    struct.pack_into("<IHHII", r, 0, 0x52334B47, 1, 2048, 1 if migrated else 0, 1 if migrated else 0)
    r[20] = streak
    r[27] = ok_streak
    struct.pack_into("<I", r, 2044, zlib.crc32(bytes(r[:2044])) & 0xffffffff)
    return bytes(r)


def bcb_recovery(*args):
    """bootloader_message 的 boot-recovery 写法（bootloader_message.cpp:214-232）"""
    b = bytearray(2048)
    b[0:13] = b"boot-recovery"
    r = ("recovery\n" + "".join(x + "\n" for x in args)).encode()
    b[64:64 + len(r)] = r
    return bytes(b)


def cmd_misc(a):
    """实机 misc 向量（_a 15/1/已成功、_b 14/0、VAB 无合并、BCB 空、8 KiB 全零）改出来的变体：
      b-active   像 setActive(b) 之后、新槽还没开过机：_b 15/6 未成功，_a 降到 14、仍是已成功
      b-try3     同上但 _b 只剩 3 次（tries 扣到 0 → 第 4 次开机回落到 _a）
      b-ok       _b 15/1/已成功、_a 14/0（= 2026-10-05 开发机的样子，E4 日志）
      bcb-wipe   原样 + BCB = 设置里的"清除所有数据"（boot-recovery / --wipe_data --reason=…）
      bcb-wipe-migrated  bcb-wipe + 一份已迁移的 GK3 记录（分派打开时会走到"进执行端 why=wipe"那一支）
      merging    VAB merge_status=MERGING（源槽 _a）+ _b 15/0 未成功（不可启动）、_a 14/1 已成功 → 守卫：不许回落到 _a
      bcb-bootloader-migrated  BCB = bootonce-bootloader（adb reboot bootloader）+ 已迁移的记录（S7c：进执行端 why=bootloader）
      bootloop   已迁移的记录、boot_streak = ok_streak = 5（已确认的 _a 连续 5 次没开机完成）、BCB 空（S7c：why=bootloop）"""
    m = bytearray(open(MISC_VEC, "rb").read())
    bc = m[2048:2080]
    v = a.variant
    if v in ("b-active", "b-try3", "merging"):
        bc[0:3] = b"_b\0"
        tries = {"b-active": 6, "b-try3": 3, "merging": 0}[v]
        struct.pack_into("<HH", bc, 12, bcab_slot(14, 1, True), bcab_slot(15, tries, False))
    elif v == "b-ok":
        bc[0:3] = b"_b\0"
        struct.pack_into("<HH", bc, 12, bcab_slot(14, 0, False), bcab_slot(15, 1, True))
    elif v == "bcb-bootloader-migrated":
        m[0:2048] = b"\0" * 2048
        m[0:19] = b"bootonce-bootloader"
        m[8192:10240] = gk3_record(migrated=True)
    elif v == "bootloop":
        m[8192:10240] = gk3_record(migrated=True, streak=5, ok_streak=5)
    elif v in ("bcb-wipe", "bcb-wipe-migrated"):
        m[0:2048] = bcb_recovery("--wipe_data", "--reason=MasterClearConfirm", "--locale=zh-CN")
        if v == "bcb-wipe-migrated":
            m[8192:10240] = gk3_record(migrated=True)
    else:
        die("不认识的 misc 变体 %s" % a.variant)
    if v == "merging":
        vab = m[32768:32832]
        if vab[0] != 2 or struct.unpack_from("<I", vab, 1)[0] != 0x56740AB0:
            die("实机向量里的 virtual_ab 消息不是 v2")
        m[32768 + 5] = 3           # merge_status = MERGING（bootloader_message.h:88-94）
        m[32768 + 6] = 0           # source_slot = _a
    struct.pack_into("<I", bc, 28, zlib.crc32(bytes(bc[:28])) & 0xffffffff)
    m[2048:2080] = bc
    open(a.out, "wb").write(m)
    print("✓ misc %s（%s，bcab %s）" % (a.out, a.variant, bytes(bc).hex()))


def main():
    ap = argparse.ArgumentParser()
    sp = ap.add_subparsers(dest="cmd", required=True)
    p = sp.add_parser("mkdisk")
    p.add_argument("--out", required=True)
    p.add_argument("--bootimg")
    p.add_argument("--variant", default="normal", choices=["normal", "broken", "espfull"])
    p.add_argument("--gk3boot-options")
    p.add_argument("--boot-a")
    p.add_argument("--boot-b")
    p.add_argument("--misc")
    p.add_argument("--corrupt-a", action="store_true")
    p.add_argument("--corrupt-b", action="store_true")
    p.add_argument("--gk3boot-entry", default=GK3BOOT_ENTRY)
    p.add_argument("--loader-default", default="*-android-a.conf")
    p.add_argument("--fastboot-img", help="放到 EFI/gk3boot/e4/fastboot.img 的执行端 initramfs（S7c）")
    p.add_argument("--windows", action="store_true", help="S15：ESP 上放 EFI/Microsoft/Boot/bootmgfw.efi（假 Windows）")
    p.add_argument("--gk3-windows-entry", action="store_true", help="S15：再写 loader/entries/gk3-windows.conf（U22）")
    p = sp.add_parser("snapshot")
    p.add_argument("disk")
    p.add_argument("manifest")
    p.add_argument("out")
    p = sp.add_parser("diff")
    p.add_argument("before")
    p.add_argument("after")
    p.add_argument("--allow-misc", action="store_true")
    p.add_argument("--allow-parts", help="逗号分隔：这些分区允许变（执行端的恢复出厂擦 userdata / metadata）")
    p.add_argument("--new-logs-out")
    p = sp.add_parser("vars-get")
    p.add_argument("input")
    p.add_argument("name")
    p = sp.add_parser("part-get")
    p.add_argument("disk")
    p.add_argument("manifest")
    p.add_argument("name")
    p.add_argument("out")
    p = sp.add_parser("misc-get")
    p.add_argument("disk")
    p.add_argument("manifest")
    p.add_argument("out")
    p = sp.add_parser("vars")
    p.add_argument("input")
    p.add_argument("out")
    p.add_argument("--oneshot")
    p.add_argument("--set", action="append", help="NAME=VALUE（systemd-boot 厂商 GUID，属性 7，UTF-16LE + NUL）")
    p.add_argument("--del", dest="delete", action="append", help="删掉这个变量")
    p = sp.add_parser("misc-set")
    p.add_argument("disk")
    p.add_argument("manifest")
    p.add_argument("--streak", type=int)
    p.add_argument("--next", choices=["windows", "sdboot-menu", "none"])
    p.add_argument("--set-default", choices=["windows", "android", "none"])
    p.add_argument("--default-os", choices=["unknown", "android", "windows"])
    p.add_argument("--poweroff", action="store_true", help="置 flags bit2 clean_poweroff（= on shutdown 的 mark-poweroff）")
    p = sp.add_parser("esp-get")
    p.add_argument("disk")
    p.add_argument("manifest")
    p.add_argument("path")
    p.add_argument("out")
    p = sp.add_parser("initramfs")
    p.add_argument("--init", required=True)
    p.add_argument("--marker", required=True)
    p.add_argument("--out", required=True)
    p = sp.add_parser("exec-initrd")
    p.add_argument("--img", required=True)
    p.add_argument("--modules", required=True)
    p.add_argument("--out", required=True)
    p = sp.add_parser("fdt-mark")
    p.add_argument("input")
    p.add_argument("out")
    p.add_argument("name")
    p.add_argument("value")
    p = sp.add_parser("mkbootimg")
    p.add_argument("--kernel", required=True)
    p.add_argument("--ramdisk", required=True)
    p.add_argument("--dtb", required=True)
    p.add_argument("--cmdline", required=True)
    p.add_argument("--name", default="")
    p.add_argument("--out", required=True)
    p = sp.add_parser("misc")
    p.add_argument("--variant", required=True)
    p.add_argument("--out", required=True)
    a = ap.parse_args()
    dict(mkdisk=cmd_mkdisk, snapshot=cmd_snapshot, diff=cmd_diff, vars=cmd_vars, esp_get=cmd_esp_get,
         vars_get=cmd_vars_get, misc_get=cmd_misc_get, part_get=cmd_part_get, misc_set=cmd_misc_set,
         initramfs=cmd_initramfs, fdt_mark=cmd_fdt_mark, exec_initrd=cmd_exec_initrd, mkbootimg=cmd_mkbootimg, misc=cmd_misc)[
        a.cmd.replace("-", "_")](a)


if __name__ == "__main__":
    main()
