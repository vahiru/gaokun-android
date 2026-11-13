"""gk3l probe / plan 与 shell 版 gk3_probe / gk3_plan 的逐项比对（test-probe.sh 调用）。

  cmp_probe.py probe <gk3l 输出> <shell 输出> <盘=期望的 table>...
  cmp_probe.py plan  <gk3l 输出> <shell 输出>

比的是含义而不是字节：两边的记录格式本来就不同（gk3l 与 Windows 版 gk3w 同格式）。
probe：只看命令行上列出的盘。table 以期望值为准（shell 版把没有分区表的盘也报成 gpt）；
  两边都是 gpt 时逐个分区比 num start end size_mib size_kib type name fs fslabel os medium，空闲区整组比。
  4Kn 盘（gk3l 报 sector=4096）shell 版的 size_mib / size_kib 按 512 字节算，是错的，不比这两项。
  已知的差异：gk3rescue 分区 gk3l 报 os=android（安装器自己的分区），shell 版按文件系统猜；
  分区名里有单引号时 shell 版截断在引号处。
plan：PLAN op=mkpart 的 name start end size_mib 逐行比，PLANSUM 比 userdata_mib，失败时比 PLANERR 的 msg 与 need_mib。
退出码：0 一致；1 有差异（差异逐条打在 stdout）。
"""
import sys


def pct_decode(v: bytes) -> bytes:
    out = bytearray()
    i = 0
    while i < len(v):
        if v[i:i + 1] == b'%' and len(v[i + 1:i + 3]) == 2:
            try:
                out.append(int(v[i + 1:i + 3], 16))
                i += 3
                continue
            except ValueError:
                pass
        out.append(v[i])
        i += 1
    return bytes(out)


def records(path):
    recs = []
    with open(path, 'rb') as f:
        for line in f.read().split(b'\n'):
            parts = line.split(b' ')
            if not parts or not parts[0]:
                continue
            kv = {}
            for p in parts[1:]:
                if b'=' in p:
                    k, v = p.split(b'=', 1)
                    kv[k.decode()] = pct_decode(v)
            recs.append((parts[0].decode(errors='replace'), kv))
    return recs


def s(v):
    return v.decode(errors='backslashreplace') if isinstance(v, bytes) else str(v)


def cmp_probe(lx, sh, want):
    bad = []
    a, b = records(lx), records(sh)
    for disk, table in want.items():
        da = [r for t, r in a if t == 'DISK' and r.get('path') == disk.encode()]
        db = [r for t, r in b if t == 'DISK' and r.get('path') == disk.encode()]
        if len(da) != 1 or len(db) != 1:
            bad.append(f'{disk}: DISK 记录数 gk3l={len(da)} shell={len(db)}')
            continue
        da, db = da[0], db[0]
        if s(da.get('table', b'')) != table:
            bad.append(f'{disk}: table gk3l={s(da.get("table", b""))} 期望={table}')
        for k in ('size_mib', 'model', 'removable', 'medium'):
            if da.get(k) != db.get(k):
                bad.append(f'{disk}: DISK {k} gk3l={s(da.get(k))} shell={s(db.get(k))}')
        if table != 'gpt' or s(db.get('table', b'')) != 'gpt':
            # 没有可比的分区：gk3l 不该列任何分区与空闲区
            extra = [t for t, r in a if t in ('PART', 'FREE') and r.get('disk') == disk.encode()]
            if extra:
                bad.append(f'{disk}: table={table}，gk3l 却列了 {len(extra)} 条 PART/FREE')
            continue
        k4 = s(da.get('sector', b'512')) == '4096'
        pa = {r['path']: r for t, r in a if t == 'PART' and r.get('disk') == disk.encode()}
        pb = {r['path']: r for t, r in b if t == 'PART' and r.get('path', b'').startswith(disk.encode() + b'p')}
        if set(pa) != set(pb):
            bad.append(f'{disk}: 分区不同 gk3l={sorted(map(s, pa))} shell={sorted(map(s, pb))}')
        for p in sorted(set(pa) & set(pb)):
            x, y = pa[p], pb[p]
            keys = ['num', 'start', 'end', 'type', 'name', 'fs', 'fslabel', 'os', 'medium']
            if not k4:
                keys += ['size_mib', 'size_kib']
            for k in keys:
                if k == 'os' and x.get('name') == b'gk3rescue':
                    continue
                # shell 版用 cut -d"'" 取分区名，名字里有单引号就截断了（shell 版的错）
                if k == 'name' and b"'" in x.get(k, b'') and x[k].split(b"'")[0] == y.get(k):
                    continue
                if x.get(k) != y.get(k):
                    bad.append(f'{s(p)}: PART {k} gk3l={s(x.get(k))!r} shell={s(y.get(k))!r}')
            if x.get('idx') != x.get('num'):
                bad.append(f'{s(p)}: idx={s(x.get("idx"))} 与 num={s(x.get("num"))} 不同')
        fa = sorted((r['start'], r['end'], r['size_mib']) for t, r in a if t == 'FREE' and r.get('disk') == disk.encode())
        fb = sorted((r['start'], r['end'], r['size_mib']) for t, r in b if t == 'FREE' and r.get('disk') == disk.encode())
        if k4:
            fa = [x[:2] for x in fa]
            fb = [x[:2] for x in fb]
        if fa != fb:
            bad.append(f'{disk}: 空闲区不同 gk3l={[tuple(map(s, x)) for x in fa]} shell={[tuple(map(s, x)) for x in fb]}')
    return bad


def cmp_plan(lx, sh):
    bad = []
    a, b = records(lx), records(sh)

    def mk(rs):
        return [tuple(r.get(k) for k in ('name', 'start', 'end', 'size_mib')) for t, r in rs if t == 'PLAN' and r.get('op') == b'mkpart']

    if mk(a) != mk(b):
        bad.append(f'PLAN 不同 gk3l={mk(a)} shell={mk(b)}')
    sa = [r for t, r in a if t == 'PLANSUM']
    sb = [r for t, r in b if t == 'PLANSUM']
    if bool(sa) != bool(sb):
        bad.append(f'PLANSUM 有无不同 gk3l={bool(sa)} shell={bool(sb)}')
    elif sa and sa[0].get('userdata_mib') != sb[0].get('userdata_mib'):
        bad.append(f'userdata_mib gk3l={s(sa[0].get("userdata_mib"))} shell={s(sb[0].get("userdata_mib"))}')
    ea = [r for t, r in a if t == 'PLANERR']
    eb = [r for t, r in b if t == 'PLANERR']
    if bool(ea) != bool(eb):
        bad.append(f'PLANERR 有无不同 gk3l={bool(ea)} shell={bool(eb)}')
    elif ea:
        for k in ('msg', 'need_mib', 'avail_mib'):
            if k in eb[0] and ea[0].get(k) != eb[0].get(k):
                bad.append(f'PLANERR {k} gk3l={s(ea[0].get(k))} shell={s(eb[0].get(k))}')
    return bad


def main():
    if sys.argv[1] == 'probe':
        want = dict(x.split('=', 1) for x in sys.argv[4:])
        bad = cmp_probe(sys.argv[2], sys.argv[3], want)
    else:
        bad = cmp_plan(sys.argv[2], sys.argv[3])
    for x in bad:
        print('    ' + x)
    sys.exit(1 if bad else 0)


main()
