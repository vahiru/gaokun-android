#!/usr/bin/env python3
"""把华为 MateBook E Go（gaokun3）BIOS 2.16 的官方升级包拆到 DXE 模块一级。

    python3 extract.py Gaokun_8CX_BIOS_P02-02W-06F_2.16.exe <输出目录>

只用 Python 标准库（lzma / zlib / hashlib），不依赖 7z、UEFITool。
拆包链（每一层都有断言，结构不符就停，不猜）：

  1. NSIS 安装包（非 solid，LZMA）     → 按块顺序解出 16 个数据块，
                                         其中以 FMP capsule GUID 开头的那块就是固件 capsule
  2. FMP capsule（EFI_CAPSULE_HEADER + EFI_FIRMWARE_MANAGEMENT_CAPSULE_HEADER，
     ImageHeader v2 + EFI_FIRMWARE_IMAGE_AUTHENTICATION（PKCS7 签名））
                                       → capsule 内的 FV
  3. 该 FV 里的 RAW 文件 0a85a45e-…    → 一个 aarch64 ELF（XBL 的 UEFI 段，
                                         构建路径 Build_Gen3_XBL_V_FLASH/…/SocPkg/Makena）
  4. ELF 里 VA 0x9f000000 的 PT_LOAD 段 → UEFI FD 的 FV（6 MiB）
  5. 其中 9e21fd93-… 文件的 GUIDED 段（1d301fe9-…，内容是 gzip） → 解压
  6. 解压结果里的 FV image 段          → DXE FV（全部 DXE 驱动 / 应用 / 配置文件）

输出：
  <out>/stages.tsv        每一层中间产物的偏移、大小、sha256
  <out>/manifest.tsv      FD FV 与 DXE FV 里每个 FFS 文件：所在 FV、GUID、类型、UI 名、
                          文件大小、PE/TE 段大小与 sha256、depex 原文（hex）
  <out>/pe/<名字>.efi     PE32/TE 段原样导出（同名冲突时加 GUID 前缀）
  <out>/pe/<名字>.depex   依赖表达式段
  <out>/raw/<名字>        FREEFORM / RAW 段（uefiplat.cfg、BDS_Menu.cfg、bmp 等）
  <out>/stage/*.bin       中间产物（capsule、ELF、FD FV、gzip 段、DXE 段流）

⚠️ 产物是华为 / 高通的专有固件，**不要入库**。入库的只有本脚本和 manifest（见 ../README.md）。
"""
import hashlib
import lzma
import os
import struct
import sys
import uuid
import zlib

EXE_SHA256 = "b24abe76148bed32e3add10827701025cb917e216a359861e6f57f0f9797d1a6"

FMP_CAPSULE_GUID = "6dcbd5ed-e82d-4c44-bda1-7194199ad92a"   # EFI_FIRMWARE_MANAGEMENT_CAPSULE_ID_GUID
CERT_TYPE_PKCS7 = "4aafd29d-68df-49ee-8aa9-347d375665a7"    # EFI_CERT_TYPE_PKCS7_GUID
FFS2_GUID = "8c8ce578-8a3d-4f1c-9935-896185c32dd3"          # EFI_FIRMWARE_FILE_SYSTEM2_GUID
XBL_ELF_FILE = "0a85a45e-915f-49db-8bd5-5337861f8082"       # capsule FV 里装 ELF 的 RAW 文件
FD_VA = 0x9F000000                                          # ELF 里 UEFI FD 段的装载地址
DXE_CONTAINER_FILE = "9e21fd93-9c72-4c15-8c4b-e77f1db2d792"  # FD FV 里装压缩 DXE FV 的文件
GZIP_SECTION_GUID = "1d301fe9-be79-4353-91c2-d23bc959ae0c"   # 高通的 gzip GUIDED 段
LZMA_SECTION_GUID = "ee4e5898-3914-4259-9d6e-dc7bd79403cf"   # LZMA_CUSTOM_DECOMPRESS_GUID

FFS_TYPES = {
    0x01: "RAW", 0x02: "FREEFORM", 0x03: "SEC", 0x04: "PEI_CORE", 0x05: "DXE_CORE",
    0x06: "PEIM", 0x07: "DRIVER", 0x08: "COMBINED", 0x09: "APPLICATION", 0x0A: "MM",
    0x0B: "FV_IMAGE", 0x0C: "COMBINED_MM_DXE", 0x0D: "MM_CORE", 0xF0: "PAD",
}


def die(msg):
    sys.exit("extract.py: " + msg)


def guid(b):
    return str(uuid.UUID(bytes_le=bytes(b[:16])))


def sha(b):
    return hashlib.sha256(b).hexdigest()


# ---------------------------------------------------------------- 1. NSIS

def nsis_blocks(exe):
    i = exe.find(b"\xef\xbe\xad\xdeNullsoftInst")
    if i < 4:
        die("没找到 NSIS firstheader")
    base = i - 4
    flags, sig = struct.unpack_from("<II", exe, base)
    hlen, total = struct.unpack_from("<II", exe, base + 20)
    p, end = base + 28, base + total
    blocks = []
    while p + 4 <= end:
        n = struct.unpack_from("<I", exe, p)[0]
        comp, n = bool(n & 0x80000000), n & 0x7FFFFFFF
        raw = exe[p + 4:p + 4 + n]
        if len(raw) != n:
            break                                   # 末尾的 CRC32
        if comp:
            if raw[0] != 0x5D:
                die("NSIS 块不是 LZMA（props 0x%02x）" % raw[0])
            props, dic = raw[0], struct.unpack_from("<I", raw, 1)[0]
            lc, lp, pb = props % 9, (props // 9) % 5, props // 45
            dec = lzma.LZMADecompressor(lzma.FORMAT_RAW, filters=[
                {"id": lzma.FILTER_LZMA1, "dict_size": dic, "lc": lc, "lp": lp, "pb": pb}])
            data = dec.decompress(raw[5:])
        else:
            data = raw
        blocks.append((p, data))
        p += 4 + n
    if not blocks or len(blocks[0][1]) != hlen:
        die("NSIS 头块长度不符")
    return blocks


# ---------------------------------------------------------------- 2. FMP capsule

def capsule_fv(cap):
    if guid(cap) != FMP_CAPSULE_GUID:
        die("不是 FMP capsule")
    hsize, cflags, csize = struct.unpack_from("<III", cap, 16)
    ver, ndrv, nitem = struct.unpack_from("<IHH", cap, hsize)
    if ver != 1 or ndrv != 0 or nitem != 1:
        die("FMP capsule 头不认识（ver %d drivers %d items %d）" % (ver, ndrv, nitem))
    item = hsize + struct.unpack_from("<Q", cap, hsize + 8)[0]
    iver = struct.unpack_from("<I", cap, item)[0]
    type_id = guid(cap[item + 4:])
    idx = cap[item + 20]
    isize, vsize = struct.unpack_from("<II", cap, item + 24)
    ihdr = {1: 32, 2: 40, 3: 48}.get(iver)
    if ihdr is None:
        die("ImageHeader 版本 %d 不认识" % iver)
    img = item + ihdr
    mono = struct.unpack_from("<Q", cap, img)[0]
    wlen, wrev, wtype = struct.unpack_from("<IHH", cap, img + 8)
    if wtype != 0x0EF1 or guid(cap[img + 16:]) != CERT_TYPE_PKCS7:
        die("capsule 签名不是 WIN_CERT_UEFI_GUID/PKCS7")
    fv_off = img + 8 + wlen
    # 签名之后、FV 之前还有 16 字节的厂商头：'MSS1' + u32 头长 0x10 + 两个 u32，
    # 本包两处都是 0x00020016（= 2.16，与实机 ESRT fw_version 131094 相同）。字段含义是推断。
    vendor = cap[fv_off:fv_off + 16]
    if vendor[:4] == b"MSS1":
        fv_off += struct.unpack_from("<I", vendor, 4)[0]
    info = {
        "vendor_hdr": vendor.hex(),
        "capsule_size": csize, "update_image_type_id": type_id, "update_image_index": idx,
        "image_header_version": iver, "update_image_size": isize, "vendor_code_size": vsize,
        "monotonic_count": mono, "pkcs7_len": wlen, "fv_offset": fv_off,
    }
    return fv_off, info


# ---------------------------------------------------------------- FV / FFS / 段

def fv_files(data):
    """遍历一个 FFS2 FV，产出 (偏移, guid, 类型, 文件体)。"""
    if data[0x28:0x2C] != b"_FVH":
        die("FV 签名不对：" + data[:0x30].hex())
    if guid(data[0x10:]) != FFS2_GUID:
        die("不是 FFS2 文件系统：" + guid(data[0x10:]))
    flen = struct.unpack_from("<Q", data, 0x20)[0]
    hlen = struct.unpack_from("<H", data, 0x30)[0]
    ext = struct.unpack_from("<H", data, 0x34)[0]
    p = hlen
    if ext:
        p = ext + struct.unpack_from("<I", data, ext + 16)[0]
    p = (p + 7) & ~7
    while p + 24 <= flen:
        if data[p:p + 24] == b"\xff" * 24:
            break
        ftype, attr = data[p + 18], data[p + 19]
        size, hl = int.from_bytes(data[p + 20:p + 23], "little"), 24
        if attr & 0x01 and size == 0:               # FFS_ATTRIB_LARGE_FILE
            size, hl = struct.unpack_from("<Q", data, p + 24)[0], 32
        if size < hl:
            die("FFS 文件大小坏了 @%#x" % p)
        yield p, guid(data[p:]), ftype, data[p + hl:p + size]
        p = (p + size + 7) & ~7


def sections(data):
    """展开段流（GUIDED 段就地解压），产出 (类型, 段体)。"""
    q = 0
    while q + 4 <= len(data):
        size, stype, hl = int.from_bytes(data[q:q + 3], "little"), data[q + 3], 4
        if size == 0xFFFFFF:
            size, hl = struct.unpack_from("<I", data, q + 4)[0], 8
        if size < hl:
            break
        body = data[q + hl:q + size]
        if stype == 0x02:                           # GUID_DEFINED
            sg = guid(body)
            off, attr = struct.unpack_from("<HH", body, 16)
            inner = body[off - hl:]
            if sg == LZMA_SECTION_GUID:
                inner = lzma.LZMADecompressor(lzma.FORMAT_ALONE).decompress(inner)
            elif sg == GZIP_SECTION_GUID:
                z = zlib.decompressobj(31)
                inner = z.decompress(inner)
            elif attr & 0x01:                       # PROCESSING_REQUIRED 但不认识
                yield ("GUIDED?" + sg, body)
                q = (q + size + 3) & ~3
                continue
            yield ("GUIDED " + sg, body)
            yield from sections(inner)
        else:
            yield (stype, body)
        q = (q + size + 3) & ~3


# ---------------------------------------------------------------- 主流程

def main():
    if len(sys.argv) != 3:
        die("用法：extract.py <BIOS 安装包 .exe> <输出目录>")
    exe = open(sys.argv[1], "rb").read()
    out = sys.argv[2]
    if sha(exe) != EXE_SHA256:
        print("⚠️ 安装包 sha256 不是已知的 2.16（%s），照拆但结果可能不同" % sha(exe), file=sys.stderr)
    for d in ("pe", "raw", "stage"):
        os.makedirs(os.path.join(out, d), exist_ok=True)

    stages = [("installer.exe", 0, exe)]

    blocks = nsis_blocks(exe)
    caps = [(o, b) for o, b in blocks if len(b) > 16 and guid(b) == FMP_CAPSULE_GUID]
    if len(caps) != 1:
        die("NSIS 里应恰好一个 FMP capsule，实际 %d 个" % len(caps))
    cap_off, cap = caps[0]
    cap_index = [o for o, _ in blocks].index(cap_off)
    stages.append(("capsule.bin (NSIS 块 #%d)" % cap_index, cap_off, cap))

    fv_off, capinfo = capsule_fv(cap)
    cap_fv = cap[fv_off:]
    elf = None
    for p, g, t, body in fv_files(cap_fv):
        if g == XBL_ELF_FILE:
            elf = body
            elf_off = fv_off + p          # FFS 文件头所在偏移（文件体在其后 24 字节）
    if elf is None or elf[:4] != b"\x7fELF" or elf[4] != 2:
        die("capsule FV 里没有 64 位 ELF 文件 " + XBL_ELF_FILE)
    stages.append(("xbl_uefi.elf (capsule FV 文件 %s)" % XBL_ELF_FILE, elf_off, elf))

    phoff = struct.unpack_from("<Q", elf, 0x20)[0]
    phes, phnum = struct.unpack_from("<HH", elf, 0x36)
    fd = None
    for i in range(phnum):
        ptype, pflags, off, va, pa, fsz, msz, al = struct.unpack_from("<IIQQQQQQ", elf, phoff + i * phes)
        if ptype == 1 and va == FD_VA and fsz:
            fd, fd_off = elf[off:off + fsz], off
    if fd is None:
        die("ELF 里没有 VA %#x 的 PT_LOAD 段" % FD_VA)
    stages.append(("fd_fv.bin (ELF PT_LOAD va %#x)" % FD_VA, fd_off, fd))

    manifest = []
    gz = dxe_stream = None
    for p, g, t, body in fv_files(fd):
        manifest.append(("FD", p, g, t, body))
        if g == DXE_CONTAINER_FILE:
            size, stype = int.from_bytes(body[0:3], "little"), body[3]
            if stype != 0x02 or guid(body[4:]) != GZIP_SECTION_GUID:
                die("DXE 容器文件的第一段不是 gzip GUIDED 段")
            off = struct.unpack_from("<H", body, 20)[0]
            gz = body[off:size]
            gz_off = p
            z = zlib.decompressobj(31)
            dxe_stream = z.decompress(gz)
            if z.unused_data:
                die("gzip 段尾部有多余数据")
    if dxe_stream is None:
        die("FD FV 里没有 DXE 容器文件 " + DXE_CONTAINER_FILE)
    stages.append(("fvimg_gzip.bin (文件 %s 的 GUIDED 段体)" % DXE_CONTAINER_FILE, gz_off, gz))
    stages.append(("dxe_sections.bin (gunzip 结果 = 段流)", 0, dxe_stream))

    dxe_fv = None
    for st, body in sections(dxe_stream):
        if st == 0x17:
            dxe_fv = body
    if dxe_fv is None:
        die("段流里没有 FV image 段")
    stages.append(("dxe_fv.bin (FV image 段体)", 0, dxe_fv))
    for p, g, t, body in fv_files(dxe_fv):
        manifest.append(("DXE", p, g, t, body))

    # ---- 写产物
    for name, off, blob in stages:
        fn = name.split(" ")[0]
        if fn != "installer.exe":
            open(os.path.join(out, "stage", fn), "wb").write(blob)
    with open(os.path.join(out, "stages.tsv"), "w") as f:
        f.write("# 产物\t在上一层里的偏移\t大小\tsha256\n")
        for name, off, blob in stages:
            f.write("%s\t%#x\t%d\t%s\n" % (name, off, len(blob), sha(blob)))
        f.write("# capsule: %s\n" % " ".join("%s=%s" % kv for kv in capinfo.items()))

    used = {}
    rows = []
    for fvname, p, g, t, body in manifest:
        ui = pe = te = depex = None
        raws = []
        for st, sb in sections(body) if t not in (0x01, 0xF0) else []:
            if st == 0x15:
                ui = sb.decode("utf-16le").rstrip("\0")
            elif st == 0x10:
                pe = sb
            elif st == 0x12:
                te = sb
            elif st == 0x13:
                depex = sb
            elif st in (0x19, 0x18):                # RAW / FREEFORM_SUBTYPE_GUID
                raws.append(sb)
        name = ui or g
        key = name
        if key in used:
            key = g + "-" + name
        used[key] = 1
        img = pe or te
        if img is not None:
            open(os.path.join(out, "pe", key + (".efi" if pe else ".te")), "wb").write(img)
        if depex is not None:
            open(os.path.join(out, "pe", key + ".depex"), "wb").write(depex)
        if t == 0x01:
            open(os.path.join(out, "raw", key), "wb").write(body)
        elif raws and img is None and t != 0x0B and any(raws):
            open(os.path.join(out, "raw", key), "wb").write(b"".join(raws))
        rows.append((fvname, "%#x" % p, g, FFS_TYPES.get(t, "%#x" % t), ui or "-", str(len(body)),
                     ("PE32" if pe else "TE" if te else "-"),
                     str(len(img)) if img is not None else "-",
                     sha(img) if img is not None else "-",
                     depex.hex() if depex is not None else "-"))
    with open(os.path.join(out, "manifest.tsv"), "w") as f:
        f.write("# fv\t偏移\tguid\t类型\tUI名\t文件大小\t映像\t映像大小\t映像sha256\tdepex\n")
        for r in rows:
            f.write("\t".join(r) + "\n")
    print("stages %d, FD 文件 %d, DXE 文件 %d → %s" % (
        len(stages), sum(1 for m in manifest if m[0] == "FD"),
        sum(1 for m in manifest if m[0] == "DXE"), out))


if __name__ == "__main__":
    main()
