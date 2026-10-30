# R19 只读传感器RPC诊断验证

设备保持R18，boot ID `1d6d8c09-52a8-4a2a-81fc-f93f17898df8`。未启用ALS、未设置-W、未写remoteproc state或修改registry/校准数据。没有替换持久镜像。

## 构建与ABI

生产435ed1c加既有逐段CR修复，仅修改apps_std.c/Android.bp，加入RPC边界校验与进程最多64条的拒写元数据日志。有效w/a仍返回20。独立host ASan/UBSan fixture通过，包括并发上限和原读失败码对照。

候选hexagonrpcd SHA256 `6f9be656f8691b3137047c9ac2a06d5b043a9285591696ba168dbd35aab2c8a9`；唯一新增依赖liblog.so。共享libhexagonrpc.so仍为 `16f700635c88226f585ce9277d154631ae3e741149740851be857733f73023f2`，字节和动态符号一致。构建原源已恢复，VM已deallocated。

## 运行时诊断及恢复

`tools/hos/runtime-rpc-trace.sh`在隔离guard、namespace、原daemon/共享库hash、SLPI running、两服务running、无自动恢复在途等条件下执行。先记录自动恢复原值（空），临时设0避免服务控制竞争，持有独立probe锁，停止Sensors HAL与RPC服务，tmpfs候选以原hexagonrpcd_exec标签bind后重启两服务。没有直接重启DSP。

诊断前、候选运行时、恢复后分别运行严格四传感器探针：每种均约343–348事件，NaN/非有限/时间戳错误均0。证明正常IMU和融合数据可继续交付，不是光感/绝对方向校准验证。

首次热重启后的定点logcat使用了 `hexagonrpcd-denied:W`，但候选事件级别为 `ANDROID_LOG_INFO`；因此这次未捕获不构成任何拒写请求不存在的证据。

第二次在替换服务之前启动连续90秒采集，使用 `logcat -b all -v threadtime -T1 -s hexagonrpcd-denied:V`。确认运行进程 `/proc/<pid>/exe` 的SHA与候选一致、域为hexagonrpcd，未设置抑制标签的属性，策略允许写logd；完整采集仍为0行。此次覆盖服务启动窗口，但只是热重启，DSP注册表可能早已初始化，所以不能排除正常冷启动的写入请求。没有激活ALS、重启remoteproc或写入校准数据。下一步改为冷启动只读诊断，并保留logcat缓冲中的早期事件（不能使用 `-T1` 丢弃已有日志）。

第二次测试后也恢复原daemon和自动恢复属性；严格四类传感器探针通过，测试APK与tmpfs已清理。证据为 `~/.cache/hos-r19-rpc-continuous/events.txt` 与 `imu-restored.txt`。

随后显式恢复原daemon，读回SHA256 `122b57f908ad74c02fb9b43621fe8aa2f3a46c410c075bef0e5f570579bbf69c`；Sensors/RPC均running，自动恢复值恢复空（原策略启用），SLPI running、USB device、SELinux Enforcing。恢复后再次严格传感器验证通过。测试APK、tmpfs和probe独立锁已清理，原device-only锁保持。

## 与写能力候选分离

`patches/hexagonrpc-registry-production-backport.patch`是另一项尚未上机的候选，旧interp2及默认只读契约保留，显式-W受限写通过五套离线fixture。Android构建、SELinux输出目录和真实DSP协议尚未验收，rename/部分方法仍未支持；不能把它与本次已验证的只读日志二进制混为一谈。

证据位于 `~/.cache/hos-r19-rpc-runtime/`、`hos-r19-rpc-trace/` 和 `hos-r19-sensors/registry-trace-independent.log`。
