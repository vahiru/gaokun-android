# 显示色彩：工厂校准、Display P3、10-bit

面板是8 bpc的IPS屏，原生色域接近Display P3，白点约8000 K。不校准的话sRGB内容会偏艳偏冷。下面几块改动互相配合，出问题时按这里的顺序查。

## 校准数据从哪来

BIOS从DPP分区读出每台机器的工厂3D LUT，开机时装成ACPI表`DLUT`（OEM `HUAWEI`，长度`0x20034`）。Windows用`GetSystemFirmwareTable`读它。这块内存是ACPI reclaim，设备树启动时也完好，内核顺着`efi.acpi20` → RSDP → XSDT能找到。

表里有两条记录，间隔`0x6400`，头部是`$HUA02`，记录`+0x50`处的类型字节1 = sRGB、2 = P3（顺序不固定）。记录`+0x90`起是17³的LUT，先是4913个大端`u32`（R低8位、G 12位、B 12位），后面4913字节是R的高4位。LUT在编码域，按gamma 2.2算。

## 怎么用（内核，`patches/kernel/drm/0004–0005`、`0018–0019`）

DSPP在PCC前面有一个3D LUT（`0018`）。它的13点模式有两个存储体：写当前没在用的那个不影响画面，切换只写一次opmode，随下一次flush生效，所以按帧换LUT不会撕裂。17点模式只有一个存储体，扫描中重写大约每5次切换撕裂1帧，所以不用。表的格式是在硬件上用CRC一项项测出来的（表不能回读），写在`dpu_hw_dspp.c`的注释里。

`dpu_panel_cal.c`在probe时把两张17点LUT重采样成13点（`0019`）：存储体A放sRGB，B放P3。每个DSPP记着自己的存储体是否已写好。CRTC保持点亮的modeset会清掉这个标记并在同一次commit里重新写入，因为modeset可能换来断过电的DSPP。

- 按连接器的`Colorspace`选存储体：`BT709_YCC`用sRGB，`DCI-P3_RGB_D65`和`Default`用P3。`Default`对应“鲜艳”档，见下文。
- 13点重采样和工厂17点LUT的平均色差约0.2，按面板模型两者的准确度相同（sRGB平均色差约1.1）。以前化简成3×3 PCC矩阵时是5.6。
- PCC只做CTM（夜间模式等），排在3D LUT后面，作用在校准后的面板值上。外接显示器不受影响。
- CRTC要设过CTM或gamma LUT才会分到DSPP。Android的HWC每次都带CTM，所以Android上一直有校准；从没设过这两者的DRM客户端（例如fbcon）没有校准。
- 不设`Colorspace`的客户端得到的是P3存储体，即“鲜艳”的效果；以前得到的是sRGB校准。
- `msm.panel_calibration=0`运行时关掉校准，默认打开。
- 两张表缺一张、校验失败或者不是这台机器，校准就不启用，也不挂`Colorspace`属性，HWC退回只报告`NATIVE`。

## sRGB和P3怎么切换

```
应用声明 P3 ─→ SF 选 DISPLAY_P3 ─→ HWC 把模式写成连接器 Colorspace ─→ 内核换 P3 存储体
```

- 这是“标准”档的行为。默认输出sRGB，有P3图层才切，不需要modeset，LUT和那一帧同时生效。
- 输出是P3时，DPU没有逐图层的色域映射，所以不是Display P3的图层都交给GPU（`patches/android/drm-hwcomposer/0002`）。
- 需要`ro.surface_flinger.has_wide_color_display`和`vendor.hwc.drm.force_p3_support`（`device.mk`）。
- Android的广色域是应用自己声明的。Chromium系浏览器默认不用，要在`flags`里把`Force color profile`设成`Display P3 D65`，这是浏览器的策略。

## 鲜艳和标准

设置 › 显示 › 颜色里有两档：

| 档位 | 框架的颜色模式 | SurfaceFlinger | HWC写的`Colorspace` | 3D LUT |
|---|---|---|---|---|
| 鲜艳（默认） | `COLOR_MODE_SATURATED`（2） | 不做颜色管理，输出`NATIVE` | `Default` | P3 |
| 标准 | `COLOR_MODE_NATURAL`（0） | 颜色管理，按内容选`SRGB`或`DISPLAY_P3` | `BT709_YCC`或`DCI-P3_RGB_D65` | sRGB或P3 |

- “鲜艳”把所有内容当成P3显示：sRGB内容铺满面板接近P3的色域，白点和原色按工厂P3校准，和这台机器上Windows的P3模式一样。
- “标准”下sRGB内容是准确的sRGB，P3内容走P3存储体，同样准确。
- `persist.sys.sf.native_mode=1`时，WMS不再向应用报告广色域和HDR（`Configuration.colorMode`，见`WindowManagerService.hasWideColorGamutSupport()`），应用按sRGB渲染。`Display.isHdr()`不受影响。
- 两档列在overlay的`config_availableColorModes`里，设置里的名字在`overlay/packages/apps/Settings`，AOSP原来叫“饱和色”和“自然色”。默认档由`device.mk`的`persist.sys.sf.native_mode=1`决定。ColorDisplayService在开机设置完成时就把当前模式写进`/data`的持久属性，之后以那里为准，所以改`device.mk`里的默认值只影响新安装。
- 不提供“增强色”（在“自然色”上加饱和度矩阵）和“自动调节”（要HWC报告`ENHANCE`渲染意图）。

## 10-bit怎么到面板

SF的帧缓冲写死`RGBA_8888`。有10-bit图层走GPU合成时，HWC用`ClientTargetProperty`请求`RGBA_1010102`，图层没了再请求回来（`patches/android/drm-hwcomposer/0001`）。平面列表要有`ABGR2101010`（`patches/kernel/drm/0007`）。`RGBA_1010102`只有2位alpha，所以client target不在最底层时保持8位。

面板只收8 bit，所以PINGPONG的空间抖动把多出来的精度抖下去（`patches/kernel/drm/0006`）。两路PINGPONG各管半屏。抖动也会处理3D LUT和PCC引入的取整。面板描述必须有`.bpc = 8`，否则`display_info.bpc`是0，抖动不会开。

## HDR提亮

HDR视频的高光要比界面白更亮，只能抬背光，所以界面要同时压暗，看上去亮度不变：

```
HDR 图层上屏 ─→ 框架算出提亮后的亮度 ─→ SF 压暗 SDR 图层 ─→ HWC 同一帧写背光
```

- 背光归HWC（`sepolicy/hal_graphics_composer_default.te`）。灯光HAL还在，但框架不再用它调背光。
- 面板背光寄存器和亮度成线性（`patches/kernel/drm/0008`）。提亮倍数就是寄存器的比值。
- DPU平面不能逐图层调增益，压暗的图层都交给GPU（`patches/android/drm-hwcomposer/0003`）。压暗期间帧缓冲也用10-bit，空间抖动把精度带到面板，暗部不会有色带。
- 倍数表和上限在`etc/displayconfig/display_port_0.xml`。320尼特来自Windows的面板配置，没有实测；提亮和压暗都只用比值，这个数不准只影响上报的尼特值。
- 有这份配置后，框架内部的亮度0..1对应背光的最小值到最大值。亮度条的位置和overlay里的自动亮度曲线都按背光换算，和背光的对应关系不变。
- 面板没有分区背光，提亮时黑位跟着背光一起升高。背光比画面慢几百毫秒，所以进出HDR时能看到亮度过渡。
- 框架只在HDR图层占屏幕10%以上时提亮，省电模式下不提亮。亮度条在最大时没有提亮空间。
- 色调映射保持默认的500尼特目标。调低它中间调会变亮，但高光压缩得更早，天空这类大面积亮部会发白。
- HWC只把背光绑给内置屏。sysfs没有说明背光属于哪个连接器，绑给外接显示器的话，它的亮度会写到内置屏上。
- HWC给内置屏声明HDR10和HLG（`patches/android/drm-hwcomposer/0004`，`vendor.hwc.drm.force_hdr_support`），应用靠`Display.isHdr()`选片源。声明之后SF不再把HDR图层强制交给GPU，平面又做不了色调映射，所以HWC自己转给GPU。
- `v4l2_codec2`声明HDR静态元数据（`patches/android/v4l2-codec2/0003`），框架才会给HEVC列出`Main10 HDR10`。元数据只来自容器，不解析码流里的SEI；系统的MP4提取器不读`mdcv`和`clli`，只有Media3这类自己解析容器的播放器传得进来。HDR10+不支持。
- ToneMapper13的内容峰值写死4000尼特，不读静态元数据，所以元数据不改变画面。
- Ultra HDR要应用自己申请`COLOR_MODE_HDR`。Google相册可以提亮；Glimpse和Edge不申请，按SDR显示。Chromium只在系统已经提亮时才按HDR渲染，单张图片不会触发。

## 排查

```sh
dumpsys display | grep -E 'BacklightAdapter|mHdrBrightnessData|mMode='   # useSurfaceControl=true，播 HDR 时 mMode=HBM_HDR
dumpsys SurfaceFlinger | grep displayBrightnessNits              # 提亮时 displayBrightnessNits 大于 sdrWhitePointNits
dumpsys SurfaceFlinger | grep 'HWC Support'                      # hdr10=true hlg=true
dumpsys media.player | grep Main10HDR10                          # HEVC 解码器列出 HDR10 profile
cat /sys/class/backlight/*/scale                                 # linear
dmesg | grep 'factory calibration'     # 两行，0 = sRGB，1 = P3，后面是 LUT 的白点
dumpsys SurfaceFlinger | grep DisplayColorSetting               # Unmanaged = 鲜艳，Managed = 标准
dumpsys SurfaceFlinger | grep -E 'Device supports wide color|Current color mode'
grep -A8 DSI-1 /sys/kernel/debug/dri/1/state | grep colorspace   # Default（鲜艳）、BT709_YCC 或 DCI-P3_RGB_D65（标准）
grep format= /sys/kernel/debug/dri/1/state                       # AB24 = 8 位，AB30 = 10 位
cmd activity get-config | grep -o widecg                         # 应用看到的广色域
service call SurfaceFlinger 1023 i32 0 i32 7                     # 标准档强制 sRGB 输出，P3 图层交给 GPU 转换
service call SurfaceFlinger 1023 i32 1                           # 临时切到鲜艳；i32 0 i32 0 是标准，设置里的选择不变
```

确认某个改动是否改变了输出，用编码器CRC：`/sys/kernel/debug/dri/1/crtc-0/crc`，`control`写`encoder`，`data`每行以NUL开头。注意不要用`devmem`读DPU寄存器，显示掉电时会让整机崩溃。
