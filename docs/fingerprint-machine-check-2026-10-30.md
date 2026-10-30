# 2026-10-30 指纹上机前检查

11:32–11:36 实机只读检查：当前为 Android 17 / lineage_gaokun3、B 槽，boot ID b62d537f-a298-42ef-935c-06d717f91a17。不是 HyperOS 隔离启动，不能套用 HyperOS 测试结果。内核 6.6.30-android15-8-g0a4d3c12b3 #20；boot_completed=1、SELinux Enforcing、tainted=0。电量89%，USB供电、role=device，Wi-Fi IPv4 192.168.10.239。这些只证明服务/链路状态，未验证互联网吞吐或触摸手感。

当前无加载模块、无 fingerprint Binder service；QSEECOM存在，app_get_id/app_send已导出，app_load/app_shutdown缺失。两个SPI节点均为Himax触摸/笔，不能将它们视为指纹枚举证据。指纹在安全世界的SPI路径尚未验证。

瞬时观察到显示dma_fence及触摸SPI等待线程，后续读栈均已返回空闲worker/irq等待；当前日志未找到RCU stall/Oops，不能将瞬时D状态判为永久挂死。电池温度报告0，未验证其真实性。

设备遗留fpcmd模块vermagic为7.2.9-gaokun3+；初步按uname判为不适配的推断已撤回，见下文版本伪装勘误。不尝试强制加载。CICD构建树gk3-kernel-72y导出正式app_load/shutdown和listener接口；其vmlinuz.efi与本机/private/tmp/gk3-vmlinuz-fp.efi一致，SHA256 9c6cb6794d5ab6aecf7373a3a6afe2c8daf39a65355bcef6d6b8bcdfd67d64b2。存在产物不代表当前已启动它，也不代表M2已通过。

下一步先固定只读0x8031探测的request=12、response=16字节，并保留失败状态。不能用裸SMC绕过正式内核接口；listener循环上限不能中断不返回的SMC。待独立实验内核启动并核实接口后，再执行一次只读通信。仍未实现指纹录入/认证/HAL。

证据：~/.cache/fingerprint-20261030-check/baseline.txt。当前没有刷写、插入模块、触发LOAD/SEND，也没有改默认启动项。

## 实验修正与首轮结果（11:39）

用户确认在设备旁。新独立B槽探针启动项 `gaokun3-b-fpread.conf` 复用已核验Image.fp及B槽DTB/ramdisk，默认项仍B原内核。重要勘误：uname和/proc/version受版本伪装影响，不能用显示的6.6.30断言实际内核版本或模块不兼容。新启动dmesg明确为7.2.9-gaokun3+ #21，LOAD/SHUTDOWN/listener符号均存在；旧启动确实缺少LOAD/SHUTDOWN符号。

新只读模块编译成功，SHA256 13d6f94f89174a27c4f4ed91a95e2b604028096423ce192fe4c1b89811e74651，无裸SMC依赖。固件SHA256 b081543c7b6ae4360969007836747601e8c48ec48a8335a5479e45ec0e34355a。boot ID e99eb3b2-6e41-4a13-b8f7-bc76f378d9fd。

第一次唯一probe：8MiB分配成功，对齐物理地址0xf6400000；LOAD返回app_id=5。紧接LOOKUP("fingerprint")返回-2，未执行SEND或SHUTDOWN，探针保留全部共享内存和模块引用并拒重试。ADB响应正常。已发起普通重启恢复，不能把LOAD成功写成M2通信成功。结果位于probe-result.txt；后续须核实真正名字或LOAD返回ID的权威语义。

## 第二轮：M2通过

独立重启后 boot ID 89e418d1-85e5-417a-876f-4d7aec7e5416。历史#125已明确记录LOOKUP名称不可查而LOAD id可用，故修正版仅将LOOKUP -ENOENT作为例外；其他错误仍停止。提取实际run_probe的ASan/UBSan控制流测试通过。修正版模块SHA256 71d266288f76694e59416a75499a9d8bcb6b7e344bffe9d548d3556a5a2d2738。

真机probe_exit=0，LOAD app_id=5；实际请求12字节/响应16字节，ACK为 a5 5a 00 00 00 00 00 00 00 00 00 00，返回状态0。SHUTDOWN成功，缓冲释放，模块引用计数回0；随后正常rmmod完成，SELinux保持Enforcing。证据probe-v2-result.txt、after-v2.txt。

M2（真实可信应用只读通信）通过，不能扩展为SPI已初始化或指纹录入/认证可用。当前继续运行实验内核；默认启动项未改，下次正常重启回原B内核。独立探针启动项保留以便后续受控测试。

## M3前置复核

只读pinctrl观察：GPIO181 input high、GPIO185 input low，均func0/2mA/pull-down、UNCLAIMED。尚未确认这些电平的正确含义，未写GPIO、未添加SPI节点。证据gpio-readonly.txt。

复核真实TA指令发现旧协议表不可信的部分：0x8026进入EnrollByImage/ForceEnrollByImage，0x8027进入IdentifyByImage；不能当作“版本/模板列表”只读探测。0x8023仅写TA内0x226178布尔配置，不能称作已确认GPIO复位。此次均未发送。M3仍需从Windows调用链闭合MCU帧及初始化前提，不能猜测命令字节。

## SC版本查询：协议闭合、实机未通过

Windows QSEE对象/vtable分支及调用参数已闭合（见fingerprint/sc-version-wire-2026-10-30.md），固定MCU请求02 00 01 30 31；TA写请求17/响应12，20ms后TA读请求12/响应81。新增独立qcom_qseecom_fpversion.c，仅此固定请求；实际query_version提取式ASan/UBSan测试及5项离线解析测试通过。模块SHA256 fabc7478e4f1ae90cc441c57633165f32e30451f957630d5454247bcd5fd7c3b。

同一实验boot下，LOAD得到id6（使用实际返回值，无固定id假设）；8031 ACK及状态0通过。版本查询读回69字节全部00，原哨兵a5被改写，但MCU长度0/状态0无效；探针返回-EPROTO（-71），SHUTDOWN成功、全部缓冲释放，随后正常rmmod。不能据ACK或被改写的缓冲断言SPI硬件成功；TA还可能预清零响应。

当前需核对真实供电/复位与SPI配置、厂商完整前置序列；未擅自翻转GPIO，未发送录入/模板写入命令。证据version-result.txt、version-cleanup.txt。M3/实际芯片识别仍未完成。

## 复位后SC版本查询通过（12:04）

ACPI及Windows真实调用链明确GPIO185复位输出。新用户态helper `fp-reset-version.c`严格核对gpiochip4标签f100000.pinctrl、230lines、185无消费者，以及debugfs实际input/low/func0/2mA/pull-down。v2 line info仅有INPUT，不代表实际没有bias；不依赖该标志恢复继承配置。

请求GPIO185输出1+pull-up，读回1，等待10ms后输出0并读回0，等待400ms后运行固定版本探针。这里复现同步Linux脉冲，不把Windows异步提交误称为精确物理时序。此次TA在复位后加载；不声称完全复制Windows所有初始化顺序。

LOAD返回id7，8031状态0；真正MCU回应：
`02 00 29 04 46 54 39 37 36 39 5f 41 50 50 5f 53 54 5f 53 50 49 5f 45 4e 43 5f 53 56 30 2e 31 5f 32 30 32 33 30 33 31 35 32 30 33 35 50`。
BE长度41，状态04，40字节版本载荷为 **FT9769_APP_ST_SPI_ENC_SV0.1_202303152035**，XOR=0x50通过；后面的填充字节不计入版本/校验。主机独立解析再次通过。

probe及helper退出0，SHUTDOWN成功，secure_pending=N；GPIO明确设回INPUT|PULL_DOWN、释放后debugfs及v2复核input/low/pull-down且无消费者，模块正常rmmod。SELinux Enforcing、boot_completed=1。

结果证明复位配置下的真实SPI/MCU版本通信已通过，不能代替数值芯片ID、取图、录入、识别或Android HAL验收。未写模板。精确版本包含ENC，后续图像协议/加密含义仍须按实际调用链核实。

证据reset-version-result.txt、reset-version-decoded.json。模块SHA256 0bcbfa86626218fcfd44bba08e662e6038b7b808c2913279c7070b4dbace2e11；helper SHA256 38799917642f857bd5a70313b581ae1df0f85a801cdc9941637e7a70dd8cc099。若secure_pending非N/读失败，helper保留GPIO输出0及FD等待重启，不会自动复位仍在运行的TA。

## 三项只读尺寸/状态查询通过

Windows ReadInfo_directly真实帧闭合后，在版本成功之后读取index14/count1、15/count1、20/count2。写TA请求19/响应12，读请求12/响应18、18、19；写读间无延时，每项读后1ms。实际query_info提取式ASan/UBSan测试通过；GPIO helper新增申请前secure_pending=N检查、提前设置保持信号策略，实际C主体36错误路径测试通过。

真机LOAD id8、版本一致；三条MCU回包分别为 `02 00 02 04 28 2e`、`02 00 02 04 c4 c2`、`02 00 03 04 a5 5a f8`。尺寸项40与196、乘积7840；设备状态LE16=0x5aa5。长度/status/XOR全部通过，主机独立解析复核。宽高名称还不确定，不擅自命名。info_probe_exit=0，SHUTDOWN成功、secure_pending=N、模块卸载，GPIO恢复input/low/pull-down且无消费者，SELinux Enforcing。

仍未触发取图、录入、模板写入或认证。已定位厂商芯片初始化中的power/config写，但语义尚待闭合。

## 初始化前配置快照：index01响应长度不匹配

五项只读探针（追加01/30）实际C ASan/UBSan与模块构建通过。复位后LOAD id9、版本/14/15/20均再次通过。请求01/count1时，读窗口6B得到 `02 00 03 04 91 93`，声明L=3即两字节payload，超出原假设的一字节字段，校验字节未包含在窗口。严格长度检查返回-EPROTO，后续30查询没有执行。不能把91或9193当作已验证配置值，也不能将该回包称校验成功。

SHUTDOWN成功、secure_pending=N，helper确认GPIO恢复input/low/pull-down，无consumer；模块已卸载。没有配置写、取图或模板变更。证据config-snapshot.txt。下一步核对字段长度语义，并以独立受限read容量取得完整带校验回包，不删边界检查。

另已闭合取图传输窗口：当前40×196尺寸对应15680编码字节、TA响应15697字节，旧4KiB缓冲不能用于取图。离线固定帧/响应工具及3项测试通过；尚未采图。

## 受限扩大响应窗口：取得完整原始应答

保留MCU请求count1，仅将01/30的SPI读容量扩为4字节（总MCU9/TA21）；允许1..4B实际payload并严格CRC，前三项长度要求不变。真实C ASan/UBSan测试覆盖全部容量和错误分支后上机。

LOAD id10、版本及14/15/20通过。01和30均得到 `02 00 03 04 91 93 05 05 05`，实际帧7B，payload9193、CRC05，剩余05为尾部。主机独立解析通过。不能据此称两个配置原值均为9391，不能单凭其与FT9391名称一致就判数值chip ID验收；目前只能证明两个请求得到相同校验有效应答，默认回应/索引语义仍待定。

wide_exit=0，SHUTDOWN成功、GPIO恢复input/low/pull-down，无消费者，模块卸载，SELinux Enforcing。证据config-wide.txt、config-wide-decoded.json。尚未执行配置写、取图或模板操作。后续正常初始化应按厂商已核实的写应答与状态迁移验收，不能把这两个值用作虚构的配置回滚值。

## 固定厂商初始化：四写应答通过，89收尾未通过

新增fp-init-sequence.h：固定87配置写、88状态、89睡眠收尾及8e条件唤醒；每步校验长度/status/XOR，50轮sleep及10轮wake有界。实际header ASan/UBSan测试通过91项注错及时序/边界检查。模块SHA256 24e8dfbb6acfa6b236f3b9c46ec138a1c134e526713124ec6fa8616f519fd014。

实际运行版本、尺寸、ready状态均通过；87 01=01、30=bb、1f=01、1e=01全部得到有效 `02 00 01 04 05` MCU应答。01/30之后ready仍5aa5；1e之后首次3ms轮询也得到有效5aa5。

下一条89 1d01（读6B）返回 `f8 f8 f8 f8 f8 f8`，不是有效MCU帧，流程严格停止，未执行末态判断或唤醒，不能声称睡眠成功。SHUTDOWN成功、secure_pending=N、GPIO恢复。原Windows此处外层最多重发89三次，当前诊断仅试一次，属于有记录的覆盖差异，下一轮需核对并按厂商有限重试验证；通信uncertain不能盲重试。

随后卸载init模块，使用先前通过的只读info模块复位复测：版本一致、尺寸40/196、设备状态5aa5，全部通过；再次SHUTDOWN、卸载模块并复核GPIO恢复。说明设备可恢复到可通信ready状态，不证明所有配置持久性或原值回滚。没有取图、录入或模板操作。

证据init-result.txt、init-recovery.txt；当前仍实验boot89e418d1…，默认启动项未改。

## 底层时序修复与三次89复测

发现并修正先前遗漏：Windows TA_Commication在0xc360经Sleep IAT为每次IOCTL返回后增加10ms；之前只考虑上层延时不完整。现query_version、query_info、初始8031和fpinit_exchange每次成功app_send后均加10ms；LOAD/LOOKUP/SHUTDOWN不加。原上层延时保留。SCM失败诊断立即停止（不模拟Windows失败尾部等待）。actual query/header测试重新通过，init头覆盖134注错及三次重试边界。

复测四项87写ACK及前置ready依旧通过，但89的三次完整协议尝试均为f8×6，ret=-71、uncertain=0。故真实底层延时遗漏已修，不能将其宣称为89失败根因或已修复睡眠。失败后未执行finalwake；SHUTDOWN和GPIO恢复成功。

随后明确复位，并用旧已验证只读info探针确认同版本、尺寸40/196、ready5aa5；卸载模块、GPIOinput/low/pull-down、SELinux Enforcing。证据init-delay-result.txt、init-delay-recovery.txt。尚需区分89触发的无响应状态与固件协议差异，不能以无效帧解释为已休眠。

## 89失败后的只读诊断：MCU仍返回ready

新增单次direct20只读诊断，仅在89三次已完成SCM但协议失败后执行，保留原始初始化错误；诊断通信错误仍置uncertain以保留资源。实际header测试通过162项注错，覆盖诊断成功/坏帧/SCM错误，不能靠诊断成功改判init成功。

实机89三次仍f8×6，随后同一TA会话、没有复位/唤醒/额外配置写的direct20回包为 `02 00 03 04 a5 5a f8`，长度/status/XOR有效，state5aa5，diagnostic_ret=0。原结果仍-EPROTO。说明失败后普通MCU通信仍可用，并非能据此认定整条SPI或MCU已失联；也没有证据休眠完成。

SHUTDOWN成功、secure_pending=N、GPIO原状态读回通过、模块卸载、SELinux Enforcing。此次已由诊断明确ready，不再为重复验证另外执行一次复位/版本probe。尚不声称配置原值回滚。证据init-diagnose.txt、init-diagnose-cleanup.txt。

## 主动单帧取图链路通过，图像有效性未通过

新增固定fp-active-scan.h和active_scan只读模块参数；使用厂商OnScanImage的ST00/3a状态查询/条件raw00唤醒、mode6写87 00=6c、最多7轮ready等待、固定ReadSram15680字节流程。不是把失败的sleep判成功；完整驱动的休眠/事件/录入仍待完成。

主模块response扩到16384，完整物理区间检查低4GiB，编译期拒4KiB误配；active_scan要求实测尺寸严格40/196，协议通过后才置capture有效。debugfs capture0400，mutex确保整个probe与shutdown结束才读，uncertain时作废。实际header测试通过80项故障与边界检查，独立审查确认全部时序/CRC长度。

真机ST返回aa55、扫描配置有效ACK、ready5aa5；读取15680编码字节，精确L/status/XOR通过，SHUTDOWN成功。随后通过exec-out取得精确15680B到主机mode0600临时缓存，无图像dump入内核日志。用户接触条件尚未答复，因此不能称已确认空载或手指图像。

采用已由原DLL合成向量验证的AES-128-CBC解码，统计7840个LE16样本：min0/max36867，超过4096的样本998个，非均匀；不符合Windows6f00初步检查（最多100个超限），不能宣布有效图像/指纹取图验收完成。数据布局、配置、实际样本条件仍需核对，没有录入或写模板。

模块正常卸载、GPIO恢复input/low/pull-down且无人占用，SELinux Enforcing。证据active-result.txt、active-manifest.json、active-stats.json；编码数据仅在私有缓存active-frame.bin，不入库。下一步对比原DLL解码结果及图像数据布局，再做接触对照测试。

## 首帧进一步分析与AES交叉验证

首帧解码异常集中于前998个LE16样本，后6842样本全零；非零部分1996B，末尾13684B全零。按BE16解释所有样本可落到4096以内，但Windows6f00实际用小端ldrh，不能通过交换字节序使检查变绿。1996=2000-4仅为线索，未证明包头/有效尺寸。

使用原DLL的0x1c10在受限主机子进程处理同一私有编码帧，输出与PyCryptodome逐字节一致；合成向量也通过。因此此帧异常不来自这两个AES实现不一致。未输出密钥/像素，临时解码文件已删除；不意味着采集配置/数据布局已经正确。

下一对照是在active路径前补上厂商常规chip_init的01=1和30=bb，两个合法ACK与前后ready均须通过。它只检验必要前置假设，不把仍未通过的sleep流程视作已解决。

## 补01/30前置后的第二帧对照

active路径加入严格ready→87 01=1→87 30=bb→严格ready，再执行ST/mode6/SRAM流程。该实验明确没有完成仍失败的sleep步骤，不宣称完整chip_init通过。实际headers测试144注错通过，两个配置或状态错误均不会进入扫描。

实机两写ACK、STaa55、扫描ready5aa5及15680B数据长度/status/XOR再次通过，SHUTDOWN成功。第二帧与首帧编码字节不同，独立AES统计min0/max36611，仍998/7840样本>4096、非零解码字节仍1996；basic_checks_passed=false。因此两项缺失前置不足以解释/修复布局异常，不更改字节序或尺寸来制造通过。

模块已卸载、GPIOinput/low/pull-down复核通过，SELinux Enforcing。用户接触条件仍未确认，不能把两帧差异归因于手指。证据active-configured-result.txt、active-configured-stats.json；编码帧active-configured-frame.bin保存在主机mode0600缓存，不入库。下一步核对采集完成/数据范围和接触对照。

## 重要勘误：同版本MCU固件与Windows接口并不等价

独立提取并复核DLL内嵌ST Cortex-M固件（只在主机cache，无刷写）：长度0x12c04，flash映射08060000，Reset08062131，SHA256 9cc81095c955175045e178f9de99960306a07c45a16d4385b0854c81b1aba79b。版本字符串与实机一致，不等于已读取真机flash证明字节完全相同。

真实处理器080606d0的MCU87路径不读取payload：分配/清零应答，cpsid/cpsie之间无配置写，直接返回空ACK。08060764–076a的88/index20直接写常量a5/5a。因此历史实验中的四项87“ACK通过”和5aa5“ready”必须降级为协议应答证据：不能证明配置生效、MCU真实睡眠/awake、扫描就绪。历史原始日志保留，不以更改日志掩盖这一纠正。

这也解释补01/30后布局未变；但尚未单独证明其他全部异常原因。01/30读到相同9193来自未匹配index默认读取对象u16字段，不能作为配置原值。固件命令表尚未见89，默认分发路径还需核实后定论。对采集的有效证据仍只是完整编码帧及CRC，图像质量仍失败。

下一阶段直接分析MCU数据对象、采集循环和88/index1d的标志写入；该查询有副作用，不能再称纯只读状态。停止把固定compatibility marker用作功能验收。

## 用户指定空载：真实1d帧状态基线

用户明确“暂时不方便，先测空载”。本轮未要求接触，不执行有手指对照。新status_only固定查询88/index1d/count1，最多10次，有效低bit1即停，其余间隔100ms；不读取SRAM、不发87/89。该查询设MCU的主机确认标志+e，不能称无副作用读寄存器。实际header测试80项故障注入通过。

实机10次均收到 `02 00 02 04 00 06`，raw1d=00，CRC有效，MCUframe_available=0；完成后SHUTDOWN成功，模块卸载、GPIO恢复、SELinux Enforcing。这只覆盖此次约1.5秒窗口，不推断所有空载场景或未来无误触。

同版本固件证明+c是完成真实传感器事件填图后置1、初始化0、8b消费后清0的标志。这个空载结果表明必须以真实帧标志控制读取：没有标志时拒绝导出旧缓冲，而不是将以前任意读出的完整编码帧认作新图像。证据empty-status.txt、empty-status-cleanup.txt。正在将这条门槛加入实际capture路径。

## 空载采集入口回归：拒绝旧缓冲导出

实际capture路径已改为只轮询88/index1d，最多10次、间100ms；bit0真才读取一次8b，原87/ST/fake20控制路径删除且无回退。无帧返回-ENODATA，旧data清零、valid=false；184项真实header故障测试及容量编译检查通过。模块默认只做version/info；历史不适配init不再默认执行。

用户指定空载下实机10次均raw1d00，capture明确不读SRAM，返回-61。SHUTDOWN、GPIO恢复成功。首次导出验证误用了adb exec-out（无法按预想区分shell退出码/错误流），主机断言失败，不作为设备导出结果证据。为完成该项验收，仅重跑一次同条件空载probe，改用adb shell v2复核：退出码1、stdout0字节、stderr明确No data available。由此确认无帧时不会导出旧编码数据。

最终模块已卸载，GPIO input/low/pull-down且无consumer，SELinux Enforcing。证据empty-framegate.txt、empty-framegate-export-repeat.txt、empty-export-check.json。空载短窗口的拒绝行为通过，真实手指触发→完整图像尚待用户方便时验证；不声称指纹录入或解锁可用。

本次frame-gate模块SHA256：`dbce66b694d1c000253dc2ecdf642d27a39aae024fd3f7e83787d237baf27247`。


## HyperOS R21系统配指纹候选内核：用户接触对照

用户现已方便并确认手指动作；不再仅有空载条件。用既有r19-fp一次性入口启动，ESP EFI SHA4b846c…cdd6核验，当前boot `e4417420-7d4d-4235-a981-01a173145e74`。入口build标签仍r19-fingerprint-kernel，但super为既有R21且HAL SHAab794e…48d4；默认Android B未改。匹配frame-gate模块dbce66…7247，TA SHA b08154…355a，SELinux Enforcing，USBdevice。

1. 开始明确无人接触：LOAD id5，版本/尺寸通过，十次raw1d=00，ENODATA，未读SRAM。
2. 用户回复“放好了”，保持轻放：LOAD id6，版本/尺寸通过，十次raw1d=00，ENODATA，未读SRAM。此短窗口没有证明手指触发新帧。
3. 用户回复“开始了”，每秒轻放/抬起：LOAD id7与8031 ACK通过，但版本查询全零，EPROTO，尚未进入帧状态轮询。不能将这轮称为有手指采图失败或断言接触导致通信异常；手指与MCU复位/启动重叠，时序尚未隔离。
4. 用户确认“已移开”：仅version/info恢复检查LOAD id8、相同固件/尺寸通过，exit0。所有四轮SHUTDOWN成功、secure_pending=N、GPIO恢复input/low/pull-down；最终模块卸载，crash buffer为空、同boot、Enforcing。

本轮未导出指纹图像、未录入、未写模板，没有取得有效真实帧。证据`~/.cache/hos-r21-fingerprint-contact/`。下一步应把MCU复位/版本确认与手指接触时间窗分开，先明确采集初始化/事件触发条件，再做有界接触对照，不能仅反复读旧SRAM或延长无效轮询冒充修复。


## 初始化后独立接触窗口：状态回包异常，接触时点待确认

新外部模块SHA7a9be7db5fae903c81cfdf6b0c26d67d8e523534a7f452ff5f209bcd4c14fc88增加contact_wait_ms只读参数，默认0，非零仅active_scan且<=15000；版本/信息成功后才等待并进入原framegate，未新增MCU命令。真实源码提取ASan/UBSan通过；匹配R19FP内核外部模块构建、导入/参数权限及关键内核文件保护核验通过。

用户回复方便后，使用已有oneshot指纹内核启动，保留R22 super；boot64b3ea5c-0428-4d05-ba5a-bda6586d4dd3，实际库仍a24232…2185。本轮仅一次active_scan=1 contact_wait_ms=15000。LOAD id5、8031、固件版本与信息通过；t149.494642打印ready-for-contact，主线据实际标记提示用户再接触；t164.576881进入framegate，首1d回包05×6，不是有效帧，返回EPROTO、未读SRAM。未取得图像。

截至记录时用户尚未确认实际接触发生在窗口内，因此此轮接触条件待确认，不能写成已证的手指引发故障。05与之前01/30回复末尾CRC相同只是SPI填充线索，不证明休眠、忙碌或硬件故障。下一步应先获知接触/移开时点，再做同样15秒等待的空载对照，区分等待时间与接触变化。

SHUTDOWN成功，secure_pending=N，helper恢复GPIOinput/low/pull-down并核验，模块已卸载。Enforcing、同boot、crash buffer为空。证据~/.cache/hos-r22-fingerprint-contact-window/runtime/。


## 用户要求重测：明确空载的等待时间对照

用户回复“已移开”后，保持同boot64b3ea5c进行一次active_scan=1/contact_wait_ms=15000：t526.922548前版本/信息均通过，t542.243287首1d仍05×6，EPROTO。SHUTDOWN正常，模块卸载、GPIO恢复。因此无须手指接触，也能复现长空闲后的协议失败；前次未确认时点的窗口不能作为接触触发证据。

随后同模块默认contact_wait_ms=0，重新复位后版本/信息通过，t670.374892–671.910983连续10个合法raw1d=00，返回ENODATA且没有SRAM读取，SHUTDOWN与GPIO恢复成功并卸载。这证明本次复位后立即轮询可恢复通信，不证明硬件不存在其他问题。

下一候选改为受限contact_poll_count窗口：初始化后立即持续合法状态查询，不插入15秒idle；任何协议或SCM错误仍立即停止。MCU固件已有idle计数阈值路径，但实际时基与睡眠因果尚待完整闭合，不把本对照直接命名为“15秒休眠已证”。证据runtime-empty-delay/{result.txt,zero-delay-control.txt}。


## 首次确认接触触发的新帧：持续状态查询窗口通过

用户要求重测并明确已移开。新模块SHA12d7d780ae97df644a6663747f8c739844693f5171653e8a3bd2d514fc10ba3f增加contact_poll_count=10..200：只在active_scan且与idle wait互斥时可用；默认仍原10轮。初始化后立即做原1d查询，每次成功且无帧才按原100ms间隔继续；错误立即停止、有帧立即读取一次SRAM。原184项注错保留、新增测试及匹配内核外部模块构建/保护核验通过。

同boot64b3ea5c先做明确空载150轮：150个合法raw1d00，运行约26.54秒仍保持通信，ENODATA、未读SRAM，SHUTDOWN/GPIO恢复。随后重新复位空载初始化、开启200轮窗口，主线观察到版本/信息与第一笔合法状态后提示接触。用户明确回复“放好了”；第1–120笔raw1d00，第121笔于t1301.526389变01，立即读取单帧，t1301.628479确认15680B framing/CRC，SHUTDOWN成功validation0。

以shell v2导出恰15680B到主机私有cache，模式0600/目录0700。原已交叉验证的AES解码得到7840个LE16样本，min69/max503、非均匀、>4096数量0，Windows初步数值门槛通过。与旧未检查ready的无效缓存不同，本轮同时有真实帧标志、用户接触确认、协议及基础解码证据。尚未验证完整图像处理、二维方向/质量分数、模板录入或认证；不能称Android指纹解锁已经可用。

模块已卸载，GPIO input/low/pull-down复核通过，secure_pending=N，Enforcing、同boot且crash buffer为空。未录入或写模板。证据~/.cache/hos-r22-fingerprint-active-window/runtime-comparison.json及runtime-empty/runtime-contact；私有编码帧不入库，不将像素或密钥输出日志。


首接触帧随后用原Windows DLL ARM64解码例程在受限主机子进程交叉验证：16/32/256/15680B合成向量通过，该私有15680B捕获的原DLL输出与Python AES-128-CBC逐字节一致。未输出像素或密钥，临时解码文件按验证器流程清理。该检查闭合解码一致性，不扩大为完整厂商图像处理或认证验收；证据runtime-contact/original-dll-crosscheck.json。


## 原厂预处理的离线Linux对照

在已有arm64 Linux容器中执行固定TA0x1c68与Windows0x6f00及其真实算法helper，仅替换分配/释放/内存/日志等宿主边界；未调用设备或模板接口。两次17项记录完全一致，真实接触帧及渐变/0..4096边界输入均生成7840B逐字节一致的输出，输入16位图会被原地修改。常量输入不写输出；超量程、分配失败场景不计作成功。第3/4/5次分配失败空指针故障，第6次Windows返回0但输出未写，说明不能仅按返回值判处理成功。

早期macOS重放受平台x18保留寄存器影响，偶发故障结果降级，不能归为TA缺陷或用于稳定性结论；正式证据使用Linux v2/v3。容器与临时映射均清理，明文只在内存/管道中，没有新增设备采集或输出像素/密钥。详见fingerprint/preprocess-original-code-replay-2026-10-30.md；原始证据~/.cache/fingerprint-preprocess-replay-linux-v{2,3}/。
