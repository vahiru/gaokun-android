# R21 传感器与融合修复部署记录

合并两层修复：HAL没有有效Sample时不发送默认零事件，空事件队列不调用front/FMQ/wake；框架Fusion在ACC单样本norm为零或非有限时拒绝初始化，保留状态而不制造输出。合法有效零样本仍可由HAL发送，框架保护仍独立必要。

## 构建与范围

- HAL候选SHA256 `ab794e19343bfee8e3690d656203ff4b3a737189c8527591dfaa4029b0f548d4`，仅Sensor.cpp与Sensors.h两处源码变化，无ALS扩展。
- 实际旧FFC、新无ALSbaseline与candidate动态依赖/符号合同一致；新baseline仍与FFC存在.text及其他alloc差异，不能称历史二进制逐字节或全行为等价，需实机回归。
- Fusion库SHA256 `3c54d8c74b94fa69e96f4c1dd5862983e7658e78ce81c2b3d234bf987dda2e21`，实际OEM函数重放、临时运行与下述冷启动验证通过，早期数据交付仍有失败。
- system EROFS SHA256 `f13825109282103654e0e92fc95d899ebf1ec1a88a1b9824362e0fb8c9a5599a`，4807条目核对仅该库内容改变。
- vendor重新生成后，R18既有19项替换记录及全部开关保持，新增唯一sensors HAL替换，原inode权限/owner/SELinux xattr回读通过，e2fsck通过；未修改RC/VINTF/策略。
- super SHA256 `aafb5cd8c5c7a979fb5850a25564bd51bef1873c7f46c6b1f234a03c0f4e09a0`，12884901888字节，六分区解包逐一匹配。相对R18只system_a/vendor_a变动，其他四分区相同。

产物位于 `~/.cache/hos-r21-sensors-fusion/`，HAL构建与CICD恢复审计位于`~/.cache/hos-r20-sensors-hal/`。候选已写入固定super_hos并完成完整读回，userdata未擦除或格式化；冷启动验证见下文。

## 上机方式

用户确认前次切出HyperOS不是人工操作；日志与BCB明确为RescueParty自动恢复请求。该请求已完整备份并仅清除BCB请求字段，无userdata擦除，普通misc保持。后续用完整冷启动，不反复热重启zygote；不关闭RescueParty。

已只读确认救援系统保存当前Wi-Fi配置，但原enabled USB role服务会强制host，且system-sleep hook可直接调用它。冷更新前需完成临时device-only救援入口与连接验证，保留正常启动入口/默认项；相关辅助文件位于scripts/hos-rescue，现已安装并验证，见下节。新HAL冷启动时未有数据应为0事件，不能把数据交付FAIL改成PASS；真实数据到达后再验原始/融合数据与所有既有回归项。

## 救援连接与更新

已安装仅flag启动生效的救援device/ECM与未认领回退辅助文件；正常救援原host脚本主体、Wi-Fi配置、原unit和睡眠hook保持，host脚本仅增加专用flag早退。独立入口追加device标记、headless multi-user及runtime sleep masks，默认入口仍Android B。认领使用O_EXCL仲裁，不允许更新与回退竞争。

实际救援遇到configfs MAC尾NUL，以及绑定后内核自动派生bcdUSB0000→0210；两者均在保留严格身份/当前boot/UDC/速率约束下修复。第二救援boot98ca0de3-8d5e-4d1f-82bf-ee6030a7d8fa中ECM与Wi-Fi均连通；helper重复运行两次成功，未重绑或移除地址。Mac报告泛化MAC，因此主机链路以已知SSH key、同一boot ID和显式169.254源地址核验，不盲用旧en9地址。

旧R18更新记录仍在，未盲删。GPT两份CRC及12条目与旧记录相同，完整p7哈希仍ED290。BootB保护哈希已过期：当前64MiB镜像8de927…291f1中的kernel a08e…228e1、DTB82fa…40373均匹配本轮工作开始前ESP；BootA仍756170…2cd12。两镜像完整备份并验证后，新manifest保护当前版本。70个ESP文件与新增专用入口后的清单完全一致。

新manifest pin51bb79624aa285f4c8d16a8051a78e19309d5bcbe50cf3c8b4ef3e0227f4ab56独立审阅通过。完整复核R18已完成状态后，旧marker独占硬链接归档于p3；正式只读preview通过。USB stream传输12GiB、EOF/流SHA、BLKFLSBUF后全分区读回、GPT/bootA/B/全ESP保护均通过，工具退出0输出VERIFIED，新marker保留。随后才新增R21入口，复用未变R17内核；既有ESP文件和默认保持。

## 首次冷启动结果

boot1b12838f-7cd9-42be-9323-4ab21411ec87/build r21-sensors-fusion，SELinux Enforcing、boot_completed1、USBdevice/device_only=true、ADB端口5555持久值保持，userdata仍p12/370G。实际HAL SHAab794…48d4、SystemServer namespace库SHA3c54…2e21均核对，无临时Fusion挂载。

采样开始于uptime134.13秒，三轮12项strict全部PASS；nonfinite/NaN/badTimestamp均0，acc/gyro warmup与sample零向量计数均0。首次有效raw事件约127–143ms，融合约148–168ms；这是有效数据交付与数值健全性验证，不是绝对方向校准或启动最初一分钟的覆盖。

为补最早窗口，提前启动的下一轮观察器在uptime11.21秒被Android以Too early to start activity拒绝。该轮没有新数据；旧results已标stale并剔除，不计入测试。随后清理已归档的旧测试输出、改为boot_completed后立即启动，新的完整冷启动观察已完成，结果见下节。未再热重启zygote，重启前两次检查misc_hos都没有recovery命令。

证据：cache/hos-r21-sensors-fusion/deploy及rescue-install。R21冷启动初测通过不代表全部功能完成：早期窗口和温控/网络结果见后文；光感、真实指纹触发/图像/HAL等未完成。

## 早期窗口与同次启动恢复

boot `dd9f16a5-7219-4408-b0a5-d009c7320ce4` 在boot_completed后、uptime15.73秒启动三轮检查，四种传感器均0事件，12项数据交付FAIL。这不是PASS，也不能以没有NaN掩盖无数据。约69秒才记录到物理accel/gyro枚举；同次启动uptime约630秒再次三轮检查，12项PASS，无NaN/非有限值/异常时间戳，无默认零向量。说明未就绪输入不再持续污染融合，但SSC启动延迟尚未解决。

复核after-ready原始OBS又发现：每轮gyro最前两帧均为三轴`-34.906586 rad/s`，随后恢复小读数。原测试通过只覆盖有限值、时间戳、数据交付，且有2秒预热；不能据此宣称首帧准确。该值约对应-2000度/秒，来源和accuracy状态待进一步核验，不应直接按数值过滤或伪造替代值。

## 其他回归与自动重启复核

- 实际Thermal Binder契约检查`RESULT PASS failures=0`，真实SKIN约26.28°C，负forecast参数正确拒绝。
- 无线ADB实际连接到同一boot，持久端口5555；公网单次ping成功仅证明当时连通，不能表示网络稳定性全部通过。
- 后续uptime约31分钟复查仍为同一boot；crash buffer为空、pstore无条目、misc_hos前832字节全部为零，无待执行恢复请求。该观察不能排除未记录的故障。
- SELinux Enforcing，USB role=device/device_only=true，RescueParty未关闭。FindDevice仍hidden=true，未反复尝试受保护的disable操作。
- allow_suspend=0仍是调试保持设置，本轮不能声称深睡回归通过。
- 已停止本轮探针应用并删除临时thermal可执行文件；保留探针APK供后续测试。

证据目录：`~/.cache/hos-r21-sensors-fusion/deploy/cold-early-v2/`（早期FAIL与同boot后续PASS分别保留）、`thermal-regression.txt`、`network-regression.txt`、`followup-check/`。此前自动重启属于已记录的RescueParty事件；本轮没有重新触发或关闭该机制。真实指纹取帧、HAL/录入解锁、ALS、设置按压渲染等仍未完成。


## 首帧accuracy实机补验

增强APK原位更新（既有签名），同boot uptime约2282秒开始三轮，12项原检查通过。每轮gyro前两帧三轴`0xc20ba058`、accuracy=0（UNRELIABLE）；第三帧转正常小值、accuracy=3。因此不是HAL将该瞬态强行标成高精度；仍须区分SSC独立报告与HAL周期缓存重发，并核验融合消费者如何处理低精度输入。无数值过滤、截断或填充。

框架实测maxRange=17.453293 rad/s、resolution=0.0005326322 rad/s，与观察到的-34.906586负满量程及约0.0010652645量化步长不一致；不能只依据该瞬态盲改硬件量程，需要属性或驱动证据。证据`~/.cache/hos-r21-accuracy-probe/`，测试后已force-stop。

枚举时间线进一步复核：早期16–39秒零事件，69.04秒才枚举完成，约630秒复测出数；不能将69秒当作已测得首样本。main缓冲最早仅保留26.69秒，缺Open/WaitForService起点。源码每个FindSensors可等待10×1000ms，accel/gyro串行加sleep，注释“30轮=60秒”不是实际deadline；未据此缩短超时或更换会话。下一诊断仅补阶段耗时和受限首帧SSC日志。


早期采集工具`tools/hos/collect-rpc-cold-boot.py`新增可选`--sensor-hal-sha256`：同时收取HAL标签并核验实际HAL哈希，保留RPC哈希、隔离系统/build/Enforcing与结束boot ID检查；不加-T/-t，包含缓冲日志且立即流式保存，工具自身不重启或刷写。当前R21下5秒只读采集完成、同boot检查通过，错误HAL哈希被拒绝并标capture_complete=false；这只是采集工具验收，不是新的冷启动结果。证据在accuracy-probe/collector-smoke及collector-wrong-hal。
