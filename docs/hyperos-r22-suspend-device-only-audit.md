# R22 device-only策略与休眠现状

只读现场boot64b3ea5c-0428-4d05-ba5a-bda6586d4dd3，R22 super配独立指纹内核，当前无加载模块。未改变任何休眠/USB属性、未释放锁、未设置RTC或触发挂起。

息屏时PowerManager Wakefulness=Asleep，WakeLockSummary0、应用wakelocks0，框架两个SuspendBlocker均false；内核suspend success/fail均0。/sys/power/wake_lock仍有gaokun3_nosuspend、gaokun3_usb_device_only、gaokun3_usbrole。allow_suspend=0且device_only=true。因此当前不能将不休眠归因于旧FindDevice服务，也不能只把allow_suspend设1当作修复：device-only另有专用保持锁。

部署脚本与RC已原样保存到~/.cache/hos-r22-suspend-audit/。需要在满足用户“不自动切host”的要求下核对实际USB/PHY挂起与恢复，而不是删除既有失败保护锁。RTC pm8xxx存在、wakeup=enabled且当前wakealarm空，只是潜在定时恢复通道，尚未验证其能从目标状态唤醒，不能据此直接保证实验可恢复。

后续先复核当前内核与既有device模式挂起故障的关系、拔线状态及UDC生命周期，再设计有独立恢复路径的限定测试；本记录不宣称休眠或USB兼容问题已修复。


## 运行时DT与RTC验证更新

当前实际DT的soc@0/rsc@18200000/regulators-2/ldo9具有regulator-always-on，regulator-name为vreg_l9d；regulator_summary显示912mV启用。归档补丁0086已经处理旧的L9d断电问题，不能将旧#52/#54/#56直接作为当前device模式必定复位的结论。

在保持全部三锁、USB role和持久属性不变的条件下，临时RTC +5秒闹钟触发pm8xxx_rtc_alarm IRQ计数0→1，闹钟已清空，suspendstats仍0/0。仅证明清醒状态RTC中断可工作，不代表目标s2idle已验。实际pm_test为none、mem_sleep为s2idle。

原shell短时测试未执行：USB断链可能清理ADB进程组，trap不能覆盖SIGKILL。正在准备独立session/cgroup2的native监督器，使用独立CLOCK_BOOTTIME_ALARM timerfd，先READY再显式gate；测试结束前必须读回原三锁，失败保留自有保护锁。当前仍未释放原锁，未进行新的真实休眠。
