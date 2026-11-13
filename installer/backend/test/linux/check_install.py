"""装完的盘对不对（test-apply.sh 调用）。只拿测试的输入与独立的工具（sgdisk、dumpe2fs）作参照，不拿 gk3l 自己的代码：

  check_install.py --disk /dev/loopN --esp <只读挂好的 ESP> --expect <test-apply.sh 生成的期望目录>
                   --out <gk3l apply 的 stdout> --mode alongside|wipe|reinstall --default android|windows|none
                   [--mid <machine-id>] [--keep-data]

查的是：
  分区   Android 的 6 个分区名字各一个、在盘上依次相连、起点按 1 MiB 对齐、大小是这一版要的（misc 4、metadata 32、
         boot 64、super 12288 MiB，重新安装不查大小）、类型是 Linux data；新建的与 apply 报的方案（PLAN op=mkpart）
         逐个相同；整盘清空时 ESP 在最前面（2048 起 300 MiB，类型 EFI System）。
  内容   boot_a / boot_b 与 boot.img 相同（之后到扇区末尾全零），super 的开头与展开后的原始镜像相同，
         misc 的前 64 KiB 与按格式独立构造的初始状态相同、其余全零。
  ext4   metadata / userdata 的卷标，userdata 没有保留块；新格式化的填满整个分区（dumpe2fs）。
  ESP    systemd-boot 在回落路径与 EFI/systemd/ 下；两个槽的内核、dtb、ramdisk 与 boot.img 拆出来的相同；
         两个启动项与 loader.conf 的内容；双系统时 Windows 的引导文件原样在、回落路径的原件留成 .before-gaokun3；
         整盘清空时 ESP 上只有我们的文件。
不对的逐条打在 stdout，退出码 1；全对退出码 0。
"""
import argparse
import hashlib
import os
import re
import struct
import subprocess
import sys
import zlib

LINUX = '0FC63DAF-8483-4772-8E79-3D69D8477DE4'
ESP = 'C12A7328-F81F-11D2-BA4B-00A0C93EC93B'
ORDER = ['misc', 'metadata', 'boot_a', 'boot_b', 'super', 'userdata']
SIZES = {'misc': 4, 'metadata': 32, 'boot_a': 64, 'boot_b': 64, 'super': 12288}
LOADER_CONF = 'timeout 5\nconsole-mode keep\neditor no\ndefault *-android-a.conf\n'

bad = []


def fail(msg):
    bad.append(msg)


def run(*cmd):
    return subprocess.run(cmd, check=True, capture_output=True).stdout.decode(errors='replace')


def parts(disk):
    """sgdisk 读回的分区：{名字: [ {num, start, end, type}... ]}，以及扇区大小"""
    out = run('sgdisk', '-p', disk)
    sector = int(re.search(r'Sector size \(logical(?:/physical)?\): (\d+)', out).group(1))
    res = {}
    for m in re.finditer(r'^\s+(\d+)\s', out, re.M):
        n = m.group(1)
        info = run('sgdisk', '-i', n, disk)
        name = re.search(r"^Partition name: '(.*)'$", info, re.M).group(1)
        res.setdefault(name, []).append({
            'num': int(n),
            'start': int(re.search(r'^First sector: (\d+)', info, re.M).group(1)),
            'end': int(re.search(r'^Last sector: (\d+)', info, re.M).group(1)),
            'type': re.search(r'^Partition GUID code: ([0-9A-F-]+)', info, re.M).group(1),
        })
    return res, sector


def plan_lines(path, op):
    res = []
    for line in open(path, encoding='utf-8', errors='replace'):
        t = line.split()
        if t and t[0] == 'PLAN' and 'op=' + op in t:
            res.append(dict(x.split('=', 1) for x in t[1:] if '=' in x))
    return res


def read_part(dev, off, n):
    with open(dev, 'rb') as f:
        f.seek(off)
        return f.read(n)


def misc_want(default):
    """misc 的前 64 KiB：BCB 全零，2048 起是 bootloader_control（_a，priority 15 / tries 6），8192 起是 GK3 记录 v1"""
    setdef = {'none': 0, 'windows': 1, 'android': 2}[default]
    m = bytearray(65536)
    bc = bytearray(32)
    bc[0:2] = b'_a'
    struct.pack_into('<I', bc, 4, 0x42414342)
    bc[8] = 1
    struct.pack_into('<H', bc, 9, 2)
    struct.pack_into('<H', bc, 12, 15 | (6 << 4))
    struct.pack_into('<I', bc, 28, zlib.crc32(bytes(bc[0:28])) & 0xffffffff)
    m[2048:2080] = bc
    r = bytearray(2048)
    struct.pack_into('<IHHIII', r, 0, 0x52334B47, 1, 2048, 1, 1, 1)
    r[26] = 1
    r[48:68] = hashlib.sha1(bytes(2048)).digest()
    r[360] = setdef
    struct.pack_into('<IHBBII', r, 1024, 1, 8, 0xff, 0, 1, 0)
    struct.pack_into('<I', r, 2044, zlib.crc32(bytes(r[0:2044])) & 0xffffffff)
    m[8192:10240] = r
    return bytes(m)


def ext4(dev):
    out = run('dumpe2fs', '-h', dev)
    get = lambda k: (re.search(r'^%s:\s*(.*)$' % re.escape(k), out, re.M) or [None, ''])[1].strip()
    return get('Filesystem volume name'), int(get('Block count') or 0), int(get('Block size') or 0), int(get('Reserved block count') or -1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--disk', required=True)
    ap.add_argument('--esp', required=True)
    ap.add_argument('--expect', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--mode', required=True, choices=['alongside', 'wipe', 'reinstall'])
    ap.add_argument('--default', required=True, choices=['android', 'windows', 'none'])
    ap.add_argument('--mid', default='0123456789abcdef0123456789abcdef')
    ap.add_argument('--keep-data', action='store_true')
    a = ap.parse_args()
    exp = lambda n: open(os.path.join(a.expect, n), 'rb').read()

    # ── 分区 ──
    ps, sector = parts(a.disk)
    per_mib = (1 << 20) // sector
    ours = {}
    for n in ORDER:
        if len(ps.get(n, [])) != 1:
            fail('分区 %s 有 %d 个' % (n, len(ps.get(n, []))))
        else:
            ours[n] = ps[n][0]
    if len(ours) == len(ORDER):
        for i, n in enumerate(ORDER):
            p = ours[n]
            if p['type'] != LINUX:
                fail('%s 的类型 %s' % (n, p['type']))
            if p['start'] % per_mib:
                fail('%s 的起点 %d 没有按 1 MiB 对齐' % (n, p['start']))
            if i and p['start'] != ours[ORDER[i - 1]]['end'] + 1 and a.mode != 'reinstall':
                fail('%s 没有紧接在 %s 后面' % (n, ORDER[i - 1]))
            if a.mode != 'reinstall' and n in SIZES and (p['end'] - p['start'] + 1) != SIZES[n] * per_mib:
                fail('%s 的大小 %d 扇区，应为 %d MiB' % (n, p['end'] - p['start'] + 1, SIZES[n]))
        for pl in plan_lines(a.out, 'mkpart'):
            got = ps.get(pl['name'], [{}])[0]
            if (got.get('start'), got.get('end')) != (int(pl['start']), int(pl['end'])):
                fail('%s 不在方案说的位置：方案 %s-%s，盘上 %s-%s' % (pl['name'], pl['start'], pl['end'], got.get('start'), got.get('end')))
        if a.mode == 'wipe':
            e = ps.get('esp', [{}])[0]
            if (e.get('num'), e.get('start'), e.get('end'), e.get('type')) != (1, per_mib, per_mib + 300 * per_mib - 1, ESP):
                fail('整盘清空的 ESP 不是 1 号、2048 起 300 MiB、EFI System：%s' % e)
        if a.mode == 'reinstall':
            for pl in plan_lines(a.out, 'reuse'):
                got = ps.get(pl['name'], [{}])[0]
                if (got.get('start'), got.get('end')) != (int(pl['start']), int(pl['end'])):
                    fail('重新安装的方案里 %s 的位置与盘上不同' % pl['name'])

    # ── 内容 ──
    if len(ours) == len(ORDER):
        boot = exp('boot.img')
        padded = (len(boot) + sector - 1) // sector * sector
        for n in ('boot_a', 'boot_b'):
            got = read_part(a.disk, ours[n]['start'] * sector, padded)
            if got[:len(boot)] != boot or got[len(boot):].strip(b'\0'):
                fail('%s 与 boot.img 不同' % n)
        raw = exp('super.raw')
        if read_part(a.disk, ours['super']['start'] * sector, len(raw)) != raw:
            fail('super 的开头与展开后的原始镜像不同')
        mb = (ours['misc']['end'] - ours['misc']['start'] + 1) * sector
        got = read_part(a.disk, ours['misc']['start'] * sector, mb)
        if got[:65536] != misc_want(a.default):
            fail('misc 的前 64 KiB 不是"槽 _a、默认 %s"的初始状态' % a.default)
        if got[65536:].strip(b'\0'):
            fail('misc 64 KiB 之后不全是零')

    # ── ext4 ──
    if len(ours) == len(ORDER):
        part_dev = lambda n: '%sp%d' % (a.disk, ours[n]['num'])
        for n in ('metadata', 'userdata'):
            label, blocks, bs, reserved = ext4(part_dev(n))
            size = (ours[n]['end'] - ours[n]['start'] + 1) * sector
            if label != n:
                fail('%s 的卷标是 %r' % (n, label))
            if not a.keep_data and blocks * bs != size:
                fail('%s 的 ext4 没有填满分区（%d × %d ≠ %d）' % (n, blocks, bs, size))
            if n == 'userdata' and reserved != 0:
                fail('userdata 有 %d 个保留块' % reserved)

    # ── ESP ──
    E = a.esp
    rel = lambda p: os.path.join(E, p)

    def same(p, want, what):
        try:
            got = open(rel(p), 'rb').read()
        except OSError:
            return fail('ESP 上没有 %s（%s）' % (p, what))
        if got != want:
            fail('ESP 上的 %s 不是%s' % (p, what))

    sdboot = exp('fake-sdboot.efi')
    same('EFI/BOOT/BOOTAA64.EFI', sdboot, 'systemd-boot')
    same('EFI/systemd/systemd-bootaa64.efi', sdboot, 'systemd-boot')
    same('loader/loader.conf', LOADER_CONF.encode(), '这一版的 loader.conf')
    cmdline = exp('cmdline.txt').decode()
    for s in 'ab':
        for f in ('Image', 'gaokun3.dtb', 'ramdisk.img'):
            same('%s/android/slot_%s/%s' % (a.mid, s, f), exp(f), 'boot.img 里的 %s' % f)
        entry = ('title      crDroid 16.0 (gaokun3) \u2014 slot _{s}\nversion    gaokun3-slot-{s}\nsort-key   zandroid{s}\n'
                 'options    {c} androidboot.slot_suffix=_{s}\nlinux      /{m}/android/slot_{s}/Image\n'
                 'devicetree /{m}/android/slot_{s}/gaokun3.dtb\ninitrd     /{m}/android/slot_{s}/ramdisk.img\n'
                 ).format(s=s, c=cmdline, m=a.mid)
        same('loader/entries/%s-android-%s.conf' % (a.mid, s), entry.encode(), '槽 _%s 的启动项' % s)
    win = exp('win-bootmgr.efi')
    files = set()
    for root, _, fs in os.walk(E):
        for f in fs:
            files.add(os.path.relpath(os.path.join(root, f), E))
    if a.mode == 'wipe':
        want = {'EFI/BOOT/BOOTAA64.EFI', 'EFI/systemd/systemd-bootaa64.efi', 'loader/loader.conf'}
        want |= {'%s/android/slot_%s/%s' % (a.mid, s, f) for s in 'ab' for f in ('Image', 'gaokun3.dtb', 'ramdisk.img')}
        want |= {'loader/entries/%s-android-%s.conf' % (a.mid, s) for s in 'ab'}
        lower = {f.lower() for f in files}
        if lower != {f.lower() for f in want}:
            fail('整盘清空后 ESP 上的文件不对：多 %s，少 %s' % (sorted(lower - {f.lower() for f in want}), sorted({f.lower() for f in want} - lower)))
    else:
        same('EFI/Microsoft/Boot/bootmgfw.efi', win, 'Windows 原来的引导文件')
        same('EFI/BOOT/BOOTAA64.EFI.before-gaokun3', win, '回落路径上原来的 Windows 引导文件')
        same('EFI/gk3test/hello.txt', b'hello\n', '别人的文件')

    for b in bad:
        print('    ' + b)
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
