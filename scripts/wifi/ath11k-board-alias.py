#!/usr/bin/env python3
"""给WCN6855的board-2.bin加一个板级数据名称，让本机芯片使用HW_GK3条目的数据。

用法：
  python3 -I scripts/wifi/ath11k-board-alias.py <board-2.bin>          # 写入（已有则不改动）
  python3 -I scripts/wifi/ath11k-board-alias.py <board-2.bin> --check  # 只检查

ath11k按“PCI ID + qmi-chip-id + qmi-board-id + 设备树的qcom,calibration-variant”拼出名称，
在board-2.bin里查找板级数据；找不到时去掉variant再找一次。本机芯片上报chip_id 0x12（18），
linux-firmware里的HW_GK3条目却登记在qmi-chip-id=2下（与联想X13s的qmi-chip-id=2条目共用同一份数据；
X13s的qmi-chip-id=18条目用的是NTM_TW220那份数据），
所以设备树写HW_GK3时匹配不到，会落到去掉variant的通用条目：实测信号比HW_GK3数据弱约22 dB。
这里为qmi-chip-id=18补一个指向同一份数据的名称，不改动任何数据。

写入前核对HW_GK3数据的SHA-256，linux-firmware换了这份数据时直接报错，由维护者重新评估。
"""
import hashlib
import os
import struct
import sys

MAGIC = b"QCA-ATH11K-BOARD\0"
PADDING = b"\x6d"  # ath11k-bdencoder的填充字节，保持与原文件一致
IE_BOARD = 0
SUB_NAME = 0
SUB_DATA = 1

PREFIX = "bus=pci,vendor=17cb,device=1103,subsystem-vendor=17cb,subsystem-device=0108"
SRC_NAME = PREFIX + ",qmi-chip-id=2,qmi-board-id=255,variant=HW_GK3"
DST_NAME = PREFIX + ",qmi-chip-id=18,qmi-board-id=255,variant=HW_GK3"
DATA_SHA256 = "3ee912c4bf015d314198c7c50829deb611b97e2499c13debd736485e72c67fa0"


def pad(b):
    return b + PADDING * ((-len(b)) % 4)


def parse(raw):
    """返回[(ie_id, [(sub_id, bytes), ...] 或 原始bytes), ...]。"""
    if not raw.startswith(MAGIC):
        sys.exit("错误：不是ath11k的board-2.bin（文件头不符）")
    off = len(pad(MAGIC))
    ies = []
    while off < len(raw):
        if off + 8 > len(raw):
            sys.exit(f"错误：偏移{off}处的IE头不完整")
        ie_id, ie_len = struct.unpack_from("<II", raw, off)
        off += 8
        body = raw[off:off + ie_len]
        if len(body) != ie_len:
            sys.exit(f"错误：偏移{off}处的IE长度越界")
        off += ie_len + (-ie_len) % 4
        if ie_id != IE_BOARD:
            ies.append((ie_id, body))
            continue
        subs = []
        o = 0
        while o < len(body):
            sub_id, sub_len = struct.unpack_from("<II", body, o)
            o += 8
            subs.append((sub_id, body[o:o + sub_len]))
            o += sub_len + (-sub_len) % 4
        ies.append((ie_id, subs))
    return ies


def encode(ies):
    out = pad(MAGIC)
    for ie_id, body in ies:
        if ie_id == IE_BOARD:
            body = b"".join(struct.pack("<II", s, len(d)) + pad(d) for s, d in body)
        out += struct.pack("<II", ie_id, len(body)) + pad(body)
    return out


def lookup(ies, name):
    """按ath11k的规则：名称匹配后，同一个IE里随后的第一个DATA就是它的数据。"""
    want = name.encode()
    for ie_index, (ie_id, subs) in enumerate(ies):
        if ie_id != IE_BOARD:
            continue
        matched = None
        for i, (sub_id, data) in enumerate(subs):
            if sub_id == SUB_NAME and data == want:
                matched = i
            elif sub_id == SUB_DATA and matched is not None:
                return ie_index, matched, data
    return None


def main():
    args = [a for a in sys.argv[1:] if a != "--check"]
    if len(args) != 1:
        sys.exit(__doc__)
    path = args[0]
    check_only = "--check" in sys.argv[1:]
    raw = open(path, "rb").read()
    ies = parse(raw)
    if encode(ies) != raw:
        sys.exit("错误：重新编码后与原文件不一致，文件格式超出本脚本的处理范围")

    src = lookup(ies, SRC_NAME)
    if src is None:
        sys.exit(f"错误：找不到{SRC_NAME}，请确认board-2.bin来自足够新的linux-firmware")
    got = hashlib.sha256(src[2]).hexdigest()
    if got != DATA_SHA256:
        sys.exit(f"错误：HW_GK3数据的SHA-256是{got}，不是预期的{DATA_SHA256}；"
                 "linux-firmware更换了这份数据，需要重新评估射频表现")

    dst = lookup(ies, DST_NAME)
    if dst is not None:
        if hashlib.sha256(dst[2]).hexdigest() != DATA_SHA256:
            sys.exit(f"错误：{DST_NAME}已存在，但指向的不是HW_GK3数据")
        print(f"已包含{DST_NAME}，无需修改")
        return
    if check_only:
        sys.exit(f"缺少{DST_NAME}：请运行 python3 -I scripts/wifi/ath11k-board-alias.py {path}")

    ie_index, name_index, _ = src
    ie_id, subs = ies[ie_index]
    subs.insert(name_index + 1, (SUB_NAME, DST_NAME.encode()))
    out = encode(ies)
    check = lookup(parse(out), DST_NAME)
    if check is None or hashlib.sha256(check[2]).hexdigest() != DATA_SHA256:
        sys.exit("错误：写入结果自检失败，未修改原文件")
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(out)
    os.replace(tmp, path)
    print(f"已添加{DST_NAME}")
    print(f"SHA-256：{hashlib.sha256(raw).hexdigest()} → {hashlib.sha256(out).hexdigest()}")


if __name__ == "__main__":
    main()
