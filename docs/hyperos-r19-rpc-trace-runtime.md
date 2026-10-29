# R19 只读传感器RPC诊断验证

设备保持R18，boot ID `1d6d8c09-52a8-4a2a-81fc-f93f17898df8`。未启用ALS、未设置-W、未写remoteproc state或修改registry/校准数据。没有替换持久镜像。

## 构建与ABI

生产435ed1c加既有逐段CR修复，仅修改apps_std.c/Android.bp，加入RPC边界校验与进程最多64条的拒写元数据日志。有效w/a仍返回20。独立host ASan/UBSan fixture通过，包括并发上限和原读失败码对照。

候选hexagonrpcd SHA256 `6f9be656f8691b3137047c9ac2a06d5b043a9285591696ba168dbd35aab2c8a9`；唯一新增依赖liblog.so。共享libhexagonrpc.so仍为 `16f700635c88226f585ce9277d154631ae3e741149740851be857733f73023f2`，字节和动态符号一致。构建原源已恢复，VM已deallocated。

## 运行时诊断及恢复

`tools/hos/runtime-rpc-trace.sh`在隔离guard、namespace、原daemon/共享库hash、SLPI running、两服务running、无自动恢复在途等条件下执行。先记录自动恢复原值（空），临时设0避免服务控制竞争，持有独立probe锁，停止Sensors HAL与RPC服务，tmpfs候选以原hexagonrpcd_exec标签bind后重启两服务。没有直接重启DSP。

诊断前、候选运行时、恢复后分别运行严格四传感器探针：每种均约343–348事件，NaN/非有限/时间戳错误均0。证明正常IMU和融合数据可继续交付，不是光感/绝对方向校准验证。

热重启后的定点logcat未捕获hexagonrpcd-denied记录，采样中也未见目标DSP fatal。没有在服务启动前建立连续日志捕获，所以不能排除早期日志已滚出；不能把“未捕获”解读为当前固件不需要写入。也没有通过激活ALS强行触发请求。下一次诊断应在服务启动前连续采集，或覆盖正常冷启动的registry初始化。

随后显式恢复原daemon，读回SHA256 `122b57f908ad74c02fb9b43621fe8aa2f3a46c410c075bef0e5f570579bbf69c`；Sensors/RPC均running，自动恢复值恢复空（原策略启用），SLPI running、USB device、SELinux Enforcing。恢复后再次严格传感器验证通过。测试APK、tmpfs和probe独立锁已清理，原device-only锁保持。

## 与写能力候选分离

`patches/hexagonrpc-registry-production-backport.patch`是另一项尚未上机的候选，旧interp2及默认只读契约保留，显式-W受限写通过五套离线fixture。Android构建、SELinux输出目录和真实DSP协议尚未验收，rename/部分方法仍未支持；不能把它与本次已验证的只读日志二进制混为一谈。

证据位于 `~/.cache/hos-r19-rpc-runtime/`、`hos-r19-rpc-trace/` 和 `hos-r19-sensors/registry-trace-independent.log`。
