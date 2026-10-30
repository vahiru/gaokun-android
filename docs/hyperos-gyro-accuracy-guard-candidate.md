# 陀螺仪UNRELIABLE输入的融合消费保护候选

本轮只生成离线候选和测试，没有部署、改设备或改量程。已有证据是每次启动陀螺仪
前两个事件accuracy=0、三轴float bits均为c20ba058，之后accuracy=3且值恢复正常。
仅凭数值不能丢弃真实极端运动；此候选依据状态字段，不依据幅值。

## 实际OEM二进制证据

R21已部署库SHA256：
`3c54d8c74b94fa69e96f4c1dd5862983e7658e78ce81c2b3d234bf987dda2e21`。
它保留早先ACC零norm保护，以下gyro逻辑与原OEM库一致：

- `SensorFusion::process` VA0x684c4。gyro分支按类型、handle和0<dT<50ms筛选，
  0x68598/6859c只从event+18/+20复制三float；685bc/685d8/685f4调用handleGyro。
  此路径不读取status。
- 实际AIDL→native转换函数0x50d10的type4跳转表项指向0x50d90；0x50dcc/50dd0
  将status字节复制到native event+0x24。它也明确写event.size=0x68。
  因此offset0x24有OEM实际转换代码证据，不只引用AOSP头文件。
- `Fusion::handleGyro` 0x56c44只接受vec3+dT；初始化后尾调predict，没有accuracy参数。
- SensorService的0x4ead4–4eae8批循环对每个0x68字节event调用process，没有逐事件
  status筛选。因此已有有限但UNRELIABLE的陀螺仪事件会进入融合路径。

## 独立补丁范围

工具：`tools/hos/patch-fusion-gyro-status-guard.py`。
只接受上述精确R21 SHA，不接受原OEM库或其他候选，也不覆盖旧ACC patcher。

候选：`~/.cache/hos-gyro-status-guard/candidate/libsensorservice.so`
SHA256：`a24232d05bba1ffb748baf9f9fa8d75a1b066462460daaf60d354dfd36d12185`。

只改：

- RX filesz/memsz增加16字节（文件0xd0–0xe0）。
- 0x68524原 `ldr x8,[x20,#0x10]` 改成一条B。
- 0x80a08–0x80a18放四条指令：读有符号status；status==0跳原0x685f8；
  否则重放原ldr并回到0x68528。

无效状态走原来的timestamp载入/更新和PAC/栈恢复尾部，**仍把mGyroTime推进到该事件**。
下一可靠样本使用正常间隔，而不是跨越整个丢弃窗口积分。不会改status、raw event、
传给其他消费者的原始数值或时间戳。accuracy1/2/3及其极端值、真实零角速度均保留。
现有原始精度映射只有0..3；候选仅对精确0作新处理，未扩大为任意状态过滤。

旧ACC guard、全部其他代码/符号/重定位/依赖/文件长度保留。buildID仍保留原值，
区分库必须用整文件SHA。新增16字节没有独立FDE，异步采样落在guard时回溯可能截断；
guard没有修改SP或LR，所有返回仍走原函数尾部。

## 验证与界限

`test-fusion-gyro-status-guard.py` 检查错误原hash、入口/丢弃目标/RX/gap拒绝、
四条ARM64指令精确分支和既有ACC保护未变。

`fusion-gyro-status-study.py` 在独立macOS arm64进程执行实际OEM转换函数和process，
仅mock传感器type/handle getter及handleGyro消费记录边界，不执行服务构造或设备调用。
已验证：

- 原OEM消费accuracy0极端三轴；候选不调用融合消费但推进时间。
- 两个UNRELIABLE事件之后的可靠事件dT仍为20ms。
- 所有accuracy1/2/3的极端值与零角速度均保留，原/候选对象逐字节一致。
- 三个融合模式启用时，无效状态全部跳过，可靠状态全部调用。
- 原始event在所有调用前后逐字节不变。
- 长间隔及既有时间差上限行为保持，没有把丢弃时长积到下次可靠样本。

这些是实际消费路径/ABI/时间基准验证，**不是完整Fusion估计器或真机方向精度验收**。
RAW陀螺仪的启动异常仍需在SSC生产端解释；maxRange不因本候选改变。

```sh
python3 tools/hos/test-fusion-gyro-status-guard.py ~/.cache/hos-r19-fusion-guard/candidate/libsensorservice.so
python3 tools/hos/fusion-gyro-status-study.py ~/.cache/hos-r19-fusion-guard/candidate/libsensorservice.so \
  --candidate ~/.cache/hos-gyro-status-guard/candidate/libsensorservice.so
```
