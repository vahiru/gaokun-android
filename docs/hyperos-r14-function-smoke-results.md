# R14 主要功能抽查与触摸调查（2026-10-29）

设备 gaokun3，本次boot ID始终为 `1cf6b929-49cd-47cc-875a-0c34aa38d11e`，cmdline仍隔离到super_hos，SELinux Enforcing。未部署R15诊断内核或尚待集成的属性修复。

## 本轮实际结果

| 项目 | 实测结果 | 边界 |
| --- | --- | --- |
| Wi-Fi/互联网 | 网关3/3回复，App HTTPS 200并读取785字节 | 非吞吐/漫游/长时间稳定测试 |
| 存储 | 16MiB写入、fsync、读回逐字一致，373ms | 非寿命/全盘扫描 |
| Keystore | AES/GCM加密成功 | 未证明硬件级密钥保护 |
| 麦克风 | 144000样本，RMS1237、peak32767 | 非零采样通过；有峰值饱和，音质/实际声源需听测 |
| 播放 | AudioTrack播放头推进48000帧 | 是否听到/是否爆音未人工确认 |
| 后摄0 | YUV357帧，meanLuma3.75；JPEG86426字节 | 机身平放时画面偏暗，未评估画质；JPEG探针只查SOI/长度 |
| 前摄1 | YUV267帧，meanLuma69.62；JPEG58226字节 | 非对焦/录像/完整JPEG解码测试 |
| 蓝牙 | 开启，BLE扫描209次广播 | 未配对/测试耳机路由 |
| 视频 | AVC/HEVC/VP9各Surface60帧、ByteBuffer实际读像素60帧，均EOS | Surface输出至ImageReader，非屏幕影片播放 |
| AVC往返 | 软件编码器→自动选择硬件解码器，120/120帧EOS | 编码器实际为c2.android.avc.encoder，不能称硬件编码 |
| 媒体服务稳定 | HAL PID784测试前后不变，ccodec4、poolmask0xfc0000 | 本轮短时测试 |
| 加速/陀螺/重力/游戏旋转向量 | 严格同时订阅3轮，每轮2s预热+5s采样，全部finite且时间戳递增 | 自动旋转/物理方向精度仍需实物转动复测 |
| 休眠/USB | 新增2次成功休眠、1次快速freeze中止；USB重新configured | 中止由pending wakeup触发；Type-C初始化仍失败，未验证PD/DP |
| 显示比例 | 1600×2560、240DPI，截图布局正常 | 用户反馈所有界面滑动不灵敏，尚未关闭 |
| 充电 | USB powered=true，电量59→64% | 上报温度0、电流/电压上限0，不能据此评估电池热管理/充电功率 |

第一轮测试启动时设备已睡眠/锁定，出现摄像头policy拒绝和麦克风全零，不能用作硬件失败结论。随后正常唤醒、解除无凭据锁屏，确认probe窗口在前台，再跑得到上表结果。未关闭隐私保护或放宽AppOps；只给测试App明确授予声明的临时相机/麦克风/蓝牙权限。

## 传感器探针纠正

原探针只凭events>0标PASS，曾把Gravity末值 `[NaN,NaN,NaN]` 误标通过。已将可维护源码放到tools/hos/probe，新增严格同时监听，检查注册、finite和时间戳；任何预热或采样阶段NaN都FAIL。三个严格实测轮次各传感器均约340–346事件，nonFinite/NaN/badTimestamp均0。不能据此删除首次NaN证据或声称修好了系统融合：同步订阅与原串行订阅的初始化条件不同。

证据本机 `/tmp/hos-smoke-bundle.txt`（睡眠无效基线）、`/tmp/hos-smoke-bundle-awake.txt`、`/tmp/hos-smoke-jpeg-avc.txt`、`/tmp/hos-smoke-codecs.txt`、`/tmp/hos-smoke-strict-sensors.txt`。

## 触摸仍在调查

用户明确所有界面的滑动都不灵敏。当前坐标映射1:1、DPI240，game预设已生效；没有把PressureScale打印舍入0.000误判为0压力。手写笔掌压拦截关闭，但驱动早期palm过滤仍存在；累计拒绝计数不能单独证明故障。

发现确定边缘越界：算法出生轨迹可输出x2560，经过max_x2559的invert再swap得到ABS_Y=-1；另一轴也可能超上界。局部钳位补丁正在准备，只针对边界，不声称它解释所有界面中央滑动不灵敏。

两次短时getevent窗口均未收到真实手指事件，不据此宣称触摸无响应（无人操作也会为空）。目前白色触摸诊断页保持亮屏，同步记录原始event7及App MotionEvent，等待用户3次短滑+1次慢长滑的操作；不能用注入事件替代真实手指验收。测试结束应停止记录、退出/卸载本次临时probe，恢复用户界面。

尚未覆盖：蓝牙配对/耳机、扬声器听测、录像/AV同步、真实键盘/触控板/手写笔操作、外接DP/USB外设、自动旋转与滑动主观体验。以上是抽查，不是“全部功能无故障”。


## 后续导航定位

实采29段触摸接触、1080个SYN_REPORT，28帧出现x1600或y-1，确认边缘越界。最大同时触点2；不能据此认定幽灵触点，因为没有同步用户手指数确认。期间用户已离开触摸诊断页，App HosTouch日志为空，不能作raw→App逐手势配对。

上划回桌面另有确定配置错误：framework默认config_recentsComponentName仍为不存在的Launcher3，实际HOME/QUICKSTEP为MIUI。临时单项fabricated overlay及重启SystemUI/launcher使代理Connected/Enabled/Bound均true，并实际从设置上划回桌面。RecentTasks在system_server中的缓存仍uid=-1/component=null。

随后完整重启已验证该donor会丢弃com.android.shell fabricated overlay：新boot `63b5237f-6753-4b7a-82ac-a32316ad148b` 资源回到Launcher3，不能宣称临时修复持久。正在将单项静态RRO加入vendor；实际Activity已resolve验证、APK签名和idmap2 vendor policy映射通过，未使用ignore-overlayable。静态镜像验收尚未完成。

这次重启Type-C port0、port0-partner、port1正常出现，支持先前初始化超时是间歇性，未据此撤销问题。保持R14内核，未上机自动重试或边缘钳位候选。
