# R16 后台重复错误只读审计

2026-10-30 02:12 左右，只读 ADB 采集。证据缓存：`~/.cache/hos-r16-startup-audit/`，包含 logcat、crash、dmesg、状态、包信息和实际 loaded-policy。未清日志、未改设备。

## 当前确认

| 项目 | 证据 | 结论 |
|---|---|---|
| Millet | logcat 46 次 socket type1 创建失败，02:05:13–02:12:01；退出后立即重启 | 约9.08秒循环，仍未修复 |
| 热传感器扩展 | 49次 board_sensor_temp ENOENT，02:04:02–02:12:03 | 约10秒一次，不存在的OEM节点被持续读取 |
| cadlauncher | PM找不到包，`pm list packages -u`也没有cad/caj/wps；mslgrdp仍安装启用 | 当前无crash不能证明旧SELinux故障修好 |
| Java/native crash及ANR | 13203行中FATAL、Abort、am_crash、am_anr、ANR均0；crash缓冲为空 | 仅说明采样窗口未见 |
| SELinux | Enforcing；采样AVC为0，但audit rate limit exceeded 19次 | 不能断言完全无拒绝 |
| cgroup迁移 | AddTidToCgroup 0；init failed write pid to memcg 46次 | 后者与46次Millet重启相伴，不等价于此前跨UID迁移故障 |

USB HAL人为stopped与allow_suspend=0属于本轮调试状态，未归为新增问题。

## Millet 最窄候选

当前 `persist.sys.powmillet.enable=true`、`sys.millet.monitor=2`、init服务running；`cached_apps_freezer=null`，`persist.sys.millet.cgroup`未设。真机rc依旧是 disabled 服务，由 `sys.millet.monitor=1` 显式启动，onrestart写2。

沿用 [R14审计](hyperos-r14-residual-log-audit.md) 已证明的兼容路径：在缺Xiaomi内核接口设备上关闭 `persist.sys.powmillet.enable`，同时处理产品默认和已有data持久值，干净重启后验证至少两次180秒alarm周期。已有工具 `prepare-startup-compat.py --kind product-props` 只生成默认属性补丁，不更新data。

此开关也影响OEM job/network/Immobulus与CN Google freezer否决。当前freezer设置为null，不能复用“已disabled”的旧假设；应记录并验证实际冻结策略及后台恢复。不能把 `persist.sys.millet.cgroup=true` 当修复，它会重挂cgroup。当前socket错误无errno，已有证据支持协议不兼容方向但不应声称本轮已捕获socket syscall的精确errno。

## ThermalResourceMonitor 最窄候选

`out/hyperos-r8/framework/decompiled/sources/com/miui/server/mirim/scenario/customscenario/resourceredline/ThermalResourceMonitor.java` 硬编码 `/sys/class/thermal/thermal_message/board_sensor_temp`。`readTemperature()` 捕获IOException后返回MIN_VALUE，`onSample()` 对无效值仍scheduleNext，因此无限重试与日志精确对应。

这不是崩溃、不是权限拒绝，也不能推出AOSP/内核热管理失效。当前设备有thermal_zone0–14而无thermal_message。

窄修复应让此OEM附加监视器在节点不存在时标记不支持并不启动采样；保留真实温度可用时的原逻辑，以及独立Thermal HAL/内核保护。若要适配真实传感器，则需先核实sensor语义、单位和阈值，不能随意把任一thermal_zone链接过去、伪造0度或全局关闭热管理。本轮尚未修改framework，也未证明有独立属性开关。

## cadlauncher 的证据边界

此前 [R15审计](hyperos-r15-cadlauncher-audit.md) 的缺失vendor应用域实现仍是历史故障的强候选。当前 `com.xiaomi.mslgrdp` 位于 `/product/app/MSLgRdp`，installed=true、hidden=false、enabled=0；cadlauncher不在PM内，不能靠启动不存在的包验证策略。

已保存当前 `/sys/fs/selinux/policy` (1534418字节)，本机无seinfo/sesearch，尚未对实际二进制策略完成查询。不要从本轮零AVC推出已经恢复mslg_app appdomain；也不建议在没有实际失败包/用途确认前加权限或换普通应用域。
