# gaokun3 相机 HAL（AIDL，基于 libcamera）

把 libcamera 的 `simple` 流水线 + 软件 ISP 接进 Android 的相机框架。

## 为什么是"自己写 AIDL"而不是用现成件

三条路都查过（[#91](../../../../docs/stage4-findings.md#91)），结论是只剩自己写：

* ❌ **HIDL + 上游 `provider@2.4-legacy`**（能直接加载 libcamera 产出的传统
  `camera_module_t`，一行代码不用写）—— **本机 `hwservicemanager` 根本不存在**
  （`/system/bin/hwservicemanager` 是悬空符号链接，init 报 "service not found"），
  而 `CameraProviderManager` 发现 HIDL provider 只能靠它。
  而且 FCM 202504 的兼容性矩阵里 camera.provider **只剩 `format="aidl"`**。
* ❌ **libcamera 的 V4L2 垫片 + AOSP 自带的 ExternalCameraProvider** ——
  软件 ISP **只输出 RGB 族**（`debayer_cpu.cpp:436-441`），而那个 HAL 要 YUYV/MJPEG。
  和 [#84](../../../../docs/stage4-findings.md#84) 撞的是同一堵墙。
* ✅ **自己写 AIDL**：`CameraMetadata.aidl` 原文说它就是
  "A serialized metadata buffer created by libcamera_metadata,
  access by casting to a `camera_metadata*`" ⇒ 与 camera3 同一个东西，
  转换成本为零；色彩转换用 AOSP 自带的 libyuv、静态图用 libjpeg。

## ⚠️ 构建形态（以及一笔要还的债）

libcamera 本身**不在 AOSP 里编**，走 `scripts/camera/build-libcamera-android.sh`
（meson + NDK）产出 `.so`，本模块以 `cc_prebuilt_library_shared` 链接。

理由：libcamera 的 Android HAL 层需要 `libjpeg`/`libexif`，而 libcamera 的
subprojects 里**只有 `libyuv.wrap`，没有这两个**；反过来，本模块是 Soong 模块，
AOSP 自带的 libjpeg/libyuv 直接可用 ⇒ 绕开整个依赖问题。

⚠️★ **但这笔债必须记着**：预编译产物**不在仓库里**（[#89](../../../../docs/stage4-findings.md#89)
的决定：可复现的东西不入库），于是构建 ROM 前**必须先手动跑一次那个脚本**。
**这正是 [#82](../../../../docs/stage4-findings.md#82)/[#85](../../../../docs/stage4-findings.md#85)
那种"手动步骤终将被遗忘"的形状。**
⬜ 正解是把 libcamera 移植成 Soong 模块（TODO A7 的 M3）：生成文件那部分
（control_ids / property_ids / mojom→C++）可以用 `genrule` 跑它自带的 Python 生成器。
在那之前，`build-libcamera-android.sh` 必须进发版流程的检查单。

## 闪光灯 / 手电筒（2026-09-14，#110/#111）

* **硬件**：LED 挂在 PM8350C（`pmc8280c`）闪光模块的 **1+4 路**（四路逐个点亮、有人看背面定出来的），
  内核 `patches/0036` 暴露成一个 `/sys/class/leds/white:flash`。GPIO93 那个"flash"是假的，已删。
* **手电筒**：`Device::setTorchMode` 写 `brightness = max_brightness`；相机被会话占用时报 `CAMERA_IN_USE`；
  `open()` 时经 provider 回调报 `NOT_AVAILABLE`，会话关闭报 `AVAILABLE_OFF`。快捷设置的手电筒砖走的就是这条。
* **拍照闪光 = 预闪点灯到拍完**：没有与曝光同步的 strobe（libcamera 排队深度 4、每帧约 33 ms（早期 60–130 ms），strobe 打下去
  哪一帧在曝光不可控）。所以收到 `AE_PRECAPTURE_TRIGGER_START` 就点灯、报几帧 `AE_STATE_PRECAPTURE` 让 AGC 适应，
  一路亮到那张静态照片（BLOB / `STILL_CAPTURE` intent / `FLASH_MODE_SINGLE`）**完成**才灭。
  ⚠️ 不能在"下一个不要灯的请求入队时"就灭：框架把预览请求紧跟拍照请求发来，那帧还没曝光——
  用 `firedPending_` 计数在途的点灯请求，全完成才灭。
* `ON_AUTO_FLASH` 的"太暗"判据是交付帧的采样平均亮度（`kDarkLuma`），因为软件 ISP 的 IPA 不往结果元数据里
  写曝光/增益。是启发式。
* 权限：`ueventd.gaokun3.rc` 给 LED 节点 `0664 root camera`；sepolicy 里 provider 在 `hal_camera_default` 域，
  `sysfs_leds` 可读写。features xml 声明 `android.hardware.camera.flash`（没有它 SystemUI 不出手电筒砖）。
* ⬜ 亮度档位（`FLASH_INFO_STRENGTH_*` + `turnOnTorchWithStrengthLevel`）还没做。

## 两条容易再踩的事实

* **请求设置优先走 FMQ**（`AidlCamera3Device.cpp:1268`）：`fmqSettingsSize > 0` 时 `settings` 字段是空的，
  要从 `requestQueue_` 读。2026-09-14 之前本 HAL 只读字段，等于几乎没看过任何请求设置。
* **相机编号必须自己排**：libcamera 的枚举顺序随 media 设备出现先后而变，同一台机器两次开机后摄的 ID 会换。
  `Provider::init` 按 `Location` 排（Back = 0），符合 Android 的约定。
