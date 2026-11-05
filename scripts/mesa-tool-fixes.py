#!/usr/bin/env python3
"""补齐 AOSP external/mesa3d 里 meson_to_hermetic 工具与 mesa 25.3 的 API 差距。

**仅 A16（crDroid 16.0，mesa 25.3 / 26.0.3 + 旧工具 meson_to_hermetic）。**
A17（crDroid 17.0，mesa 26.1.0-devel）的 external/mesa3d 已删掉 meson_to_hermetic，
改用 meson2hermetic（`meson convert`）；A17 的管线见 docs/dev/mesa-a17.md 与
scripts/mesa-a17/regen.sh。本文件只作 A16 历史留档，在 A17 树上运行会直接退出。

背景：AOSP 的 external/mesa3d（mesa 25.3.0-devel）自带 meson_to_hermetic 工具，
能把 meson 构建翻译成 Soong（Android.bp），官方脚本 build-android-turnip.sh 就是
用它生成 turnip（freedreno Vulkan）的。但该工具快照落后于同仓库的 mesa 源码，
生成过程会连撞多个 API 缺口。本脚本把这些缺口一次补齐，幂等可重跑。

用法（在 external/mesa3d 目录下）：
    python3 scripts/mesa-tool-fixes.py

修复清单：
  1. module_import 不认 'fs' 模块（顶层 meson.build 用 fs.relative_to 算
     OpenCL 的 -fmacro-prefix-map）→ 加最小存根
  2. Meson 对象缺 global_source_root / global_build_root（非子项目场景等价于
     project_ 版）
  3. FeatureOption 缺 enable_if()（meson 0.59+ API）
  4. meson_common.run_command() 少解包 commands（导致 subprocess 收到嵌套
     list），且未开 capture_output（调用方 .stdout() 拿到 None）
  5. CommandReturn.stdout() 返回 bytes，且生成代码按 meson 语义调
     str.version_compare() → 加 MesonStr（str 子类，split/strip 保持类型）
  6. find_program 不支持多候选名 find_program('flex','lex',...)（第二个名字
     落进 required 触发报错）
  7. 程序白名单只有 'python'，mesa 25.3 找 'python3'
  8. get_option('b_sanitize') 返回 bool，生成代码按字符串用（.count/=='none'）
"""
import os
import re
import sys

if not os.path.isdir('meson_to_hermetic'):
    sys.exit('本脚本只适用于带 meson_to_hermetic 的 A16 mesa 树；'
             'A17（mesa 26.1）请用 scripts/mesa-a17/regen.sh（见 docs/dev/mesa-a17.md）')

COMMON = 'meson_to_hermetic/meson_common.py'
IMPL = 'meson_to_hermetic/meson_impl.py'
MAIN = 'meson_to_hermetic/meson_to_hermetic.py'

applied, skipped = [], []


def edit(path, marker, fn):
    """fn(src) -> new_src 或 None（表示锚点缺失）。marker 命中则跳过。"""
    if not os.path.isfile(path):
        sys.exit('缺少文件 %s —— 请在 external/mesa3d 目录下运行' % path)
    src = open(path).read()
    if marker in src:
        skipped.append(marker)
        return
    out = fn(src)
    if out is None:
        sys.exit('锚点缺失，无法应用: %s（%s）' % (marker, path))
    open(path, 'w').write(out)
    applied.append(marker)


# 1 + 3..8 分别处理 ----------------------------------------------------------

def fix_fs_module(src):
    anchor = '\ndef module_import(name: str):'
    if anchor not in src:
        return None
    stub = '''
class GaokunFsModule:
    """Minimal 'fs' module stub: the Android build never compiles OpenCL, so the
    -fmacro-prefix-map prefix derived from fs.relative_to() does not matter."""

    def relative_to(self, a, b, *args, **kwargs):
        return "."

    def exists(self, path, *args, **kwargs):
        import os
        return os.path.exists(str(path))

    def is_dir(self, path, *args, **kwargs):
        import os
        return os.path.isdir(str(path))

    def name(self, path, *args, **kwargs):
        import os
        return os.path.basename(str(path))

    def parent(self, path, *args, **kwargs):
        import os
        return os.path.dirname(str(path))


def module_import(name: str):'''
    src = src.replace(anchor, stub, 1)
    hook = "    if name == 'python':\n        return impl.PythonModule()"
    if hook not in src:
        return None
    return src.replace(hook, hook + "\n    if name == 'fs':\n        return GaokunFsModule()", 1)


def fix_global_roots(src):
    m = re.search(r'\n(\s+)def project_source_root\(self[^\)]*\):\n', src)
    if not m:
        return None
    i = m.group(1)
    add = ("\n{i}def global_source_root(self, *args, **kwargs):\n"
           "{i}    # gaokun: 非子项目场景与 project_source_root 等价\n"
           "{i}    return self.project_source_root(*args, **kwargs)\n"
           "\n{i}def global_build_root(self, *args, **kwargs):\n"
           "{i}    return self.project_build_root(*args, **kwargs)\n").format(i=i)
    return src[:m.start()] + add + src[m.start():]


def fix_enable_if(src):
    m = re.search(r'\n(\s+)def disable_if\(self, value: bool, error_message: str\):', src)
    if not m:
        return None
    i = m.group(1)
    add = ("\n{i}def enable_if(self, value: bool, error_message: str = ''):\n"
           "{i}    # gaokun: meson 0.59+ API\n"
           "{i}    if not value:\n"
           "{i}        return self\n"
           "{i}    if self.state == EnableState.DISABLED:\n"
           "{i}        exit(error_message)\n"
           "{i}    return FeatureOption(self.name, state=EnableState.ENABLED)\n").format(i=i)
    return src[:m.start()] + add + src[m.start():]


def fix_run_command(src):
    old = "def run_command(program, *commands, check=False):\n    return program.run_command(commands)"
    if old not in src:
        return None
    new = ("def run_command(program, *commands, check=False):\n"
           "    # gaokun: 解包 commands 并开启输出捕获（调用方会 .stdout()）\n"
           "    return program.run_command(*commands, capture_output=True)")
    return src.replace(old, new, 1)


def fix_meson_str(src):
    klass = '''
def _gaokun_ver_tuple(text):
    parts = []
    for chunk in str(text).strip().split('.'):
        digits = ''
        for ch in chunk:
            if ch.isdigit():
                digits += ch
            else:
                break
        parts.append(int(digits) if digits else 0)
    return tuple(parts)


class MesonStr(str):
    """字符串 + meson 语言方法（gaokun）。"""

    def version_compare(self, spec):
        spec = str(spec).strip()
        for op in ('>=', '<=', '==', '!=', '>', '<'):
            if spec.startswith(op):
                rhs = spec[len(op):].strip()
                break
        else:
            op, rhs = '==', spec
        a, b = _gaokun_ver_tuple(self), _gaokun_ver_tuple(rhs)
        w = max(len(a), len(b))
        a, b = a + (0,) * (w - len(a)), b + (0,) * (w - len(b))
        return {'>=': a >= b, '<=': a <= b, '==': a == b,
                '!=': a != b, '>': a > b, '<': a < b}[op]

    def split(self, *args, **kwargs):
        return [MesonStr(s) for s in str.split(self, *args, **kwargs)]

    def strip(self, *args, **kwargs):
        return MesonStr(str.strip(self, *args, **kwargs))


class CommandReturn:'''
    if '\nclass CommandReturn:' not in src:
        return None
    src = src.replace('\nclass CommandReturn:', klass, 1)
    old = "    def stdout(self):\n        return self.completed_process.stdout"
    new = ('    def stdout(self):\n'
           '        # gaokun: capture_output 下是 bytes；返回带 meson 语义的字符串\n'
           '        out = self.completed_process.stdout\n'
           '        if isinstance(out, bytes):\n'
           '            out = out.decode("utf-8", "replace")\n'
           '        return MesonStr(out if out is not None else "")')
    if old not in src:
        # 可能已被早期手改过，尽力匹配
        m = re.search(r'    def stdout\(self\):\n(?:.*\n)+?        return [^\n]+\n', src)
        if not m:
            return None
        src = src[:m.start()] + new + '\n' + src[m.end():]
        return src
    return src.replace(old, new, 1)


def fix_find_program(src):
    old = "def find_program(name: str, required=False, native=False, disabler=False, version=''):"
    if old not in src:
        return None
    new = ("def find_program(name: str, *alt_names, required=False, native=False, disabler=False, version=''):\n"
           "    # gaokun: meson 允许多候选名 find_program('flex','lex',...)\n"
           "    del alt_names")
    return src.replace(old, new, 1)


def fix_python3_whitelist(src):
    old = "        or name == 'python'\n"
    if old not in src:
        return None
    return src.replace(old, old + "        or name == 'python3'  # gaokun: mesa 25.3 用 python3\n", 1)


def fix_anonymous_custom_target(src):
    """meson 允许 custom_target 省略名字（名字从 output 推导），mesa 25.3 就有
    这种写法；工具把 target_name 当必填位置参数，于是 TypeError。"""
    old = "def custom_target(\n    target_name: str,"
    if old not in src:
        return None
    new = ("def custom_target(\n"
           "    target_name: str = None,  # gaokun: meson 允许匿名，见下方推导\n")
    src = src.replace(old, new, 1)
    # 在函数体开头补推导逻辑：找第一条语句的缩进
    m = re.search(r"def custom_target\(\n(?:.*\n)+?\):\n", src)
    if not m:
        return None
    insert_at = m.end()
    derive = ("    # gaokun: 匿名 custom_target —— 用 output（或首个 input）推导名字\n"
              "    if target_name is None:\n"
              "        _o = output if not isinstance(output, list) else (output[0] if output else None)\n"
              "        if _o is None:\n"
              "            _i = input if not isinstance(input, list) else (input[0] if input else 'anon')\n"
              "            _o = str(_i)\n"
              "        target_name = str(_o).replace('/', '_')\n")
    return src[:insert_at] + derive + src[insert_at:]


def fix_program_found(src):
    """白名单里的程序把 found 直接取自 required（默认 False），于是
    prog_python=find_program('python3','python',version='>=3.8') 得到
    found=False，后面 custom_target 的 assert program.found() 就炸。
    改成：系统 PATH 里真有就算找到。"""
    old = "        return impl.Program(name, found=required)"
    if old not in src:
        return None
    new = ("        # gaokun: PATH 里真有就算找到（原来只看 required，导致\n"
           "        # prog_python.found() 为假，custom_target 断言失败）。\n"
           "        # 注：glslangValidator 也要算“找到”—— turnip 的 bvh 目录是无条件\n"
           "        # subdir，视其不可用会让 vk_bvh_include_dir 未定义而生成失败。\n"
           "        # Soong 沙箱不许用 PATH 上的它，所以我们在树里补了 host 模块\n"
           "        # （patches/0003-*.patch），合并脚本会把 cmd 改写成 $(location ...)。\n"
           "        import shutil\n"
           "        return impl.Program(name, found=(shutil.which(name) is not None) or required)")
    return src.replace(old, new, 1)


def fix_libdrm_dependency(src):
    """工具把 libdrm 硬编码成 found=False。可是 mesa 里
    `if dep_libdrm.found()` 门控着 vk_drm_syncobj.c —— 不编它，turnip 链接期
    就缺 vk_drm_syncobj_finish/get_type。AOSP 树里 libdrm 是 vendor_available
    的共享库，应当报告"找到"。"""
    old = "            or name == 'libdrm'\n"
    if old not in src:
        return None
    # 从硬编码的 not-found 名单里摘掉 libdrm，并在其上方单独返回一个已找到的依赖
    src = src.replace(old, '', 1)
    anchor = "        if name in external_dep:\n"
    if anchor not in src:
        return None
    inject = (
        "        # gaokun: libdrm 在 AOSP 里是 vendor_available 共享库，报告为已找到；\n"
        "        # 否则 mesa 跳过 vk_drm_syncobj.c，turnip 链接期缺符号。\n"
        "        if name == 'libdrm':\n"
        "            return Dependency(\n"
        "                name,\n"
        "                targets=[DependencyTarget('libdrm', DependencyTargetType(1))],\n"
        "                version=version,\n"
        "                found=True,\n"
        "            )\n")
    return src.replace(anchor, inject + anchor, 1)


def fix_b_sanitize(src):
    m = re.search(r"(\n\s+if name == 'b_sanitize':\n\s+return )([^\n]+)\n", src)
    if not m:
        return None
    return src[:m.start()] + m.group(1) + "'none'  # gaokun: 生成代码按字符串用\n" + src[m.end():]


# 标记用最短的稳定特征串（宽松匹配，兼容手工先改过的树）
edit(MAIN, 'GaokunFsModule', fix_fs_module)
edit(IMPL, 'def global_source_root', fix_global_roots)
edit(IMPL, 'def enable_if', fix_enable_if)
edit(COMMON, '解包 commands', fix_run_command)
edit(IMPL, 'class MesonStr', fix_meson_str)
edit(COMMON, '多候选名', fix_find_program)
edit(COMMON, "== 'python3'", fix_python3_whitelist)
edit(COMMON, "'none'  # gaokun", fix_b_sanitize)
edit(COMMON, 'shutil.which(name) is not None', fix_program_found)
edit(IMPL, "gaokun: libdrm 在 AOSP", fix_libdrm_dependency)
edit(MAIN, 'gaokun: 匿名 custom_target', fix_anonymous_custom_target)

print('已应用: %d 项' % len(applied))
for a in applied:
    print('  + %s' % a)
if skipped:
    print('已存在（跳过）: %d 项' % len(skipped))
