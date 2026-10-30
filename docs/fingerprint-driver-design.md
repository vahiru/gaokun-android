# 指纹驱动设计（gaokun3 / FocalTech FTE7001）

> **现状（2026-10-30）**：M2 已在真机通过：LOAD 返回 id=5，固定 wire 0x8031 返回有效 ACK=0x5aa5、状态0，SHUTDOWN成功。见 [实机记录](fingerprint-machine-check-2026-10-30.md)。
> GPIO185复位后，MCU/SPI版本查询已通过，返回 `FT9769_APP_ST_SPI_ENC_SV0.1_202303152035`。只读尺寸返回40、196，兼容应答0x5aa5（同版本MCU固件固定返回，不能证明ready）；空载真实帧标志等待和无帧拒绝导出已实测通过；用户接触确认后已取得帧标志置1的新编码帧，7840个LE16样本基础解码检查通过；完整图像质量/处理、录入、认证或 Android HAL 尚未完成。
> 旧文档把 0x8026/0x8027 当作只读信息查询的结论已撤回：它们涉及录入/匹配算法，不能按旧表执行。
> **2026-10-31 客户端命令面已闭合**：Windows 产品只发 0x8020/0x8021/0x8029/0x8031；0x8026/0x8027/0x802B 在该产品中不可达，且 TA 工作区越界，继续禁止发送。TA 只存 16 字节模板元数据，录入/匹配在用户态引擎适配器；Android 侧需主机算法方案（见 §4.1）。

## 背景：这颗传感器为什么不能按常规驱动

* 型号 **FocalTech `FTE7001`**（`FTE7001` 是华为的 ACPI PNP ID；真实芯片是 FocalTech FT9769 模组 + FT9391 AFE，
  电源键式小面阵）。部分机型是 Goodix `GDIX5125`，由 GPIO61/62 两个板级 strap 选型；本机实测是 FocalTech。
* 已验证的传输路径是签名TA `fingerpr.mbn` 经QSEE-SPI桥接MCU；本机ACPI `\_SB.SPBA` 只暴露中断181/复位185，没有SpiSerialBus（#120）。LOOKUP("fingerprint")仍返回ENOENT，当前只在已验证例外下使用成功LOAD返回的动态app_id。
* 当前MCU产帧，编码图像经TA返回普通世界后由主机解码。TA另有可达的录入/识别算法入口，但完整调用链、模板持久化和Android认证令牌尚未验证。当前路线复用厂商签名TA，不修改TA或绕过加载验签；不能由“使用TEE”推断整个采集/认证链已符合某种安全强度。
* 为什么以前判"不可做"（#120）又翻案（#123）：#120 只看到"上游 mainline 的 qcom_scm 只有 LOOKUP/SEND、没有 LOAD"，
  漏了两点——指纹走的是**旧 QSEECOM**（不是新 QTEE），gaokun3 恰在它的 allowlist 里、本机 `uefisecapp` 已 probe
  说明这条传输今天就通；而缺的 LOAD 已有人（samcday/Dawid）在别的 SoC 上真机验证过、可移植。

## 0. 一句话架构

当前已实现的是：由 **TrustZone 里的高通签名 TA**（`fingerpr.mbn`，单一 secelf；LOOKUP名仍未确认）桥接SPI读出MCU编码帧。TA的录入/识别入口已离线定位，但实际算法调用、模板生命周期及Android认证令牌契约尚未闭合。
当前Linux侧管理TA与共享缓冲、发送已核实wire命令、接收编码图像并做诊断解码；生产驱动还需明确模板存储和listener的真实契约。
Android 侧再包一层 `IFingerprint` HAL。**全程不改 TA、不绕安全启动，TZ 逐段验签。**

```
Android FingerprintService
        │  AIDL IFingerprint/ISession
   [指纹 HAL(用户态)]  ── 强度与HAT契约待验证；尚未实现录入/认证
        │  ioctl/字符设备
   [in-kernel client driver]  ── LOAD→app_id、按状态机发 wire 0x80xx、listener 服务(安全存储)
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
| D5 | 按物理地址验证4MiB对齐及完整区间低于4GiB | 旧内核8MiB分配曾失败；当前7.2.9实验内核8MiB池成功并取得0xf6400000对齐子区。不能假设虚拟/物理对齐偏移相同，分配失败必须在LOAD前退出 |
| D6 | 生产会话生命周期待验证；当前诊断每轮LOAD后SHUTDOWN | 单轮始终使用该次成功LOAD的app_id；不能据取图探针推定长期驻留/多用户认证已通过 |
| D7 | listener 无人认领时安全网兜底 | patch 0050 的 service_listeners 自动 FAILURE、最多256轮；只限制已返回SMC的循环，不能防止单次SMC不返回 |
| D8 | Windows 客户端只使用 0x8020/0x8021/0x8029/0x8031 | 驱动唯一传输封装 7 个调用点全枚举；0x8026/27/2B 无调用（2026-10-31） |
| D9 | TA 算法 wire 路径判为未使用遗留/缺陷路径 | tz_app_init 仅分配 1 MiB 且是 qword_2A4638 唯一写入；算法路径仍取 +0x950000；初始化链无重新分配（2026-10-31） |
| D10 | 不在 TA 内做录入/匹配，改为主机侧算法 | 厂商 Windows 产品由 ftWbioEngineAdapter 在用户态完成 enroll/identify；TA 仅存 16 B 元数据 |

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
- ⬜ 仍待定：MCU初始化/识别帧、录入/匹配实际调用链，以及 listener 请求/响应缓冲格式
  （驱动 `->service()` 要解析，要看真实回调）。
  ~~FF_CMD payload 格式~~ → **已判 moot**：word8 发不进（§6.4），payload 在 GPAppLib 内这件事
  只对 Goodix/GP 变体有意义，不再列为 FTE7001 的未决项。
- **2026-10-31 追加（取代上一条的 listener 假设）**：Windows 产品驱动只发
  0x8020/0x8021/0x8029/0x8031；0x8026/0x8027/0x802B 在该产品中不可达；
  0x8029 只同步模板计数和 16 字节元数据。不要再把 TA 算法入口当作可用 enroll 路径（§4.1）。

## 3. 驱动状态机（草案，待命令号落定）

```
init：      LOAD → app_id → 注册 listener(安全存储) → 已核实的 wire init / MCU芯片识别帧（具体字节待定；不把0x8026当版本查询）  ← FF_CMD 的 INIT/INIT_SPI/PROBE_CHIP_ID/INIT_DEVICE 是
            TA↔TA GP 内部序列（普通世界发不进，协议文档 §6.4），Android 侧用 wire 0x80xx 复刻
idle：      等 GPIO181 finger-down 中断（或 QUERY_FINGER_STATUS 轮询）
authenticate：CAPTURE_IMAGE → 主机侧预处理/匹配 → 结果 →（若走 STRONG 需另证 TEE 链路）
enroll：    多帧 CAPTURE_IMAGE → 主机侧特征提取/模板 → 主机侧存储
teardown：  FREE_SPI → FREE → 注销 listener（若未使用 listener 则跳过） → app_shutdown
```
⚠️ enroll/authenticate 会触发 listener 回调 —— **第一次跑这些命令仍需人在设备旁**（listener轮数上限不能阻止单次SMC卡死；模板的安全存储后端要落地，见 §4）。

## 4. 安全存储后端（旧假设，已被 §4.1 的厂商实际分工取代）

⚠️ 2026-10-31：Windows 产品并不通过 listener/8026/802B 搬运完整模板，TA 只存
16 字节元数据；本节保留为旧假设记录，Android 方案见 §4.1。

模板持久化的实际wire契约仍在复核，不能将旧设计中“统一listener搬运SFS密文、RPMB防回滚”直接当成本机已验证事实。特别是0x802B存在删除既有slot并提交模板的路径，不能用于无副作用探测。

应先闭合存储入口、用户/slot命名空间、模板格式与完整失败处理，再设计隔离的测试存储。当前只做MCU新帧采集及主机离线解码，未注册生产存储listener、未录入或覆盖模板。0x802D长模式导出的是处理图像，不是模板；0x802C有校验失败仍落入预处理的路径，ACK不能视为图像处理成功。新增纯离线ta_image_contract.py只描述精确7852B响应的图像导出边界，不执行命令，且要求同一TA会话内处理图缓冲已独立确认。

### 4.1 厂商实际模板/算法分工（2026-10-31）

Windows 产品不把完整生物模板交给 TA：

* 驱动 `0x8029` 只同步模板计数和 16 字节元数据（`focal_tpl_info.bin` / `tplNN.bin`），
  `GET_TPL_DATA` 返回 16 字节，`SET_TPL_DATA` 甚至未把输入送给 TA；
* 完整模板和 enroll/identify 算法在 `ftWbioEngineAdapter.dll`（WinBio
  `EngineAdapter*` 接口、`FtEnrollByTemplate`/`FtVerifyByTemplate`、
  `OpenDatabase`/`StorageAdaptor`），不经过 QSEE；
* TA 的 0x8026/0x8027/0x802B 算法路径未被该驱动调用，且工作区越界（§1 D9）。

因此 Android 侧不能再按「listener 搬模板 + TA 匹配」推进，需要主机侧算法方案：
可复用 TA 内的 FocalTech 算法做受限离线执行（已有 0x1c68 预处理重放基础），
或移植引擎适配器算法。两条路线都不具备 TEE 支撑的采集/匹配链，不能声明
STRONG/HAT；功能 bring-up 与安全等级验收必须分开记录。

`0x8029` 本身会写/删 SFS 文件，仍有副作用，只能作为受限诊断候选，不能当只读枚举。

## 5. GPIO 与设备树

Windows ACPI `\_SB.SPBA`：中断 **GPIO181**（edge/active-high/wake）、**GPIO185**（复位，推测）。
本机 DT 里这两脚都空闲、不在 `gpio-reserved-ranges`（#120）。驱动需要 GPIO181 做 finger-down 中断；
GPIO185 复位可能由 TA 经 QSEE-GPIO 自己做（待确认）。届时加一个 fingerprint 的 DT 节点带这两脚。
⚠️ 传感器的 SPI **归 TZ**，DT 里【不要】给它建 spi 节点。

## 6. Android HAL

AOSP AIDL定义可用作接口与生命周期参考，但虚拟HAL的模拟录入/匹配不能作为实机功能实现。
需要先闭合真实图像处理、模板添加/枚举/删除、匹配结果、用户隔离、取消/超时、锁定策略与会话生命周期，再实现IFingerprint/ISession及回调。

此前“设STRONG并共享软件HMAC密钥就可用”的设计结论撤回：软件KeyMint能否接收某种令牌，不等于满足生物识别强度要求；当前编码图像可出TEE、主机解码，尚无证据证明安全采集/匹配链、抗重放或认证令牌来源满足STRONG。不能仅为使keystore接受而声明强度或生成成功HAT。生产SensorProps、HAT与KeyMint/Gatekeeper集成须以真实协议和平台要求验证，当前不注册可用指纹HAL，也不把采图成功回调成认证成功。

## 7. 里程碑

- [x] M1 LOAD 成功（#125）
- [x] M2 SEND 0x8031 只读命令成功（2026-10-30：ACK=0x5aa5、状态0、SHUTDOWN成功；见 fingerprint-machine-check-2026-10-30.md）
- [ ] M3（部分通过）GPIO185复位、版本/尺寸、真实帧门槛及基础解码已通过；完整图像处理、可重复接触/抬起和低功耗恢复仍待验。通信仅使用已核实的0x8020/0x8021及MCU子命令；0x8026是录入算法，不能当作芯片探测。
- [x] M3a 首次用户接触触发的新帧与基础解码：空载150个合法无帧状态；接触后第121次状态置1，15680B传输校验通过，7840样本min69/max503，无>4096样本。
- [x] M3b 限定离线预处理对照：Linux arm64重放真实TA/Windows算法，真实接触帧生成的7840B输出逐字节一致。分配失败/超量程行为有缺陷，不据此开放wire录入或宣称完整质量验收。
- [ ] M4 主机侧算法桥：受限离线执行 TA/引擎的 enroll/identify 算法（原 listener 假设作废）
- [ ] M5 主机侧模板存储 + 一次 enroll/authenticate 闭环（不含 STRONG/HAT 声明）
- [ ] M6 client driver 成形（字符设备接口）
- [ ] M7 Android 指纹 HAL，Settings 里能录/解锁

> 2026-10-31 修正：M4/M5 原定义依赖 TA listener 安全存储；厂商客户端证据表明
> 模板与算法在主机侧，故重定义为上述主机侧方案。M4 的离线算法桥仍须先证明
> 模板 ABI 和工作区可独立提供，不能直接上机试 0x8026/802B。

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

## 9. 历史交接记录（已被当前实机记录取代）

以下为早期#125阶段记录，旧IP、启动项、构建机状态及“只能上机”判断均不能用于当前操作。当前状态应读取真机并对照fingerprint-machine-check-2026-10-30.md最后一节。

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
* **2026-10-31 更新**：客户端命令面已闭合，0x8026/27/2B 禁止发送；原「只剩 listener
  缓冲格式上机」的说法作废。下一步是主机侧算法可行性（离线）与模板存储设计，
  不再向 TA 发算法命令。
