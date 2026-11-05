# `libcamera`本仓补丁集

上游仓库：`https://gitlab.freedesktop.org/camera/libcamera.git`  
基线提交：`87c7285663aaad7608fdc18d5216ec6811c685c7`（2026-09-09）

相机整体架构与标定说明见[`docs/dev/camera.md`](../../docs/dev/camera.md)。

## 一、文件清单

| 文件 | 作用 |
|---|---|
| `0001-base-thread-use-sched_setaffinity-on-bionic.patch` | Android bionic无`pthread_setaffinity_np`，改用`sched_setaffinity` |
| `0002-ipa-libipa-add-hi846-camera-sensor-helper.patch` | 前摄Hi-846的黑电平（4096）与线性模拟增益模型（`gain = 1 + code/16`） |
| `0003-ipa-manager-allow-trusting-unsigned-ipa.patch` | 支持宏`LIBCAMERA_IPA_TRUST_UNSIGNED`，允许IPA在无签名时直接加载至进程内运行 |
| `0004-ipa-libipa-add-ov13b10-sensor-helper-and-properties.patch` | 后摄OV13B10的模拟增益模型与传感器静态属性（像元尺寸、测试图案表） |
| `0005-libipa-camera-sensor-helper-ov13b10-model-the-analogue-gain-steps.patch` | 按OV13B10硬件离散档位对模拟增益取整，消除AGC在相邻档位间来回跳变引发的亮度闪烁 |
| `0006-ipa-softisp-update-the-sensor-info-on-configure.patch` | 每次`configure()`时更新软件ISP的传感器模式信息，确保帧长与曝光上限随分辨率模式同步更新 |
| `0007-ipa-softisp-set-vblank-from-the-frame-duration-limits.patch` | 根据`FrameDurationLimits`设置传感器`VBLANK`，使HAL请求的帧率生效 |
| `0008-ipa-libipa-agc-let-the-exposure-time-follow-the-frame-length.patch` | 允许AGC曝光上限随当前帧长动态扩展，并补充传感器曝光余量（OV13B10为8行，Hi-846为6行） |
| `0009-libcamera-software-isp-leave-saturated-pixels-out-of-the-colour-sums.patch` | 灰度世界白平衡统计排除饱和像素，改善高光场景偏绿问题 |
| `0010-ipa-softisp-awb-start-from-the-last-white-balance.patch` | 重新`configure()`时沿用上一次收敛的白平衡增益，避免切换分辨率或拍照时短暂偏绿 |
| `0011-libcamera-software-isp-correct-phase-detection-pixels-before-debayering.patch` | 在去拜耳前插值修补OV13B10全尺寸模式下的PDAF相位检测像素，消除成片中32像素周期的蓝点网格 |
| `0012-ipa-libipa-agc-filter-over-the-frames-between-statistics.patch` | 软件ISP每4帧统计一次亮度，AGC滤波按统计间隔帧数折算收敛速度 |
| `0013-ipa-libipa-agc-mean-luminance-keep-the-target-within-the-exposure-limits.patch` | 将AGC有效目标值钳制在最大曝光×最大增益可达范围内，防止从极暗环境回到亮处时长时间过曝 |
| `libcamera-Android.bp` | AOSP Soong构建定义（复制到`external/libcamera/Android.bp`） |
| `config.h` | 替代meson生成的`config.h`，将IPA与配置文件路径指向`/vendor/`对应目录 |

## 二、构建说明

1. **必须在AOSP树内由Soong编译**：Android NDK的`libc++`使用`std::__ndk1`内联命名空间，而AOSP平台使用`std::__1`。跨工具链预编译会导致所有含标准库类型参数的C++符号无法链接。
2. **生成源码准备**：Soong构建消费的6个生成源文件（`control_ids.cpp`、`property_ids.cpp`、`version.cpp`、`ipa_pub_key.cpp`、`softisp_ipa_proxy.cpp`及`generated/include/libcamera/**`）由`scripts/camera/build-libcamera-android.sh`运行meson生成后拷入`external/libcamera/`。
