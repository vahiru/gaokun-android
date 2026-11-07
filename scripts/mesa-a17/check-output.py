#!/usr/bin/env python3
"""检查 meson2hermetic 生成的 external/mesa3d Android.bp —— 不跑 m 能查的都在这。

用法：
  python3 scripts/mesa-a17/check-output.py <mesa3d 目录> [--before-names F] [--tree ROOT]
  python3 scripts/mesa-a17/check-output.py <mesa3d 目录> --dump-names     # 只列模块名
退出码：0 = 全过；1 = 有 ✗。

检查项（每条都对应一个踩过 / 预见到的坑）：
  1. vulkan.freedreno 存在、是 vendor 的 cc_library_shared、装进 hw/、只编 64 位
  2. turnip 编的是 tu_knl_drm_msm.cc（主线 msm DRM），没有 kgsl / virtio 后端
     —— A16 时代 AOSP 默认 kgsl，照抄会得到一个在本机打不开设备的驱动（stage5 "关键正确性"）
  3. vulkan.freedreno 及其静态依赖闭包里的 shared_libs 只能是 SP-HAL 能加载的库：
     app 进程按 same_process_hal_file 加载 turnip，依赖要么是 LLNDK，要么在
     device/huawei/gaokun3/sepolicy/file_contexts 里标过（libui、libdrm 等）。
     libzstd 就是反例（见 dependencies.toml 的说明）
  4. 模块名闭合：生成文件里引用的每个模块，要么在 mesa 里定义，要么是已知的外部模块；
     给了 --tree 就在整棵树里逐个核实（Soong 解析期就会因未定义模块失败，哪怕没人编它）
  5. 跟生成前相比消失的模块名（--before-names）：给了 --tree 就查树里 external/mesa3d
     以外有没有人引用它们（device/generic/goldfish、hardware/google/gfxstream …）
  6. genrule 的 cmd 里没有绝对路径（A16 M3 坑 2：生成器把 /home/…/external/mesa3d 烤进 cmd）
"""
import argparse
import os
import re
import sys

REF_KEYS = {
    'defaults', 'static_libs', 'whole_static_libs', 'shared_libs', 'header_libs',
    'generated_headers', 'generated_sources', 'tools', 'libs', 'rustlibs',
    'proc_macros', 'srcs', 'export_generated_headers', 'export_header_lib_headers',
    'export_static_lib_headers', 'export_shared_lib_headers',
}
# app 进程（SP-HAL 命名空间）能加载的依赖：bionic / LLNDK + 本机 file_contexts 标过的 vendor 库。
# A16 实测 vulkan.freedreno 的依赖就是这一组（turnip-shared.bp.in；标签见 sepolicy/file_contexts
# 里 vulkan.freedreno.so / libui.so 那两行 same_process_hal_file）。
SPHAL_OK = {
    'libc', 'libm', 'libdl', 'libc++', 'liblog', 'libcutils', 'libhardware',
    'libnativewindow', 'libsync', 'libz', 'libdrm', 'libui',
}
TURNIP = 'vulkan.freedreno'

MOD_RE = re.compile(r'^([a-z_][a-z0-9_]*) \{\n(.*?)^\}\n', re.M | re.S)
NAME_RE = re.compile(r'^    name: "([^"]+)",', re.M)
PROP_RE = re.compile(r'^    ([a-z_]+): ')
STR_RE = re.compile(r'"([^"]*)"')


def parse_modules(mesa):
    """→ {name: {'type':…, 'file':…, 'text':…, 'refs': {key: set(str)}}}"""
    mods = {}
    for root, dirs, files in os.walk(mesa):
        dirs[:] = [d for d in dirs if d != '.git']
        if 'Android.bp' not in files:
            continue
        path = os.path.join(root, 'Android.bp')
        text = open(path, encoding='utf-8').read()
        for m in MOD_RE.finditer(text):
            mtype, body = m.group(1), m.group(2)
            nm = NAME_RE.search(body)
            if not nm:
                continue                  # package { … }
            refs, key = {}, None
            for line in body.split('\n'):
                pm = PROP_RE.match(line)
                if pm:
                    key = pm.group(1)
                elif line.startswith('    ') and not line.startswith('     '):
                    key = None if line.strip() in ('},', '}),', ']', '],') else key
                if key not in REF_KEYS:
                    continue
                # select 分支键（("arm64", "android"): [ / "android": [ / soong_config_variable(…)）不是模块名
                if re.search(r'"\s*\)?\s*:\s*\[', line) or 'select(' in line:
                    continue
                for s in STR_RE.findall(line.split(': ', 1)[1] if pm else line):
                    refs.setdefault(key, set()).add(s)
            mods[nm.group(1)] = {'type': mtype, 'file': os.path.relpath(path, mesa),
                                 'text': body, 'refs': refs}
    return mods


def module_refs(mod):
    """模块引用的其他模块名（srcs 只算 ":xxx"，tools 去掉 //path: 前缀）。"""
    out = set()
    for key, vals in mod['refs'].items():
        for v in vals:
            if key == 'srcs':
                if v.startswith(':'):
                    out.add(v[1:].split('{')[0])
            elif key == 'tools' and v.startswith('//'):
                out.add(v.split(':', 1)[1])
            else:
                out.add(v)
    return out


def tree_index(root, wanted_names, wanted_refs):
    """一次遍历整棵树的 Android.bp：哪些名字有定义；哪些名字在 external/mesa3d 之外被引用。"""
    defined, referenced = set(), {}
    skip = {'.repo', 'out', '.git'}
    pat_ref = re.compile(r'"(:?)(%s)"' % '|'.join(re.escape(n) for n in sorted(wanted_refs))) \
        if wanted_refs else None
    for dirpath, dirs, files in os.walk(root):
        rel = os.path.relpath(dirpath, root)
        if rel == '.':
            dirs[:] = [d for d in dirs if d not in skip]
        if 'Android.bp' not in files:
            continue
        try:
            text = open(os.path.join(dirpath, 'Android.bp'), encoding='utf-8', errors='replace').read()
        except OSError:
            continue
        for n in NAME_RE.findall(text):
            if n in wanted_names:
                defined.add(n)
        # 缩进不一定是 4 格：兜底再扫一遍任意缩进的 name:
        for n in re.findall(r'^\s*name:\s*"([^"]+)"', text, re.M):
            if n in wanted_names:
                defined.add(n)
        if pat_ref and not (rel == 'external/mesa3d' or rel.startswith('external/mesa3d/')):
            for m in pat_ref.finditer(text):
                referenced.setdefault(m.group(2), set()).add(rel)
    return defined, referenced


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('mesa')
    ap.add_argument('--dump-names', action='store_true')
    ap.add_argument('--before-names')
    ap.add_argument('--tree')
    a = ap.parse_args()
    mods = parse_modules(a.mesa)
    if a.dump_names:
        print('\n'.join(sorted(mods)))
        return 0

    bad = []
    def fail(msg):
        bad.append(msg)
        print('  ✗ ' + msg)
    def ok(msg):
        print('  ✓ ' + msg)

    print('▶ check-output: %d 个模块' % len(mods))
    # 1–2. turnip 本体
    t = mods.get(TURNIP)
    if not t:
        fail('没有 %s 模块' % TURNIP)
    else:
        body = t['text']
        (ok if t['type'] == 'cc_library_shared' else fail)('%s 类型 %s' % (TURNIP, t['type']))
        (ok if 'vendor: true' in body else fail)('vendor: true')
        (ok if 'relative_install_path: "hw"' in body else fail)('relative_install_path: "hw"')
        (ok if 'compile_multilib: "64"' in body else fail)('compile_multilib: "64"（postprocess.py）')
        srcs = t['refs'].get('srcs', set())
        (ok if 'tu_knl_drm_msm.cc' in srcs else fail)('srcs 含 tu_knl_drm_msm.cc（msm KMD）')
        for k in ('tu_knl_kgsl.cc', 'tu_knl_drm_virtio.cc'):
            (fail if k in srcs else ok)('srcs 不含 %s' % k)
        (ok if 'tu_image.cc' in srcs else fail)('srcs 含 tu_image.cc（patches/mesa/0001 落在这里）')
        # 3. SP-HAL 依赖闭包
        seen, stack, shared = set(), [TURNIP], {}
        while stack:
            n = stack.pop()
            if n in seen or n not in mods:
                continue
            seen.add(n)
            r = mods[n]['refs']
            for s in r.get('shared_libs', ()):
                shared.setdefault(s, set()).add(n)
            stack += list(r.get('static_libs', ())) + list(r.get('whole_static_libs', ()))
        extra = {s: v for s, v in shared.items() if s not in SPHAL_OK}
        if extra:
            for s, v in sorted(extra.items()):
                fail('SP-HAL 不允许的 shared_libs：%s（来自 %s）' % (s, ', '.join(sorted(v))))
        else:
            ok('shared_libs 闭包 ⊆ SP-HAL 白名单：%s' % ', '.join(sorted(shared)))

    # 4. 模块名闭合
    allrefs = set()
    for m in mods.values():
        allrefs |= module_refs(m)
    external = sorted(r for r in allrefs if r not in mods)
    print('  · 引用的外部模块 %d 个：%s' % (len(external), ' '.join(external)))

    # 5. 消失的模块
    gone = []
    if a.before_names:
        before = {l.strip() for l in open(a.before_names) if l.strip()}
        gone = sorted(before - set(mods))
        new = sorted(set(mods) - before)
        print('  · 与生成前相比：新增 %d 个模块，消失 %d 个' % (len(new), len(gone)))
        if gone:
            print('    消失：' + ' '.join(gone))

    if a.tree:
        print('▶ 在 %s 里核对外部模块 / 消失模块的引用（遍历全部 Android.bp）…' % a.tree)
        defined, referenced = tree_index(a.tree, set(external), set(gone))
        missing = [n for n in external if n not in defined]
        if missing:
            for n in missing:
                fail('外部模块在树里找不到定义：%s' % n)
        else:
            ok('外部模块全部有定义')
        for n in gone:
            if n in referenced:
                fail('消失的模块 %s 仍被引用：%s' % (n, ', '.join(sorted(referenced[n])[:5])))
        if gone and not any(n in referenced for n in gone):
            ok('消失的模块在 external/mesa3d 以外无人引用')

    # 6. genrule cmd 里的绝对路径（只看 cmd：xmlconfig 的 -DDATADIR="/usr/local/share" 这类
    #    宏是 mesa 自己的默认值，A17 原版里就有，不是构建路径）
    absp = []
    cmd_re = re.compile(r'^    cmd: (.*?)(?=^    [a-z_]+: |\Z)', re.M | re.S)
    for n, m in mods.items():
        for c in cmd_re.findall(m['text']):
            if re.search(r'(?<![\w$)])/(home|tmp|usr|opt|root)/', c):
                absp.append('%s（%s）' % (n, m['file']))
    if absp:
        fail('genrule cmd 里有绝对路径：%s' % ', '.join(absp[:10]))
    else:
        ok('genrule cmd 里没有绝对路径')

    print('▶ check-output: %s' % ('全过' if not bad else '%d 项失败' % len(bad)))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
