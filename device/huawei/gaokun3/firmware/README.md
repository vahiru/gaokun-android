# 专有固件准备说明（`device/huawei/gaokun3/firmware/`）

`.gitignore`忽略本目录下的专有固件二进制文件，仅保留本说明文档。在构建ROM前，请按以下任一方式将固件放入本目录。硬件固件路径总表见[`docs/dev/hardware.md`](../../../../docs/dev/hardware.md)。

## 一、从已安装Android或Linux的本机提取

若设备上已运行本项目的Android系统，可直接从`/vendor/firmware/`拉取：

```bash
adb pull /vendor/firmware /tmp/fw
cp -r /tmp/fw/firmware/* device/huawei/gaokun3/firmware/
```

注：拉取结果中`qcom/sc8280xp/SC8280XP-HUAWEI-GAOKUN3-tplg.bin`与`qcom/sc8280xp/HUAWEI/gaokun3/audioreach-tplg.bin`内容相同，`device.mk`会从后者自动安装至两个目标路径，可删除多余副本。

## 二、固件清单与来源

| 相对路径 | 用途 | 来源 |
|---|---|---|
| `qcom/a660_sqe.fw`<br>`qcom/a660_gmu.bin` | Adreno 690 GPU微码 | `linux-firmware` ≥ 20241210 |
| `qcom/sc8280xp/HUAWEI/gaokun3/qcdxkmsuc8280.mbn` | GPU受保护上下文切换（Zap Shader） | 华为Windows驱动包 |
| `qca/wcnhpbtfw21.tlv`<br>`qca/wcnhpnv21g.bin` | WCN6855蓝牙固件 | `linux-firmware` ≥ 20241210 |
| `ath11k/WCN6855/hw2.0/{amss,board-2,m3,regdb}.bin` | WCN6855 Wi-Fi固件 | `linux-firmware` ≥ 20241210 |
| `qcom/sc8280xp/HUAWEI/gaokun3/qcadsp8280.mbn`<br>`qccdsp8280.mbn`<br>`qcslpi8280.mbn` | ADSP / CDSP / SLPI远端处理器固件 | 华为Windows驱动包 |
| `qcom/sc8280xp/HUAWEI/gaokun3/audioreach-tplg.bin` | Audioreach声卡拓扑（构建前需运行`micarray/tplg-four-channel-capture.py`开启四声道采集上限，详见[`../micarray/README.md`](../micarray/README.md)） | `audioreach-topology` / 驱动包 |
| `qcom/sc8280xp/HUAWEI/gaokun3/qcvss8280.mbn` | Iris / Venus硬件视频解码固件 | 华为Windows驱动包 |
| `qcom/sc8280xp/HUAWEI/gaokun3/{adspr,adspua,battmgr,cdspr}.jsn` | `pd-mapper`服务映射表 | 华为Windows驱动包 |

## 三、从公开Windows驱动归档提取（无需保留Windows分区）

可从`matebook-e-go/uup-drivers-sc8280xp`发布包提取所有板级专有固件：

```bash
curl -LO https://github.com/matebook-e-go/uup-drivers-sc8280xp/releases/download/200.0.10.0/200.0.10.0.zip
unzip -o 200.0.10.0.zip 'qc*.cab' 'Qc*.cab'
# 使用cabextract（Linux）或expand.exe（Windows）解包对应.cab文件
```

| 所需文件 | 所在cab包 |
|---|---|
| `qcslpi8280.mbn`、`RSCS.bin` | `qcsubsys_ext_scss8280.cab` |
| `qcadsp8280.mbn`、`RADS.bin` | `qcsubsys_ext_adsp8280.cab` |
| `qccdsp8280.mbn`、`RCDS.bin` | `qcsubsys_ext_cdsp8280.cab` |
| 传感器配置JSON、`sns_reg_config`、`socinfo` | `qcSensorsConfigQrd8280.cab` |
| 指纹可信应用`fingerpr.mbn`（详见[`docs/dev/fingerprint-driver-design.md`](../../../../docs/dev/fingerprint-driver-design.md)） | `QcTreeExtOem8280.cab` |

## 四、指纹TA与Goodix配置模板

只在`GAOKUN3_FINGERPRINT=true`时打包，两者都是专有二进制，不入库：

- `fingerpr.mbn`：指纹可信应用（TA名`fingerprint`，单一secelf，3669575字节，sha256前缀`b081543c7b6ae4`），与上表`QcTreeExtOem8280.cab`中的相同。fpcmd驱动（补丁0212）通过`kernel_read_file_from_path(..., READING_FIRMWARE)`读取`/vendor/firmware/fingerpr.mbn`。
- `gaokun3-goodix-cfg.bin`：装Goodix GDIX5125的机器初始化时下发的传感器配置（类型14，224字节，sha256前缀`273537378e759cc1`），取自同一发布包`gfspi.cab`中的`gfspi.dll`。驱动（补丁0213）从`/vendor/firmware/gaokun3-goodix-cfg.bin`读取，只接受这一份，再按每台机器的OTP修补。

`python3 scripts/fingerprint/prepare-crdroid.py`核对交接包和哈希后把TA放到这里，加上`--gfspi <gfspi.dll>`时同时导出Goodix模板；完整步骤见[指纹HAL说明](../fingerprint/README.md)。
