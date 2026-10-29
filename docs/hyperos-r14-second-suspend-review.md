# R14 第二次拔线休眠复测：独立审查

2026-10-29。只读分析 `/tmp/hos-suspend-trace-collected.txt`、`/tmp/hos-suspend-kernel-check.txt`、`/tmp/hos-power-process-v7.2.9.c`；未操作设备。

## 结果

boot_id 仍为 `1cf6b929-49cd-47cc-875a-0c34aa38d11e`。与主代理保存的前置计数相比，success从6增至8，fail从2增至3。新增失败属于 **freeze**：failed_freeze从0增至1，failed_suspend仍为2，各resume失败计数为0。last_failed_errno=-16、last_failed_dev为空。

因此本轮增加两个成功返回的挂起流程，以及一次在冻结用户进程阶段被唤醒事件中止的尝试；没有证据表明本轮新增了设备suspend回调失败。这里的success计数不等于已证明低功耗驻留时间或耗电表现正常。

## 新增失败已能区分：唤醒中止，不是冻结超时

kernel-check第2014行记录：

```
Freezing user space processes aborted after 0.005 seconds
(1653 tasks refusing to freeze, wq_busy=0)
```

v7.2.9 process.c 的 `try_to_freeze_tasks()` 第69–72行，只有 `pm_wakeup_pending()` 成立才将wakeup置true；第89–93行以该值选择打印 `aborted` 或 `failed`。随后仍有未冻结任务时返回-EBUSY。默认freeze_timeout_msecs为20秒，而本次打印耗时0.005秒；这是明确的唤醒中止路径，不是达到超时。

1653是中止瞬间尚未完成冻结的任务数，不能解释为1653个任务卡死。此处是user_only分支，不会执行freezable workqueue busy查询，因此wq_busy=0也不能单独证明全部工作队列无问题。下一次尝试用户进程冻结成功（日志耗时0.008秒），进一步不支持“永久无法冻结”的判断。

## 电源键证据的边界

收集时 `/sys/power/pm_wakeup_irq` 为22，interrupts把IRQ22标为 `pmic_pwrkey`。它支持最近记录的唤醒涉及电源键，与用户按键唤醒相符。但它不是每次attempt的独立历史，不能据此断言首次freeze中止必然由电源键造成；在后续成功挂起/按键恢复之后，该值可能已被更新。

## trace严重覆盖，不能作排除性结论

trace头显示保留56087条、累计写入574461条；各CPU overrun总和518374，正好等于差值。保留事件全部是suspend_resume，其中56043条为PID0的timekeeping_freeze，只有44条其他阶段事件。

保留片段能看到两段machine_suspend以及恢复阶段，但早期事件已缺失。没有保留下来的device_pm_callback_end错误，**不等于没有设备回调错误**。没有ucsi_run_command也不能证明UCSI未出错或初始化恢复；事件可能未发生在窗口、也可能被覆盖。

主代理已在采集脚本对power:suspend_resume增加 `common_pid != 0`，保留 `device_pm_callback_end: error != 0` 并完成start/collect及清理验证。离线查看脚本可确认这两条过滤；它能减少以后采集的idle洪泛，不能补回本次丢失事件。后续仍需检查各CPU overrun，不能仅以过滤设置成功判定记录完整。

## 时间与后续判断

不使用kernel logcat转换后的墙钟entry/exit差值推算真正睡眠时长；这批时间存在挂起偏移。也不将trace的machine_suspend差值直接解释为电源域深睡驻留。本轮0.005秒的判断来自内核直接打印的冻结阶段耗时，只用于区分快速中止与冻结超时。

当前可报告“本轮两次成功挂起流程、一次唤醒导致的冻结中止，未重启”。不能报告“休眠完全正常”“所有回调均无错”或“已修复UCSI”。如需定位中止源，下一轮应使用已减噪的独立trace实例，保留前后计数、完整overrun统计，并对唤醒事件逐轮对应；无需为本次已识别的快速中止直接修改内核冻结逻辑。
