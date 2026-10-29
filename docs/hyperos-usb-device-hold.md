# 临时保持 USB device 模式（2026-10-30）

用户明确要求“先别让它做host”。当前隔离HyperOS boot ID `9eb885f6-2f6a-4298-a030-bfe416f671ac`。

已执行（每次设备写入前核对androidboot.force_super_partition=super_hos）：

1. 保持内核gaokun3_usbrole休眠锁。
2. persist.vendor.gaokun3.allow_suspend从1改为0，停止息屏host切换触发。
3. stop gaokun3_role_host，取消可能还在等待拔线的任务。
4. stop vendor.usb-gaokun3，停止其按对端/无数据超时自动切host的逻辑。
5. 数据角色设为device，vendor.gaokun3.usbrole.adb_pause=0。

即时读回：role=device、UDC=configured、USB端口HAL=stopped、allow_suspend=0。这不是永久驱动修复；停服务仅当前启动有效，而allow_suspend属性会跨重启保留，重启后自动探测服务会重新运行，需重新评估。没有停止adbd、松绑控制器或修改SELinux。

影响：当前调试期间不进入深度休眠；该双角色端口不提供host外设服务；停止端口HAL同时暂停端口角色/连接状态上报。已有ADB连接仍可使用，不能据此宣称所有重新插拔/MTP/充电协商场景都已通过。

恢复原策略（仅在结束固定device模式时执行）：

```sh
adb -s gaokun3 shell 'grep -qw androidboot.force_super_partition=super_hos /proc/cmdline || exit 90; start vendor.usb-gaokun3; setprop persist.vendor.gaokun3.allow_suspend 1; start gaokun3_role_device'
```

不要手工释放gaokun3_usbrole锁；原脚本会依据角色和连接状态管理。原persist.sys.gaokun3.allow_suspend为空，本次未改它。证据保存在 `/Users/vahiru/.cache/hos-usb-device-hold/`。

同时现场只读确认：p2起点839845888、长度108716032扇区；p12起点616448、长度788561920扇区，与搬迁日志一致；HyperOS /data来源p12，文件系统约370G、可用约360G。
