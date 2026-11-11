# 相机子系统架构（CAMSS + IFE硬件ISP + `libcamera` + AIDL HAL）

本文档说明`gaokun3`前后置相机的板级供电拓扑、Qualcomm CAMSS电源域与时钟修复、IFE硬件ISP的内核驱动、`libcamera`的`qcom-ife`流水线与IPA及传感器标定，以及自研AIDL Camera HAL（`device/huawei/gaokun3/camera/`）的设计。

## 一、端到端流水线概览

主线内核`qcom-camss`在Titan 17x上只驱动RDI（Raw Dump Interface），把CSI-2原始拜耳帧原样写入内存，不做任何图像处理。本项目在内核中补上IFE（VFE的像素通路）硬件ISP（`patches/kernel/camera/0009`–`0036`），由`libcamera`的`qcom-ife`流水线与IPA（`patches/libcamera/0014`–`0018`）按帧下发ISP参数、读取统计并运行3A，再由自研AIDL Camera HAL向Android框架提供相机服务：

```
前摄 Hi-846 (2-0020) ──► CSIPHY3 ──┐
                                   ├──► CSID0 ─┬─ IPP ─► VFE0像素通路（IFE）─┬─► FULL输出（NV12）
后摄 OV13B10 (1-0036) ─► CSIPHY0 ──┤           │          ▲ 每帧ISP参数       ├─► 显示输出（NV12，缩放、变焦）
                                   │           │                              └─► 统计（AWB网格、对焦、拜耳直方图）
                                   │           └─ RDI0 ─► VFE0 RDI0 ─► 原始拜耳（相位对焦）
                                   └──► CSID1 ── IPP ─► VFE1像素通路（后摄全尺寸时处理右半幅，双IFE）
                                                   │
                          libcamera qcom-ife流水线 + IPA（AGC/AWB/CCM、镜头阴影、降噪、对焦统计）
                                                   │
                          android.hardware.camera.provider-service.gaokun3（预览、录像、YUV、JPEG）
```

### 1. IFE像素通路（内核）

- **处理模块**：CSID的IPP把CSI-2数据解成像素送入IFE。用到的模块有相位对焦像素修正（PDPC）、坏点修正（BPC）与拜耳降噪（ABF）（这三者处理时数据里仍含黑电平），黑电平扣除（BLS）、镜头阴影（LSC）、去马赛克与白平衡、颜色校正（CC12）、伽马、色彩空间转换（BT.601全范围）、缩放与裁剪，输出NV12。寄存器布局取自SC8280XP的Windows相机栈并经实机核对。
- **输出**：每个全功能VFE有FULL与显示两路图像输出，同一传感器帧可同时出两种尺寸（如全尺寸拍照与预览）；显示输出可按帧设置缩放窗口，用于数码变焦。
- **参数与统计节点**：`msm_vfeN_pix_params`（META_OUTPUT）接收V4L2可扩展ISP参数缓冲，`msm_vfeN_pix_stats`（META_CAPTURE）输出AWB网格、对焦统计（BFStats23）与拜耳直方图（BHistStats14）。格式定义在UAPI头`include/uapi/linux/media/qcom/ife17x-config.h`，`patches/libcamera`中有同名的一份，两者须一致。
- **帧编号与时间戳**：参数在CAMIF epoch 0（第20行）中断时应用到下一帧；图像、参数、统计与RDI原始帧按epoch统一编号，同一请求的各路缓冲始终属于同一传感器帧。像素通路、显示与统计缓冲的时间戳取该帧epoch 0的中断时刻（队列标记`V4L2_BUF_FLAG_TSTAMP_SRC_SOE`），RDI缓冲仍为写完时刻。
- **双IFE**：单个IFE一行最多3840像素，后摄全尺寸（4208×3120）由两个CSID各裁一半送两个IFE，主IFE持有缓冲，从IFE按主IFE的步幅写右半幅；参数同时下发两侧，统计按条带拆分后合入同一统计缓冲。
- **出错恢复**：VFE总线错误或CSID FIFO溢出后，像素通路不会自行恢复，内核以`vb2_queue_error()`使相关队列全部失败，用户态收到`EPOLLERR`/`-EIO`后重启数据流（实测60–70 ms恢复出帧）；CSI-2 CRC/ECC错误只使当帧的FULL与显示缓冲失败。
- **供电与时钟**：CAMSS按各时钟频率投MMCX电压档（屏幕关闭时显示控制器不再抬高MMCX，未投票会导致全尺寸通路溢出甚至整机复位）；`rpmhpd`使MX不低于MMCX，`camcc`为PLL投MX；各CSIPHY使用自己的供电（`patches/kernel/dts/0023`、`0024`）。

### 2. 视频节点

| 节点 | 作用 |
|---|---|
| `msm_vfe0_video3`（`msm_vfe0_pix`） | FULL输出，NV12 |
| `msm_vfe0_video_disp`（`msm_vfe0_pix_disp`） | 显示输出，NV12，链接`msm_vfe0_pix:1 -> msm_vfe0_pix_disp:0`默认关闭 |
| `msm_vfe0_pix_params` / `msm_vfe0_pix_stats` | ISP参数与统计 |
| `msm_vfe0_video0`（`msm_vfe0_rdi0`） | 原始拜耳帧，供HAL做相位对焦 |
| `msm_vfe1_pix` | 双IFE的从通路，链接到`msm_vfe0_video3`，不单独出图 |

## 二、板级硬件拓扑与上电序列

根据BIOS 2.16 `DSDT`及Windows驱动包（`CAMS_RES_QRD.bin` / `CAMF_RES_QRD.bin`）解析并与实机总线扫描核对，前后摄及闪光灯的硬件连接如下：

| 部件 | 前置相机（`CAMF`） | 后置相机（`CAMS`） |
|---|---|---|
| 传感器型号 | Hynix Hi-846（8 MP，3264×2448） | OmniVision OV13B10（13 MP，4208×3120） |
| CCI总线与I2C地址 | `cci1` bus 1，`0x20`（`2-0020`） | `cci0` bus 0，`0x36`（`1-0036`） |
| MIPI CSI-2通路 | `CSIPHY3`，4-lane | `CSIPHY0`，4-lane（560 MHz link freq） |
| 拜耳排列格式 | `SGBRG10_CSI2P` | `SGRBG10_CSI2P` |
| MCLK时钟 | `camcc_mclk3_clk`（24 MHz） | `camcc_mclk4_clk`（19.2 MHz，匹配上游寄存器表） |
| 复位与供电使能GPIO | Reset `GPIO 15`；DVDD 1.2V使能`GPIO 44`（`vreg_camf`） | Reset `GPIO 7`；DVDD使能`GPIO 92`（`vreg_camr`） |
| PMIC供电轨 | `vdda` = `LDO11_C`（2.8V），`vddio` = `LDO2_C`（1.8V） | `avdd` = `LDO2_B`（2.8V），`dovdd` = `LDO2_C`（1.8V） |
| 附加外设 | — | 对焦马达DW9714（`1-000c`，`LDO7_B` 2.8V）、模组EEPROM（`1-0050`，16 KiB） |

### 1. 后摄`LDO2_B`与显示面板共轨机制（`patches/kernel/dts/0005`、`patches/kernel/camera/0003`）

`LDO2_B`同时连接显示面板的`vddi`（1.8V）与后摄OV13B10的模拟供电`avdd`（2.8V）。RPMh稳压器仲裁器对同一条LDO取所有消费者投票的**最大值**：当后摄打开时，`ov13b10`驱动在`power_on()`中调用`regulator_set_voltage(avdd, 2800000, 2800000)`将`LDO2_B`抬升至2.8V；后摄关闭时在`power_off()`中放宽回`1.8V..2.8V`区间，稳压器自动回落至面板所需的1.8V。这与Windows PEP电源表的设计一致。

### 2. 后置闪光灯（`patches/kernel/dts/0006`）

机背闪光灯由PM8350C内置闪光模块驱动（而非GPIO 93直驱），物理LED并联在**通道1与通道4**（`led-sources = <1>, <4>`），在sysfs下注册为`/sys/class/leds/white:flash`（手电筒模式上限200 mA，闪光strobe上限600 mA / 400 ms）。为避免`v4l2-async`因等待额外subdev阻塞，设备树故意不在传感器节点引用`flash-leds`，由Camera HAL直接读写`/sys/class/leds/white:flash/brightness`。

### 3. 后摄模组EEPROM（`1-0050`）

后摄模组搭载16 KiB EEPROM（挂载在CCI0总线`0x50`，与面板共用1.8V轨，亮屏即应答）。因高通CCI适配器不支持`I2C_RDWR`复合消息，读取时需使用两步SMBus操作（`i2cset`写16位大端地址指针，再连续调用`i2cget`逐字节读取）。原始转储位于[ov13b10-module-eeprom-0x50.bin](hw/ov13b10-module-eeprom-0x50.bin)。

## 三、CAMSS电源域（`titan_top_gdsc`）死锁修复

### 1. 故障现象与触发条件

在未打补丁的主线v7.2内核上，开机后第一次打开相机流（`VIDIOC_STREAMON`）可以正常出帧，但一旦关闭相机流（`VIDIOC_STREAMOFF`）并等待`titan_top_gdsc`完成运行期自动下电（runtime suspend），**第二次打开相机必定失败**：

```
titan_top_gdsc status stuck at 'off'
WARNING: drivers/clk/qcom/gdsc.c:185 at gdsc_toggle_logic
qcom-camss ac5a000.camss: Failed to power up pipeline: -110
```

且一旦超时，`CFG_GDSCR`状态机卡死在`0x010e0000`，软件侧无论重试上电还是解绑驱动均无法恢复（在已锁死状态下unbind `qcom-camss`还会因等待runtime PM而拖死整机）。

### 2. 产生机制与三行修复（`patches/kernel/camera/0002`）

通过寄存器轨迹转储与二分实验证实：

1. 开机初始状态下，`camcc_camnoc_axi_clk_src`挂在常开的19.2 MHz晶振源`bi_tcxo`上，此时无论对`titan_top_gdsc`执行多少次纯掉电/上电循环，均100%成功（`after-OFF CFG_GDSCR = 0x00088000`）。
2. 当相机执行一次`STREAMON`时，`vfe_get()`调用`camss_enable_clocks()`将`camnoc_axi`的时钟频率设为150 MHz，其父时钟源切换至`camcc_pll0_out_even`。
3. 当`STREAMOFF`关闭分支时钟后，`camcc_pll0`因引用计数归零而熄灭。然而`drivers/clk/qcom/camcc-sc8280xp.c`为`camnoc_axi_clk_src`、`slow_ahb_clk_src`（`cpas_ahb`的源）和`fast_ahb_clk_src`配置的是普通`clk_rcg2_ops`而非`clk_rcg2_shared_ops`。普通ops没有`disable`回调，导致RCG的CFG寄存器在关钟后**仍然指向已经停振的`camcc_pll0_out_even`**。
4. 随后`titan_top_gdsc`下电与再上电时，硬件GDSC控制器需要与CAMNOC执行总线静默/隔离握手，但CAMNOC此时没有任何有效时钟输入，握手信号永远无法完成（`after-OFF CFG_GDSCR`变为`0x00008000`，再次上电时卡死在`0x010e0000`）。

**修复方案**：

- **`patches/kernel/camera/0002`**：参照`camcc-x1e80100.c`与`camcc-sc8180x.c`，将`camnoc_axi_clk_src`、`slow_ahb_clk_src`、`fast_ahb_clk_src`三个RCG的`.ops`改为`&clk_rcg2_shared_ops`。关闭时钟时`clk_rcg2_shared_disable()`自动将RCG停靠回安全晶振源`bi_tcxo`，使`titan_top_gdsc`在任意次数启停流后均可正常塌缩与唤醒。
- **配套补丁**：
  - `patches/kernel/camera/0001`：为`camcc-sc8280xp`的8个GDSC显式填入硬件复位等待周期（`en_rest_wait_val = 2, en_few_wait_val = 2, clk_dis_wait_val = 0xf`），避免`gdsc_init()`用MSM8974的历史默认值`2/8/2`覆盖硬件配置。
  - `patches/kernel/camera/0004`：为`camss`的`v4l2-async`通知器增加20秒超时降级，若某台机器的后摄模组未应答，仅跳过未绑定的endpoint，保证前摄仍能正常注册`/dev/v4l-subdev*`。

## 四、`libcamera`移植与传感器标定

### 1. Soong同树编译与ABI约束

Android NDK的`libc++`使用内联命名空间`std::__ndk1`，而AOSP平台源码编译的HAL使用`std::__1`。若用NDK/meson预编译`libcamera.so`，其导出的所有带`std::span`、`std::shared_ptr`、`std::function`签名的C++符号均无法与AOSP平台编译的Camera HAL链接。

因此`libcamera`必须通过`patches/libcamera/libcamera-Android.bp`放进AOSP树内由Soong统一编译：

- **编译产物**：`libcamera_base_gk3.so`、`libcamera_gk3.so`（只含`qcom-ife`流水线）以及IPA模块`libcamera_ipa_qcom_ife_gk3.so`。软件ISP（`simple`流水线与`softisp` IPA）不再编译。
- **IPA安装路径**：`config.h`将`IPA_MODULE_DIR`定为`/vendor/lib64/libcamera/ipa`，因此`libcamera_ipa_qcom_ife_gk3`必须设置`relative_install_path: "libcamera/ipa"`（缺失该模块时流水线无法创建IPA，摄像头不会注册）。
- **免签名进程内IPA加载（`patches/libcamera/0003`）**：编译宏`LIBCAMERA_IPA_TRUST_UNSIGNED`使`IPAManager`在没有`.sign`签名的情况下将IPA直接加载到HAL进程内线程运行。
- **生成文件**：`scripts/camera/setup-libcamera-tree.sh`铺上游源码、打补丁、放置`Android.bp`，并调用`scripts/camera/gen-libcamera-sources.sh`由meson产出`control_ids.cpp`、`qcom-ife_ipa_proxy.cpp`等生成文件，详见[`patches/libcamera/README.md`](../../patches/libcamera/README.md)。

### 2. 传感器增益与黑电平标定（`patches/libcamera/0002`、`0004`）

内核传感器驱动（`hi846.c`、`ov13b10.c`）仅将`V4L2_CID_ANALOGUE_GAIN`原样写入传感器寄存器，不包含物理增益换算公式；若`libcamera`的`CameraSensorHelper`未收录该传感器，AGC会将`analogue-gain`锁死在`0`，导致画面接近全黑。曝光、模拟增益与`VBLANK`的生效延迟（前后摄均为2帧，`patches/libcamera/0014`）经IFE统计逐帧实测。

| 传感器 | 黑电平（16位归一化） | 模拟增益模型 | 说明 |
|---|---|---|---|
| `hi846` | `blackLevel_ = 4096`（10位下为64，位于模拟增益之后的数字偏置） | `AnalogueGainLinear{1, 16, 0, 16}`，即$\text{gain} = 1 + \frac{\text{code}}{16}$ | 经固定曝光留出法实测标定（`code=0..240`对应`1.0×..16.0×`，留出验证误差<2.3%） |
| `ov13b10` | `blackLevel_ = 4096`（10位下为64） | $\text{gain} = \frac{\text{code}}{128}$，按传感器实际保留的档位取整：1×–2×步长1/16，2×–4×为1/8，4×–8×为1/4，8×–15.5×为1/2（`patches/libcamera/0005`） | 档位经实机增益扫描测得，避免AGC在相邻档位间来回跳变 |

### 3. `qcom-ife`流水线（`patches/libcamera/0018`）

- **传感器模式与双IFE**：前摄最大3264×2448，单IFE；后摄全尺寸4208×3120走双IFE，流水线按镜头阴影网格允许的起点、并使显示输出的接缝尽量落在整列上来选分割列；2104×1560等合并模式走单IFE。
- **数据流**：
  - 一路处理流来自FULL输出；配两路时（取景与拍照），较大的一路来自FULL，另一路来自显示输出，两路视场相同。
  - RAW流取自RDI，固定为最大模式，供HAL做相位对焦。
  - 请求可以不带FULL或RAW缓冲：每个参数缓冲写明该帧要写的输出，同一请求的所有缓冲取参数缓冲的帧号。
  - `ScalerCrop`按帧作用于显示输出，元数据回报实际显示的区域。
  - 内核报队列出错（`patches/libcamera/0015`）后，流水线重启数据流并重发IPA的完整参数。

### 4. IPA与调校文件

- **算法**（每帧基于统计运行）：
  - AGC：以`AfWindows`为测光窗口在AWB网格上计亮度均值，亮度约束基于同一窗口的拜耳直方图。`AeFlickerMode`设为手动时曝光按闪烁周期取整（周期默认10 ms，对应50 Hz市电；默认关闭，`patches/libcamera/0016`）。传感器拿到的是由最新统计算出的曝光，与在途请求数无关。
  - AWB：`libipa`的灰度世界或贝叶斯算法（由调校文件选择），CCM按色温插值。
  - 黑电平、伽马、镜头阴影、坏点与PD像素修正、ABF降噪表（按增益）。
  - 对焦：在`AfWindows`区域内由对焦统计算出`FocusFoM`，对焦算法在HAL中。
  - 帧长由`FrameDurationLimits`设定。
- **调校文件**：`device/huawei/gaokun3/camera/ipa-data/qcom-ife/{hi846,ov13b10,uncalibrated}.yaml`，安装到`/vendor/etc/libcamera/ipa/qcom-ife/`，与`0018`中`src/ipa/qcom-ife/data/`的同名文件一致。数据来自华为Windows相机驱动的模组调校与实机标定；`Awb`与`Ccm`两段由`scripts/camera/gen-*-ccm.py`生成（见[`scripts/camera/README.md`](../../scripts/camera/README.md)）。缺少调校文件时IPA初始化失败，该摄像头不会注册。

### 5. 已知限制（内核与`libcamera`）

- **AE响应**：统计在帧末才可用，曝光与增益又要2帧后生效，亮度突变后约3帧画面才开始变化。
- **防闪烁**：只有手动闪烁周期（默认对应50 Hz市电），未实现自动检测。
- **双IFE几何**：显示输出在两个条带接缝处有约0.1个显示像素的残差；变焦窗口的几何误差最多约1个显示像素。
- **拜耳直方图**：2104宽的整幅窗口实测只统计2096列，对AE约束的影响可忽略。
- **快速重复上电**：VFE在毫秒级的断电再上电后偶发复位超时，重试也无法消除；`libcamera`正常的启停不会触发。
- **只按`libcamera`的用法验证**：`libcamera`总是一起启停所有节点。单独重启某一个节点（例如显示节点继续推流时只重启FULL，或只重启共用同一VFE的RDI）可能使数据流停住而不报错；双IFE时VFE1上的RDI、关闭AWB网格的统计都不受支持。
- **CSI-2错误的标记**：CRC/ECC错误落在某路输出不写的帧上时，该输出写的下一帧会被标为出错。

## 五、自研AIDL Camera HAL（`device/huawei/gaokun3/camera/`）

HAL实现`android.hardware.camera.provider-V3`，组件说明详见[`device/huawei/gaokun3/camera/README.md`](../../device/huawei/gaokun3/camera/README.md)。核心实现要点如下：

1. **枚举顺序固定**：`Provider.cpp`按`Location`排序（后摄`BACK`固定为`device@1.1/internal/0`，前摄`FRONT`固定为`internal/1`），避免因内核media节点探测先后波动导致相机ID翻转。
2. **画面与格式**：`qcom-ife`流水线输出全范围BT.601的NV12。HAL按各路流的尺寸向流水线要一幅与传感器阵列同宽高比、刚好覆盖它们的画面，传感器模式由流水线选，IFE缩放；YUV流由CPU裁剪、缩放后直接写进Gralloc缓冲（半平面布局按行复制，平面布局写J420），不做颜色转换；JPEG由J420以YCbCr原始数据交给`libjpeg-turbo`编码（末尾附8字节`CameraBlob`尾标），并写入EXIF与缩略图。
3. **拍照模式两路输出**：JPEG比其余各路大得多时，向流水线要显示路径（`Viewfinder`）与FULL（`StillCapture`）两路NV12，需要相位对焦时再加`Raw`。显示路径由IFE缩放器缩小，给预览、录像与对焦；FULL只加在带JPEG的请求里，JPEG在单独的线程上从FULL编码，预览与结果元数据照常先交付。两路时数码变焦交给显示路径的`ScalerCrop`，HAL按元数据里实际显示的矩形换算裁剪区域与对焦区域。
4. **依赖的流水线行为**（流水线一侧的说明见第四节第3部分）：
   - 两路时显示路径的尺寸调成FULL视场的宽高比（宽度16对齐），只缩小不放大；请求可以不带FULL与RAW；同一请求的各路缓冲来自同一帧，请求按入队顺序完成。
   - `ScalerCrop`按请求下发（坐标相对`ScalerCropMaximum`），只作用于显示路径，元数据报实际显示的矩形。
   - `FocusFoM`每帧输出，`AfWindows`按请求下发；RAW取最大的传感器模式（只有它带相位像素），读出变换为HVFlip时HAL才做相位对焦。
   - 同一帧的图像缓冲都打该帧开始（SOE）的时刻，不随变焦与是否带FULL变化；HAL以显示路径（单路时就是那一路）的时间戳作快门与`SENSOR_TIMESTAMP`，不用RAW的时间戳。
   - 帧状态不是`FrameSuccess`时HAL把整个请求按出错上报；写主出错后由流水线重启流并取消在途请求。`Camera::stop()`返回前所有完成回调都已返回，HAL只在此之后解除帧缓冲的映射。
   - 帧缓冲是可`mmap()`的dma-buf，CPU读之前做`DMA_BUF_IOCTL_SYNC`。
5. **对焦**：后摄`dw9714`马达由HAL经v4l2子设备直接驱动（`libcamera`不驱动镜头）。全尺寸模式下先按RAW里的相位像素对焦，再在相位结果两侧各采一个对比度点精调；对比度分数在增益不高时取AF区域亮度的梯度能量，暗处（增益≥10）取`FocusFoM`。
6. **3A控制**：曝光补偿、AE/AWB锁定、手动曝光（`MANUAL_SENSOR`）、手动对焦、白平衡预设与手动白平衡、防频闪（50 Hz/60 Hz，AUTO按50 Hz）都换成`libipa`的对应控制；能力按流水线实际接受的控制声明。帧率按探测到的传感器模式声明（后摄合并模式60 fps）。
7. **画质**：IFE不做锐化，降噪只有拜耳域的ABF（见第一节）。HAL在输出分辨率上做带门限的亮度锐化（`EDGE_MODE`），照片另做亮度ε滤波（门限按噪声估计）与固定强度的色度模糊；预览、录像在增益高时只模糊色度。
8. **AIDL缓冲缓存契约与Gralloc导入**：
   - 遵循`StreamBuffer.aidl`规范：框架对同一`(streamId, bufferId)`仅在首次请求时携带非空`buffer`句柄，后续请求句柄为空。`Session.cpp`维护`(streamId, bufferId) -> buffer_handle_t`映射表，并在`processCaptureRequest`中按`cachesToRemove`及时释放已淘汰缓冲。
   - 使用`GraphicBufferMapper::importBufferNoValidate()`导入缓冲，避免因框架内部附加的私有`usage`位与HAL侧声明不一致而触发`BAD_BUFFER(2)`错误。
9. **FMQ请求设置读取**：`AidlCamera3Device`优先通过Fast Message Queue（`requestQueue_`）传递每帧capture settings（此时`r.settings.metadata`为空，`fmqSettingsSize > 0`），HAL同时支持从FMQ与内联metadata读取请求参数。
10. **手电筒与预闪拍照状态机**：
    - `ICameraDevice::setTorchMode`直接控制`/sys/class/leds/white:flash/brightness`，并在相机打开会话期间自动互斥并回调`TorchModeStatus::NOT_AVAILABLE`。
    - 因PMIC LED无硬件帧同步信号，拍照闪光采用预闪常亮策略：收到`AE_PRECAPTURE_TRIGGER_START`时点亮LED并上报`AE_STATE_PRECAPTURE`让AGC收敛，通过`firedPending_`计数在途拍照请求，直至静态JPEG帧曝光完成才熄灭LED。
11. **传感器朝向（`Device.cpp`）**：
    - 设备树按横屏使用的方向写安装角：后摄180、前摄0。Android把`1600×2560`的面板视为天生竖屏，接键盘横放时显示旋转为`ROTATION_90`。
    - `ANDROID_SENSOR_ORIENTATION`相对竖屏计算，并扣掉`libcamera`已用翻转消掉的部分（后摄ov13b10的180°由水平加垂直翻转读出消掉）：后摄为90，前摄为270。接键盘横放时应用算出的JPEG角为0，交付的画面横放时本来就是正的。
    - JPEG按应用下发的`ANDROID_JPEG_ORIENTATION`转像素；转过时EXIF方向写0（未定义），0°时写1。
