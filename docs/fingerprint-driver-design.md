# 指纹驱动设计（gaokun3 / FocalTech FTE7001）

> **现状（2026-09-24）**：走到里程碑 **M1（把厂商签名的指纹 TA 加载进 QSEE）已在硬件验证成功**，
> 命令收发工具就位并上机跑通到"发送边界"。**M2（发第一条真命令）暂停**，卡在命令帧的精确逆向——
> 那是块需要专门啃的硬骨头（见 §2）。本文是这项工作的完整记录 + 架构 + 复现步骤，接手从这里读起。
> 一路证据：`docs/stage4-findings.md` #120（判定不可做）→ #123（翻案：其实可做）→ #124（内核移植）→
> #125（上机 LOAD 成功）；Windows 侧逆向报告在 `docs/fingerprint/`（专有二进制的笔记，不入库）。

## 背景：这颗传感器为什么不能按常规驱动

* 型号 **FocalTech `FTE7001`**（`FTE7001` 是华为的 ACPI PNP ID；真实芯片是 FocalTech FT9769 模组 + FT9391 AFE，
  电源键式小面阵）。部分机型是 Goodix `GDIX5125`，由 GPIO61/62 两个板级 strap 选型；本机实测是 FocalTech。
* **取图、特征提取、模板库、比对全在 TrustZone 的一个高通签名可信应用里**（`fingerpr.mbn`，TA 名 `fingerprint`），
  它经 **QSEE-SPI 直接持有传感器的 SPI 总线**。非安全世界（Windows/Linux）**拿不到这条 SPI**，
  ACPI `\_SB.SPBA` 只暴露两根 GPIO（中断 181 / 复位 185），没有 SpiSerialBus（#120）。
* 所以路线不是"写个 SPI 驱动读图"——那条 SPI 归 TZ，读不到；就算读到，MOC 小传感器的图质差、匹配也难。
  **唯一可行的路线**是：用厂商自己的 QSEECOM SMC 把厂商自己签名的 TA 加载进 QSEE，让 TA 去驱动传感器、做比对，
  普通世界只当"发命令 + 替 TA 搬运加密模板"的中间人。**这不改 TA、不绕安全启动，TZ 仍逐段验签**——
  性质与我们已在做的 GPU/DSP/WiFi 固件加载相同。
* 为什么以前判"不可做"（#120）又翻案（#123）：#120 只看到"上游 mainline 的 qcom_scm 只有 LOOKUP/SEND、没有 LOAD"，
  漏了两点——指纹走的是**旧 QSEECOM**（不是新 QTEE），gaokun3 恰在它的 allowlist 里、本机 `uefisecapp` 已 probe
  说明这条传输今天就通；而缺的 LOAD 已有人（samcday/Dawid）在别的 SoC 上真机验证过、可移植。

## 0. 一句话架构

传感器的取图/比对/模板都在 **TrustZone 里的高通签名 TA `fingerprint`**（`fingerpr.mbn`，单一 secelf）。
普通世界（Linux）**不碰传感器 SPI**，只做三件事：① 用 QSEECOM SMC 把这个签名 TA 加载进 QSEE、
② 给 TA 发命令（`FF_CMD_TA_*`）、③ 应答 TA 回调普通世界读写它加密模板的 listener 请求。
Android 侧再包一层 `IFingerprint` HAL。**全程不改 TA、不绕安全启动，TZ 逐段验签。**

```
Android FingerprintService
        │  AIDL IFingerprint/ISession
   [指纹 HAL(用户态)]  ── STRONG，ISharedSecret 签 HAT（本机全软件 KeyMint，#120 §3 已论证可行）
        │  ioctl/字符设备
   [in-kernel client driver]  ── LOAD→app_id、按状态机发 FF_CMD_TA_*、listener 服务(安全存储)
        │  qcom_scm_qseecom_app_load / _app_send / listener  ← patch 0050
   [QSEECOM / TZ]  ── 验签、映射 TA、TA 经 QSEE-SPI 直连传感器
        │
   FocalTech FT9769 模组（FT9391 AFE），中断 GPIO181 / 复位 GPIO185
```

## 1. 已定的决策（有实测支撑）

| # | 决策 | 依据 |
|---|---|---|
| D1 | 走**旧 QSEECOM**（不是 QTEE/SMCInvoke） | gaokun3 在 `qcom_scm_qseecom_allowlist`，本机 `qcom_qseecom`+`uefisecapp` 已 probe（#123 §1） |
| D2 | LOAD 用 samcday/Dawid 的移植（patch 0050），非自研 | 有 SDM670 真机验证；SMC 参数逐字段对上 Windows 逆向（#124） |
| D3 | 单一 secelf ⇒ `mdt_len=0`，整文件传，跳过 mdt 拼接 | fingerpr.mbn 是单 secelf；Windows QcTrEE 的 LOAD 也是 mdt_len=0（#124） |
| D4 | **必须把 scm 设备约束到 32 位 DMA** | 本机 dma-ranges 40 位+内存>4GB；LOAD 物理地址是 SMC32 裸参数会截断（#124/#125 实测镜像落 `0xec000000`） |
| D5 | staging 池分配**正好 4MiB**（不是 img+4MiB） | img<4MiB；img+4MiB=7.7MB 超 MAX_PAGE_ORDER(4MB) `-ENOMEM`（#125）。⚠️ img 若>4MiB 得改走 CMA |
| D6 | 驱动**常驻**加载 TA（不是每次用完卸） | 收发命令要 app_id 稳定；`qcom_qseecom_fpcmd` 已按此验证 |
| D7 | listener 无人认领时安全网兜底 | patch 0050 的 service_listeners 自动 FAILURE、256 轮 -ELOOP，发错命令不硬挂（#125 续） |

## 2. 命令协议（⬜ 逆向中，`fp-cmd-protocol.md`）

已知：命令名 `FF_CMD_TA_*`(37) / `FF_CMD_SVC_*`(34) / `CMD_TO_DEVICE_*`(13)；Windows 客户端观察到用
`0x80xx` 号 + `{cmd(4B), req_len(4B), rsp_len(4B), payload}` 结构（报告 §7）。TA 发送封装函数
`ff_trustlet_client_exchange_message`（别名 `TA_Commication`/`ff_trustlet_RWQSEE`）。
⚠️ 2026-09-24 一次静态复核校正了"安全首发命令"的候选：Windows 的 `ff_sc_GetSCFirmwareVersion` /
`ff_sc_check_health` 里的 **`sc` = sensor controller（传感器 MCU），走 SPI** —— 它们**不是** SPI-free，
不能当第一条只读命令（要先 `INIT_SPI`）。
✅ 2026-10-30 命令协议已基本定死（详见 `fingerprint/fp-cmd-protocol.md`，本节被其取代）：
- 请求 header 两说已对齐：wire 帧 `{cmd,dlen,rlen,payload}` 12B 头（0x80xx，WBDI）走 sub_ADFC；
  InvokeCommand 魔数 `0x1255` 是 sub_CEA4 case 2 的 a4 校验值，与 wire 帧无关（协议文档 §8.4）；
- `CREATE=0x1001`、`INIT=0x1004` 命令号已定死（ff_cmd_name 跳表全解，协议文档 §3）——**但它们走
  word8≠0 的 FF_CMD 路径（36B 描述符 +32）**；§6.4 已指令级闭合（2026-10-30）：word8 全链**不可设**
  （SMC=5 参无第 6 参、IOCTL +12 死槽），FF_CMD 是 TA↔TA GP 内部通道——对 FTE7001 **不该发也发不进**，
  M2-M4 全走 wire 0x80xx（协议文档 §6.4）；
- **M2 首发已修正**：不得发 0x1001 当 wire cmd（会毒化 dword_2A4640=2 不可逆），首发只允许
  0x8031 只读探测（协议文档 §7）；
- ⬜ 仍待定（只剩一项，且**只能上机**）：enroll/authenticate 的 listener 请求/响应缓冲格式
  （驱动 `->service()` 要解析，要看真实回调）。
  ~~FF_CMD payload 格式~~ → **已判 moot**：word8 发不进（§6.4），payload 在 GPAppLib 内这件事
  只对 Goodix/GP 变体有意义，不再列为 FTE7001 的未决项。

## 3. 驱动状态机（草案，待命令号落定）

```
init：      LOAD → app_id → 注册 listener(安全存储) → wire init（0x8022 握手 / 0x8020+0x8021 SPI /
            0x8026 / 芯片 ID 探测序列）  ← FF_CMD 的 INIT/INIT_SPI/PROBE_CHIP_ID/INIT_DEVICE 是
            TA↔TA GP 内部序列（普通世界发不进，协议文档 §6.4），Android 侧用 wire 0x80xx 复刻
idle：      等 GPIO181 finger-down 中断（或 QUERY_FINGER_STATUS 轮询）
authenticate：START_SCANNING → CAPTURE_IMAGE → AUTNENTICATE →(listener 读模板)→ 结果 → HAT
enroll：    PRE_ENROLL → 多帧 CAPTURE_IMAGE + ENROLL →(listener 写模板)→ POST_ENROLL
teardown：  FREE_SPI → FREE → 注销 listener → app_shutdown
```
⚠️ enroll/authenticate 会触发 listener 回调 —— **第一次跑这些命令仍需人在设备旁**（安全网只保证不硬挂，
不保证一次成功；模板的安全存储后端要落地，见 §4）。

## 4. 安全存储后端（listener 的 `->service()`）

TA 会请求普通世界替它读写加密模板（它自己用 `qsee_sfs_*` 封装、带防回滚+HMAC）。普通世界只是搬运
密文，看不到明文。落地选项：① 存到 `/data/vendor/` 下的文件（简单，够用）；② RPMB（防回滚，Windows 用的）。
先做 ①。格式由逆向出的 listener 消息结构决定。

## 5. GPIO 与设备树

Windows ACPI `\_SB.SPBA`：中断 **GPIO181**（edge/active-high/wake）、**GPIO185**（复位，推测）。
本机 DT 里这两脚都空闲、不在 `gpio-reserved-ranges`（#120）。驱动需要 GPIO181 做 finger-down 中断；
GPIO185 复位可能由 TA 经 QSEE-GPIO 自己做（待确认）。届时加一个 fingerprint 的 DT 节点带这两脚。
⚠️ 传感器的 SPI **归 TZ**，DT 里【不要】给它建 spi 节点。

## 6. Android HAL

以 AOSP 虚拟 HAL（`hardware/interfaces/biometrics/fingerprint/aidl/default/`）为骨架，
把 `FakeFingerprintEngine` 的 enroll/authenticate 换成"经字符设备驱动真实取图+TA 比对"，
`SensorProps` 设 `STRONG`，接 `ISharedSecret` 用同一把软件 HMAC 密钥签 HAT —— 本机 KeyMint/gatekeeper
都是软件实现，三方协商同一密钥，keystore 会认（#120 §3）。强度设 STRONG 才能绑 keystore 与 72h 免密。

## 7. 里程碑

- [x] M1 LOAD 成功（#125）
- [ ] M2 SEND 一条只读命令成功（证明能与 TA 对话）← **当前目标**
- [ ] M3 init 序列跑通（wire 0x8020/0x8021/0x8026 + 芯片 ID 探测序列拿到芯片 ID——非 FF_CMD，协议文档 §7.3）
- [ ] M4 listener 服务 + 一次 authenticate（含安全存储读）
- [ ] M5 enroll 落一枚模板
- [ ] M6 client driver 成形（字符设备接口）
- [ ] M7 Android 指纹 HAL，Settings 里能录/解锁

## 8. 复现 M1（加载测试）

前提：内核带 **patch 0050**（`patches/0050-firmware-qcom-scm-qseecom-app-load-shutdown-listener.patch`，
在 `scripts/kernel-apply-patches.sh` 的 `FP_PATCHES` 里）。⚠️ 2026-10-05 起（v1.0-plan D21）它**不进发版内核**：
实验内核要 `bash scripts/kernel-apply-patches.sh <树> --with-fp`（`--verify` 同样加 `--with-fp`）。工具在 `tools/fingerprint-bringup/`。

1. **构建**：内核树打满补丁链（含 0050）后 `make Image modules`；out-of-tree 编两个模块：
   `make -C <kernel> M=<repo>/tools/fingerprint-bringup ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules`。
2. **取 TA**：`fingerpr.mbn`（sha256 `b081543c7b6ae4…`）从公开 `uup-drivers-sc8280xp` release 的
   `QcTreeExtOem8280.cab` 取（见 `device/huawei/gaokun3/firmware/README.md`），推到设备 `/data/local/tmp/`。
3. **上机（⚠️ 需人在设备旁）**：无回退槽，用一次性启动项指向【单独的】新内核文件、不覆盖 `slot_b/Image`——
   `bash scripts/boot-oneshot.sh <entry>` 起带 0050 的内核（起不来一次重启就回 stock，#125 记了做法）。
4. **触发**：
   ```
   adb push qcom_qseecom_fptest.ko /data/local/tmp/
   adb shell su -c 'insmod /data/local/tmp/qcom_qseecom_fptest.ko'
   adb shell su -c 'echo load > /sys/kernel/debug/gaokun3_fptest/trigger'
   adb shell su -c 'dmesg | grep fptest | tail'
   ```
   预期：`★★★ LOAD 成功！app_id=N`（实测 app_id=5，镜像落物理 `0xec000000`，无挂机）。
   `qcom_qseecom_fpcmd.ko` 则是常驻版：`echo 1 > .../gaokun3_fpcmd/load` 后可 `echo <hex> > .../send_hex` 发命令。

## 9. 设备与工作区现状（交接用）

* **设备**：#125/后续测试后，若仍在 RAM 里的"fp 内核"（`#25`，= stock + 0050 休眠，经 oneshot 单独 `Image.fp` 启动），
  **任意一次重启都自动回发布版内核 `1790206017`**；ESP 已复原（临时 `Image.fp` 与启动项已删）。
  持久值：`persist.vendor.gaokun3.allow_suspend=0`（开发机不睡）。IP 静态 `192.168.10.239`。
* **本地提交未推**：指纹整条线 #120→#125 + 设计文档 + 工具，均为本地提交（发版/推仓需用户点头）。
* **构建机**：`az vm deallocate` 后用完即停，树在 `~/gk3-kernel`（已含 0050 的工作区改动，未提交，靠 `patches/` 复现）。
* **下一步的入口**：M2 = `send_hex` 发 0x8031 只读探测（唯一允许首发，见 `fingerprint/fp-cmd-protocol.md` §7）。
  命令号逆向已完成（`FF_CMD_TA_CREATE=0x1001`/`INIT=0x1004` 已定死），不再是 M2 前置；
  本地能做的逆向已收口（2026-10-30，含 §6.4 word8 指令级闭合），剩余**只能上机**一项：
  ① listener 缓冲格式（要看真实回调）。
  ~~FF_CMD payload 格式~~ 已判 moot：word8 发不进（协议文档 §6.4），上机也测不了；
  若日后拿到带 GPAppLib 的 blob 可离线续 Goodix/GP 变体分析，与 FTE7001 主线无关。
