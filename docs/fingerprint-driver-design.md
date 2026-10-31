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
| D11 | 主机侧算法桥的工作区前提已离线验证，TA 分配不变 | 16 MiB 受保护映射下原 `0x9a94`+`0xd350` 精确清零 `+0x950000..+0x9f2c00`，窗口外未动；真实静态 index 上限 24 使 `0x8026` index=0 直达 `0x9b57c`；TA 自身仍只分配 `0xf8000`，继续禁止上机发算法命令（2026-10-31） |
| D12 | 全镜像离线装载证明录入算法本体可执行 | 373 RELATIVE + 37 导入绑定 + 4 ABS64；原 `0xaccc`→1、`0x9746c`→0；质量函数按 `off_361530` 的 [96,96] 读图，配置改为 96×96 后真实阈值下 `0x9b57c`→0（模板计数 1，0 次 SFS/SPI 调用）；模板进入 `off_361540` 内存库（520 B/槽，含运行期指针），持久化格式仍待闭合（2026-10-31） |
| D14 | 传感器几何必须在模板缓冲初始化前设定 | `off_361530` 是运行期传感器几何（宽,高），`FtInitErollTemplate`/`FtInitTempalteBuf` 据此分配模板 bin/mask 缓冲。实测：40×196（7840 B）若在 `0x9746c` **之后**才改 dims，`FtSubTemplateCopy` 报目标 1160/1153 与源 984/981 不一致 → 录入 -2；在 `0x9746c` **之前**设成 [40,196]（或 [196,40]）后缓冲变为 984/981，录入→0，有效化后 identify→0（score 54/71）。默认 96×96 只是我们没灌传感器资源时的静态值（2026-10-31） |
| D13 | enroll 后必须先“模板有效化”，identify 才会比对 | `FtVerifyByTemplate`（`0xF1334`）对每个 finger 先查 `*(u16*)(db+50118) == 1`，否则整库跳过 → `machCnts=0`、`ret=-1`、上层 `-2`。enroll（`0x9b57c`）只把模板写进内存库并令 count=1，**不置该标志**；`FtGetTplData`（`0xDFD20`，导出模板给主机）和 `FtSetTplValidFlag`（`0xE83B4`）都会置位。离线在 enroll 后调用原 `0xE83B4(0)` 复现该步：同图 identify→0（score 77），±24 灰阶扰动仍通过（score 45）；不置位则稳定 `-2`/`machCnts=0`（2026-10-31） |
| D15 | 主机侧模板持久化 ABI 已离线闭合（导出→新进程重灌→识别） | `FtGetTplData(0xDFD20)`：`(fingerId, &tplType, &tplSize, buf)`，`tplType=121`、magic `0x1415927D`、CRC16 在尾部；40×196 实测 238,970 B、196×40 为 239,938 B、96×96 为 273,962 B（布局公式吻合）。布局 = 4 B magic + 212 B 头 + 96×(489 B 定长 + bin + mask) + 单模板 feat + 2 B CRC。`FtIniAlgTplData_v1(0xE0460)`：`(fingerId, tplType, tplSize, buf)` 在全新进程重灌→0，count=1、`+50118=1`（加载器自己置有效位，无需再调 `0xE83B4`），同图 identify→0（40×196 score 54、196×40 score 71、96×96 score 77）。重灌后 ±4 灰阶扰动仍→0（score 40）；换成不同图案则→-2（RANSAC 后 m=0、score 0，far level 8），无错误接受。加载器不校验几何：把 40×196 的 blob 灌进 196×40 实例仍返回 0，但后续 identify→-2，所以存储层必须把 blob 与几何/配置绑定，不能依赖 TA 拒绝。fingerId 0..5 同一 ABI：finger 1 导出/重灌后 identify 返回 id=1，`off_361540` 按 8×fingerId 索引，存储需按手指分文件。已有 2 个子模板时导出 242,358 B（40×196）/243,810 B（196×40），重灌后 `+50116` 仍为 2、identify→0。`50136` 只是内存 `sizeof(ST_FocalEnrollTemplate)`，不是导出长度（2026-10-31） |
| D16 | 增量录入有引导阈值，HAL 必须把 -6 反馈成“移动手指” | `FtEnrollTipsTemplate_v2`：与已存子模板 `DeltaOverlap >= 80` 且 `DeltaAng < 10` 时拒绝（`ret=-1` → `focal_EnrollByImage` 返回 **-6**，日志 `should move a little!`）；模板 `area < 90` 返回 **-4**；足够不同才新增子模板（`db+50116` 增长）。实测 40×196 连续 4 次录入 rc `[0,-6,-4,0]`、最终 2 个子模板；196×40 为 `[0,-6,0,-6]`。2 个子模板后同图与 2–3 px 平移探针都 identify→0（score 54/40、71/49）。HAL 录入流程需把 -6 当作“请移动手指”继续，而不是终止录入（2026-10-31） |
| D17 | 模板删除走主机侧存储，不在 TA 内调 `FtDelTpl` | 模板与算法在主机侧（D10），删除一次录入 = 删除对应 blob 并在下次 init 时不重灌；离线每个重灌都是新进程，未重灌的 finger 库为空、identify 不会命中。TA 侧确有 `FtDelTpl` 名称串，wire 分发器 `sub_104B0` 也有 case 4/5（del user tpl / del all tpl），但那属于 TA 自身命令路径；只有将来保留长驻 TA 上下文时才需要定位 `FtDelTpl` 入口（2026-10-31） |
| D18 | 模板 blob 按 fingerId 隔离，跨手指不会误匹配 | 同一进程给 finger 0（ridge+dots）与 finger 1（随机纹理）分别录入并导出 blob（238,970 / 239,762 B）；新进程同时重灌两个 blob 后，探针 A→id 0（score 54）、B→id 1（score 56，比较 2 个模板）；只重灌 finger 1 时探针 A→-2（max 0）、B→id 1。主机存储按手指保存/重灌即可获得正确隔离，不需要额外过滤（2026-10-31） |
| D19 | 主机侧必须自行校验模板 blob；TA 导入先写库、后校验 CRC | CRC16 参数离线确认：反射多项式 0x8005（实现用 0xA001）、init 0、无输出反射/异或，覆盖 `[0, len-2)`，小端存于 `[len-2, len)`；三种几何导出 blob 复算全部一致（40×196 `0xB785`、196×40 `0xCEC0`、96×96 `0x645A`）。`FtIniAlgTplData_v1` 用头部解析出的 end offset（40×196 为 238,968）定位 CRC，**不按 `tplSize` 截断**：长度减 2 且尾部清零 → `-3`；若尾部仍是旧字节（短缓冲但内存残留）则 CRC 通过 → 短缓冲会读到声明长度之外 2 B，主机必须在调用前校验 `len == 该几何的期望长度`。40×196 篡改实验：payload 字节翻转 → `-3`；CRC 尾字节翻转 → `-3`；截断 2 B（尾部清零）→ `-3`；magic 翻转 → `-2`（`FtInitEnrollTplData` 报 headerSize 不符，库不可用）。关键：**CRC 失败发生在库已填充之后**——三种 `-3` 情形下 `db+50116=1`、`db+50118=1`，随后 identify 仍返回 0/id 0；因此主机不能忽略 load 返回值，必须预校验 magic/几何/CRC 使 `-3` 不可达；若仍出现非零 rc，应失败关闭本次认证、不得继续 identify。CRC 只防意外损坏，不防可重算 CRC 的恶意改写，blob 的机密性/完整性仍需主机存储层保护（2026-10-31） |
| D20 | 模板 blob 不自带几何/配置，主机存储必须外带元数据 | 用同一图案分别导出 40×196 与 196×40 的 blob，前 216 B 头部**逐字节相同**（magic、208、0..95 计数表、count/valid/fid/50136/accum），没有 width/height 或 bin/mask 尺寸字段；两者总长度只差在特征数（72 vs 94，238,970 vs 239,938 B），而 96 个槽的 bin/mask 同为 984/981。几何与 124 B 算法配置只在进程内 `off_361530`/资源里，加载器不校验（D15）。因此主机存储必须把 `{width,height,config-hash}` 与 blob 一起持久化，启动时先按该几何建实例再重灌；只按长度或 CRC 无法区分 40×196 与 196×40（2026-10-31） |
| D21 | 默认配置下成功认证不改写模板导出内容 | `mutate` 用例：enroll → 导出 hash A → identify（同图与 2 px 平移探针，均 rc 0/id 0）→ 再导出 hash B。40×196（238,970 B）、196×40（239,938 B）、96×96（273,962 B）三种几何的 A/B 长度与 FNV 哈希完全一致，count/flag 保持 1/1。即默认 124 B 配置的 identify 路径不会在内存库里做自适应模板更新，主机存储无需在每次认证后回写。若将来启用自适应更新配置，需重新验证并定义回写/版本策略（2026-10-31） |
| D22 | 真实接触帧已离线走通 preprocess→enroll→export→identify→主机存储恢复 | 取一次真实接触的 15680 B 编码帧（私有，不入库），用 DLL 的 AES key/iv 本地解码为 7840 个 LE16 样本（min 69/max 503，无 >4096）。在 harness 内以 40×196 建实例，调用原 TA `0x1c68` 预处理得 7840 B 8-bit 图：质量 63（阈值 30）、面积 99（阈值 90），`0x9b57c` enroll→0（count=1，极值 81/79），`0xDFD20` 导出→0（type 121，242,842 B，CRC `0x58CC`），`0x9E1F0` identify→0（id 0，score 158，RANSAC 后 158 点，overlap 7769/7840）。随后把该 blob 经主机侧 `ft_tpl_store_write` 落盘，另一进程 `ft_tpl_store_read` 重灌→0（count=1、flag=1）并对同一解码帧 identify→0。证明离线算法与存储链能接受真实传感器帧；实时 SPI/SFS wire 路径仍未执行（2026-10-31） |

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
- [x] M4 主机侧算法桥：受限离线执行 TA/引擎的 enroll/identify 算法（原 listener 假设作废）
  - 2026-10-31：工作区前提已在离线 harness 验证（16 MiB 受保护映射，原 `0x9a94`+`0xd350` 精确清零）；全镜像装载后原算法本体已可执行（D12）。同日闭合 **enroll→有效化→identify** 离线闭环（D13）：96×96 合成图录入 40/68 极值、`0x9b57c`→0、调用原 `0xE83B4` 置模板有效后 `0x9E1F0`→0，同图 score 77、±24 扰动 score 45。也已闭合 **模板导出→新进程重灌→识别**（D15）：`FtGetTplData` 导出 238,970 B（40×196），`FtIniAlgTplData_v1` 重灌→0 并置有效位，同图 identify→0。真实帧闭环已完成（D22）：真实接触帧经 `0x1c68` 预处理后 enroll/export/identify 全部→0（score 158），不再只依赖合成图。
- [x] M5 主机侧模板存储 + 一次 enroll/authenticate 闭环（不含 STRONG/HAT 声明）
  - 2026-10-31：持久化 ABI 已离线闭合（D15），存储内容可跨进程字节级还原（导出/读回 FNV 哈希一致）；2 个子模板的 blob 也能导出→重灌→识别（count 保持 2）。增量录入的引导语义（-6/-4）已记录（D16）。主机侧校验契约已定（D19）：magic/几何长度/CRC16 预校验，非零 load rc 视为会话失败；blob 不自带几何/配置，存储需外带 `{width,height,config-hash}`（D20）。参考实现 `tools/fingerprint-bringup/host-store/`（`FPT1` 容器、0600 原子写入、读取时元数据比对）已通过单测；harness `storesave`/`storeload` 在 40×196、196×40、96×96 三种几何完成“导出→写容器→校验读回→重灌→识别”，几何错配的容器被主机侧拒绝；两指容器（ridge+dots / 随机纹理）恢复后探针分别命中 id 0/1，未录入图案→-2。真实帧也已完成“preprocess→enroll→export→容器写入→另一进程读回→重灌→识别”闭环（D22）：242,842 B blob、load→0、count/flag 1/1、identify→0（score 158）。剩余工作转入 client driver（M6）。
- [ ] M6 client driver 成形（字符设备接口）
- [ ] M7 Android 指纹 HAL，Settings 里能录/解锁
  - 测试环境约定（2026-10-31 用户要求）：安卓系统侧的录入/解锁联调**使用 CrDroid 镜像**；HyperOS 仅继续用于已有 TA/QSEECOM/算法实验。

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
