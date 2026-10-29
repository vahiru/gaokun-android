# R16 扩容后的功能回归与 Millet 处理

本次通过无线ADB `192.168.10.182:5555` 在隔离HyperOS执行，boot ID `9eb885f6-2f6a-4298-a030-bfe416f671ac`。证据目录 `/Users/vahiru/.cache/hos-r16-regression/`。未重启，保留用户要求的USB device临时保持策略，未改SELinux。

## 实施变更

确认Millet因缺少Xiaomi内核socket协议约每9秒重启。将已存在的兼容开关 `persist.sys.powmillet.enable` 从true改为false，并执行 `stop millet_monitor`。约65秒后仍stopped、无进程，完成视频测试后再次确认stopped。持久属性读回false。

这是当前重启循环的运行时止损与下次启动兼容配置，不能称完整重启验收。该属性在system_server部分类加载时缓存，OEM job/network/应用冻结策略完整变化需要下次启动验证。未改cached_apps_freezer、cgroup挂载或温控服务。恢复方式（仅决定重新启用时）：guard后把persist.sys.powmillet.enable设true，重启复测；只恢复属性不足以刷新类缓存。

## 测试结果

- /data实际p12，370G文件系统、约360G可用；p2/p12起止与新分区日志一致。
- 存储16MiB写入/fsync/读回一致，384ms。
- 应用HTTPS实际200、785字节；无线ADB命令往返可用。
- 加速度/陀螺/重力/游戏旋转向量同时订阅，均有有效事件，NaN/非有限/时间戳异常均0。未证明物理旋转方向精度。
- 麦克风144000采样，RMS980、峰值16394；AudioTrack推进48000帧。未做人工音质/听感验收。
- 后摄0取355帧、前摄1取267帧；平均亮度分别3.45/1.67，画面偏暗，不能宣称成像内容质量通过。测试未保存图像或音频。
- Keystore AES/GCM通过，不等于硬件证明验证。
- BLE收到214条广播；未测试配对/耳机。
- 自动选择AVC硬解c2.v4l2.avc.decoder，编解码120/120、EOS=true。
- AVC/HEVC/VP9各Surface与ByteBuffer两路径，全部60帧、EOS、RESULT PASS；三次ByteBuffer均实际读取60帧像素。Codec HAL PID全程790未变。
- Recents资源与UID10132匹配。无线ADB持久端口5555，USB仍device，端口HAL仍按用户要求stopped。

首轮安装-g未实际授予探针运行时权限，导致音频未初始化、两摄/蓝牙SecurityException；明确授予四项正常运行时权限并读回granted=true后重跑才得到上述成功结果。未绕过隐私开关。测试结束卸载探针并清理本轮媒体样片/执行文件。

## 尚未关闭

设置触摸时卡片边缘融合：正确首页蓝色候选曾截图生效，但尚无完整用户验收与持久部署，本次overlay list已无该临时项，不宣称解决。OEM ThermalResourceMonitor仍读取不存在的board_sensor_temp；不能用伪造温度或禁用正常温控掩盖。当前cadlauncher不在已安装包中，未观察到其旧崩溃不代表策略已修复。休眠本轮因用户要求保持USB device而主动禁用，不作休眠验收。
