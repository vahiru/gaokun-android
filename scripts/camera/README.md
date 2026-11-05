# 相机V4L2 / Media Controller诊断与ISP标定工具

本目录包含不依赖`media-ctl`/`v4l2-ctl`的最小化V4L2/Media Controller诊断程序（直接使用内核UAPI头文件构建），以及从华为Windows驱动调校包提取SoftISP色彩矫正矩阵（CCM）与自动白平衡（AWB）先验数据的生成脚本。相机子系统完整架构详见[`../../docs/dev/camera.md`](../../docs/dev/camera.md)与[`../../device/huawei/gaokun3/camera/README.md`](../../device/huawei/gaokun3/camera/README.md)。

| 工具 | 用途 |
|---|---|
| `mediatopo` | 转储`/dev/media0`的entity、pad、link拓扑及启用状态（等效于`media-ctl -p`） |
| `camtest` | 建立media link、沿管道配置格式并采集RAW帧（依次测试默认曝光、满增益、传感器测试彩条与分辨率图案） |
| `gen-ov13b10-ccm.py` | 从Windows驱动调校文件`com.qti.tuned.ofilm_ov13b10.bin`生成后摄`ov13b10.yaml` |
| `gen-hi846-ccm.py` | 结合Windows驱动调校文件`com.qti.tuned.hlt_hi846.bin`与实测白点生成前摄`hi846.yaml` |

编译C诊断工具：`bash scripts/camera/build.sh <内核源码树路径>`（生成静态链接二进制，`adb push`至`/data/local/tmp/`以root运行）。

---

## 一、CAMSS硬件与接口注意事项

1. **`titan_top_gdsc`电源域复位时序**：
   - 未打[`../../patches/0027-clk-qcom-camcc-sc8280xp-gdsc-wait-vals.patch`](../../patches/0027-clk-qcom-camcc-sc8280xp-gdsc-wait-vals.patch)与[`../../patches/0031-clk-qcom-camcc-sc8280xp-mark-camnoc-ahb-rcgs-shared.patch`](../../patches/0031-clk-qcom-camcc-sc8280xp-mark-camnoc-ahb-rcgs-shared.patch)前，一旦`titan_top_gdsc`在下电后真正塌缩，再次上电会报错`titan_top_gdsc status stuck at 'off'`（`STREAMON`返回`-110`），并将genpd锁死在`runtime_error`状态。
   - **严禁在`camcc`处于`runtime-suspend`时通过`devmem`读取`camcc`寄存器**，否则会因寄存器块时钟门控触发同步外部中止（external abort）导致整机死锁。若必须读取寄存器，需先执行`echo on > /sys/devices/platform/soc@0/ad00000.clock-controller/power/control`固定供电状态。
2. **像素格式与Media Bus码匹配**：RDI（Raw Dump Interface）直接转储CSI-2裸数据而不做格式转换，`video`节点的像素格式必须与传感器输出的media bus码同族（映射表见内核`drivers/media/platform/qcom/camss/camss-vfe.c`及`camtest.c`中的`kBusToPix`），否则`STREAMON`返回`-EPIPE`。
3. **Multiplanar API约束**：`camss`驱动仅实现多平面接口（`V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE`）；若使用单平面`V4L2_BUF_TYPE_VIDEO_CAPTURE`调用，`ENUM_FMT`会返回空且`S_FMT`返回`-EINVAL`。

---

## 二、测试彩条判据（以`hi846`为例）

```text
hi846 2-0020 :0 -> msm_csiphy3 :0    [DT固定连接]
msm_csiphy3  :1 -> msm_csid0   :0    <- camtest启用
msm_csid0    :1 -> msm_vfe0_rdi0 :0  <- camtest启用
msm_vfe0_rdi0:1 -> msm_vfe0_video0   [固定连接] => /dev/videoN
```

`camtest`第3轮开启传感器彩条模式（Test Pattern 2）后，从原始Bayer帧解出的色带序列应严格为**黄、青、绿、品、红、蓝**（R/G/B通道满量程`1023`或`0`，相邻行像素差为`0`）。

---

## 三、SoftISP标定配置生成

### 后摄（`ov13b10`）：`gen-ov13b10-ccm.py`

从华为Windows相机驱动包（200.0.10.0）`qccamrearsensor_extension8280.cab`内的`com.qti.tuned.ofilm_ov13b10.bin`提取欧菲光`ov13b10`模组调校参数：

- **CCM**：提取无闪光灯常亮档（lux index `1..180`）随色温变化的3×3矩阵（每行归一化之和为1）。
- **AWB**：提取标准光源（H、A、TL84、D50、D65、D75）下的白点增益曲线供`libcamera`贝叶斯AWB使用。

```bash
python3 scripts/camera/gen-ov13b10-ccm.py com.qti.tuned.ofilm_ov13b10.bin \
    > device/huawei/gaokun3/camera/ipa-data/softisp/ov13b10.yaml
```

### 前摄（`hi846` / `ov5675`）：`gen-hi846-ccm.py`

从`qccamfrontsensor_extension8280.cab`内的`com.qti.tuned.hlt_hi846.bin`提取CCM，并结合同光源下前后摄灰卡/白纸实测RAW增益比值将后摄色温曲线映射为前摄AWB曲线：

```bash
python3 scripts/camera/gen-hi846-ccm.py com.qti.tuned.hlt_hi846.bin \
    > device/huawei/gaokun3/camera/ipa-data/softisp/hi846.yaml
```
