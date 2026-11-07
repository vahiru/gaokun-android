# crDroid FTE7001 候选 FP1 / FP2 构建与实机记录

2026-11-01，用户授权构建、上机与验证。当前 FP2 已完成构建、永久部署、Enforcing 启动和服务检查；会话重试与固定初始化探测通过，真实录入/认证尚待完成，不据此声明可解锁或达到安全认证标准。

## 候选与实际安装范围

- crDroid 17.0 / Android 17，vendor 标识 `1.0.0-rc.3-fp1`。
- boot/vendor 部分 OTA，manifest 确认 `partial_update=true`，分区列表恰为 boot、vendor。
- OTA 签名公钥与设备 `/system/etc/security/otacerts.zip` 中的信任证书匹配。
- 部分 OTA 保留原 system/system_ext/product；实际系统指纹仍为原 20261030 版本。
- OTA 禁用 postinstall：该候选的唯一 hook 是 vendor 的 ESP 解包脚本，已在设备上手工完成同一启动文件解包、cmdline 同步和逐文件哈希核验。既有 gk3boot 版本未变。
- 框架识别 sensor 0，强度为 CONVENIENCE（4095）；无 HAT 签发和可信 MAC 验证能力，不提供 Keystore 加密认证。

## 构建与修正

原 Android 内核树保留，在独立副本中应用 0113（现编号 0212）。0050 在基线中已存在。配置差异仅新增 `CONFIG_GAOKUN3_FPCMD=y`，DTB 与原已知可用 B 槽一致。基线关键文件哈希在构建前后未变。

内核构建、实际 EFI 嵌入配置检查、AIDL V4 服务编译/链接、`selinux_policy` 和 target-files 构建均通过。构建中修正了多余 lambda 捕获、构建机缺少 ripgrep 的配置检查，以及新源码同步与旧对象时间戳重叠导致的增量对象复用；最终相关源码强制重编。打包工具使用现有 JDK 完成签名。

核对真实 AIDL 框架后补充 HAL 失败计数持久化：5/10/15 次失败时锁定 30 秒，20 次永久锁定；计数随模板数据库保存。进程重启在 timed 边界重新等待完整 30 秒，不缩短锁定。PIN token 仍仅做结构检查，不能验证 MAC。认证持续等待取消，未继承桌面后端的 30 秒总超时。

离线 core/matcher 测试启用 ASan/UBSan，通过模板往返、权限/损坏/截断/符号链接拒绝、AES 有效解码/无效帧拒绝、合成同图/异图匹配和锁定状态检查。合成分数不代表 FAR/FRR。

## 产物摘要

| 产物 | SHA256 |
|---|---|
| 签名部分 OTA | `242ddf6dbaecc0c7368d614b3cd556ba674c7bcf8587d742228751ad3ffb145f` |
| boot.img（29364224 B） | `90443bbcdfc6b0d650a058abb8d7f0d6c85356ba0b3a220c04fdf8c998d5fb17` |
| vendor.img（278888448 B） | `a415e446aef9cfd5bab73f143cce4ff861676e6c3b23c156c4f18f2d27fe77fa` |
| EFI kernel（16024064 B） | `a0b29ecd3963dab7934e0d9395f194376c0bf93aefa1f8983a963b2c1a811faa` |
| DTB | `82fa1fa3242434ebdb602b7ba071082e77d331c5f49c1e7ad27ca0e23c540373` |
| ramdisk | `3658b0bd89b9986d95089aab83dd6fa10d0006bdf966b00951f3ee790b6ffaa7` |
| 运行中的 HAL | `9ac5dd7092f7f806ee6e3cec7dfaeebd3f360803d529bcf26797e3ac7972022b` |

## 备份、空间和部署

部署前独立备份两槽 boot、misc、ESP 和完整 super；每份 gzip 解压后核对原始分区 SHA256。首次 super USB 流备份因 USB 重连截断，未作为有效备份；改为设备后台生成并经 Wi-Fi 拉回，最终 12884901888 B 原始 super 哈希为 `08d5e6b8ccaa53f07d95725ab27d0ff7515b96f9309ad62b15152e52e783bd9b`。

构建盘最初剩余 11732811776 B。仅清理两份已核对哈希的重复输出缓存：旧 super.img 与保留的 super.img.zst 解压流一致；旧 OTA 与 sec1-artifacts 中保留副本一致。清理后实际可用 16570867712 B。target-files 启动前另记录 staging、镜像副本、生成重叠和 2 GiB 余量预算；使用较小的部分 OTA 输入，未增加完整 super/OTA 大副本。

ESP 原可用 12902400 B。先核对 A/B ramdisk 字节相同且有 boot/ESP 备份，暂移除 A 槽的重复派生 ramdisk 缓存；实际可用变为 26062848 B，再用已有解包器逐段原子替换 A 槽 kernel/ramdisk/DTB。完成后实际可用 12890112 B。B 槽文件和其余启动入口内容保持；仅 A 槽启动文件及其标准入口改变。

update_engine 报告成功后，核对 boot_a 前缀与新 boot.img 一致，boot_b 全分区哈希保持不变，将 active 恢复 B，default 保持 B，OneShot 指向 A 槽标准直连入口后重启。后续 Virtual A/B 合并可能改变旧槽可用性，不能把首次启动前的回退设置当作永久回退保证；完整分区备份仍保留。

## 首次启动检查

boot ID `755e73d9-4f78-474e-97f4-bac06b5d395d`：

- A 槽、boot_completed=1、vendor 标识 FP1，SELinux Enforcing。
- 实际 dmesg 内核 #21，编译时间 `Sun Nov 01 07:20:27 UTC 2026`。
- 运行中的 vendor_a 原始哈希与 payload 目标一致，TA 哈希与交接包固定值一致。
- `/dev/gaokun3_fpcmd` 和 `/dev/gpiochip4` 均为 system:system 0600，专用 SELinux 标签正确。
- HAL 运行于 `hal_fingerprint_default`，AIDL IFingerprint/default 与框架 fingerprint 服务均已注册，硬件 feature 查询为 true。
- 框架报告 sensor 0、模板 0，HAL deaths=0；初始未发现指纹相关 AVC 或 crash buffer 记录。
- thermal HAL ready，读取有限温度；USB adb 与持久 TCP 5555 正常。
- 用户已完成首次图案解锁，进入真实录入；后续发现固件加载权限错误和会话重试引发的框架崩溃，详见下节。

详细构建日志、分区备份、签名/manifest 校验、ESP before/after 哈希表及 update_engine 终态记录保存在维护者本机 `~/.cache/crdroid-fingerprint-20261101/`，构建端目录 `~/fp-crdroid-20261101/`。不包含采集的指纹原始帧；后续模板保留在设备受限的 fpdata 中，不导出。

构建及打包结束时，构建盘实际可用 5896716288 B，仍高于 2 GiB 余量。已核对无其他 soong/ninja/make/java 构建任务，构建机已回读确认 deallocated；设备端等待用户解锁，不导出或猜测 PIN。


## 首次录入失败与修复验证（进行中）

首次进入录入时，`kernel_read_file_from_path(READING_FIRMWARE)` 运行在 HAL ioctl 调用者上下文，SELinux 拒绝 `hal_fingerprint_default` 对 `gaokun3_firmware_file:system firmware_load`。已在源码补充该精确权限，并以 KernelSU 临时规则用于本次诊断；系统仍为 Enforcing，临时规则不跨重启持久化。

16:32:02 再次进入页面时，`FingerprintHandler` 在 `FingerprintStartUserClient.startHalOperation → IFingerprint.createSession` 收到 `IllegalStateException`，导致 system_server 退出。boot ID 未变，确认为框架软重启。根因是 HAL 在 HW_UNAVAILABLE 后仍保留旧会话，再创建会话返回 EX_ILLEGAL_STATE；框架此路径仅捕获 RemoteException。当前已停止旧 HAL 和自动重试。

候选修复：硬件不可用时静默退役会话；新会话替换旧会话而不抛 IllegalStateException；不可用的旧会话返回 DEAD_OBJECT；保留旧 worker 至回调结束，避免回调重入时析构自身线程；共享硬件互斥量串行旧操作取消/关闭和新操作启动。新增 Android 会话回归测试覆盖重复创建、硬件错误回调内立即重建、关闭回调内重建。编译和设备运行结果待补充，尚未声明录入成功。


会话修复版 Soong/SELinux 构建完成；设备 100 轮共 300 次会话回归全部通过。
HAL SHA256 `832f0e73c4493dc030458d008d9cbb887859d4b91c70a0fe9ea6d12b1db88792`。
以单文件 bind mount 临时替换 HAL；因来源 /data 挂载的 nosuid 标志阻止 init 域转换，
仅对该单文件 bind mount 清除 nosuid 后正常启动，没有对整个 /data 或 /vendor 重挂载。
HAL 运行域仍为 `hal_fingerprint_default`，Enforcing。用户重试确认不再软重启，但提示错误。
日志为 INIT 的无效传感器帧头（-EPROTO），system_server PID 18879 未变。

固定初始化探测 `tools/fingerprint-bringup/android-init-probe.c`（静态 arm64 Linux 二进制）
停止 HAL 后单独运行：LOAD 成功，GPIO185 配置 OUTPUT|BIAS_PULL_UP、高10ms→低400ms，
保持 GPIO 所有权并读回；INIT、PROBE、UNLOAD 全部成功。没有采集或导出图像。
相比原 HAL 增加了明确上拉和查询期间持有 GPIO，当前不能单独区分两项的贡献。
因此将这套已验证顺序整体移入 HAL，shutdown 后恢复 input/pull-down，待真实录入重试。
Linux 主机模拟 Transport 测试已通过：初始化/读回错误清理、持有上拉与关闭失败保留。


GPIO 修复版 Soong HAL/会话测试/SELinux 构建成功，设备再次通过 100 轮共 300 次会话测试。
当前临时运行 HAL SHA256 `9d8853be00a44b2eff56db6bdbb93d7099e28d94b3517206018d3dc4802cccf0`。
精确补充 GPIO SET_CONFIG/GET_VALUES 两项 ioctl 扩展权限，Enforcing 未改变。
boot ID 保持 `755e73d9-4f78-474e-97f4-bac06b5d395d`、system_server PID 18879。
该修复目前仍为单文件临时 bind mount 和临时策略，重启会失效，尚未固化到 vendor 镜像。


## FP2 永久安装与当前状态

FP2 vendorimage 构建成功，boot 保持 #21，版本 `1.0.0-rc.3-fp2`。
生成仅含 boot/vendor、skip_postinstall 的签名部分 OTA，输入预算 3867513953 B，
构建前实际可用 6374182912 B，全部打包/验证副本完成后实际可用 5735604224 B。

- OTA SHA256：`f99d724883a26f59a1d8baa990348869f16b91dedb5367ba283487492397376f`
- vendor（278888448 B）SHA256：`9522923b9294f65a9dc07a7a7742f4b5294f33717ce8dc664d0564da979b1972`
- HAL SHA256：`9d8853be00a44b2eff56db6bdbb93d7099e28d94b3517206018d3dc4802cccf0`
- boot、kernel、DTB、ramdisk 哈希均与 FP1 相同。

校验 ZIP、payload FILE/METADATA hash、manifest 目标分区/哈希/partial_update/postinstall，
并独立使用 OpenSSL 校验 metadata 和完整 payload 的 RSA/SHA256 签名；公钥直接来自
设备 otacerts.zip，DER SHA256 为 `ef57b690165cb561b5026922c00d2d6574e8b184fa7d161e076f06e06e6d35db`。
两份签名均通过，篡改 digest 的负例均被拒绝。构建树现有 delta_generator 的 VerifySignature
被既有改动短路并输出 skipped，因此其成功退出不能作为验签依据；本次以独立验签为准，未改动该既有源码。

设备在临时验证之后曾完整重启，boot ID 变为 `6d02d813-884b-4eeb-bb37-2bd00c41e380`，
临时挂载消失、HAL 恢复 FP1。停止旧服务后另备份当时的 A 槽 boot，全 67108864 B
原始 SHA256 为 `51b8a2c25a3b5a22b1a8bc2067887eac9e46106ba5909095584a6c7a7e0112b1`。
随后 update_engine 安装 FP2 到 B 槽并报告最终成功；核对目标 boot 前缀及未改变的 A 槽完整哈希。

ESP 上 B/A 的 ramdisk 字节相同且有备份，暂移除 B 的重复 ramdisk 缓存，实际可用由
12886016 B 变为 26046464 B，再从 boot_b 解出并逐项核对所有启动文件。
完成后实际可用 12877824 B；除 B Image 和 B 标准入口外，其余 ESP 文件哈希不变。
一次性直连 B 启动前默认保持 A；FP2 确认正常启动且 B 已标记 successful 后，
默认入口与 active slot 均设为 B，避免下次重启又进入旧 FP1。A 启动文件与完整分区备份保留；
不把旧槽视为永久不受动态分区更新影响的回退保证。

当前 boot ID `764d0b83-e8db-40d5-901e-430cd74ca1bf`：

- B 槽、boot_completed=1、版本 FP2、SELinux Enforcing。
- HAL 和 vendor_b 原始哈希均匹配目标；没有临时 HAL bind mount。
- vendor_sepolicy.cil 已包含固件加载权限，新增 GPIO ioctl 权限编译入策略。
- HAL 运行域 `hal_fingerprint_default`；system_server PID 1107，HAL deaths=0。
- 原 system 构建指纹仍为 20261030010048，未部署现有不相关 framework 修改。
- thermal status=0，SH3001/TCS3701 传感器服务正常，Wi-Fi 路由与 USB adb 正常。
- 尚处于 RUNNING_LOCKED、指纹计数 0；等待用户图案首次解锁后做真实录入/认证验证。
- 已核对构建机没有其他构建任务，日志和验证记录同步本机后停机，并回读确认 deallocated。

FP2 独立验证、安装和启动记录位于本机 `~/.cache/crdroid-fingerprint-20261101/fp2/`。
未导出真实指纹帧或模板。


## FP2 首次录入后的新问题（修复验证进行中）

用户完成图案解锁后进入 FindSensor，再进入 Enrolling 时提示错误；boot ID 仍为
`764d0b83-e8db-40d5-901e-430cd74ca1bf`，system_server PID 1107，HAL deaths=0，
没有软重启。驱动再次报告 INIT 无效传感器帧头 -EPROTO。
FindSensor 使用一次初始 enrollment，取消后立即发起正式 enrollment。
当前 HAL 每个操作结束都会卸载 TA、恢复 GPIO，然后在下个操作重新 LOAD/reset/INIT；
这与交接后端在设备 open→close 期间复用 TA 的生命周期不同。

停止 HAL 后单独执行固定 init-probe，GPIO 脉冲/读回、INIT、PROBE、UNLOAD 再次成功。
不采图，不据此宣布录入正常。候选修改为会话期间保留 TA/GPIO，close 或 HW_UNAVAILABLE
才关闭；替换会话等待前驱完全释放硬件，避免新操作争抢旧设备。
补充取消→再次录入的初始化复用测试和前驱关闭/新操作竞态测试，真实设备 syscall
在测试二进制中由链接包装器模拟。编译与真实录入结果待补充。


## 2026-11-02 夜间停止时状态

会话复用候选 HAL `98ea4fe4dbd97d7436154f6a916ed78a6e92740d78dee09638bd8cc6346540db`
编译成功，新增取消→再次录入复用与前驱关闭交接测试，5 项重复 20 轮共 100 次通过。
临时启动后首次 INIT 仍遇 -EPROTO；随后开启独立 kprobe 仅记录寄存器编号/长度和返回码，
再次进入正式录入可持续约 73 秒，最终状态寄存器 0x1d 连续三次坏帧使 HAL 报 HW_UNAVAILABLE。
指纹计数仍为 0，未完成录入。内核 boot ID 与 system_server PID 1107 均未变；
不能把用户描述的“炸了”认定为系统软重启。旧 Settings crash buffer 还存在 23:54 的
FingerprintSettingsRequireScreenOnToAuthPreferenceController 空指针，时间早于这一轮失败，
不能据此断言为本次事件。

独立 C 探测、匹配 HAL GPIO 打开/关闭顺序的 C 探测、与生产编译/链接参数相同的
Android/Bionic Transport 探测、system UID/hal_fingerprint_default 域探测，以及最小
PR_SET_TIMERSLACK 探测均成功；尚未证明 INIT 间歇失败的根因。
真实跟踪显示寄存器 0x1d 有时前两次 -EPROTO 后第三次成功，最终一次三次均失败。
另外需核查初始 latched=true 与交接后端初始 false 的行为差异，不能声明已修好。

用户要求关闭构建机休息，停止继续测试。日志与非图像跟踪保存在本机 fp3 子目录。
设备指纹服务已停止；本轮 kprobe 和独立 trace instance 已删除，wifi 等其他跟踪实例未改。
临时 HAL bind mount 已移除，磁盘仍为 FP2；没有重启设备或恢复录入。
后续若重启，磁盘上的 FP2 服务会重新启动，当前的停止并非永久禁用。


## 2026-11-02 白天继续：真实录入和解锁通过

恢复时仍为同一 FP2 B 槽启动（boot ID 764d0b83-e8db-40d5-901e-430cd74ca1bf），
指纹服务停止，系统 PID 1107、Enforcing 保持。构建机以 module 档位 D16 重启。

固定无图像状态探测：空载 150 次全部正常；早先触碰窗口 150/450 次均无就绪，
动作与采样对齐不足。改为收到用户“完成”后才在已返回 ioctl 的边界结束轮询：
315 次中就绪 15 次、无帧 300 次、状态错误 0，TA 正常关闭。
独立 trace 只记录寄存器编号/返回码及协议头，未采图或导出图像。

候选修改保留昨晚的会话复用；对状态查询的 EPROTO/EBADMSG 做最多 5 次额外轮询，
不复位、不接受错误数据、不将失败读误判为抬起。成功读清除连续错误计数，
持续坏帧/真实 I/O 错误仍报 HW_UNAVAILABLE 并退役会话。
新增 3 项真实 capture 主循环测试，使用模拟设备和独立空白临时目录：
短暂 EPROTO/EBADMSG 后能继续检测、持续坏帧结束会话、EIO 不重试。
共 8 项设备模拟测试重复 10 轮（80 次）通过；离线 Core/ASan/UBSan 和集成检查通过。

解码增加仅分类日志（AES/样本范围/平帧/质量），不输出像素或匹配分数。
构建中发现 Soong/Siso 已开始执行时追加的 Core 日志源码未进入首轮对象，
通过已生成构建规则单独重编 Core/服务/测试，核对诊断字符串和最终对象。
最终候选又通过 8 项设备检查，临时 HAL SHA256：
`d485c347fdf726a1b2969e1c747e96bab4def6158b880ca4f43461986196a910`。

FindSensor 初次录入取消→正式录入连续运行。正式录入 16 帧全部接受，框架模板数 1；
模板文件仅在设备，system:system、0600，未导出。诊断显示少量 image quality 拒绝，
未见 AES 或样本范围失败。用户说明需要每次略换位置，已补入使用说明。
用户确认未录入手指被拒绝、已录入手指可解锁；框架 accept=1/reject=2、模板数 1。

带协议头跟踪确认间歇坏帧为重复字节（例如 0x06/0x66/0x07），并非只因首字节不等于 0x02；
应保留长度/状态/XOR 等校验，不能放宽为可接受图像。独立 trace/kprobe 已清理。
FP3 vendorimage 构建成功，正在生成仅含 boot/vendor 的永久更新包，模板保留与重启恢复待验证。


## 按电源键唤醒时必须抬起再碰（修复验证中）

用户所述“识别率不高”具体为按电源键唤醒时无反应，抬起再碰才识别。
检查发现认证开始时 latched=true，错误地要求先观察帧标志归零；MCU 的 bit0
表示帧可用，并不能当作必须先出现的手指抬起事件。匹配阈值 12 保持原样。

新增实际 Session::authenticate 主循环测试，使用生成的随机纹理、AES-CBC、
独立临时模板库和真实匹配器，所有设备 syscall 仍为模拟：开始时即 ready=1，
匹配样本须被处理并成功，无关样本须被处理并拒绝。旧代码两项测试均失败，
mockCaptures=0；将认证起始 latched 改为 false 后，全部 10 项测试重复 5 轮通过。
登记样本、解码/质量/匹配标准均未放宽，未删除或重新录入用户指纹。

在用户解锁停留桌面后，关闭仍在后台验证的 Settings，确认框架无当前操作，
再停旧 HAL、临时替换新候选。HAL SHA256：
`96157247d8d5c72435449645cae6a3d355718c5a48e9ef68704bd8ffc399aab6`。
system_server PID 1107、boot ID 和 Enforcing 不变。等待按键唤醒连续测试反馈。
初版 FP3 OTA 已打包但未刷写，永久包将更新为本修复后的实现。


## 2026-11-02 同指频繁不匹配：离线匹配器修复候选

用户补充说明：当前主要表现为“有反应但经常提示不匹配”。此前一次 accept 和少量
reject 只证明基本闭环，不能作为真实同指识别率可用的证据；首帧修复也不等于匹配
问题已解决。本轮按工作区默认约定离线工作，未读取设备、导出模板、替换 HAL 或刷写。

发现 `gaokun3_ransac_similarity` 的明确计算错误：对两组采样点距离求出 `nb/na`，
检查其位于 0.77～1.3，却在后续坐标变换中漏乘该比例，只应用旋转和平移。距锚点
越远的正确对应点，越容易被 2.5 像素容差排除。修复为将 `cos/sin` 同乘 `nb/na`。
匹配阈值仍为 12，缩放边界与像素容差未改；接受的几何模型由刚体变为原代码意图的
相似变换，仍需真实同指/异指验收，不能仅凭阈值不变推断误接受行为不变。

固定种子生成纹理（不是指纹）的离线结果：

| 输入变化 | 修复前 | 修复后 |
|---|---:|---:|
| 中心缩放 0.9 倍 | 7.55 | 16.98 |
| 中心缩放 1.1 倍 | 8.26 | 22.94 |
| 中心缩放 1.2 倍 | 5.50 | 12.84 |

以上为合成匹配分数；不能换算为真实识别率或误接受率。同一合成纹理旋转 10° 的
得分仍约 6，说明描述子对角度变化的适应也有限，本修复没有解决全部匹配问题。
另发现每帧都重新提取 16 张已录入样本特征；本机优化编译下单指拒绝样本约
29 ms/帧，尚未做 Android 性能测量，本轮没有引入缓存。

新增 `test_matcher.cpp`：64 个具有确定对应关系的生成特征，在 0.8～1.2 倍尺度
及小角度旋转/平移后应全部支持同一变换；旧实现该测试失败，修复后通过。2 倍尺度
仍被拒绝；描述子相同但坐标随机的输入被拒绝。端到端特征提取对 ±10% 缩放合成图
通过，32 张独立纹理的 992 次有方向异图比较均低于阈值（最大 1.83486），全部同图
比较通过。`scripts/fingerprint/test-offline.sh` 集成上述测试，ASan/UBSan、既有
核心与集成检查通过。没有 Android 编译或真机验证，不更改运行候选的哈希记录。

下一步应先在设备核对当前运行版本与失败分类，再对照候选测试相同接触方式下的
同指接受/拒绝、异指拒绝和质量拒绝次数；保留用户已有模板，验证缩放修复是否实际
有益。只有真实数据才能区分采集质量、录入覆盖与匹配器限制各自的影响。

## 2026-11-03 恢复诊断：当前为 HyperOS，等待切回 crDroid

用户授权连接设备读取诊断。USB 序列号 `gaokun3`，boot ID
`72b11f8d-99eb-4782-8df8-cd13c2a75242`，SELinux Enforcing，system_server PID 1095。
实际命令行为 `androidboot.force_super_partition=super_hos`、
`androidboot.hos_build=r22-gyro-startup`，当前是 HyperOS R22；没有
`/dev/gaokun3_fpcmd`、候选 HAL/匹配库或 framework fingerprint 服务。
因此不能将这次空日志解释为 crDroid 指纹运行正常，也不能在此系统上验证识别率。
仅做只读查询，没有改动启动项、重启或部署候选。

复核保留的 `session-20261102/held-live-fingerprint.txt`，旧统计为 accept=1、
reject=12、lockout=2（当时模板数 1）；这些计数混合了未知接触条件，不能换算
同指拒绝率。其关联的临时 HAL 是 `96157247…399aab6`，并非缩放修复候选。
已请求确认由用户手动切回 crDroid，或授权核对启动项后一次性启动 crDroid；
收到选择前不改变设备启动状态。

用户随后明确授权自行切换。只读挂载 ESP 核对标准直连 B 项：kernel、ramdisk、DTB
SHA256 与 FP2 记录逐项一致。设置并回读 OneShot
`8a29534fa802480d9fbb71aa18c01d7b-android-b.conf` 后重启，未修改默认启动项或刷写。
新 boot ID `c1b9f39d-8d7d-46c1-b20d-4819112b3bc3`，crDroid B 槽、
boot_completed=1、Enforcing，指纹驱动节点与服务存在，HAL deaths=0，模板数仍为 1。
HAL SHA256 `9d8853be00a44b2eff56db6bdbb93d7099e28d94b3517206018d3dc4802cccf0`，
匹配库 `b02858d0aa9add6962a66ddbf97cc450ed63f2bdc17620abe8eb34da860e9ef8`。
确认重启已回到永久 FP2，之前的临时首帧/状态重试等修复不再运行，缩放修复未部署。
已请求用户先图案首次解锁，再按通常方式尝试 3 次已录入手指，后续对照新日志。

用户回复 done 后，确认同一 boot ID，用户 0 为 RUNNING_UNLOCKED，统计 accept=0、
reject=5、acquire=1、lockout=1，HAL deaths=0，模板数 1。不能把框架计数直接当作
用户实际触碰次数。HAL 日志 18:17:49/50 分别记录 INIT 的 EPROTO 与 EIO；内核也有
对应坏帧头和初始化失败。锁定期间框架在约一秒内产生 154 条
`In lockout mode(1) ; disallowing authentication`，确认既有 Class 1 SystemUI
重试问题仍存在；这 154 次未进入真实匹配。图案解锁后框架完成 resetLockout，
采样时 scheduler 空闲、桌面未显示 keyguard。模板只检查元数据（system:system、
0600、125508 B），未导出或读取模板内容。

诊断证据保存于本机 `/private/tmp/gaokun3-fp-diagnostic-20261103/`。本轮准备模块候选，
恢复当前源码的会话复用/首帧/状态坏帧重试与认证窗口处理，并加入缩放修正；额外
日志只记录认证拒绝窗口内可用帧数量，不记录图像、描述子或分数。构建机确认原为
deallocated，使用 module/D16as_v5 启动，无其他构建任务。实际输出盘构建前可用
7804968960 B；只构建三个模块，预算 1 GiB 增量与至少 2 GiB 余量，无镜像生成或
清理。远端修改前保留 fingerprint 源码副本，源码与诊断记录独立保存。

三个模块 Android 构建成功，39 个实际动作；依赖冷加载占约 5 分钟，完整构建
5m10.71s。构建后输出盘实际可用 7805333504 B。候选摘要：

| 文件 | SHA256 |
|---|---|
| HAL | `44fcd9f4f5fadb035b4b7176cc5db569e3c42d4d9938dfa94c3eff6df4580a12` |
| arm64 匹配库 | `a6439e474ff95beb986b8f303be81f92c30df06e8a6b26c7f78a903a17377eb3` |
| session-test | `2f9adb8c4ec6dcf4ea2d9ccbbd0d019e3713391a6f082533739a203a8d07ad1c` |

文件回传本机并推至设备独立临时目录，逐项核对哈希。测试进程通过
`LD_LIBRARY_PATH` 使用候选匹配库，GPIO/TA syscall 仍为链接包装的模拟设备，
数据库在独立临时目录；11 项重复 3 轮，33 次全部通过。本机 ASan/UBSan 核心、
缩放与异图回归和集成检查也通过。system_server PID 1086、boot ID、Enforcing
均保持；生产 HAL/匹配库仍为 FP2，尚未临时启用候选。临时替换脚本已准备，要求
桌面、scheduler 空闲、固定 boot ID 与原文件哈希，提供失败时卸载挂载并恢复 FP2。

确认远端无其他 soong/ninja/make/siso 构建，日志和候选已回传后停机，脚本回读
Azure 状态 deallocated。已请求用户批准临时替换 HAL/匹配库；目前授权明确涵盖
读取诊断与系统切换，候选运行状态改变待批准。没有刷写、删除、导出或重新录入模板。

用户回复 ok 后临时启用候选：核对桌面与 scheduler 空闲、原文件和候选哈希、boot ID
及 system_server PID 后，停止 HAL，对 HAL/arm64 匹配库分别 bind mount；HAL 挂载
恢复 suid 语义，沿用原精确 SELinux 标签。核对 init 视图中的两文件摘要，启动并
确认 `/proc/<HAL pid>/exe` 匹配目标、进程已映射匹配库。HAL PID 7552，
system_server PID 1086、boot ID、Enforcing 均保持，模板数仍为 1。
框架 HAL deaths 从 0 变 1，符合主动停止并替换服务；启动后未见相关崩溃/AVC。
测试前累计计数仍 accept=0、reject=5、acquire=1、lockout=1。
请求用户同一已录入手指、平常姿势尝试 3 次，开始独立分类日志窗口，实测结果待补。

用户反馈“无法使用指纹硬件”。候选开始的 4 次 INIT 返回 EPROTO；后续曾恢复，
两个拒绝窗口分别含 4 张、2 张可用帧，另有 2 条质量拒绝。因此缩放修正未证明
改善真实同指识别，问题同时涉及间歇初始化与真实匹配拒绝。累计统计变为
accept=0、reject=7、acquire=3、lockout=1。

为检查初始化停止服务时，锁屏 framework 立即通过 ctl.interface_start 拉起 HAL，
独立探测因设备仍被占用返回 EBUSY，未发起初始化。此次主动停止发生在 framework
认证仍活动期间，留下对应 scheduler Crash State；它是 HAL 断开记录，不是系统
软重启。后续仅在确认桌面、scheduler 空闲时停止并探测，不再用活动会话下的
ctl.stop 作为普通取消。用户随后确认已图案解锁、停留桌面。

桌面空闲时停止 HAL，固定 C init-probe 的 LOAD/GPIO 脉冲/INIT/PROBE/UNLOAD
全部通过；Android/Bionic Transport 独立探测也 start/status/stop 全部通过。
均未采图，不能据此断言真实 HAL 的初始化问题已解决。

创建唯一 trace instance `gk3fp_diag_20261103`，只记录 fp_read_register 的编号/
长度、fp_read_response 的长度及对应返回码、wake 入口/返回，无数据缓冲。
fp_vendor_init 本内核没有可探测符号，第一次设置失败后已完整撤销，再仅设置
6 个可用事件。root 固定探测通过；恢复 HAL 后，记录到完整初始化成功和连续
状态查询，marker 后 352 条记录无负返回码。另以 KEYCODE_SLEEP/WAKEUP 做一次
无手指输入的屏幕切换；该窗口未重现初始化失败，不能说明根因已解决。
跟踪已停止、保存并仅删除本组 6 个事件及 instance，其他跟踪保持。

当前候选仍临时启用，HAL 持有 /dev/gaokun3_fpcmd 与 gpio-line；system_server
PID 1086、boot ID、Enforcing 保持，累计 HAL deaths=3 对应本轮主动服务停止，
不是三次 system_server 崩溃。请求用户在初始化已完成的状态下再做一次同指尝试，
区分成功、匹配拒绝与硬件不可用；尚未收到该次结果。

## 2026-11-03 继续：真实匹配仍拒绝，窗口门槛与方向描述子候选

用户反馈“不匹配，然后有的时候按下了也不行”。同一 boot 下读取新增日志：
18:40:47/54/58 三个窗口均含 3 张可用帧并被拒绝，另有 1 条质量拒绝；累计
accept=0、reject=10、acquire=4。没有新的 INIT 失败，但内核有短暂状态坏帧。
所以缩放修复已真实启用却未证明改善；本阶段不能继续归咎于首帧初始化。

发现第二个门槛错误：3 秒认证窗口结束后重新设置 latched=true，导致后续 ready
一直为 1 时不再取帧。新增 Session 模拟测试：先提供异图；失败回调后在同一 worker
改为匹配图，状态连续保持 ready=1。旧代码等待 10 秒仍无成功回调，失败计数保持 1，
测试正常报告失败并清理（没有故意 abort）。修正为窗口结束后 latched=false，
仍按原有解码、质量、匹配阈值及锁定流程处理新帧；修复回归正在运行。

另完成方向描述子候选。原实现只减去 8 桶主方向，空间采样网格始终固定；新候选
同步旋转网格和梯度，使用加权/平滑的 36 桶主方向与峰值插值、双线性像素采样、
空间/方向插值及截断后重新归一化。边界不支持旋转采样的 patch 被拒绝，不补造
像素；几何、图像质量、匹配阈值 12、RANSAC 比例边界和容差未改。

离线对照生成更大的 320×320 纹理场景后采样 40×196 视角，确保旋转后仍是完整
纹理覆盖，避免空白填边干扰质量检查。探索组 seed 1～32：±10° 接受由 8/64
变为 56/64；独立回归组 seed 1000～1031 接受 57/64，1984 次有方向跨场景
比较全部低于阈值（最大 4.05405），旧代码未通过此回归。另 32 张独立纹理的
992 次异图比较全部拒绝（最大 4.10959）；既有核心、缩放和集成检查随
test-offline.sh 在 ASan/UBSan 下通过。算法更改不能仅凭阈值未改推断实际误接受率
保持；这些仅为合成回归，真实同指/异指结果仍待新候选。

构建机原为 deallocated，再次按 module/D16 启动，确认无其他构建。构建前输出盘
7804649472 B；先构建新测试配旧 Service，实际设备模拟测试复现失败；随后同步
修复源码，39 个实际构建动作耗时 10.82 秒（热缓存），构建成功。构建后实际可用
7803097088 B，未清理或生成镜像。候选目录本机
`/private/tmp/gaokun3-fp-diagnostic-20261103b/`，设备使用新的独立临时目录，避免
改写仍被旧进程映射的候选 inode。下一步完成 12 项回归 3 轮，桌面空闲后临时更新。

新候选 12 项重复 3 轮共 36 次全部通过，窗口回归从旧代码 10 秒无成功变为约
3.9 秒完成“先拒绝，再处理持续 ready 下的匹配帧并成功”，失败计数恢复为 0。
没有访问生产模板或真实设备 syscall。候选 SHA256：

| 文件 | SHA256 |
|---|---|
| HAL | `d630bccdc99e4008ceb7859a316530580ba16a856a6823c7637242d0c255161a` |
| arm64 匹配库 | `865da2823469f90e6016401122d8b3c31793a94c4419e1ae9d3d86172df65d6c` |
| session-test | `b09c59f2c2ce359b6de4b171c5e798ebdb97a53c62d5049615bda6ec5946b212` |

源码哈希、构建日志、预修复失败及修复后成功测试记录均已回传；确认无其他构建后
停机并回读 deallocated。新候选已在独立设备临时目录完成哈希验证，生产仍运行
上一版临时候选。设备仍显示锁屏且认证活动；已请求用户图案解锁并停留桌面，收到
该状态前不停止 HAL 或替换挂载。新的替换脚本失败时回到上一版临时候选，最终
重启仍恢复磁盘 FP2。

用户回复 done 后确认桌面、scheduler 空闲，按既有临时测试授权替换为第二候选。
两项新 bind mount 覆盖在上一版临时候选之上，原 inode 保留；未写分区。
新 HAL PID 12763，核对 exe 和 init 视图摘要匹配 d630…/865d…，映射匹配库存在；
system_server 1086、boot ID、Enforcing 保持，模板数 1，累计 HAL deaths=4，
新增 1 次对应主动停止。认证前累计 accept=0、reject=10、acquire=4、lockout=1。

通过 KEYCODE_SLEEP/WAKEUP 先软件锁屏、点亮，随后确认 HAL 持有指纹/GPIO FD，
framework 认证 client 处于运行状态，尚无新 INIT 错误。请求用户只做一次普通
已录入手指轻触，以减少电源键动作和初始化并发对匹配对照的干扰；实际结果待补。

用户反馈仍不匹配。新候选在 19:08:36/40/44/47 的四个窗口分别含 4/3/3/3 张
可用帧，仍全部拒绝；一次短暂状态 EPROTO 被既有重试处理，没有新的 INIT 错误。
累计 accept=0、reject=14、acquire=4。因此第二候选同样未证明改善真实识别率，
不把合成回归当作真实验收。

新增 `android-template-audit.cpp`，仅在设备内固定 user 0 路径只读加载 GK3A，
以 system UID 执行原有校验，所有像素/特征留在进程内；只输出汇总数量，无分数、
描述子、模板 ID 或图像。空白/同图/无关合成输入在主机 ASan/UBSan 和设备测试
通过。使用保存的生产编译/链接命令直接编译诊断工具，未启动 Soong 图加载。
经 Android linker 的 dynamic 日志确认实际分别载入两份指定库，结果如下：

| 检查 | 缩放候选库 | 方向对齐候选库 |
|---|---:|---:|
| 质量通过 | 16/16 | 16/16 |
| 特征不少于 6 | 16/16 | 16/16 |
| 特征数 min/max | 54/83 | 54/83 |
| 自身匹配通过 | 16/16 | 16/16 |
| 样本之间双向匹配通过 | 1/120 | 1/120 |
| 字节不同的帧 | 16 | 16 |

这排除了“所有模板特征数不足”，不能仅凭低互匹配数量认定用户录错或图像无效。
另构建仅供诊断进程使用的 196 行距布局库（同步调整空间桶，避免越界）；生产
库/源码不变。该布局同样质量/特征/自身匹配全通过，特征 min/max=57/84，样本
互匹配 0/120，未见简单交换行距即可恢复一致性的证据，不据此改变生产几何。

新增 `android-contact-audit.cpp`：先只读加载旧模板，独占固定设备并初始化，
将少量新采帧留在 RAM，对照新帧之间与旧样本的匹配通过数量；字节完全相同的
重复帧不当作独立重复性证据，不写模板或原始帧，不产生认证回调或失败计数。
独立进程 GPIO/TA 关闭失败则保留所有权，不用超时强制杀安全调用。
合成重复帧/异图/旧样本分类测试在设备通过。

第一版固定 45 秒窗口在用户确认前结束，125 次状态查询无帧、无协议错误，
不能当作已对齐的接触失败。TA 成功关闭、模板元数据未变并自动恢复候选 HAL。
随后改为独立 stop-contact-audit 控制文件：用户确认完成后，在 ioctl 返回边界
停止；8 帧上限或 5 分钟兜底，既不异步复位也不杀进程。用户已确认桌面，第二次
独立采集已 READY；当前等待用户完成接触并回复。生产服务在此期间暂停，诊断
成功关闭后自动恢复，图像和模板不导出。

这轮诊断编译前输出盘实际可用 7802396672 B，最后核对 7791403008 B；只生成
小型诊断 ELF/库和 LTO 缓存，没有镜像或清理。构建记录已回传本机
`/private/tmp/gaokun3-fp-diagnostic-20261103c/`，确认无其他构建后已请求停机，
Azure 已回读确认 deallocated。
