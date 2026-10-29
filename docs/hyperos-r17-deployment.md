# R17 USB 与真实皮温修复部署记录

当前阶段：R17隔离镜像与内核已部署，整块回读和完整启动通过。boot ID `afd51a55-eda7-4c64-ada9-a0ddfa9b0436`，ro.boot.hos_build=r17-usb-thermal，boot_completed=1。以下运行时绑定部分为部署前历史验证；最终验收见文末。

## 已验证的运行时变化

R16上正常加载同版本ADC-TM5模块后新增真实skin-temp-thermal。新Thermal HAL临时绑定并重启成功，运行于hal_thermal_default域；Current temperatures从HAL读回skin-temp约28.08°C，类型SKIN，门限[NaN,55,NaN,58,NaN,NaN,73]。CPU/GPU仍使用原裸片门限。旧framework Cached temperatures和headroom thresholds仍含旧skin代理缓存，因此需要完整重启，不能用runtime结果证明框架缓存已完全刷新。

USB HAL候选在persist.vendor.gaokun3.usbrole.device_only=true下恢复运行。dump显示port0 UFP/DEVICE/SINK，gadget CONFIGURED、callback registered、device-only policy；请求source/host被框架拒绝，息屏8秒后仍device/configured且服务running。USB与无线ADB都在线。原allow_suspend=0保留；运行时额外持有gaokun3_usb_device_only锁，新增init动作尚未加载，完整持久行为仍须重启验证。

第一次从/data staging bind的候选被nosuid相关域切换执行限制拒绝，运行脚本自动撤销并恢复原HAL SHA。没有改SELinux。改为单独root可写tmpfs staging并沿用原exec标签后成功。临时bind与手动模块都不会跨重启存在。

## 构建与镜像

- USB HAL SHA256 `b123b2269a51297d19d25552a3066f25a89f74c2553961ce70d15d9189b74598`。
- Thermal HAL SHA256 `5d7f89f13f71153883670b24e82b73c9cdfd34352c0ffd88a9d93f24b96ed6f2`。
- R17 EFI SHA256 `d22e364af4fb403a65e03b1545e847765865c5c28012473779dd6b42cf3b60bc`，仅ADC_TM5从m改y，保留R16全部源码修复和内建ramdisk/版本信息。
- 新super SHA256 `7e8f85384c1786f5b0f40ca1c67066c7ad399520d8cccff9963664725bc84c45`，已六分区解包校验。
- vendor重建保留R15所有replacement的内容、元数据、xattr，新增替换仅USB HAL、Thermal HAL、USB role脚本和usbrole.rc；5个非vendor镜像复用原文件。未部署设置蓝色tint候选或其他策略补丁。
- 构建VM已deallocated验证。产物/构建审计在~/.cache/hos-r17-hal和hos-r17-thermal。

## 扩容布局与安全升级

从实际设备读回primary/secondary GPT并验证CRC、双表一致；12条分区与用户扩容后布局一致。manifest绑定完整entries字节、GPT哈希、disk GUID、固定p7身份/几何、boot_a/b哈希和ESP全部64个文件。重启救援后ESP全清单与之前完全一致。

新工具update-isolated-super.py默认preview；stdin模式经过14项失败/成功路径测试与独立审阅。救援preview已经通过旧super整块hash及所有保护检查。apply用无PTY SSH USB链路stdin流，仅写p7，具有独占block打开、起始marker、严格大小/EOF和整块回读；没有复用要求旧32GiB userdata的历史安装器。

升级证据目录~/.cache/hos-r17-deploy。marker位于救援/var/lib/gaokun-hos-super-update.started.json；存在时禁止盲目重试，即便主机观察超时也必须先核对同一个任务和日志。


## 最终部署与完整启动验收

升级进程退出0，输出VERIFIED：12GiB流大小/EOF/流SHA、整区回读、GPT、boot_a/b、ESP64文件全集均通过。随后才新增R17内核和入口，校验EFI与DTB/ramdisk，保留R15/R16，loader.conf生产默认不变，oneshot启动R17。未重分区或格式化数据。

新启动无需insmod，CONFIG_QCOM_SPMI_ADC_TM5=y，真实skin-temp-thermal此次位于thermal_zone12（说明不能硬编码zone15）。HAL Current和framework Cached temperatures均为skin-temp，旧skin代理项已消失；真实温度约27.8°C，阈值[NaN,55,NaN,58,NaN,NaN,73]。温度探针只使用公开API，结果：status=0，headroom(0)=0.0（无量纲、不是0°C），headroom thresholds={1:0.9,3:1.0,6:1.5}，全部有效。headroom旧缓存已刷新。未测试极端加热或宣称传感器独立校准。

USB服务从镜像启动即running，device_only=true持久保留、callback registered，port0 UFP/DEVICE/SINK。框架拒绝host请求，息屏8秒后仍device/configured且服务running；USB与无线ADB都在线，persist.adb.tcp.port=5555在此次重启自动生效。用户请求的allow_suspend=0保留，深睡未测；role保护锁存在。已请求物理拔插但尚无用户完成反馈，75秒观察中未见partner断开，不能算拔插通过。恢复false模式和其他端口外设仅有离线测试，未做实机接受测试。

Millet persist.sys.powmillet.enable=false，本次启动超过376秒仍无millet_monitor进程，init.svc/sys.millet.monitor为空（disabled服务未启动），没有沿用“stopped=已启动后停止”的错误说法。探针前后台往返PID6511不变、恢复29ms；这只验证一个应用恢复，不等于全部OEM冻结/网络策略验收。

/data仍p12约370G、可用360G；导航Recents资源仍正确，PPI244.82与SELinux Enforcing保持。测试APK及本轮手动模块/运行时HAL绑定源和旧状态文件已清理；实际服务来自新vendor，非临时bind。所有云构建已结束且VM已deallocated。

仍待处理：OEM ThermalResourceMonitor硬编码板温接口与真实skin的语义适配；设置触摸背景/卡片轮廓；完整USB插拔、外设及恢复自动角色；用户暂停的休眠复测；热状态变化/后台通知等更完整功能验收。不能宣布HyperOS全部问题已解决。
