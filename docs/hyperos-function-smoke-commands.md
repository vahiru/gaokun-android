# HyperOS 本轮功能抽查命令与判据

离线整理；未执行设备命令，未启动VM。待主代理触摸测试结束后串行运行，避免Activity切换/旋转干扰当前测试。不要把历史R14通过当作本轮已通过。

## 现成探针

- App：`out/hyperos-r14/probe/probe.apk`，源码 `probe/src/org/gaokun/hosprobe/Main.java`。入口 `org.gaokun.hosprobe/.Main`。无extra默认跑存储、HTTPS、传感器、音频、YUV相机、Keystore、BLE扫描、codec枚举；没有只跑音频或传感器的开关。
- `--ez media true`：两摄JPEG + AVC roundtrip。
- `--ez codec_only true --ei frames 120`：仅AVC roundtrip，可用 `--es decoder c2.v4l2.avc.decoder` 指定硬解；不指定则测试真实自动选择。帧数范围1..240，默认120。
- `--ei orientation 0/1`：仅方向/触摸诊断，0强制横屏、1强制竖屏，不运行上述测试。它不是自动旋转传感器测试。
- Native：`out/hyperos-r14/codec/codec-output-probe`，源码同目录cpp；三个参数：样片、组件、模式。`drain`走ImageReader Surface，`bytebuffer`配置NULL Surface并实际读取输出像素，`seek:K:N`走Surface flush/seek。

## 准备与结果读取

下面是待执行命令，不含设备序列号；多设备时主代理需给每条adb指定同一 `-s`。

```sh
adb shell 'grep -qw androidboot.force_super_partition=super_hos /proc/cmdline && getenforce'
adb shell cmd wifi status
adb shell settings get global bluetooth_on
adb shell settings get system accelerometer_rotation
adb shell settings get system user_rotation
adb shell pidof android.hardware.media.c2-service-v4l2
adb install -r -g out/hyperos-r14/probe/probe.apk
```

App需要CAMERA、RECORD_AUDIO、BLUETOOTH_CONNECT、BLUETOOTH_SCAN运行时权限；若安装-g未授予，显式 `adb shell pm grant org.gaokun.hosprobe android.permission.CAMERA` 等逐项授予，勿用权限拒绝判断硬件坏。App调试签名已装时可直接用现存APK，不必重装。若签名不匹配先保留状态调查，不盲目卸载。

每次启动前force-stop以确保extras生效；启动后等待本次日志DONE，再读取文件，避免把上次results.txt误当新结果。不要清空全局logcat，当前调试证据仍需保留。

```sh
adb shell am force-stop org.gaokun.hosprobe
adb shell am start -W -n org.gaokun.hosprobe/.Main
adb shell run-as org.gaokun.hosprobe cat files/results.txt
adb shell logcat -d -s HosProbe:I
```

默认bundle可能用时一分钟以上，外网超时加额外20秒、BLE扫描8秒。会使用摄像头/麦克风并播放1秒660Hz测试音，但不保存照片/录音；暂不具备逐项选择开关。

## 音频、相机、传感器、蓝牙判据

| 项目 | 默认bundle真实做什么 | 通过证据与限制 |
| --- | --- | --- |
| 麦克风 | AudioRecord 48k mono采样约144000点 | n>0且peak>0；记录RMS/peak。证明有非零采样，不证明音质、路由、真实声源正确。需听说话/回放才能判端到端 |
| 扬声器 | AudioTrack播放测试音 | playback head推进，历史48000帧；必须人工听到正确音量/无爆音才可写扬声器端到端通过 |
| 两摄YUV | 逐cameraId打开Camera2、3秒repeat到ImageReader | 每摄>5帧、记录meanLuma。证明真实采集缓冲，非枚举；不保证内容/色彩/对焦/完整分辨率 |
| 传感器 | 类型1加速、4陀螺、9重力、15游戏旋转向量，每类1.6秒事件 | count>0，记录last值；MISSING不是PASS。需实际改变姿态见数值变化，不能仅有静止重复事件就认方向准确 |
| 蓝牙 | adapter开启时BLE扫描8秒 | 收到广播说明扫描链路，未测配对、连接、耳机音频；0广播在无外设环境属不确定，不应判硬件坏 |

JPEG补测：

```sh
adb shell am force-stop org.gaokun.hosprobe
adb shell am start -W -n org.gaokun.hosprobe/.Main --ez media true --ei frames 120
adb shell run-as org.gaokun.hosprobe cat files/results.txt
```

每cameraId应 `PASS JPEG ... bytes=...`，但探针只检查>100字节与FFD8起始标志，**不是完整JPEG解码验证**。它还会运行AVC测试。摄像头录像、AV同步、视频文件回放没有现成探针覆盖，仍需真实相机App录制并回放（拍摄内容须由用户控制）。

蓝牙不自动开关：默认关闭会SKIP；若需要扫描且主代理确认可暂时开启，可先查看 `cmd bluetooth_manager help` 再用设备支持的enable命令，记录原状态并恢复。配对需要真实外设及用户配合，不能用bondedCount充当本轮连接通过。

## 视频：两条路径都跑

准备固定路径（若样片/二进制已有，先核对hash可跳过重复push）：

```sh
adb shell mkdir -p /data/local/tmp/hos-smoke
adb push out/hyperos-r14/codec/codec-output-probe /data/local/tmp/hos-smoke/
adb push out/hyperos-r12/avc.mp4 out/hyperos-r12/hevc.mp4 out/hyperos-r12/vp9.webm /data/local/tmp/hos-smoke/
adb shell chmod 755 /data/local/tmp/hos-smoke/codec-output-probe
adb shell /data/local/tmp/hos-smoke/codec-output-probe /data/local/tmp/hos-smoke/avc.mp4 c2.v4l2.avc.decoder drain
adb shell /data/local/tmp/hos-smoke/codec-output-probe /data/local/tmp/hos-smoke/hevc.mp4 c2.v4l2.hevc.decoder drain
adb shell /data/local/tmp/hos-smoke/codec-output-probe /data/local/tmp/hos-smoke/vp9.webm c2.v4l2.vp9.decoder drain
```

把上述每条最后的 `drain` 换成 `bytebuffer` 再跑一次。固定三样片均60帧：必须60帧、EOS seen、RESULT PASS、exit0；bytebuffer还需 `read_pixels_frames=60`。单纯RESULT PASS只要求正帧数/EOS，主代理必须额外核对60，不能漏帧也算过。checksum用于证据，含padding且不是已校准图像golden，不能当画质正确证明。Surface是渲染至ImageReader，未测试用户屏幕上的视频播放。

AVC跨池容量/自动选择：

```sh
adb shell am force-stop org.gaokun.hosprobe
adb shell am start -W -n org.gaokun.hosprobe/.Main --ez codec_only true --ei frames 120
adb shell run-as org.gaokun.hosprobe cat files/results.txt
```

必须decoder名称符合预期（ccodec4通常c2.v4l2.avc.decoder）、encoded=decoded=120、EOS=true。再用 `--es decoder c2.android.avc.decoder` 验软件回归；编码器记录实际名称。测试前后HAL PID应不变；同时检查crash buffer、REQBUFS/session open失败及iris power collapse异常。若旧崩溃污染VPU，正常重启后重测，不能反复覆盖池配置掩盖前置故障。

## Wi-Fi / 实际网络

```sh
adb shell cmd wifi status
adb shell ip route
adb shell dumpsys connectivity
```

这些只确认关联/IP/路由及Android VALIDATED；默认bundle的 `PASS https code=200 bytes>0` 才是App DNS/TLS/HTTP实际传输。需在connectivity中确认活动网络确为Wi-Fi（排除蜂窝/VPN路径误判）。若HTTPS目的站不可达，结合网关、DNS和其他已授权站点分层定位，不立即归因Wi-Fi。重连/休眠恢复要另测；链路1200Mbps不等于吞吐。不要把热点密码写入日志或报告，沿用已保存网络。

## 自动旋转

先完成主代理正在进行的触摸工作；默认bundle的传感器事件PASS只证明数据到App。实际旋转需启用自动旋转、手动转动机身，观察前台允许旋转的App画面与输入viewport同步变化；记录 `dumpsys window displays` / `dumpsys input` 前后。`orientation` extra是强制Activity方向，只能用于展示/坐标映射检查。若用 `settings put system ...` 改设置，先记录是否原为null，结束按原值恢复或delete；不在正在诊断触摸时旋转。

历史参考：R14自动和显式无Surface AVC120/120 EOS、三格式Surface60帧与像素读回60帧均已记录；R11音频/传感器/YUV/BLE仅有限范围通过。历史报告时间与当前boot必须分开，逐项记录本轮结果。测试结束恢复临时设置，关闭/force-stop探针，留存日志；不要因功能抽查擅自关闭用户现有蓝牙或卸载正在使用的触摸诊断App。

## 真实触摸轨迹离线解析

已有 `getevent -lt` 文本可用以下命令分析，不连接设备：

```sh
python3 -B tools/hos/analyze-touch-events.py /tmp/hos-touch-diagnostic-raw.txt
python3 -B tools/hos/analyze-touch-events.py captured.txt --width 1600 --height 2560
python3 -B tools/hos/test_analyze_touch_events.py
```

工具按MT protocol-B slot保留稀疏坐标更新；默认slot0，ffffffff结束接触。JSON每个tracking生命周期报告active SYN_REPORT帧数、tracking开始至release报告的持续时间、首末完整xy、dx/dy、最大相邻active报告时间差，以及全记录最大同时触点和越界坐标。坐标合法范围为0..width-1、0..height-1，轴范围不同应传实际尺寸。

release报告不增加active帧数，终点取最后active SYN坐标。开始录制时已按住且无TRACKING_ID事件的手指无法重建为完整新接触；末尾未松手标capture_ended_active，不假定发生断触。SYN_DROPPED会终止当前连续轨迹并标记未知，不跨丢失窗口猜测。空文件输出no_events而非设备故障；同理，静止时驱动可能不输出新事件，长报告间隔不自动等于丢帧。真实短滑诊断需结合用户持续按住的动作、连续TRACKING_ID是否提前结束、新接触是否重建与原始日志判断。
