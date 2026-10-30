# Fusion保护运行时验证与RescueParty事件

## 已证实改善

当前观察目标为HyperOS实际库SHA a4ba29620bc241c8cc4ab07b9dee05fd3e29e170855a94f0bddbd8ab8594dcaf。新诊断APK保持原PASS判据，冷启动前Fusion q为0；随后捕获acc/gyro整段初始零值，正常原始数据恢复后Gravity/GameRotation仍持续NaN。上游HAL也已从实际FFC ELF确认valid=false缓存仍被封成新时间戳事件，不能称这些默认零为有效测量。

临时候选SHA3c54d8c74b94fa69e96f4c1dd5862983e7658e78ce81c2b3d234bf987dda2e21经root-owned tmpfs、原system_lib_file标签绑定，重启zygote。新system_server PID9675的namespace路径哈希及实际maps均核对候选，SELinux Enforcing。三轮12项strict检查全部PASS，raw warmup仍有6–7个零向量/轮，但fusion无NaN。

随后恢复原库并重启框架做控制：同样原始warmup7个零，Gravity/GameRotation再次持续NaN。这支持保护对本次零向量污染的改善；并非靠改变探针PASS或制造输出。再次绑定候选的后续观测未完成，见下节。初次helper曾因Android mksh的hash别名在前置校验退出（无绑定/服务修改），函数更名hos_sha后才实际应用，失败日志保留。

## 非预期系统重启

再次绑定/框架重启后，后续身份核验发现机器已重启至crDroid B，boot ID7f4a6ba6-b6ba-4e27-9d03-8d516dddd037；当前libsensorservice SHA7d64…不是候选。临时tmpfs绑定已随重启消失，没有永久改system分区。不得将这次CR观测算作HyperOS候选结果。

当前boot reason显示reboot,shell。只读挂载HOS userdata（ro,noload）检查：没有新增native tombstone，15:38 shutdown-metrics为reason:recovery；misc_hos BCB明确为boot-recovery及--prompt_and_wipe_data、--reason=RescueParty,null。因此自动救援请求已确认；连续受控框架重启很可能触发救援计数，不能因此断言库发生native crash。用户随后明确确认没有手动操作，属于设备自动重启。后续停止反复zygote热重启方式，不关闭RescueParty。

为避免下次误入清除提示，先完整备份4MiB misc_hos，再精确清零BCB command/status/recovery的0..831字节；其余全部字节逐一核对未变，普通misc全区哈希保持。未执行userdata擦除/格式化。备份与verification.json位于cache/hos-r19-fusion-guard/recovery-request。数据传输用的临时文件已删除。只读检查显示persist.sys.disable_rescue仍false，没有修改该保护。

## 持久候选与剩余工作

R20 system EROFS仅替换该库，4807条目内容/metadata全量核验仅一库变化，原R18属性上下文保持。六分区super已解包回读匹配，SHA51c3554f3c97ad9c838eb86903b0beca59f2f61baa4e852f30acd6708bf184c3；相对R18仅system_a变化，另五分区字节保持。尚未部署或完成冷启动验证。

producer候选按Sample.valid门控，无有效样本不发事件；真实有效零仍发送，并为空vector下游补早退，避免front()未定义行为及无谓FMQ/wake。该HAL正在独立双构建/ABI审计，尚未上机。框架guard和producer修复不能互相替代。

证据目录：~/.cache/hos-r19-fusion-probe、hos-r19-fusion-guard、hos-r20-fusion。真实手指测试仍待用户方便；上述测试不涉及指纹录入或模板写入。
