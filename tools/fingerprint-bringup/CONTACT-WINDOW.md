# 有界接触准备窗口（离线候选）

## 当前使用：立即有界轮询

15秒idle等待在明确无人接触时仍导致首个1d应答非法；因此新接触测试不再使用
`contact_wait_ms`空等，而使用只读uint `contact_poll_count`。

- 默认0：原fp_active_scan固定10轮行为保持。
- 非零范围10..200，只允许active_scan且不能status_only，与非零contact_wait_ms互斥。
- version/info成功后打印 `ready-for-contact active-polling`，立即开始第一轮1d。
- 仅对合法但无帧的响应按原100ms间隔继续；观察flag1立即读取一次SRAM。
  任何协议/SCM错误仍立即停止，没有错误重试、没有新命令或模板操作。
- 150轮名义显式等待约18.05秒，200轮约24.1秒，另加实际SMC耗时；应记录实际首末
  报告时间，不用轮数推断真实接触时长。100轮名义约12秒，不能保证超过15秒。

新增actual-header测试保留并断言原184项注错，再增加长窗口首/中/末错误停止，
共187注错；覆盖200轮无帧不得读SRAM、最后一轮有帧恰好读一次、非法limit拒绝和
旧4KiB响应容量编译拒绝。实际init/run_probe提取测试同时保留旧参数回归并验证
新模式前置失败不提示/不扫描、ready之后无idle、互斥及上下界。

## 旧idle参数的保留语义

现有reset-helper会在写probe之前复位MCU；若用户提前接触，手指与启动/校准时段
可能重叠。新参数把“版本和信息查询已成功”与“开始原有帧标志轮询”之间分开，
不增加MCU命令、不伪造事件，也不证明硬件内部校准一定已经完成。

`contact_wait_ms` 是只读uint模块参数，默认0，最大15000。非零值只允许
`active_scan=1,status_only=0`；不满足时模块init返回EINVAL，尚未建立debugfs或发SMC。
零值保持既有模式选择、日志与等待行为，包括原先status_only优先的行为。

在8031、query_version、query_info全部成功之后，非零窗口会：

1. 打印 `fpversion: ready-for-contact wait_ms=...`，明确版本/信息阶段已完成。
2. 保持既有模块引用、共享缓冲、secure_pending及reset-helper持有的GPIO，
   `msleep(contact_wait_ms)`一次。
3. 打印 `contact-window-complete`，调用未经修改的fp_active_scan：最多10次真实
   MCU帧标志查询，有帧才读一次SRAM，无帧返回ENODATA。

前置查询失败时不打印ready、不等待、不进入帧扫描。通信状态不确定仍按原规则保留
资源并要求重启恢复；窗口本身不清secure_pending、不切GPIO、不创建重试循环。
测试编译后可由主线使用例如 `active_scan=1 contact_wait_ms=10000`，但必须先核对
新模块哈希和内核兼容性。本文没有执行insmod、复位、采图或远端构建。

主线应让用户在明确ready提示后接触并保持到实际采样阶段；不要把参数值当作已经
观察到接触或得到有效指纹。结果仍须区分无帧、传输校验、解码质量与录入/认证能力。

## 离线测试

`test-fp-contact-window.py` 提取真实validate_contact_window、模块init和run_probe，
仅mock内核/通信边界，以ASan/UBSan执行：

- 15001/UINT_MAX、非active的非零窗口和status_only冲突在debugfs之前拒绝。
- 所有默认0模式保持原行为，且没有新增ready标记或额外等待。
- 1、10、5000、15000只等待一次，顺序严格为版本→信息→ready→等待→扫描。
- 版本/信息/LOAD失败不等待或扫描；协议失败正常shutdown，通信不确定继续pin。
- ENODATA没有重试；扫描通信失败不清secure_pending或释放未知状态资源。

```sh
python3 tools/fingerprint-bringup/test-fp-contact-window.py
```
