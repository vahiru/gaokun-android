# R15 导航与触摸修复部署（2026-10-30）

隔离 HyperOS 已启动；用户实测确认应用内底边上划可以回桌面，但中央滑动仍不灵敏，不能宣称触摸全部修好。

## 部署与回滚

- super_hos：`2215557f90e2e76c4d3d7f19a02ed0af7d69f7bc341cbcfc68b1310561ef0687`，12 GiB USB 流写入并整块回读通过，日志 `SUPER UPDATE VERIFIED`。
- EFI：`83bed9ab407c68fff7fb54fc99bbce5d902fb76b916de653e1f51d3385312d96`，包含触摸报告坐标钳位与 UCSI 初始化诊断，不包含 UCSI 重试候选。
- ESP 空间不足，已先将 R14 EFI、启动项、loader.conf、R9 启动项备份到救援根文件系统 `/home/user/hos-r15-rollback` 并逐个校验，再替换为 `/gaokun3-hos/r15/vmlinuz.efi` 和 R15 启动项。旧 R14 菜单项随旧 EFI 退役；R14 EFI 仍有本地及救援备份。
- R6 fallback EFI、DTB、R9 ramdisk 均保留且哈希核对不变。loader.conf 的生产默认 `*-android-a.conf` 不变，仅设置一次 R15 启动。未改生产 Android 分区、userdata、metadata 或 boot A/B。
- 证据在本机 `/Users/vahiru/.cache/hos-r15-nav/`；权威镜像是 `super-final/` 与 `vendor-complete/`，早期 `super/`、`vendor/` 不是部署产物。

## 启动与导航验收

Boot ID `9fbf3a1f-bee6-448e-913f-9a5f3137a347`；boot_completed=1，ro.boot.hos_build=r15-nav-touch，SELinux Enforcing，1600×2560、240 DPI。

`org.gaokun3.hos.recents` 静态 overlay 已启用，资源查询实际来自 `/vendor/overlay/HosRecentsOverlay.apk`。Recents 组件为 `com.miui.home/com.miui.home.recents.RecentsActivity`；mRecentsUid=10132，等于实际桌面 UID。LauncherProxyService 的 connected/enabled/bound 全 true。

解锁后从设置页注入底部上划，窗口焦点与 topResumedActivity 均回到 MIUI Launcher；用户随后用手指确认可回桌面。这比临时 fabricated overlay 验证更完整：完整重启后静态资源与 system_server 缓存均正确。

## 触摸仍待修复

新内核首次真实触摸采样未出现坐标越界，但单指横向滑动产生了短暂 slot0/slot1 重叠。用户明确确认没有第二根手指或手掌接触。不能由此直接定因 IIR 或跟踪算法，需进一步对照。

本轮 algo 读回：game 参数生效，palm 各拒绝计数为0，rej_minarea=7，tracks_new/lost=12，max_contacts=2。触点显示按用户要求开启（show_touches=1）。

Wi-Fi 已连接且 VALIDATED，ccodec=4、poolmask=0xfc0000 保持。本次 Type-C port0/port1 存在，但不据此认定间歇初始化问题修复。此前 R14 功能抽测仍见独立报告；尚未在 R15 全部重跑。

### 单变量诊断补充

临时关闭 IIR 后采到31个接触、最大同时2个、无越界；不能仅凭此证明改善或确定根因。已恢复 iir_enabled=1 并读回，其他阈值未改。用户提出充电干扰可能是老问题，现请求完全拔线电池供电对照，设备已启动有时限的离线触摸采样，尚待用户结果。

启动后另发现 com.xiaomi.cadlauncher 子进程在 SELinux setcontext(mslg_app) 阶段反复 SIGABRT，继续审阅；不是 system_server/zygote 主进程重启的证据。导航核心验证通过不代表后台所有 OEM 应用正常。

用户已反馈：**完全拔线后滑动明显正常**。对照前 IIR 已恢复1，所以不能把改善归因于关闭滤波。现有证据支持插线/供电状态相关的触摸干扰；尚不能区分充电器、线缆、接地、USB主机或驱动的抗噪处理。离线采样待设备重新连接后拉取，不能假定已拿到电池供电阶段的事件证据。

设备已重连并拉取离线记录（touch-charge-comparison.txt/json）。触摸时间529.173–553.361秒；healthd在546.677秒后报告无外部供电且放电，564.367秒重新出现USB供电。USB role切换与供电上报延迟使整段不能简单划为纯电池对照，故仍以用户反馈为主，分段事件分析另存诊断报告。最终读回 iir_enabled=1、show_touches=1，导航服务仍保持连接。
