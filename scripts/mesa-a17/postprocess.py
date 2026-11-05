#!/usr/bin/env python3
"""meson2hermetic 生成之后的唯一一处后处理：vulkan.freedreno 只编 64 位。

用法：python3 scripts/mesa-a17/postprocess.py <mesa3d 目录>      幂等

为什么要它：
  生成器只在"某个目标在**所有**配置里都存在"时才给 target.android 写
  compile_multilib（mesonbuild/convert/build_systems/soong/state.py 里
  ANDROID_MULTILIB 只在非条件分支置位）。vulkan.freedreno 只出现在 gaokun_turnip
  一个配置、一个平台（android_arm64），于是生成结果里没有 compile_multilib，
  32 位 arm 变体会落到 select 的 default 分支 —— 一个没有源文件的空 .so，
  装成 /vendor/lib/hw/vulkan.freedreno.so。32 位进程的 libvulkan 会 dlopen 它、
  找不到 HAL_MODULE_INFO_SYM 再报错。A16 的手写包装（device/huawei/gaokun3/mesa/
  turnip-shared.bp.in）同样写的是 compile_multilib: "64"，这里保持一致。
  （32 位 turnip 要另加 android_arm 平台，见 docs/dev/mesa-a17.md「待定」。）

A16 那 9 类合并后处理（scripts/mesa-bp-merge.py）在 A17 上都不需要了：
生成器直接写多文件 Android.bp、不重名、tools / libs / export_generated_headers
都自己处理。新出现的问题先查生成器能不能改，再考虑往这里加。
"""
import os
import re
import sys

MODULE = 'vulkan.freedreno'
BP = 'src/freedreno/vulkan/Android.bp'
LINE = '            compile_multilib: "64",\n'


def module_span(text, name):
    m = re.search(r'^cc_library_shared \{\n    name: "%s",\n' % re.escape(name), text, re.M)
    if not m:
        return None
    end = text.find('\n}\n', m.start())
    if end < 0:
        return None
    return m.start(), end + 1          # 含 '\n'，不含右花括号


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__.split('\n\n')[0])
    path = os.path.join(sys.argv[1], BP)
    if not os.path.isfile(path):
        sys.exit('✗ 找不到 %s（生成器没产出 turnip？看 gaokun_turnip 配置）' % path)
    text = open(path).read()
    span = module_span(text, MODULE)
    if not span:
        sys.exit('✗ %s 里没有 cc_library_shared "%s"' % (BP, MODULE))
    a, b = span
    body = text[a:b]
    if 'compile_multilib: "64"' in body:
        print('  = %s 已有 compile_multilib: "64"' % MODULE)
        return
    anchor = '    target: {\n        android: {\n'
    if anchor in body:
        body = body.replace(anchor, anchor + LINE, 1)
    else:
        body += '    target: {\n        android: {\n' + LINE + '        },\n    },\n'
    open(path, 'w').write(text[:a] + body + text[b:])
    print('  + %s: target.android.compile_multilib = "64"' % MODULE)


if __name__ == '__main__':
    main()
