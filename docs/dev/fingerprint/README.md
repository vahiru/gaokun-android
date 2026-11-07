# 指纹逆向报告（本目录`*.md`不入版本库）

MateBook E Go（gaokun3）的指纹传感器为**FocalTech `FTE7001`**（部分批次为Goodix `GDIX5125`，由GPIO61/62板级strap引脚区分）。SPI总线由高通签名的TrustZone可信应用`fingerpr.mbn`（TA名`fingerprint`）直接持有；Windows产品的录入和比对在用户态的引擎适配器里完成，TA只负责取图传输和16字节的模板元数据。Linux/Android侧通过厂商自身的QSEECOM SMC将该签名TA加载进QSEE（TrustZone逐段验签，不修改TA、不绕过安全启动），由内核驱动取图，Android HAL在用户态解码、匹配和存储模板。架构与设计详见[`../fingerprint-driver-design.md`](../fingerprint-driver-design.md)。

## 本地逆向报告清单（gitignore忽略）

| 文件 | 内容 |
| --- | --- |
| `gaokun3-fingerprint-tee.md` | TA加载机制总纲：QSEECOM LOAD缺口、SMC参数、tzmem/SHM-bridge、samcday参考实现、allowlist |
| `QcTrEE-load-reverse-report.md` | Windows `QcTrEE.sys`中的LOAD全链路（SMC `0x32000101`、4 GiB以下物理地址约束、内存分配） |
| `fingerpr-command-report.md` | `fingerpr.mbn`的命令分发与`FF_CMD_TA_*`（37条）/ `FF_CMD_SVC_*`（34条）/ `CMD_TO_DEVICE_*`（13条）清单 |

## 不入库原因

`fingerpr.mbn`、`QcTrEE.sys`、`FtWbioDriverUmdf.dll`属于华为/高通签名的专有二进制，上述笔记为其静态逆向分析记录，因此整类忽略、仅保留本说明文件。专有二进制可从公开的`matebook-e-go/uup-drivers-sc8280xp`发布包提取（方式见[`device/huawei/gaokun3/firmware/README.md`](../../../device/huawei/gaokun3/firmware/README.md)）。
