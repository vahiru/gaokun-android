# libcamera 的本仓改动

上游：`https://gitlab.freedesktop.org/camera/libcamera.git`，
本仓验证过的 commit：**`87c7285663aaad7608fdc18d5216ec6811c685c7`**（2026-09-09）。

## 文件

| 文件 | 作用 |
|---|---|
| `0001-base-thread-use-sched_setaffinity-on-bionic.patch` | bionic 没有 `pthread_setaffinity_np`（[#89](../../docs/stage4-findings.md#89)） |
| `0002-ipa-libipa-add-hi846-camera-sensor-helper.patch` | hi846 的增益模型，**实测标定**（[#94](../../docs/stage4-findings.md#94)），可发上游 |
| `0003-ipa-manager-allow-trusting-unsigned-ipa.patch` | 让 IPA 跑进程内而不是独立进程（宏 `LIBCAMERA_IPA_TRUST_UNSIGNED`，默认行为不变） |
| `0004-ipa-libipa-add-ov13b10-sensor-helper-and-properties.patch` | ov13b10的增益模型和静态属性（像素尺寸、测试图案） |
| `0005-libipa-camera-sensor-helper-ov13b10-model-the-analogue-gain-steps.patch` | ov13b10的模拟增益只有有限的档位（1–2倍每档1/16，8倍以上每档1/2），增益按档位取整；否则AGC在两档之间来回试，亮度每秒抖两次 |
| `0006-ipa-softisp-update-the-sensor-info-on-configure.patch` | 软件ISP的IPA每次configure()都更新传感器信息，帧长和曝光不再按创建相机时的旧模式算 |
| `0007-ipa-softisp-set-vblank-from-the-frame-duration-limits.patch` | 按FrameDurationLimits设传感器的VBLANK，HAL要的帧率才真正生效（ov13b10的合并模式原来固定60 fps） |
| `0008-ipa-libipa-agc-let-the-exposure-time-follow-the-frame-length.patch` | 曝光时间可以用到当前帧长，不再卡在驱动按默认帧长给的上限；CameraSensorHelper加上曝光余量（ov13b10为8行，hi846为6行） |
| `0009-libcamera-software-isp-leave-saturated-pixels-out-of-the-colour-sums.patch` | 灰度世界白平衡的统计不计饱和像素，高光多的场景不再偏绿 |
| `0010-ipa-softisp-awb-start-from-the-last-white-balance.patch` | 再次configure()时白平衡从上一次的结果开始，切换模式后不再先绿一阵 |
| `0011-libcamera-software-isp-correct-phase-detection-pixels-before-debayering.patch` | 去拜耳前修补ov13b10全尺寸模式下的相位检测像素，照片里不再有32像素间隔的蓝点网格；位置按读出方向记在传感器属性库里 |
| `0012-ipa-libipa-agc-filter-over-the-frames-between-statistics.patch` | 软件ISP每4帧才有一次统计，AGC按其间的帧数折算收敛速度，亮度突变后收敛快3–5倍 |
| `0013-ipa-libipa-agc-mean-luminance-keep-the-target-within-the-exposure-limits.patch` | AGC的目标值限制在曝光时间×模拟增益能达到的范围内，从全黑环境回到有光时不再长时间过曝 |
| `libcamera-Android.bp` | 把 libcamera 编成 Soong 模块，放到 `external/libcamera/Android.bp` |
| `config.h` | 替代 meson 自动生成的那份，路径改成 vendor 下的真实位置 |

## ⚠️★★ 为什么必须在 AOSP 里编

最初的规划是"NDK 预编译 libcamera + Soong 只编 HAL"。**那是错的**：
NDK 的 libc++ 用内联命名空间 `std::__ndk1`，AOSP 平台用 `std::__1`，
凡签名里带标准库类型的符号两边 mangled name 就对不上。实测链接报
`undefined symbol: ...generateConfigurationENSt3__14span...`。
⇒ **跨工具链混链 C++ 的前提是 ABI 相同，而 NDK 与平台恰恰不同。** 见 [#96](../../docs/stage4-findings.md#96)。

## 还需要的生成文件

Soong 不跑 meson 的生成步骤，所以这 5 个 `.cpp` 与那批生成头要另外产出：
`control_ids.cpp` / `property_ids.cpp` / `version.cpp` / `ipa_pub_key.cpp` /
`softisp_ipa_proxy.cpp`，以及 `generated/include/libcamera/**`（含
`control_ids.h` / `property_ids.h` / `formats.h` / `version.h` /
`ipa/*_ipa_interface.h` 等）。

⬜ **目前靠跑一次 meson 构建产出再拷贝**（`scripts/camera/build-libcamera-android.sh`），
这是一个**手动步骤**，正是 [#82](../../docs/stage4-findings.md#82)/[#85](../../docs/stage4-findings.md#85)
那种会被遗忘的形状。正解是用 Soong 的 `genrule` 跑 libcamera 自带的 Python
生成器（`utils/gen-controls.py`、`utils/ipc/generate.py`），它们只依赖树内的
YAML/mojom，没有别的前置。

## ★ 那组 include 路径是抄来的，不是试出来的

libcamera 的 IPA 源码大量使用**裸名** include（`<module.h>`、
`<core_ipa_interface.h>`、`<ipa_context.h>`、`<control_ids.h>` …）。
少一个的症状是一串**看不出关联**的 `unknown type name`。
`libcamera-Android.bp` 里那组 `local_include_dirs` 是从已跑通的 meson 构建的
`build.ninja` 里 grep 某个 `.o` 的编译行取 `-I` 抄来的 —— **别再一个个试**。
