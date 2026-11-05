# 指纹驱动设计（gaokun3 / FocalTech FTE7001）

> **当前状态**：**将厂商签名的指纹TA加载进QSEE已在实机验证通过**，内核补丁[`patches/0050`](../../patches/0050-firmware-qcom-scm-qseecom-app-load-shutdown-listener.patch)与测试模块[`tools/fingerprint-bringup/`](../../tools/fingerprint-bringup/)在位；驱动与Android HAL草稿见[PR #92](https://github.com/vahiru/gaokun-android/pull/92)，补丁0050默认不打入发布版内核（构建实验内核需传`--with-fp`）。Windows侧专有二进制的静态逆向笔记说明见[`fingerprint/README.md`](fingerprint/README.md)。

## 1. 硬件与TrustZone边界

* **传感器型号**：ACPI PNP ID为`FTE7001`（电源键式小面阵，真实模组为FocalTech FT9769 + FT9391 AFE；部分同系机型采用Goodix `GDIX5125`，由GPIO61/62板级strap引脚区分）。
* **SPI总线归属TrustZone**：取图、特征提取、模板加密封装与比对全部在TrustZone内的高通签名可信应用`fingerpr.mbn`（TA名`fingerprint`，单一secelf）中执行，TA经QSEE-SPI直接持有传感器SPI总线。非安全世界（Windows/Linux）无法访问该SPI控制器，ACPI `\_SB.SPBA`仅暴露中断脚**GPIO181**（edge/active-high/wake）与复位脚**GPIO185**。
* **唯一可行路径**：通过高通QSEECOM SMC将厂商原样签名的`fingerpr.mbn`加载进QSEE（TrustZone逐段验签，不修改TA、不绕过安全启动），Linux内核驱动仅负责：① 加载TA获取`app_id`；② 向TA收发`FF_CMD_TA_*`命令；③ 注册QSEECOM listener响应TA读写加密模板文件的回调。

```
Android FingerprintService
        │  AIDL IFingerprint/ISession
   [指纹HAL（用户态）]  ── STRONG，ISharedSecret签发HAT
        │  ioctl / 字符设备
   [内核客户端驱动]  ── LOAD→app_id、按状态机发FF_CMD_TA_*、listener服务（安全存储）
        │  qcom_scm_qseecom_app_load / _app_send / listener（patch 0050）
   [QSEECOM / TZ]  ── 验签并映射TA，TA经QSEE-SPI直连传感器
        │
   FocalTech FT9769模组（FT9391 AFE），中断GPIO181 / 复位GPIO185
```

## 2. 已验证的内核加载约束

| 决策 | 技术依据 |
| --- | --- |
| 走**旧QSEECOM**接口而非QTEE/SMCInvoke | SC8280XP位于主线`qcom_scm_qseecom_allowlist`中，实机`qcom_qseecom`与`uefisecapp`正常probe |
| 移植samcday/Dawid的QSEECOM LOAD实现（`patches/0050`） | SMC参数布局与Windows `QcTrEE.sys`逆向结果逐字段一致 |
| 单一secelf（`mdt_len=0`）整文件传入 | `fingerpr.mbn`已是合并后的单secelf，无需在内核内做mdt分段拼接 |
| **将`qcom_scm`设备DMA掩码限制为32位** | 本机内存大于4 GiB且`dma-ranges`为40位，而QSEECOM LOAD的物理地址通过SMC32裸参数传递，超过4 GiB会被截断（实测32位DMA下镜像落于`0xec000000`） |
| staging缓冲池固定分配**4 MiB** | `fingerpr.mbn`小于4 MiB；若按`img_size + 4 MiB`（7.7 MiB）分配会超过`MAX_PAGE_ORDER`（4 MiB）触发`-ENOMEM` |
| 驱动常驻持有TA会话 | 命令收发依赖稳定的`app_id`，不在每次操作后卸载TA |
| 未注册listener回调时自动返回失败与轮数上限 | `patches/0050`的`service_listeners`对未认领的listener自动回填FAILURE并限制最多256轮（`-ELOOP`），防止发错命令时死锁内核 |

## 3. 协议与驱动模块

### 3.1 命令协议

已知命令集包含`FF_CMD_TA_*`（37条）、`FF_CMD_SVC_*`（34条）与`CMD_TO_DEVICE_*`（13条），封装函数为`ff_trustlet_client_exchange_message`。注意Windows驱动中的`ff_sc_GetSCFirmwareVersion` / `ff_sc_check_health`访问的是传感器MCU（`sc` = sensor controller，走SPI），必须在`INIT_SPI`之后调用；首条不触碰SPI的只读验证命令应选择`FF_CMD_TA_CREATE`或`FF_CMD_TA_INIT`。

待从TA分发表确认的细节：
1. 请求头部的确切字节布局（`InvokeCommand` magic `0x1255`与`{cmd(4B), req_len(4B), rsp_len(4B), payload}`的对应关系）。
2. `0x80xx`命令号与`FF_CMD_TA_*`枚举序数的映射表。
3. `enroll` / `authenticate`期间触发的QSEECOM listener请求与响应缓冲区结构。

### 3.2 驱动状态机与安全存储

```
init：        LOAD → app_id → 注册listener（安全存储） → FF_CMD_TA_INIT → INIT_SPI → PROBE_CHIP_ID → INIT_DEVICE
idle：        等待GPIO181 finger-down中断（或轮询QUERY_FINGER_STATUS）
authenticate：START_SCANNING → CAPTURE_IMAGE → AUTHENTICATE →（listener读加密模板）→ 比对结果 → HAT
enroll：      PRE_ENROLL → 多帧CAPTURE_IMAGE + ENROLL →（listener写加密模板）→ POST_ENROLL
teardown：    FREE_SPI → FREE → 注销listener → app_shutdown
```

TA通过`qsee_sfs_*`自行完成模板加密与HMAC防篡改，普通世界listener仅负责将密文持久化到`/data/vendor/`下的文件。设备树侧只需为客户端驱动声明GPIO181（中断）与GPIO185（复位），**不可**在DT中声明传感器SPI节点。

### 3.3 Android HAL

基于AOSP `hardware/interfaces/biometrics/fingerprint/aidl/default/`改造，将`FakeFingerprintEngine`替换为通过字符设备调用内核驱动，声明`SensorProps`强度为`STRONG`，并通过`ISharedSecret`与软件KeyMint / Gatekeeper协商同一把HMAC密钥签发HAT。

## 4. TA加载验证复现步骤

1. **打补丁并构建模块**：
   ```bash
   bash scripts/kernel-apply-patches.sh <kernel-tree> --with-fp
   make -C <kernel-tree> ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- Image modules
   make -C <kernel-tree> M=<repo>/tools/fingerprint-bringup ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules
   ```
2. **准备TA固件**：从`uup-drivers-sc8280xp`发布包的`QcTreeExtOem8280.cab`提取`fingerpr.mbn`（sha256前缀`b081543c7b6ae4`），推送至设备`/data/local/tmp/`。
3. **一次性启动测试内核**：将带`0050`补丁的内核拷入ESP独立路径并用`bash scripts/boot-oneshot.sh <entry>`单次启动（避免覆盖常规启动槽）。
4. **加载并验证**：
   ```bash
   adb push qcom_qseecom_fptest.ko /data/local/tmp/
   adb shell su -c 'insmod /data/local/tmp/qcom_qseecom_fptest.ko'
   adb shell su -c 'echo load > /sys/kernel/debug/gaokun3_fptest/trigger'
   adb shell su -c 'dmesg | grep fptest | tail'
   ```
   预期日志输出`LOAD 成功，app_id=5`（镜像落于32位物理地址空间且无挂死）。需要驻留会话发送测试帧时使用`qcom_qseecom_fpcmd.ko`，通过`/sys/kernel/debug/gaokun3_fpcmd/{load,send_hex}`交互。
