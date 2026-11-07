# 传感器VFS根目录（`hexagonrpcd`服务文件）

`.gitignore`忽略本目录下的专有配置文件，仅保留本说明文档。

这些文件由AP侧的`hexagonrpcd`守护进程通过FastRPC作为只读虚拟文件系统（设备路径`/vendor/etc/hexagonrpcd-root/`）提供给SLPI DSP读取。协议与运行机制详见[`docs/dev/sensors-ssc-protocol.md`](../../../../docs/dev/sensors-ssc-protocol.md)。

## 一、目录结构要求

```
sensors/config/*.json     26个SEE传感器驱动配置JSON
sensors/sns_reg.conf      DSP注册表索引（必须为约400 B的纯文本格式，以version=1开头；不可使用2423 B的DriverData JSON格式）
dsp/sdsp/RSCS.bin         1340 B，SLPI伴生固件
socinfo/{hw_platform,platform_subtype,platform_subtype_id,
          platform_version,soc_id,revision}
```

注：`sensors/registry/registry`不在本目录中，而是作为0字节空文件受控于`../etc/hexagonrpcd-empty-registry`（保持空文件可使DSP回退至默认启用全部传感器；切勿使用`sscregistrygen`预生成注册表文件）。

## 二、提取方法

可从公开的Windows驱动发布包提取（与[`../firmware/README.md`](../firmware/README.md)同一来源）：

```bash
curl -LO https://github.com/matebook-e-go/uup-drivers-sc8280xp/releases/download/200.0.10.0/200.0.10.0.zip
unzip -o 200.0.10.0.zip 'qc*.cab'
# 使用cabextract（Linux）或expand.exe（Windows）解包
```

| 所需文件 | 来源cab包 |
|---|---|
| `sensors/config/*.json`、`sns_reg_config`、`socinfo/*` | `qcSensorsConfigQrd8280.cab` |
| `RSCS.bin` | `qcsubsys_ext_scss8280.cab` |

提取后需执行两步处理：

1. 对所有JSON与文本文件执行`dos2unix`转换换行符。
2. 将`sns_reg_config`重命名为`sensors/sns_reg.conf`。

由于本仓对`external/hexagonrpc`应用了路径末尾`\r`（CR）剥离补丁（位于分段解析函数`copy_segment_and_advance()`），`socinfo/`目录下无需创建带`\r`后缀的符号链接即可由`PRODUCT_COPY_FILES`正常打包。
