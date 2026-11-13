"""gk3l probe / plan 的输出对不对（test-probe.sh 调用）。参照的是 sgdisk、blkid 与 sysfs，不是 gk3l 自己的代码：

  check_probe.py probe <gk3l probe 的输出> <盘>=<期望的 table>...
      DISK：恰好一行，table 与期望相同，sector 与 sysfs 的逻辑块大小相同，size_mib 按 sysfs 的大小算。
      table=gpt：PART 与 sgdisk 列出的分区一一对应（序号、起止、类型、唯一 GUID、名字、属性），size_mib / size_kib
        按扇区大小算；内核认得的分区，fs / fslabel 与 blkid -p 相同。FREE 都在 usable 区里、不压着任何分区、
        不小于 16 MiB；分区之间（及两头）18 MiB 以上的缝隙都有一段 FREE 盖住它的绝大部分（差不到 2 MiB 的对齐余量）。
      其他 table：没有 PART / FREE。
  check_probe.py plan <gk3l plan 的输出> <区间起> <区间止> <扇区大小> [/data MiB]
      方案的规矩（plan.h）：起点按 1 MiB 向上对齐，misc 4、metadata 32、boot_a / boot_b 64、super 12288 MiB 依次相连，
      userdata 排最后，吃掉剩下的整 MiB（或者正好是给的大小），不越过区间末尾；放不下时 PLANERR 的原因与数字对。
不对的逐条打在 stdout，退出码 1；全对退出码 0。
"""
import re
import subprocess
import sys

FIXED = [('misc', 4), ('metadata', 32), ('boot_a', 64), ('boot_b', 64), ('super', 12288)]
UD_MIN = 8192
bad = []


def fail(msg):
    bad.append(msg)


def pct(v):
    out, i = bytearray(), 0
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
    res = []
    for line in open(path, 'rb').read().split(b'\n'):
        t = line.split(b' ')
        if not t or not t[0]:
            continue
        kv = {}
        for x in t[1:]:
            if b'=' in x:
                k, v = x.split(b'=', 1)
                kv[k.decode()] = pct(v)
        res.append((t[0].decode(errors='replace'), kv))
    return res


def s(v):
    return v.decode('utf-8', errors='surrogateescape')


def sh(*cmd):
    r = subprocess.run(cmd, capture_output=True)
    return r.stdout


def sysfs(disk, rel):
    return open('/sys/block/%s/%s' % (disk.rsplit('/', 1)[1], rel)).read().strip()


def check_disk(recs, disk, want):
    ds = [r for t, r in recs if t == 'DISK' and s(r.get('path', b'')) == disk]
    if len(ds) != 1:
        return fail('%s：DISK 有 %d 行' % (disk, len(ds)))
    d = ds[0]
    sector = int(sysfs(disk, 'queue/logical_block_size'))
    if s(d['table']) != want:
        fail('%s：table=%s，期望 %s' % (disk, s(d['table']), want))
    if int(d['sector']) != sector:
        fail('%s：sector=%s，sysfs 是 %d' % (disk, s(d['sector']), sector))
    if int(d['size_mib']) != int(sysfs(disk, 'size')) * 512 >> 20:
        fail('%s：size_mib=%s' % (disk, s(d['size_mib'])))
    parts = [r for t, r in recs if t == 'PART' and s(r.get('disk', b'')) == disk]
    free = [r for t, r in recs if t == 'FREE' and s(r.get('disk', b'')) == disk]
    if want != 'gpt':
        if parts or free:
            fail('%s：table=%s 却列了 %d 个分区、%d 段空闲' % (disk, want, len(parts), len(free)))
        return
    p = s(sh('sgdisk', '-p', disk))
    first, last = map(int, re.search(r'First usable sector is (\d+), last usable sector is (\d+)', p).groups())
    nums = [int(n) for n in re.findall(r'^\s+(\d+)\s', p, re.M)]
    got = {int(r['num']): r for r in parts}
    if sorted(got) != sorted(nums):
        fail('%s：PART 的序号 %s，sgdisk 是 %s' % (disk, sorted(got), sorted(nums)))
    spans = []
    for n in nums:
        info = sh('sgdisk', '-i', str(n), disk)
        g = lambda pat: re.search(pat, info, re.M).group(1)
        st, en = int(g(rb'^First sector: (\d+)')), int(g(rb'^Last sector: (\d+)'))
        spans.append((st, en))
        r = got.get(n)
        if not r:
            continue
        want_f = {
            'start': str(st), 'end': str(en), 'type': s(g(rb'^Partition GUID code: ([0-9A-F-]+)')),
            'uuid': s(g(rb'^Partition unique GUID: ([0-9A-F-]+)')), 'name': s(g(rb"^Partition name: '(.*)'$")),
            'size_mib': str((en - st + 1) * sector >> 20), 'size_kib': str((en - st + 1) * sector >> 10),
        }
        for k, v in want_f.items():
            if s(r.get(k, b'')) != v:
                fail('%s p%d：%s=%s，sgdisk 是 %s' % (disk, n, k, s(r.get(k, b'')), v))
        if int(s(r['attrs']), 16) != int(g(rb'^Attribute flags: ([0-9a-fA-F]+)'), 16):
            fail('%s p%d：attrs=%s' % (disk, n, s(r['attrs'])))
        node = s(r['path'])
        if subprocess.run(['test', '-b', node]).returncode == 0:
            for k, tag in (('fs', 'TYPE'), ('fslabel', 'LABEL')):
                v = sh('blkid', '-p', '-o', 'value', '-s', tag, node).rstrip(b'\n')
                if r.get(k, b'') != v:
                    fail('%s：%s=%r，blkid 是 %r' % (node, k, r.get(k, b''), v))
    spans.sort()
    per_mib = (1 << 20) // sector
    fs = []
    for r in free:
        a, b = int(r['start']), int(r['end'])
        fs.append((a, b))
        if a < first or b > last or a > b:
            fail('%s：FREE %d-%d 不在 usable 区 %d-%d 里' % (disk, a, b, first, last))
        if any(a <= e and st <= b for st, e in spans):
            fail('%s：FREE %d-%d 压着分区' % (disk, a, b))
        if (b - a + 1) < 16 * per_mib or int(r['size_mib']) != (b - a + 1) * sector >> 20:
            fail('%s：FREE %d-%d 的大小 size_mib=%s' % (disk, a, b, s(r['size_mib'])))
    edges = [first - 1] + [x for sp in spans for x in sp] + [last + 1]
    for i in range(0, len(edges), 2):
        ga, gb = edges[i] + 1, edges[i + 1] - 1
        if gb - ga + 1 < 18 * per_mib:
            continue
        if not any(a <= ga + 2 * per_mib and b >= gb - 2 * per_mib and a >= ga and b <= gb for a, b in fs):
            fail('%s：%d-%d 这段空着却没有报 FREE' % (disk, ga, gb))


def check_plan(path, rs, re_, sector, ud):
    recs = records(path)
    per_mib = (1 << 20) // sector
    cur = (rs + per_mib - 1) // per_mib * per_mib
    avail = 0 if cur > re_ else (re_ - cur + 1) // per_mib
    fixed = sum(m for _, m in FIXED)
    errs = [r for t, r in recs if t == 'PLANERR']
    if avail < fixed + UD_MIN:
        want = ('not-enough-space', None)
    elif ud and ud < UD_MIN:
        want = ('userdata-too-small', None)
    elif ud and ud > avail - fixed:
        want = ('userdata-too-big', None)
    else:
        want = None
    if want:
        if len(errs) != 1 or s(errs[0]['msg']) != want[0]:
            return fail('区间 %d-%d ud=%s：应报 PLANERR %s，得到 %s' % (rs, re_, ud, want[0], [s(e.get('msg', b'')) for e in errs]))
        e = errs[0]
        if int(e['avail_mib']) != avail or int(e['need_mib']) != fixed + UD_MIN or int(e['fixed_mib']) != fixed:
            fail('区间 %d-%d：PLANERR 的数字 %s' % (rs, re_, {k: s(v) for k, v in e.items()}))
        return
    mk = [r for t, r in recs if t == 'PLAN' and r.get('op') == b'mkpart']
    names = [s(r['name']) for r in mk]
    if names != [n for n, _ in FIXED] + ['userdata']:
        return fail('区间 %d-%d：分区顺序 %s' % (rs, re_, names))
    want_ud = ud or (avail - fixed)
    for r, (n, m) in zip(mk, FIXED + [('userdata', want_ud)]):
        st, en = int(r['start']), int(r['end'])
        if st != cur or en != cur + m * per_mib - 1 or int(r['size_mib']) != m:
            fail('区间 %d-%d：%s 应为 %d 起 %d MiB，得到 %d-%d' % (rs, re_, n, cur, m, st, en))
        cur = en + 1
    if cur - 1 > re_:
        fail('区间 %d-%d：方案越过了区间末尾' % (rs, re_))
    sums = [r for t, r in recs if t == 'PLANSUM']
    if len(sums) != 1 or int(sums[0]['userdata_mib']) != want_ud or int(sums[0]['avail_mib']) != avail:
        fail('区间 %d-%d：PLANSUM %s' % (rs, re_, [{k: s(v) for k, v in x.items()} for x in sums]))


def main():
    if sys.argv[1] == 'probe':
        recs = records(sys.argv[2])
        for a in sys.argv[3:]:
            d, t = a.rsplit('=', 1)
            check_disk(recs, d, t)
    else:
        check_plan(sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5]), int(sys.argv[6]) if len(sys.argv) > 6 and sys.argv[6] else 0)
    for b in bad:
        print('    ' + b)
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
