# R20 Fusion system 镜像候选

2026-10-30：只构建system EROFS，未打包super、未部署，也未修改vendor或历史out。
输入为已验证的R18 system，保留其中四项libc诊断属性上下文修复。

## 运行时依据

父线程已临时绑定审过的Fusion保护库并重启zygote，实际进程命名空间/maps匹配
候选SHA，SELinux仍Enforcing。增强传感器探针三轮12项均通过；原始accel/gyro预热
每轮仍出现6–7个零向量，但gravity/game rotation没有NaN。这支持拒绝无效输入
保护融合初始化的效果，不能替代producer有效性修复或完整冷启动验收。
证据为 `~/.cache/hos-r19-fusion-guard/runtime-sensors.txt` 与 `runtime-summary.json`。

## 产物

- 输入：`~/.cache/hos-r18-libc/system/image.erofs`
  SHA256 `d76882116fec9ce910afa252083462f2d9c49e6fe01ec59980c66fa85061544b`
- 输出：`~/.cache/hos-r20-fusion/system/image.erofs`
  SHA256 `f13825109282103654e0e92fc95d899ebf1ec1a88a1b9824362e0fb8c9a5599a`
- 大小：958590976字节。
- 唯一替换：`system/lib64/libsensorservice.so`
  原SHA `a4ba29620bc241c8cc4ab07b9dee05fd3e29e170855a94f0bddbd8ab8594dcaf`
  →候选SHA `3c54d8c74b94fa69e96f4c1dd5862983e7658e78ce81c2b3d234bf987dda2e21`。

采用privileged `hos-tools:alpine3.24` 容器保留security xattrs，workspace/base/candidate
均只读挂载，只有新cache输出目录可写。调用：

```
python3 tools/hos/repack-erofs.py --image /base/image.erofs \
  --replace system/lib64/libsensorservice.so \
  --with-file /candidate/libsensorservice.so --out /build/system
```

## 核验范围

重打包后重新提取，比对全部4807条目。独立再次比较R18旧tree与新tree，路径集相同，
仅上述库的文件SHA改变；其他文件内容、软链接目标、uid/gid、mode与全部提取xattrs
一致。库仍为0:0、0644、`u:object_r:system_lib_file:s0`。

`system/etc/selinux/plat_property_contexts` 内容及元数据保持，SHA为
`8e101399e5877562df1b44e1eecad2b2de31b2db84e1ff95542e4bb047a57c84`。
输入镜像在构建后重新流式计算SHA仍匹配原pin。

证据：输出目录下 `manifest.json`、`tree.json`、`commands.json`，父目录的
`system-independent-review.json` 与 `system-build.log`。镜像尚未用于冷启动；
保护库保留原buildID及短guard无FDE的局限见 `hyperos-fusion-zero-guard-candidate.md`。

完整guard-only super已打包，SHA256 `51c3554f3c97ad9c838eb86903b0beca59f2f61baa4e852f30acd6708bf184c3`，六分区解包哈希全部匹配，相对R18仅system_a变化。尚未部署；运行时临时验证及RescueParty事件见 `hyperos-fusion-guard-runtime-validation.md`。
