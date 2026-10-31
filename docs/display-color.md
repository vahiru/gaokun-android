# 显示色彩：工厂校准、Display P3、10-bit

面板是 8 bpc 的 IPS 屏，原生色域接近 Display P3，白点约 8000 K。不校准的话 sRGB 内容会偏艳偏冷。
下面几块改动互相配合，出问题时按这里的顺序查。

## 校准数据从哪来

BIOS 从 DPP 分区读出每台机器的工厂 3D LUT，开机时装成 ACPI 表 `DLUT`（OEM `HUAWEI`，长度 0x20034）。
Windows 用 `GetSystemFirmwareTable` 读它。这块内存是 ACPI reclaim，设备树启动时也完好，
内核顺着 `efi.acpi20` → RSDP → XSDT 能找到。

表里有两条记录，间隔 0x6400，头部是 `$HUA02`，记录 +0x50 处的类型字节 1 = sRGB、2 = P3
（顺序不固定）。记录 +0x90 起是 17³ 的 LUT，先是 4913 个大端 u32（R 低 8 位、G 12 位、B 12 位），
后面 4913 字节是 R 的高 4 位。LUT 在编码域，按 gamma 2.2 算。

## 怎么用（内核，patches/0133–0134）

DPU 只有 PCC（3×3 矩阵）和 GC，没有 3D LUT，所以 `dpu_panel_cal.c` 在 probe 时把每张 LUT
化简成一个矩阵：取每个基色在节点 12 的响应换到线性光，按行归一，再缩放到 LUT 的白点。
校准矩阵接在 CTM 后面，Android 自己的色彩变换（夜间模式等）不受影响，外接显示器也不受影响。

- `msm.panel_calibration=0` 运行时关掉校准，默认打开。
- PCC 只在 commit 带 CTM 时才启用。Android 的 HWC 每次都带，所以 Android 上一直有校准；不写 CTM 的 DRM 客户端没有校准。
- 两张表缺一张、校验失败或者不是这台机器，校准就不启用，也不挂 `Colorspace` 属性，HWC 退回只报告 NATIVE。
- PCC 工作在编码后的像素上，灰阶附近很准，全饱和色在 sRGB 模式下仍接近原生色度。
  P3 模式下这类颜色由 GPU 在线性光里转换，更准。

## sRGB 和 P3 怎么切换

```
应用声明 P3 ─→ SF 选 DISPLAY_P3 ─→ HWC 把模式写成连接器 Colorspace ─→ 内核换 P3 矩阵
```

- 默认输出 sRGB，有 P3 图层才切，不需要 modeset，矩阵和那一帧同时生效。
- 输出是 P3 时，DPU 没有逐图层的色域映射，所以不是 Display P3 的图层都交给 GPU（patches/0132）。
- 需要 `ro.surface_flinger.has_wide_color_display` 和 `vendor.hwc.drm.force_p3_support`（device.mk）。
- Android 的广色域是应用自己声明的。Chromium 系浏览器默认不用，要在 flags 里把 Force color profile
  设成 Display P3 D65，这是浏览器的策略。

## 10-bit 怎么到面板

SF 的帧缓冲写死 RGBA_8888。有 10-bit 图层走 GPU 合成时，HWC 用 ClientTargetProperty 请求 RGBA_1010102，
图层没了再请求回来（patches/0131）。平面列表要有 ABGR2101010（patches/0136）。
RGBA_1010102 只有 2 位 alpha，所以 client target 不在最底层时保持 8 位。

面板只收 8 bit，所以 PINGPONG 的空间抖动把多出来的精度抖下去（patches/0135）。两路 PINGPONG 各管半屏。
抖动也会处理 PCC 引入的取整。面板描述必须有 `.bpc = 8`，否则 `display_info.bpc` 是 0，抖动不会开。

## HDR 提亮

HDR 视频的高光要比界面白更亮，只能抬背光，所以界面要同时压暗，看上去亮度不变：

```
HDR 图层上屏 ─→ 框架算出提亮后的亮度 ─→ SF 压暗 SDR 图层 ─→ HWC 同一帧写背光
```

- 背光归 HWC（sepolicy/hal_graphics_composer_default.te）。灯光 HAL 还在，但框架不再用它调背光。
- 面板背光寄存器和亮度成线性（patches/0138）。提亮倍数就是寄存器的比值。
- DPU 平面不能逐图层调增益，压暗的图层都交给 GPU（patches/0137）。压暗期间帧缓冲也用 10-bit，
  空间抖动把精度带到面板，暗部不会有色带。
- 倍数表和上限在 `etc/displayconfig/display_port_0.xml`。320 尼特来自 Windows 的面板配置，没有实测；
  提亮和压暗都只用比值，这个数不准只影响上报的尼特值。
- 有这份配置后，框架内部的亮度 0..1 对应背光的最小值到最大值。亮度条的位置和 overlay 里的自动亮度曲线
  都按背光换算，和背光的对应关系不变。
- 面板没有分区背光，提亮时黑位跟着背光一起升高。背光比画面慢几百毫秒，所以进出 HDR 时能看到亮度过渡。
- 框架只在 HDR 图层占屏幕 10% 以上时提亮，省电模式下不提亮。亮度条在最大时没有提亮空间。
- 色调映射保持默认的 500 尼特目标。调低它中间调会变亮，但高光压缩得更早，天空这类大面积亮部会发白。
- HWC 只把背光绑给内置屏。sysfs 没有说明背光属于哪个连接器，绑给外接显示器的话，它的亮度会写到内置屏上。
- HWC 给内置屏声明 HDR10 和 HLG（patches/0140，`vendor.hwc.drm.force_hdr_support`），应用靠 `Display.isHdr()`
  选片源。声明之后 SF 不再把 HDR 图层强制交给 GPU，平面又做不了色调映射，所以 HWC 自己转给 GPU。
- v4l2_codec2 声明 HDR 静态元数据（patches/0139），框架才会给 HEVC 列出 Main10 HDR10。元数据只来自容器，
  不解析码流里的 SEI；系统的 MP4 提取器不读 `mdcv` 和 `clli`，只有 Media3 这类自己解析容器的播放器传得进来。
  HDR10+ 不支持。
- ToneMapper13 的内容峰值写死 4000 尼特，不读静态元数据，所以元数据不改变画面。
- Ultra HDR 要应用自己申请 `COLOR_MODE_HDR`。Google 相册可以提亮；Glimpse 和 Edge 不申请，按 SDR 显示。
  Chromium 只在系统已经提亮时才按 HDR 渲染，单张图片不会触发。

## 排查

```sh
dumpsys display | grep -E 'BacklightAdapter|mHdrBrightnessData|mMode='   # useSurfaceControl=true，播 HDR 时 mMode=HBM_HDR
dumpsys SurfaceFlinger | grep displayBrightnessNits              # 提亮时 displayBrightnessNits 大于 sdrWhitePointNits
dumpsys SurfaceFlinger | grep 'HWC Support'                      # hdr10=true hlg=true
dumpsys media.player | grep Main10HDR10                          # HEVC 解码器列出 HDR10 profile
cat /sys/class/backlight/*/scale                                 # linear
dmesg | grep 'factory calibration'     # 两行，0 = sRGB，1 = P3，后面是 Q15 矩阵
dumpsys SurfaceFlinger | grep -E 'Device supports wide color|Current color mode'
grep -A8 DSI-1 /sys/kernel/debug/dri/1/state | grep colorspace   # BT709_YCC 或 DCI-P3_RGB_D65
grep format= /sys/kernel/debug/dri/1/state                       # AB24 = 8 位，AB30 = 10 位
cmd activity get-config | grep -o widecg                         # 应用看到的广色域
service call SurfaceFlinger 1023 i32 0 i32 7                     # 强制 sRGB 输出，P3 图层交给 GPU 转换；i32 0 i32 0 恢复
```

确认某个改动是否改变了输出，用编码器 CRC：`/sys/kernel/debug/dri/1/crtc-0/crc`，`control` 写 `encoder`，
`data` 每行以 NUL 开头。⚠️ 不要用 devmem 读 DPU 寄存器，显示掉电时会让整机崩溃。
