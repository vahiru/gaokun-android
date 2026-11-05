# 麦克风阵列校准系数预置目录（`micarray/prebuilt/`）

本目录仅将本说明文件纳入Git版本控制。内置双麦克风阵列波束成形所需的校准系数文件需放置于本目录下：

```text
hdrec_bf_data.bin
```

`device.mk`通过`$(wildcard ...)`条件检测该文件：
- **存在时**：安装至`/vendor/etc/micarray/hdrec_bf_data.bin`。
- **缺失时**：不影响镜像编译；运行期`libgaokunmicarray`因无法打开系数文件而在日志中记录`weights: /vendor/etc/micarray/hdrec_bf_data.bin: cannot open: ... -- array off`，并自动旁路阵列处理，保持原始双声道采集行为。
- **正式发布校验**：[`scripts/sync-device-tree.sh`](../../../../../scripts/sync-device-tree.sh)会将本目录同步至构建机并核对`hdrec_bf_data.bin`的SHA-256校验值；若文件缺失或哈希不匹配则报错终止（如需在无系数文件下构建，可显式设置`GK3_ALLOW_NO_MICARRAY=1`）。

## 一、不纳入版本控制的原因

`hdrec_bf_data.bin`提取自华为官方Windows录音音效驱动（`HWVEAPO`），属厂商专有标定数据，未获公开再分发授权。与[`../../firmware/`](../../firmware/README.md)及[`../../effects/prebuilt/`](../../effects/prebuilt/README.md)一致，本目录通过`.gitignore`忽略专有二进制文件，仅保留说明文档。

## 二、提取方法与校验信息

可从与[`../../firmware/README.md`](../../firmware/README.md)相同的公开驱动归档中提取：

```bash
curl -LO https://github.com/matebook-e-go/uup-drivers-sc8280xp/releases/download/200.0.10.0/200.0.10.0.zip
unzip -j 200.0.10.0.zip '*HWVEAPO.cab'
cabextract -F hdrec_bf_data.bin HWVEAPO.cab     # cab尾部含约9.8 KB数字签名数据，提示额外字节属正常现象
cp hdrec_bf_data.bin device/huawei/gaokun3/micarray/prebuilt/
```

| 文件 | 字节大小 | SHA-256 |
|---|---|---|
| `HWVEAPO.cab` | `2,460,732` | `f16481286a8f243c9a8ba1a6563420a73814efb678b8f0921a791c09f31da485` |
| `hdrec_bf_data.bin` | `347,592` | `34a5fb58b694703c1db7b70f01b2cc56fc5283109ca9d3be4b8d6207a77e1654` |

`libgaokunmicarray`在运行期加载该文件时还会校验文件总长度及末尾的两个32位校验字（前`86,896`个32位字按位异或，以及再异或`0x55555555`的结果），校验不通过时自动拒绝加载并在日志中输出原因。
