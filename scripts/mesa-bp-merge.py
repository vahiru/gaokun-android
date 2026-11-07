#!/usr/bin/env python3
"""把 meson_to_hermetic 生成的 Android_res.bp 合进 external/mesa3d/Android.bp。

**仅 A16（crDroid 16.0，mesa 25.3 / 26.0.3 + 旧工具 meson_to_hermetic）。**
A17（crDroid 17.0，mesa 26.1.0-devel）的 external/mesa3d 已删掉 meson_to_hermetic，
改用 meson2hermetic（`meson convert`）；A17 的管线见 docs/dev/mesa-a17.md 与
scripts/mesa-a17/regen.sh。本文件只作 A16 历史留档，在 A17 树上运行会直接退出。

在 external/mesa3d 目录下运行，幂等（每次从 Android.bp.gaokun-orig 重来）。

要处理的坑（都是"工具快照 vs mesa 25.3 / AOSP 现状"的落差）：

1. **文件名**：Soong 只读名为 Android.bp 的文件，生成器按设计写 Android_res.bp。

2. **模块重名**：生成内容里有 7 个模块与树里已有的重名（AOSP 手维护的
   src/util、src/c11/impl、src/gfxstream/** —— 给 gfxstream/goldfish 用的，
   且被 device/generic/goldfish 等外部模块按名引用，不能动）。
   做法：把**我们生成的**那几个改名加 `_gaokun` 后缀，并在生成段内同步改引用。
   不能反过来剔除生成版：树里的 mesa_util 只有 65 个源文件（够 gfxstream 用），
   而 turnip 要的生成版有 79 个，还带 shader_stats.h 这类生成头。

3. **genrule out/tools/srcs 有重复项** → Soong 报 "cannot be overwritten" /
   "multiple locations for label"，去重。

4. **genrule 少声明 tools**：cmd 里 $(location m4)/$(location bison) 用了工具
   但 tools 是空的 → "unknown location label"。

5. **glslangValidator 被裸调用**：Soong 沙箱禁止 PATH 工具，改写成
   $(location glslangValidator)（host 模块见 patches/0003-*.patch）。

6. **生成头的 include 路径**：spv 头生成在 $(genDir)/src/freedreno/vulkan/bvh/ 下，
   源码写 `#include "bvh/encode.spv.h"`，genrule 只导出了 .../bvh 和 src，
   缺 .../vulkan。给每个 out 路径补齐"目录及其各级父目录"。

7. **python 生成脚本缺库**：Soong 的 python 沙箱不含 PyYAML/Mako，而
   u_format_parse.py 等要 import yaml/mako → 补 libs: ["pyyaml", "mako"]。

8. **libdrm 依赖漏了**：turnip 的 tu_knl_drm*.cc 要 xf86drm.h。
"""
import os
import re
import shutil
import sys

if not os.path.isdir('meson_to_hermetic'):
    sys.exit('本脚本只适用于带 meson_to_hermetic 的 A16 mesa 树；'
             'A17（mesa 26.1）请用 scripts/mesa-a17/regen.sh（见 docs/dev/mesa-a17.md）')

ORIG = 'Android.bp'
GEN = 'Android_res.bp'
BAK = 'Android.bp.gaokun-orig'
SUFFIX = '_gaokun'
HOST_TOOLS = {'m4', 'bison', 'flex', 'gzip', 'python3', 'glslangValidator'}

if not os.path.isfile(GEN):
    sys.exit('缺少 %s —— 先跑生成器（见 docs/dev/gpu-turnip.md）' % GEN)

if os.path.isfile(BAK):
    shutil.copyfile(BAK, ORIG)      # 每次从干净原文件出发
else:
    shutil.copyfile(ORIG, BAK)

orig = open(ORIG).read()
gen = open(GEN).read()

# ── 1. 收集"树里其它地方已定义"的模块名（含原 Android.bp 自己的）──────────
taken = set(re.findall(r'name:\s*"([^"]+)"', orig))
for root, _dirs, files in os.walk('.'):
    if root == '.' or 'Android.bp' not in files:
        continue
    text = open(os.path.join(root, 'Android.bp'), errors='replace').read()
    taken.update(re.findall(r'name:\s*"([^"]+)"', text))

# ── 2. 切块 ───────────────────────────────────────────────────────────────
blocks, cur = [], []
for line in gen.splitlines(keepends=True):
    cur.append(line)
    if line.rstrip() == '}':
        blocks.append(''.join(cur))
        cur = []
if cur:
    blocks.append(''.join(cur))

gen_names = set()
for b in blocks:
    m = re.search(r'name:\s*"([^"]+)"', b)
    if m:
        gen_names.add(m.group(1))

collisions = sorted(gen_names & taken)
print('生成模块 %d 个，与树内重名 %d 个 → 改名加 %s' % (len(gen_names), len(collisions), SUFFIX))
if collisions:
    print('  ' + ', '.join(collisions))

text = ''.join(blocks)
for nm in collisions:
    text = text.replace('"%s"' % nm, '"%s%s"' % (nm, SUFFIX))

blocks, cur = [], []
for line in text.splitlines(keepends=True):
    cur.append(line)
    if line.rstrip() == '}':
        blocks.append(''.join(cur))
        cur = []
if cur:
    blocks.append(''.join(cur))


# ── 3. 各类后处理 ─────────────────────────────────────────────────────────
def dedupe_lists(block):
    for key in ('out', 'tools', 'srcs'):
        m = re.search(r'(%s: \[\n)((?:\s*"[^"]+",\n)+)(\s*\],)' % key, block)
        if not m:
            continue
        seen, keep = set(), []
        for ln in m.group(2).splitlines(keepends=True):
            k = ln.strip()
            if k and k not in seen:
                seen.add(k)
                keep.append(ln)
        block = block[:m.start(2)] + ''.join(keep) + block[m.end(2):]
    return block


def fix_char_split_flags(block):
    """生成的 python 里 meson 的 `list += '字符串'` 变成 Python 的
    list += str → 整串被拆成单字符条目（"-","W","n","o",...）。
    把连续 3 个以上的单字符条目重组回一个旗标字符串。"""
    pat = re.compile(r'(?:^\s*".",\n){3,}', re.M)

    def repl(m):
        ind = re.match(r'\s*', m.group(0)).group(0)
        chars = re.findall(r'"(.)",', m.group(0))
        return '%s"%s",\n' % (ind, ''.join(chars))

    return pat.sub(repl, block)


def fix_abs_paths(block):
    """mesa 26 的部分 files() 经 dir_source_root 拼接后被生成器写成绝对路径，
    Soong 拒绝（Path is outside directory）。统一改回树内相对路径。"""
    return re.sub(r'"/[^"]*/external/mesa3d/', '"', block)


def fix_genrule(block):
    if not block.lstrip().startswith('genrule {'):
        return block
    block = dedupe_lists(block)

    if 'glslangValidator' in block and '$(location glslangValidator)' not in block:
        block = re.sub(r'(?<!\$\(location )\bglslangValidator\b',
                       '$(location glslangValidator)', block)

    cmd = re.search(r'cmd: "(.*)"\n', block, re.S)
    if cmd:
        used = {t for t in re.findall(r'\$\(location ([^)]+)\)', cmd.group(1))
                if t in HOST_TOOLS}
        tm = re.search(r'tools: \[\n((?:[^\]]*?))\n?(\s*)\],', block)
        if used and tm:
            have = set(re.findall(r'"([^"]+)"', tm.group(1)))
            missing = sorted(used - have)
            if missing:
                entries = tm.group(1).rstrip('\n')
                new_tools = ('tools: [\n' + (entries + '\n' if entries.strip() else '')
                             + ''.join('    "%s",\n' % t for t in missing)
                             + tm.group(2) + '],')
                block = block[:tm.start()] + new_tools + block[tm.end():]
    return block


def fix_genrule_exports(block):
    if not block.lstrip().startswith('genrule {'):
        return block
    outs = re.findall(r'out: \[\n((?:\s*"[^"]+",\n)+)\s*\],', block)
    if not outs:
        return block
    dirs = set()
    for chunk in outs:
        for path in re.findall(r'"([^"]+)"', chunk):
            d = os.path.dirname(path)
            while d:
                dirs.add(d)
                d = os.path.dirname(d)
    m = re.search(r'(export_include_dirs: \[\n)((?:.*?\n)*?)(\s*\],)', block)
    if not m:
        return block
    have = set(re.findall(r'"([^"]+)"', m.group(2)))
    missing = sorted(d for d in dirs if d not in have)
    if not missing:
        return block
    return (block[:m.end(2)]
            + ''.join('         "%s",\n' % d for d in missing)
            + block[m.end(2):])


def fix_drm_dep(block):
    if not re.match(r'\s*cc_library(_static|_shared)? \{', block):
        return block
    if 'libdrm' in block:
        return block
    needs = re.search(r'"src/[^"]*(knl_drm|_drm)[^"]*\.(cc|c)"', block) or 'vulkan_freedreno' in block
    if not needs:
        return block
    m = re.search(r'\n(\s*)shared_libs: \[\n', block)
    if m:
        return block[:m.end()] + '%s    "libdrm",\n' % m.group(1) + block[m.end():]
    m2 = re.search(r'(\n(\s*)name: "[^"]+",\n)', block)
    if not m2:
        return block
    ind = m2.group(2)
    return (block[:m2.end(1)]
            + '%sshared_libs: [\n%s    "libdrm",\n%s],\n' % (ind, ind, ind)
            + block[m2.end(1):])


def fix_python_module(block):
    if not re.match(r'\s*python_(binary|library)_host \{', block):
        return block
    if 'pyyaml' in block:
        return block
    m = re.search(r'\n(\s*)libs: \[\n', block)
    if m:
        ind = m.group(1)
        return block[:m.end()] + '%s    "pyyaml",\n%s    "mako",\n' % (ind, ind) + block[m.end():]
    m = re.search(r'\n(\s*)libs: \[([^\]\n]*)\],', block)
    if m:
        ind = m.group(1)
        items = [x.strip() for x in m.group(2).split(',') if x.strip()]
        for extra in ('"pyyaml"', '"mako"'):
            if extra not in items:
                items.append(extra)
        return block[:m.start()] + '\n%slibs: [%s],' % (ind, ', '.join(items)) + block[m.end():]
    m = re.search(r'(\n(\s*)name: "[^"]+",\n)', block)
    if not m:
        return block
    ind = m.group(2)
    return (block[:m.end(1)]
            + '%slibs: [\n%s    "pyyaml",\n%s    "mako",\n%s],\n' % (ind, ind, ind, ind)
            + block[m.end(1):])


def fix_gralloc_aidl_dep(block):
    """mesa 的 u_gralloc IMapper5 后端要 AIDL 头
    aidl/android/hardware/graphics/common/BufferUsage.h，
    由 android.hardware.graphics.common-V7-ndk 提供（树内通行版本），
    生成器没带这个依赖。"""
    if 'u_gralloc_imapper5_api' not in block:
        return block
    if 'graphics.common-V7-ndk' in block:
        return block
    # 同时要 libui（ui/GraphicBufferMapper.h）——它是 vendor_available，
    # 所以保留 IMapper5 路径，不必退回 HIDL mapper4。
    extra_libs = ['"android.hardware.graphics.common-V7-ndk"', '"libui"']
    m = re.search(r'\n(\s*)shared_libs: \[\n', block)
    if m:
        ind = m.group(1)
        add = ''.join('%s    %s,\n' % (ind, lib) for lib in extra_libs)
        return block[:m.end()] + add + block[m.end():]
    m2 = re.search(r'(\n(\s*)name: "[^"]+",\n)', block)
    if not m2:
        return block
    ind = m2.group(2)
    add = ''.join('%s    %s,\n' % (ind, lib) for lib in extra_libs)
    return (block[:m2.end(1)]
            + '%sshared_libs: [\n%s%s],\n' % (ind, add, ind)
            + block[m2.end(1):])


def fix_warning_flags(block):
    """AOSP 的全局 -Werror 比 mesa 自己的构建更严，mesa 代码里若干无害写法
    会被判为错误（如 vk_drm_syncobj.c 的 unreachable-code-loop-increment）。
    给生成的 cc 模块统一加抑制，遇到新的往这里加即可。"""
    if not re.match(r'\s*cc_library(_static|_shared)? \{', block):
        return block
    if 'gaokun-warn-suppress' in block:
        return block
    flags = [
        '-Wno-unreachable-code-loop-increment',
        '-Wno-error=unreachable-code-loop-increment',
        '-Wno-unused-function',
        '-Wno-unused-variable',
        '-Wno-missing-field-initializers',
    ]
    add_lines = ''.join('        "%s",\n' % f for f in flags)
    m = re.search(r'\n(\s*)cflags: \[\n', block)
    if m:
        return (block[:m.end()]
                + '        // gaokun-warn-suppress\n' + add_lines
                + block[m.end():])
    m2 = re.search(r'(\n(\s*)name: "[^"]+",\n)', block)
    if not m2:
        return block
    ind = m2.group(2)
    return (block[:m2.end(1)]
            + '%scflags: [\n%s        // gaokun-warn-suppress\n%s%s],\n' % (ind, '', add_lines, ind)
            + block[m2.end(1):])


def fix_export_generated_headers(block):
    """Soong 里 generated_headers 默认只给本模块用，不传播给依赖者；
    而 mesa 的 meson 语义是 idep（生成头随依赖一起传递）。turnip 就因此
    找不到 mesa_util 生成的 util/shader_stats.h。给每个带 generated_headers
    的 cc_library 补一份同名 export_generated_headers。"""
    if not re.match(r'\s*cc_library(_static|_shared|_headers)? \{', block):
        return block
    if 'export_generated_headers' in block:
        return block
    m = re.search(r'\n(\s*)generated_headers: \[\n((?:\s*"[^"]+",\n)+)(\s*)\],\n', block)
    if not m:
        return block
    ind, items, ind2 = m.group(1), m.group(2), m.group(3)
    add = '\n%sexport_generated_headers: [\n%s%s],\n' % (ind, items, ind2)
    return block[:m.end()] + add.lstrip('\n') + block[m.end():]


def block_name(block):
    m = re.search(r'name:\s*"([^"]+)"', block)
    return m.group(1) if m else None


def srcs_all_exist(block):
    """生成内容里有一批 gfxstream guest 模块，其源码在这版 mesa 里已经搬走
    （现在住在 hardware/google/gfxstream），路径不存在 → Soong 直接报错。
    我们不建 gfxstream，凡是源码缺失的生成模块一律剔除。"""
    if block.lstrip().startswith('genrule {'):
        return True
    for path in re.findall(r'"((?:src|include)/[^"]+\.(?:c|cc|cpp|h|hh))"', block):
        if not os.path.exists(path):
            return False
    return True


alive, ghosts = [], []
for b in blocks:
    if srcs_all_exist(b):
        alive.append(b)
    else:
        ghosts.append(block_name(b) or '?')

if ghosts:
    print('剔除源码缺失的模块 %d 个: %s' % (len(ghosts), ', '.join(ghosts)))
    # 同时清掉别处对它们的引用，否则 Soong 报依赖缺失
    cleaned = []
    for b in alive:
        for g in ghosts:
            b = re.sub(r'\n\s*"%s",' % re.escape(g), '', b)
        cleaned.append(b)
    alive = cleaned

blocks = alive
fixed = [fix_python_module(fix_warning_flags(fix_export_generated_headers(fix_gralloc_aidl_dep(fix_drm_dep(fix_genrule_exports(fix_genrule(fix_char_split_flags(fix_abs_paths(b))))))))) for b in blocks]
print('后处理: 修正 %d 个块' % sum(1 for a, b in zip(blocks, fixed) if a != b))

# ── 4. 追加手写的共享库包装（turnip 静态库 → vulkan.freedreno.so）────────
# 生成器只产出 cc_library_static，而 Android 的 Vulkan 加载器要
# /vendor/lib64/hw/vulkan.<ro.hardware.vulkan>.so，故补一个 cc_library_shared。
extra_path = os.environ.get(
    'GAOKUN_TURNIP_SHARED_BP',
    os.path.expanduser('~/aosp/device/huawei/gaokun3/mesa/turnip-shared.bp.in'))
if os.path.isfile(extra_path):
    extra = '\n' + open(extra_path).read().rstrip() + '\n'
    # 把 turnip 静态库自己的依赖清单镜像进包装模块：whole_static_libs 只带进
    # turnip 的目标文件，它依赖的那 21 个静态库不会自动跟着走，否则链接期一堆
    # undefined symbol（vk_format_aspects / vk_default_allocator / util_format_*）。
    turnip = ''.join(b for b in fixed if 'name: "vulkan_freedreno",' in b)
    if turnip:
        mirrored = []
        for key in ('static_libs', 'whole_static_libs'):  # header_libs 模板里已有
            m = re.search(r'\n    %s: \[\n((?:.*?\n)*?)    \],' % key, turnip)
            if not m:
                continue
            # 包装模块里已按共享库列出的（libz/libsync/libdrm…）不能再当静态依赖，
            # 否则 Soong 找不到对应 variant（它们的 vendor 静态变体不存在）。
            as_shared = set(re.findall(r'"([^"]+)"',
                                       re.search(r'shared_libs: \[\n((?:.*?\n)*?)    \],',
                                                 extra).group(1))) if 'shared_libs' in extra else set()
            names = []
            for n in re.findall(r'"([^"]+)"', m.group(1)):
                if n not in names and n not in as_shared:
                    names.append(n)
            if key == 'whole_static_libs':
                # turnip 已把这些整体并进自己的 .a，这里作为普通静态依赖补上即可
                key = 'static_libs'
            mirrored.append((key, names))
        merged = {}
        for key, names in mirrored:
            merged.setdefault(key, [])
            for n in names:
                if n not in merged[key]:
                    merged[key].append(n)
        block = ''
        for key, names in merged.items():
            block += '    %s: [\n%s    ],\n' % (
                key, ''.join('        "%s",\n' % n for n in names))
        extra = extra.replace('    whole_static_libs: [\n        "vulkan_freedreno",\n    ],\n',
                              '    whole_static_libs: [\n        "vulkan_freedreno",\n    ],\n' + block, 1)
        print('已把 turnip 的 %d 类依赖镜像进包装模块' % len(merged))
    print('已附加共享库包装: %s' % extra_path)
else:
    extra = ''
    print('⚠ 未找到共享库包装 %s —— 只有静态库，Android 无法加载' % extra_path)

out = (orig.rstrip() + '\n\n'
       + '// ─── 以下由 meson_to_hermetic 生成（gaokun3: turnip/freedreno on msm DRM）───\n'
       + '// 生成与合并流程见 docs/dev/gpu-turnip.md\n'
       + '// 工具缺口补丁 scripts/mesa-tool-fixes.py，合并后处理 scripts/mesa-bp-merge.py\n'
       + ''.join(fixed).lstrip('\n')
       + extra)
open(ORIG, 'w').write(out)
print('合并完成 → %s（%d 行，%d 个模块）'
      % (ORIG, out.count('\n'), len(re.findall(r'name:\s*"', out))))
