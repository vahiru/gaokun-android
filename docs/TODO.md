# 待办清单

这份清单写给愿意参与这个项目的人，列出还没有完成的事，以及每件事的下一步。

已经完成的工作和过程记录不在这里，需要时可以在git历史中查看这份文件的旧版本。面向用户的已知限制见[`known-limitations.zh-CN.md`](known-limitations.zh-CN.md)，各子系统的技术说明见[`dev/`](dev/)。

想认领某一项，请先[开一个issue](https://github.com/vahiru/gaokun-android/issues/new/choose)说明，避免和别人重复。大部分条目需要一台MateBook E Go；标注"维护者"的条目涉及发布基础设施，只能由维护者处理。

## 一、1.0发布前要完成的事

- **安装器的剩余实机测试**：在真机上验证Live安装器的"清除整个磁盘"以及系统盘开启BitLocker时Windows安装器暂停与恢复保护的完整流程。
- **恢复出厂设置的实机测试**：在做好数据备份的前提下，验证设置中的"清除所有数据（恢复出厂设置）"能否通过统一启动入口（`gk3boot`）正常清除`/data`并回到首次开机向导。
- **长时间连续运行观察**：在发布版的默认设置下，连续日常使用72小时，记录稳定性与出现的问题。

## 二、已知问题

- **USB-C偶尔开机后无法工作**（[issue #102](https://github.com/vahiru/gaokun-android/issues/102)）：开机时UCSI初始化失败（`PPM init failed`，`/sys/class/typec`为空），重启后恢复。原因是EC偶尔在完成一条命令时没有清除CCI里的BUSY位，并且一直保持这个状态；UCSI核心忽略带BUSY的CCI，命令等满10秒超时，初始化随之放弃。卡住的命令与电池驱动在0xB1事件后连续读取EC寄存器的时机重合；0xB1周期事件只在电池初始化后约3分钟内出现，此后是否还会卡住尚未实测。`kernel/ec-usb/0018`、`0019`在初始化超时后重新初始化，最多5次，这样的开机里端口比平常晚约15秒可用。重试已在真实的命令超时上验证可以恢复（第一次重试即成功）；BUSY一直不清这种超时之后的重试尚未实测。同样的超时落在后台任务上时不会导致初始化失败，但会让所有UCSI命令等待10秒；若落在`ucsi_register_partner_pdos()`上，核心对它的重试会因为`partner_pd`已经注册而直接返回，对端的PD能力（PDO）不再补读（只影响sysfs中的信息）。另有偶发的开机后电脑无法识别平板的情况，原因尚在排查。这样的开机里USB端口HAL无法判断port0上接的是什么，port0保持device，接在port0上的U盘无法使用。失败时若EC一直重复USB事件，系统无法持续休眠，`kernel/ec-usb/0017`在初始化失败后确认这些事件（效果尚未实测），见[`dev/deep-sleep.md`](dev/deep-sleep.md#ec唤醒)。
- **音频与蓝牙在长时间运行后一起卡死**（[issue #104](https://github.com/vahiru/gaokun-android/issues/104)）：有用户报告，从未复现。看门狗会把现场记录到`/data/vendor/gaokun3/hangdump-*`，请遇到的人提供。
- **无限暖暖的联网闪退、约280秒后的退出与3D画面全黑**（[issue #33](https://github.com/vahiru/gaokun-android/issues/33)）：启动即退出的原因是虚幻引擎无法从GL_VERSION读取高通驱动版本、改用了GLES，现由"GPU驱动兼容"设置处理（见[`dev/gaming.md`](dev/gaming.md)），该设置尚未实测。报告者使用修改过的安装包时，登录前有与网络连接有关的闪退，登录后约280秒退出；原版安装包停留在登录界面时没有出现前者，后者尚未实测。使用Vulkan时进入大世界后实时3D画面全黑，原因是色调映射着色器依赖Inf的clamp被删去（见[`dev/gpu-turnip.md`](dev/gpu-turnip.md)），由`patches/mesa/0015`、`0016`（上游修正的回移）修正，修正后的效果尚未在游戏中实测。
- **后摄S5K3L6模组的调校**：S5K3L6的调校文件（`s5k3l6.yaml`）取自Windows驱动，尚未在这种模组上标定，拍摄均匀的白色平面时边缘比中央略偏绿（红色低约5–10%）。模组EEPROM中的出厂镜头阴影标定与相位对焦标定均未使用（见[`dev/camera.md`](dev/camera.md)中"前后摄的两种模组与相机模组探测器"一节）。相位搜索第一步的换算系数（255）是按画面中央的RAW估计的，尚未在默认AF区域实测，也尚未在Aperture中实测。前摄改由探测器选择后，S5K4H7模组的机器尚未实测。
- **硬件解码的遗留问题**：`SYS_ERROR`之后重载固件时出现`invalid uc_region`，只在带诊断补丁的内核上出现过，`kernel/iris/0027`–`0033`整理固件恢复之后尚未复现，原因尚未确认。
- **硬件编码的遗留问题**：休眠唤醒后偶发一次编码无输出（固件不带码流地返回每一帧，内核记录告警，组件连续丢30帧后报错），原因不明。见[`dev/audio-video.md`](dev/audio-video.md)的"硬件视频编码"一节。
- **Wi-Fi芯片启动失败时泄漏内存**：`ath11k_mhi_start`失败后没有释放，每次泄漏约4 MB DMA内存（`mhi.c:446-450`）。

## 三、功能开发

- **按网络随机的Wi-Fi MAC地址**：需要同时做一次性迁移，避免老网络的设置突然生效。
- **GPU最高优先级的权限检查**：目前内核和Turnip都不检查调用者权限。ROM里的ANGLE看到Turnip声明了REALTIME，就会让每个进程都用它，所以用OpenGL ES的游戏和SurfaceFlinger同在ring 0，享受不到抢占。可以参照panthor的`group_priority_permit()`：内核只允许带`CAP_SYS_NICE`的进程或DRM master使用最高一档ring；Turnip在初始化时试建一个最高优先级的队列，建不成就不声明REALTIME。不能让队列创建直接失败，因为ROM的ANGLE遇到`VK_ERROR_NOT_PERMITTED`不会降级重试，GLES应用会无法启动。动手之前先确认两点：SurfaceFlinger在本ROM上带有`CAP_SYS_NICE`；常玩的GLES游戏在现有内核上确实有合成掉帧。代价是SystemUI等界面也会落到ring 2，游戏在前台时下拉通知栏，动画可能变卡。背景见[`dev/gpu-turnip.md`](dev/gpu-turnip.md)的"GPU抢占"一节。
- **120 Hz无缝降频**
- **平行视界的全屏播放（部分情况）**：右侧页面请求横屏时已能展开为整屏；左侧页面请求横屏、不切换为横屏的全屏，以及按窗口大小判断分屏的应用仍不能全屏显示（见[`dev/parallel-view.md`](dev/parallel-view.md)）。
- **固定WLAN直连的设备地址**：P2P Device的地址取自固件每次开机变化的MAC，对方设备保存的群组在平板重启后可能无法直接使用。可以为它派生与`wlan0`类似的固定地址。wpa_supplicant的`p2p_device_random_mac_addr=2`从`wlan0`使用的配置读取持久地址，而该配置每次开机都由`/vendor/etc/wifi/wpa_supplicant.conf`覆盖，并且需要框架开启P2P的MAC随机化，不能直接使用。背景见[`dev/android-bringup.md`](dev/android-bringup.md)的Wi-Fi一节。
- **在设置里显示Android ID**，方便用户自己登记Google认证
- **收紧SELinux策略**：rproc-kick、hexagonrpcd、bootctl的规则收窄；crDroid 17.0的`external/tinyalsa`已给`tinymix`加上`vendor_available: true`，可在`device.mk`改装`tinymix.vendor`并调整`audio-route.sh`的查找顺序，让`audioroute`不再需要`vendor_executes_system_violators`。
- **统一启动入口的后续**：不再依赖ESP上的派生文件；AVB校验。
- **otapreopt**

## 四、维护者事项

- 是否预装KernelSU管理器
- 下载与OTA的长期基础设施，包括国内可达的下载方式
- 下载服务器写密钥的轮换和域名自动续费
- 补传v0.7.1的内核源码附件；在v0.2到v0.5的发布页说明旧下载链接已失效
- 核对issue表单的显示效果
- 构建机上的设备树改为本仓库的checkout
