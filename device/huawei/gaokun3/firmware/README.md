# 固件获取（blob 不进版本库）

`.gitignore` 把本目录**整个**忽略掉，只放行这份 README。
换机器 / 重建工作区时按下面任一条恢复。

## 最快的恢复办法：从一台还能开机的本机 Android 上 adb pull

`/vendor/firmware/` 里就是构建时装进去的全套（2026-08-19 实测 23 个文件）：

```bash
adb pull /vendor/firmware /tmp/fw
cp -r /tmp/fw/firmware/* device/huawei/gaokun3/firmware/
```

⚠️ 拉回来会多一个 `qcom/sc8280xp/SC8280XP-HUAWEI-GAOKUN3-tplg.bin` ——
它和 `HUAWEI/gaokun3/audioreach-tplg.bin` 内容完全相同（同 sha256，24296 字节），
`device.mk` 用同一个源文件装两个路径，所以**这份多出来的可以删掉**。

若设备已经刷成别的 ROM，就从跑主线 Linux 的本机 `/lib/firmware/` 里取（下表"来源"列）。

## 清单

| 文件 | 用途 | 缺了会怎样 | 来源 |
|---|---|---|---|
| `qcom/a660_sqe.fw`<br>`qcom/a660_gmu.bin` | GPU (Adreno 690) 微码 | GPU 不 probe | linux-firmware ≥ 20241210 |
| `qcom/sc8280xp/HUAWEI/gaokun3/qcdxkmsuc8280.mbn` | GPU **zap shader** | GPU 锁在安全模式，`adreno_zap_shader_load` 报错 | **华为专有** |
| `qca/wcnhpbtfw21.tlv`<br>`qca/wcnhpnv21g.bin` | 蓝牙 WCN6855 | 蓝牙不起 | linux-firmware ≥ 20241210 |
| `ath11k/WCN6855/hw2.0/{amss,board-2,m3,regdb}.bin` | WiFi | 无 WiFi | linux-firmware ≥ 20241210（上游把 hw2.1 软链到 hw2.0；`device.mk` 把同一份装到两个路径） |
| `qcom/sc8280xp/HUAWEI/gaokun3/qcadsp8280.mbn`<br>`qccdsp8280.mbn`<br>`qcslpi8280.mbn` | ADSP / CDSP / SLPI | 音频、传感器全无 | **华为专有** |
| `qcom/sc8280xp/HUAWEI/gaokun3/audioreach-tplg.bin` | 音频**拓扑** | **声卡不注册** | **华为专有** |
| `qcom/sc8280xp/HUAWEI/gaokun3/qcvss8280.mbn` | 语音服务 | （未用到，一并带上） | **华为专有** |
| `qcom/sc8280xp/HUAWEI/gaokun3/{adspr,adspua,battmgr,cdspr}.jsn` | pd_mapper 服务表 | remoteproc 域映射缺失 | **华为专有** |

华为专有那几个不在 linux-firmware 里，从跑主线 Linux 的本机
`/lib/firmware/qcom/sc8280xp/HUAWEI/gaokun3/` 直接拷，
或从 Windows 驱动包（`uup-drivers-sc8280xp`）提取。**不可公开再分发。**

## ★ 已验证的第三种来源：uup-drivers-sc8280xp 的 release（不需要 Windows 分区）

2026-08-20 实测通过。**这条路不需要机器上还留着 Windows**，全部来自公开源
（该项目用 forked UUPMediaCreator 从 Windows Update 抓驱动）：

```bash
# 185 MB，最新 tag
curl -LO https://github.com/matebook-e-go/uup-drivers-sc8280xp/releases/download/200.0.10.0/200.0.10.0.zip
unzip -o 200.0.10.0.zip 'qc*.cab'
# .cab 用 cabextract（Linux）或 Windows 自带 expand.exe -F:* x.cab 目标目录
```

| 需要的东西 | 在哪个 cab |
|---|---|
| `qcslpi8280.mbn`、**`RSCS.bin`**（SLPI 伴生固件） | `qcsubsys_ext_scss8280.cab` |
| `qcadsp8280.mbn`、`RADS.bin` | `qcsubsys_ext_adsp8280.cab` |
| `qccdsp8280.mbn`、`RCDS.bin` | `qcsubsys_ext_cdsp8280.cab` |
| 传感器全套 JSON、**`sns_reg_config`（407 B 文本格式）**、socinfo 原件 | `qcSensorsConfigQrd8280.cab` |

★ **SCSS = Sensor Core SubSystem** —— 这就是为什么 SLPI 的东西在 `scss` 包里。
命名自成体系：ADSP→`RADS.bin`、CDSP→`RCDS.bin`、SLPI→`RSCS.bin`。

★ **交叉校验（做过，值得一直做）**：cab 里的 `qcslpi8280.mbn` 与本仓在用的那份
**sha256 逐字节相同**（`9c1ce6f5…`）—— 证明这个来源与本机固件同出一脉，
不是随便找来的另一个版本。

⚠️ 两个会把人绕进去的坑：
* **`.cab` 解包失败时会静默产出 0 个文件**。我一度把"0 个文件"当成"包里没有
  这个东西"，其实是传给 `expand.exe` 的路径末尾带了 `\r`
  （Python 用 `open(...,'w')` 写清单时把 `\n` 翻成了 `\r\n`）。
  **判据要看解出的文件数，而不是 find 的结果为空。**
* NTFS 上直接看 DriverStore 时，`8280_qrd_*.json` 是**重解析点**
  （ntfs-3g 显示为 34 字节的 symlink，读会 FileNotFoundError）；
  cab 里是真文件（如 `8280_qrd_sh3001_0.json` 6189 B）。

## 放置规则

保持本目录下相同的子路径结构（cmdline 里 `firmware_class.path=/vendor/firmware/`）。
实测加载路径见 `docs/hw-inventory.md` 第 2 节。

★ **拓扑固件的名字有坑**：内核请求的是
`qcom/<card->driver_name>/<card->name>-tplg.bin`
（`sound/soc/qcom/qdsp6/topology.c:1320`），本机 =
`qcom/sc8280xp/SC8280XP-HUAWEI-GAOKUN3-tplg.bin`。
老规矩的 `HUAWEI/gaokun3/audioreach-tplg.bin` **内核从不去读**。
`device.mk` 两个路径都装。详见 `docs/stage4-findings.md` #33。

## 指纹 TA（fingerpr.mbn）

指纹的可信应用 `fingerpr.mbn`（TA 名 `fingerprint`，单一 secelf，3669575 B，
sha256 `b081543c7b6ae4…`）来自同一个 uup-drivers release 的 `QcTreeExtOem8280.cab`：
```
unzip -o 200.0.10.0.zip 'QcTreeExtOem8280.cab'
cabextract QcTreeExtOem8280.cab   # 取 fingerpr.mbn
```
它是华为/高通签名的专有二进制，**不入库**（与本目录其它 .mbn 同待遇）。
* 加载路线见 `docs/stage4-findings.md` #120 / #123 / #124、`docs/fingerprint/`（不入库）。
* 冒烟测试阶段（`tools/fingerprint-bringup/`）模块直接读 `/data/local/tmp/fingerpr.mbn`。
* 2026-11-01 crDroid 候选的固定 ioctl 驱动（0113）通过
  `kernel_read_file_from_path(..., READING_FIRMWARE)` 读
  `/vendor/firmware/fingerpr.mbn`。仅 `GAOKUN3_FINGERPRINT=true` 时打包。
  `python3 scripts/fingerprint/prepare-crdroid.py` 可离线核验交接包并准备本地输入；
  完整步骤和当前限制见 [指纹候选说明](../fingerprint/README.md)。
