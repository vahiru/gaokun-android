# 指纹驱动设计（gaokun3 / FocalTech FTE7001、Goodix GDIX5125）

> **当前状态**：厂商签名的指纹TA已能加载进QSEE并收发命令；固定ioctl的内核驱动（[`patches/kernel/fingerprint/0002`](../../patches/kernel/fingerprint/0002-firmware-qcom-gaokun3-fingerprint-ioctl.patch)，Goodix分支[`0003`](../../patches/kernel/fingerprint/0003-firmware-qcom-gaokun3-fpcmd-add-the-Goodix-GDIX5125-sensor.patch)）和Android HAL（[`device/huawei/gaokun3/fingerprint/`](../../device/huawei/gaokun3/fingerprint/README.md)）由[PR #92](https://github.com/vahiru/gaokun-android/pull/92)合入，随镜像构建（`CONFIG_GAOKUN3_FPCMD`由`scripts/kernel-config-android.sh`打开）。解码、匹配和模板存储都在HAL里，服务报告`CONVENIENCE`。实机过程见[`fingerprint-machine-check-2026-10-30.md`](fingerprint-machine-check-2026-10-30.md)（M2命令收发与首帧）和[`crdroid-fingerprint-machine-check-2026-11-01.md`](crdroid-fingerprint-machine-check-2026-11-01.md)（crDroid候选）；逐条的逆向记录（D1–D22）见合并前的`docs/fingerprint-driver-design.md`（git历史，提交c902a34）。Windows侧专有二进制的本地逆向笔记说明见[`fingerprint/README.md`](fingerprint/README.md)。

## 1. 硬件与TrustZone边界

* **传感器型号**：ACPI PNP ID为`FTE7001`（电源键式小面阵，真实模组为FocalTech FT9769 + FT9391 AFE，帧40×196）；部分同系机型装Goodix `GDIX5125`（帧36×160），由GPIO61/62板级strap区分，HAL启动时读一次。
* **SPI总线归属TrustZone**：传感器MCU挂在高通签名的可信应用`fingerpr.mbn`（TA名`fingerprint`，单一secelf）后面，TA经QSEE-SPI持有SPI总线。非安全世界无法访问该SPI控制器，ACPI `\_SB.SPBA`只暴露中断脚**GPIO181**与复位脚**GPIO185**。
* **TA只负责传输**：Windows产品里，录入和识别由用户态的`ftWbioEngineAdapter.dll`（WinBio引擎适配器）完成，TA只存16字节的模板元数据（`0x8029`）；TA内的算法入口在该产品中不可达。所以Android侧也在主机上做解码和匹配：FocalTech的帧经TA以AES加密送出，HAL解密；Goodix的12位样本由内核解包后交给HAL。
* **加载方式**：通过高通QSEECOM SMC把厂商原样签名的`fingerpr.mbn`加载进QSEE（TrustZone逐段验签，不修改TA、不绕过安全启动）。

```
Android FingerprintService
        │  AIDL IFingerprint/ISession
   [指纹HAL（用户态）]  ── 解码、匹配、模板库；报告CONVENIENCE，不签发HAT
        │  固定ioctl，/dev/gaokun3_fpcmd
   [fpcmd驱动（fingerprint/0002、0003）]  ── LOAD、INIT、CAPTURE、GET_FINGER_STATUS等固定操作
        │  qcom_scm_qseecom_app_load / _app_send / listener（fingerprint/0001）
   [QSEECOM / TZ]  ── 验签并映射TA，TA经QSEE-SPI直连传感器MCU
        │
   FocalTech FT9769（或Goodix GDIX5125），中断GPIO181 / 复位GPIO185
```

## 2. 已验证的内核加载约束

| 决策 | 技术依据 |
| --- | --- |
| 走**旧QSEECOM**接口而非QTEE/SMCInvoke | SC8280XP位于主线`qcom_scm_qseecom_allowlist`中，实机`qcom_qseecom`与`uefisecapp`正常probe |
| 移植samcday/Dawid的QSEECOM LOAD实现（`patches/kernel/fingerprint/0001`） | SMC参数布局与Windows `QcTrEE.sys`逆向结果逐字段一致 |
| 单一secelf（`mdt_len=0`）整文件传入 | `fingerpr.mbn`已是合并后的单secelf，无需在内核内做mdt分段拼接 |
| **将`qcom_scm`设备DMA掩码限制为32位** | 本机内存大于4 GiB且`dma-ranges`为40位，而QSEECOM LOAD的物理地址通过SMC32裸参数传递，超过4 GiB会被截断（实测32位DMA下镜像落于`0xec000000`） |
| 按物理地址核对4 MiB对齐，且整段低于4 GiB | 不能假设虚拟地址和物理地址的对齐偏移相同；分配不满足时在LOAD之前就退出 |
| 未注册listener回调时自动返回失败与轮数上限 | `patches/kernel/fingerprint/0001`的`service_listeners`对未认领的listener自动回填FAILURE并限制最多256轮（`-ELOOP`）；这只限制已经返回的SMC循环，单次SMC不返回时无法中断 |
| 只发Windows客户端用到的命令 | Windows驱动的传输封装只有`0x8020`/`0x8021`/`0x8029`/`0x8031`四种；`0x8026`/`0x8027`/`0x802B`在该产品中没有调用，而且TA对应的工作区越界，不发送 |

## 3. 协议与驱动

### 3.1 命令协议

普通世界发给TA的是wire帧：12字节头`{cmd, dlen, rlen}`加载荷，命令号`0x80xx`。`InvokeCommand`的魔数`0x1255`是TA内部另一条路径的校验值，与wire帧无关。

* **第一条命令只发`0x8031`**（只读探测），返回`0x5aa5`表示收到。
* **`FF_CMD_TA_CREATE`（`0x1001`）、`FF_CMD_TA_INIT`（`0x1004`）不能当wire命令发**：它们走TA之间的GP内部通道，普通世界设不了那条路径的标志位；把`0x1001`当wire命令发会让TA的状态变量不可逆地进入错误状态。
* 采图只用`0x8020`/`0x8021`（SPI写/读）和MCU子命令；`0x8029`会写入或删除TA的安全存储文件，`0x802B`会删除已有的模板槽，二者都有副作用，不能当作只读查询。

### 3.2 fpcmd驱动（`patches/kernel/fingerprint/0002`、`0003`）

* **`0002`**：字符设备`/dev/gaokun3_fpcmd`（`CONFIG_GAOKUN3_FPCMD`，编为built-in），只提供LOAD、UNLOAD、INIT、PROBE、WAKE、CAPTURE和GET_FINGER_STATUS几个固定操作，没有debugfs，也没有任意QSEE命令的入口。TA从`/vendor/firmware/fingerpr.mbn`读取。同一时刻只允许一个进程打开；进程退出时驱动关闭TA；关闭失败则保留共享缓冲并拒绝再次打开，避免安全环境仍在使用的内存被释放，重启后才能恢复。
* **`0003`**：在`0002`上加Goodix分支，`SET_SENSOR`在INIT前选型号，不调用时是FocalTech。INIT按Windows驱动开机时的顺序初始化MCU，DAC、tcode和FDT阈值由每台机器的OTP算出；传感器配置模板从`/vendor/firmware/gaokun3-goodix-cfg.bin`读取，只接受已知的那一份。MCU空闲约30秒后会睡眠，唤醒它的那条命令会丢，所以超过5秒没有命令时先发一个NOP。

### 3.3 Android HAL

HAL实现AIDL fingerprint V4，取图、解码、匹配、学习和模板存储都在用户态，细节见[HAL说明](../../device/huawei/gaokun3/fingerprint/README.md)。

早先“设为`STRONG`、与软件KeyMint共享HMAC密钥签发HAT”的设想已经撤回：图像离开TEE后在主机上解码和匹配，没有证据表明采集和匹配链满足`STRONG`的要求，软件KeyMint能接受某种令牌也不等于满足生物识别强度。所以服务报告`CONVENIENCE`，不签发HardwareAuthToken，不能用于Keystore解锁。

## 4. TA内算法的离线执行（HAL未采用）

TA镜像里带有FocalTech的录入和识别算法。在离线装载器里完整装载TA镜像后，这套算法能执行：真实接触帧经原预处理后可以录入、导出模板、在新进程里重新导入并识别。要点：

* 录入后必须先把模板标记为有效，识别才会比对；导出模板的函数会顺带置位。
* 导出的模板（类型121，40×196时约239 KB）尾部是CRC16，但**导入时先写入内存库、后校验CRC**，所以调用方必须先自行校验长度和CRC；模板不记录帧尺寸，几何和算法配置要由存储层另外保存。
* 默认配置下识别不会改写模板，不需要回写。

这条路线不经过TEE的保护，而且依赖对专有二进制的离线装载，HAL改用自己的匹配器（`gaokun3-matcher.c`）。

## 5. TA加载验证复现步骤

1. **打补丁并构建模块**：
   ```bash
   bash scripts/kernel-apply-patches.sh <kernel-tree>
   make -C <kernel-tree> ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- Image modules
   make -C <kernel-tree> M=<repo>/tools/fingerprint-bringup ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules
   ```
2. **准备TA固件**：从`uup-drivers-sc8280xp`发布包的`QcTreeExtOem8280.cab`提取`fingerpr.mbn`（sha256前缀`b081543c7b6ae4`），推送至设备`/data/local/tmp/`。
3. **一次性启动测试内核**：将带`patches/kernel/fingerprint/0001`补丁的内核拷入ESP独立路径并用`bash scripts/boot-oneshot.sh <entry>`单次启动（避免覆盖常规启动槽）。
4. **加载并验证**：
   ```bash
   adb push qcom_qseecom_fptest.ko /data/local/tmp/
   adb shell su -c 'insmod /data/local/tmp/qcom_qseecom_fptest.ko'
   adb shell su -c 'echo load > /sys/kernel/debug/gaokun3_fptest/trigger'
   adb shell su -c 'dmesg | grep fptest | tail'
   ```
   预期日志输出`LOAD 成功，app_id=5`（镜像落于32位物理地址空间且无挂死）。需要驻留会话发送测试帧时使用bring-up目录里的`qcom_qseecom_fpcmd.ko`，通过`/sys/kernel/debug/gaokun3_fpcmd/{load,send_hex}`交互；它与正式驱动`0002`同名而不同，只用于bring-up。
