#!/usr/bin/env python3
# 把 Android sparse 镜像从 stdin【顺序】展开写到目标（块设备或文件）。
#
#   zstd -dc super.img.zst | python3 gk3-unsparse.py /dev/nvme0n1p5
#   python3 gk3-unsparse.py --progress 30 40 /dev/nvme0n1p5 < super.img
#
# ★ 为什么不用 simg2img：它接受 "-" 当 stdin，但【喂管道会失败】。
#   refs/lineage-system-core/libsparse/sparse_read.cpp:103 导入 RAW 块时只调
#   sparse_file_add_fd(s, fd, GetOffset(), …) 记下偏移、写出时再回头读，
#   而 GetOffset() 是 lseek64(fd, 0, SEEK_CUR)（:96），:237 还要 Seek(len) ——
#   管道上 lseek 返回 ESPIPE，于是第一个 RAW 块就 "Failed to read sparse file"。
#   发版产物是 super.img.zst（scripts/release.sh:131-137），展开成 sparse 要
#   先落一份临时文件；这个脚本让 zstd 的输出直接流到分区上，不占临时空间。
#
# ★ 顺带解决：simg2img 写 12 GiB 期间一声不吭，界面上进度条会停几分钟。
#   这里每前进 1% 往 stderr 打一行 `PROGRESS <百分比> write-super done_mib=… total_mib=…`
#   （scripts/live/installer-lib.sh 的进度协议）。
#
# 语义刻意与 simg2img 一致，这样两者的输出可以逐字节比对（test-unsparse.sh）：
#   * DONT_CARE 块 = seek 跳过、【不写零】（simg2img 的 file_skip 也是 lseek）
#   * 目标是普通文件时，最后截断到 total_blks*blk_sz（对应 simg2img 的 file_pad）
#
# ⚠️ 截断的输入必须【响亮地失败】：下载断在一半的 .zst 会让 zstd 提前 EOF，
#    这里读不满一个块就退出码 1 —— 不能写一半然后报成功。
#    结尾再核对块数与块计数是否等于头部声明的值。
#
# 格式：refs/lineage-system-core/libsparse/sparse_format.h:25-51
import os
import stat
import struct
import sys

SPARSE_MAGIC = 0xED26FF3A
RAW, FILL, DONT_CARE, CRC32 = 0xCAC1, 0xCAC2, 0xCAC3, 0xCAC4
HDR = struct.Struct("<IHHHHIIII")      # 28 字节
CHUNK = struct.Struct("<HHII")         # 12 字节
BUF = 1 << 20


def die(msg):
    sys.stderr.write("!! gk3-unsparse: %s\n" % msg)
    sys.exit(1)


def read_exact(f, n):
    out = bytearray()
    while len(out) < n:
        b = f.read(n - len(out))
        if not b:
            die("输入提前结束：还差 %d 字节（下载不完整？）" % (n - len(out)))
        out += b
    return bytes(out)


def main(argv):
    base, span = 0, 100
    args = list(argv)
    if args[:1] == ["--progress"]:
        if len(args) < 3:
            die("--progress 要两个数：起点 跨度")
        base, span = int(args[1]), int(args[2])
        args = args[3:]
    if len(args) != 1:
        die("用法：gk3-unsparse.py [--progress 起点 跨度] <目标> < sparse镜像")
    dst = args[0]

    src = sys.stdin.buffer
    hdr = read_exact(src, HDR.size)
    (magic, major, _minor, file_hdr_sz, chunk_hdr_sz,
     blk_sz, total_blks, total_chunks, _csum) = HDR.unpack(hdr)
    if magic != SPARSE_MAGIC:
        die("不是 sparse 镜像（魔数 0x%08x）" % magic)
    if major != 1:
        die("不认识的 sparse 主版本 %d" % major)
    if file_hdr_sz < HDR.size or chunk_hdr_sz < CHUNK.size:
        die("头部长度不对：file_hdr_sz=%d chunk_hdr_sz=%d" % (file_hdr_sz, chunk_hdr_sz))
    if blk_sz == 0 or blk_sz % 4:
        die("块大小 %d 不是 4 的倍数" % blk_sz)
    if file_hdr_sz > HDR.size:                     # sparse_read.cpp:423
        read_exact(src, file_hdr_sz - HDR.size)

    # 截断只对普通文件：对块设备没意义，而且不该出现在写盘路径上
    fd = os.open(dst, os.O_WRONLY | os.O_CREAT, 0o644)
    is_reg = stat.S_ISREG(os.fstat(fd).st_mode)
    if is_reg:
        os.ftruncate(fd, 0)

    total_bytes = total_blks * blk_sz
    done_blks = 0
    last_pct = -1

    def progress():
        nonlocal last_pct
        pct = base + (span * done_blks // total_blks if total_blks else span)
        if pct != last_pct:
            last_pct = pct
            # 进度代码 write-super（installer-lib.sh 文件头的协议；界面按代码查 l10n）
            sys.stderr.write("PROGRESS %d write-super done_mib=%d total_mib=%d\n"
                             % (pct, done_blks * blk_sz >> 20, total_bytes >> 20))
            sys.stderr.flush()

    for i in range(total_chunks):
        ctype, _r, chunk_blks, chunk_total = CHUNK.unpack(read_exact(src, CHUNK.size))
        if chunk_hdr_sz > CHUNK.size:              # sparse_read.cpp:439
            read_exact(src, chunk_hdr_sz - CHUNK.size)
        payload = chunk_total - chunk_hdr_sz
        nbytes = chunk_blks * blk_sz
        if done_blks + chunk_blks > total_blks:
            die("第 %d 块越界：%d + %d > %d 块" % (i, done_blks, chunk_blks, total_blks))
        os.lseek(fd, done_blks * blk_sz, os.SEEK_SET)

        if ctype == RAW:
            if payload != nbytes:
                die("第 %d 块（RAW）长度不符：%d != %d" % (i, payload, nbytes))
            left = nbytes
            while left:
                b = read_exact(src, min(BUF, left))
                os.write(fd, b)
                left -= len(b)
        elif ctype == FILL:
            if payload != 4:
                die("第 %d 块（FILL）长度不符：%d != 4" % (i, payload))
            pat = read_exact(src, 4) * (BUF // 4)
            left = nbytes
            while left:
                n = min(BUF, left)
                os.write(fd, pat[:n])
                left -= n
        elif ctype == DONT_CARE:
            if payload != 0:
                die("第 %d 块（DONT_CARE）带了 %d 字节数据" % (i, payload))
        elif ctype == CRC32:
            if payload != 4 or chunk_blks != 0:
                die("第 %d 块（CRC32）格式不对" % i)
            read_exact(src, 4)
        else:
            die("第 %d 块类型未知：0x%04x" % (i, ctype))

        done_blks += chunk_blks
        progress()

    if done_blks != total_blks:
        die("块数对不上：写了 %d，头部声明 %d" % (done_blks, total_blks))
    if src.read(1):
        die("最后一块之后还有多余数据 —— 不是一份完整的 sparse 镜像")
    if is_reg:
        os.ftruncate(fd, total_bytes)
    os.fsync(fd)
    os.close(fd)
    sys.stderr.write("gk3-unsparse: %d 块 × %d 字节 = %d MiB，%d 个块段\n"
                     % (total_blks, blk_sz, total_bytes >> 20, total_chunks))


if __name__ == "__main__":
    main(sys.argv[1:])
