#!/usr/bin/env python3
"""从本地 payload.bin 解出指定分区镜像（只支持全量包）。

用法：payload-extract.py <payload.bin> <输出目录> [分区 ...]   不给分区名 ⇒ 只列清单
支持的操作：REPLACE / REPLACE_BZ / REPLACE_XZ / ZERO / DISCARD，REPLACE_ZSTD 走 zstd 命令行。
每个操作核 data_sha256，整分区核 new_partition_info.hash，对不上就报错退出。
字段号照 update_engine/update_metadata.proto（DeltaArchiveManifest.partitions=13、
PartitionUpdate.operations=8 / new_partition_info=7、InstallOperation.type=1 data_offset=2
data_length=3 dst_extents=6 data_sha256_hash=8）；用法同 out/hyperos-r2/probe-tools/extract.py，改成读本地文件。
"""
import bz2, hashlib, lzma, os, struct, subprocess, sys


def varint(b, p):
    r = s = 0
    while True:
        c = b[p]; p += 1; r |= (c & 0x7f) << s; s += 7
        if c < 0x80:
            return r, p


def fields(b):
    p = 0; out = []
    while p < len(b):
        k, p = varint(b, p); f, t = k >> 3, k & 7
        if t == 0: v, p = varint(b, p)
        elif t == 2:
            n, p = varint(b, p); v = b[p:p + n]; p += n
        elif t == 1: v = b[p:p + 8]; p += 8
        elif t == 5: v = b[p:p + 4]; p += 4
        else: raise ValueError(f'wire type {t}')
        out.append((f, v))
    return out


def get(fs, n): return [v for f, v in fs if f == n]
def one(fs, n, d=None):
    l = get(fs, n); return l[0] if l else d


OPS = {0: 'REPLACE', 1: 'REPLACE_BZ', 6: 'ZERO', 7: 'DISCARD', 8: 'REPLACE_XZ', 14: 'REPLACE_ZSTD'}


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    pay, outd, want = sys.argv[1], sys.argv[2], sys.argv[3:]
    f = open(pay, 'rb')
    head = f.read(24)
    if head[:4] != b'CrAU':
        sys.exit('不是 payload（magic 不是 CrAU）')
    ver, msz, sigsz = struct.unpack('>QQI', head[4:24])
    M = fields(f.read(msz))
    ds = 24 + msz + sigsz
    bs = one(M, 3, 4096)
    for pu in get(M, 13):
        pf = fields(pu); name = one(pf, 1).decode()
        ni = fields(one(pf, 7, b'')); size = one(ni, 1); phash = one(ni, 2)
        ops = get(pf, 8)
        if not want:
            kinds = sorted({OPS.get(one(fields(o), 1, 0), str(one(fields(o), 1, 0))) for o in ops})
            print(f'{name:16} {size:>12}  {len(ops):5} ops  {",".join(kinds)}')
            continue
        if name not in want:
            continue
        path = os.path.join(outd, name + '.img')
        with open(path, 'wb') as o:
            o.truncate(size)
        fd = os.open(path, os.O_WRONLY)
        for op in ops:
            of = fields(op); t = one(of, 1, 0); off = one(of, 2, 0); ln = one(of, 3, 0); h = one(of, 8)
            ext = [(one(fields(e), 1, 0), one(fields(e), 2, 0)) for e in get(of, 6)]
            if t in (6, 7):
                continue  # 文件 truncate 出来本来就是 0
            f.seek(ds + off); raw = f.read(ln)
            if h and hashlib.sha256(raw).digest() != h:
                sys.exit(f'{name}: 操作数据 sha256 不符（偏移 {off}）')
            if t == 0: data = raw
            elif t == 1: data = bz2.decompress(raw)
            elif t == 8: data = lzma.decompress(raw)
            elif t == 14: data = subprocess.run(['zstd', '-dc'], input=raw, stdout=subprocess.PIPE, check=True).stdout
            else: sys.exit(f'{name}: 不支持的操作类型 {t}（只支持全量包）')
            p = 0
            for s, n in ext:
                os.pwrite(fd, data[p:p + n * bs], s * bs); p += n * bs
        os.close(fd)
        hh = hashlib.sha256()
        with open(path, 'rb') as r:
            for b in iter(lambda: r.read(1 << 24), b''):
                hh.update(b)
        ok = hh.digest() == phash
        print(f'{name}: {size} B  hash_ok={ok}')
        if not ok:
            sys.exit(1)


if __name__ == '__main__':
    main()
