# 待办清单

这份清单写给愿意参与这个项目的人，列出还没有完成的事，以及每件事的下一步。

已经完成的工作和过程记录不在这里，需要时可以在git历史中查看这份文件的旧版本。面向用户的已知限制见[`known-limitations.zh-CN.md`](known-limitations.zh-CN.md)，各子系统的技术说明见[`dev/`](dev/)。

想认领某一项，请先[开一个issue](https://github.com/vahiru/gaokun-android/issues/new/choose)说明，避免和别人重复。大部分条目需要一台MateBook E Go；标注"维护者"的条目涉及发布基础设施，只能由维护者处理。

## 一、1.0发布前要完成的事

- **安装器的剩余实机测试**：在真机上验证Live安装器的"清除整个磁盘"、"删除安装器分区"（含Windows安装器内单独删除安装器分区），以及系统盘开启BitLocker时Windows安装器暂停与恢复保护的完整流程；全新安装后核对几项默认设置（不监听5555端口，USB调试默认关闭且打开后开关状态在重启后保留，默认备份方式为Seedvault，默认时区为上海）并走通一次系统更新。
- **恢复出厂设置的实机测试**：在做好数据备份的前提下，验证"设置 → 清除所有数据"能否通过统一启动入口（`gk3boot`）正常清除`/data`并回到首次开机向导。
- **长时间连续运行观察**：在发布版的默认设置下，连续日常使用72小时，记录稳定性与出现的问题。
- **发版说明定稿**：以候选版说明（`relnotes/v1.0.0-rc.*.md`）为基础整理1.0正式版发版说明。
- **Live安装器与系统版本一致**：以1.0正式版的`boot.img`与系统镜像构建并发布Live安装器（`.github/workflows/installer-live.yml`）。
- **固件存储里的崩溃记录**：从1.0.0-rc.3起，内核不再把崩溃日志写进固件存储，但已经写进去的记录要用户按[issue #56](https://github.com/vahiru/gaokun-android/issues/56)手动清理。是否在启动入口或Android开机时自动清理，由维护者决定。报告者日志里的那次内核崩溃（`sync_file_ioctl → drm_crtc_fence_get_timeline_name`）另需排查。

## 二、需要在实机上核对的改动

下面这些代码已经合入，但效果尚未经完整实测。如果你有这台机器，欢迎挑一项测试，并在issue里报告结果，无论成功与否。

- **BOE和CSOT屏幕自动识别**（#58）：在CSOT屏的机器上确认开机日志里是`panel ID pins: 0 0`，画面正常。
- **2022款机器上的前后摄方向**（#83）：确认2022款机器上前后摄取景与成片方向正确。
- **带麦克风的有线耳机录音**：确认插入3.5 mm四段式耳机后，`Wired Headset Mic`正常录音。
- **传感器DSP崩溃后的自动恢复**：触发一次SLPI恢复，确认`gaokun3_sscrecover`在约60秒内恢复加速度计与自动旋转。
- **fastboot执行端**：用电源键确认的操作和刷写功能。
- **银行、支付类应用与游戏**：常用银行和支付应用兼容性汇总；《英雄联盟手游》进入对局。
- **开关UBWC压缩的游戏帧率对比**：对比去掉`vendor.minigbm.debug=nocompression`前后的游戏帧率与稳定性。
- **蓝牙耳机通话**（[issue #107](https://github.com/vahiru/gaokun-android/issues/107)）：用蓝牙耳机进行微信语音、网络会议或游戏语音，确认双方都能清楚听到对方；通话中在耳机上把声音切回平板，确认改由平板的扬声器与麦克风继续通话。只支持窄带（CVSD）的耳机尚未经实测。通路与调试方法见[`dev/audio-video.md`](dev/audio-video.md)中“蓝牙耳机通话”一节。
- **原神的水面闪烁**（`patches/mesa/0017`）：确认水面不再时有时无，灌木后方的物体不再显示到前方，帧率与之前相近。
- **USB-C外接显示器的四通道DP**：用不带USB hub的USB-C转DisplayPort线连接显示器，确认能输出仅靠两条通道带宽不足的显示模式（例如DP 1.2显示器上的4K 60 Hz），并在反复插拔、正反面插入和两个USB-C口上都保持正常。带USB hub的扩展坞和USB-C显示器通常只使用两条通道，不适用于这项测试。

## 三、已知问题

- **USB-C偶尔开机后无法工作**（[issue #102](https://github.com/vahiru/gaokun-android/issues/102)）：开机时UCSI初始化失败（`PPM init failed`，`/sys/class/typec`为空），重启后恢复。原因是EC偶尔在完成一条命令时没有清除CCI里的BUSY位，并且一直保持这个状态；UCSI核心忽略带BUSY的CCI，命令等满10秒超时，初始化随之放弃。卡住的命令与电池驱动在0xB1事件后连续读取EC寄存器的时机重合；0xB1周期事件只在电池初始化后约3分钟内出现，此后是否还会卡住尚未经实测。`kernel/ec-usb/0018`、`0019`在初始化超时后重新初始化，最多5次，这样的开机里端口比平常晚约15秒可用。重试已在真实的命令超时上验证可以恢复（第一次重试即成功）；BUSY一直不清这种超时之后的重试尚未经实测。同样的超时落在后台任务上时不会导致初始化失败，但会让所有UCSI命令等待10秒；若落在`ucsi_register_partner_pdos()`上，核心对它的重试会因为`partner_pd`已经注册而直接返回，对端的PD能力（PDO）不再补读（只影响sysfs中的信息）。另有偶发的开机后电脑无法识别平板的情况，原因尚在排查。失败时若EC一直重复USB事件，系统无法持续休眠，`kernel/ec-usb/0017`在初始化失败后确认这些事件（效果尚未经实测），见[`dev/deep-sleep.md`](dev/deep-sleep.md#ec唤醒)。
- **插电脑时平板反过来供电**（[issue #103](https://github.com/vahiru/gaokun-android/issues/103)）：对方不支持USB PD时偶尔发生，目前只会弹通知提示重插。自动重新协商尚未实现。
- **音频与蓝牙在长时间运行后一起卡死**（[issue #104](https://github.com/vahiru/gaokun-android/issues/104)）：有用户报告，从未复现。看门狗会把现场记录到`/data/vendor/gaokun3/hangdump-*`，请遇到的人提供。
- **休眠期间大流量下载可能断网**（[issue #105](https://github.com/vahiru/gaokun-android/issues/105)）：息屏休眠期间若后台有持续大带宽下载，有较大概率被路由器踢下线，点亮屏幕后自动重连恢复。
- **无限暖暖的联网闪退、约280秒后的退出与3D画面全黑**（[issue #33](https://github.com/vahiru/gaokun-android/issues/33)）：启动即退出的原因是虚幻引擎无法从GL_VERSION读取高通驱动版本、改用了GLES，现由"GPU驱动兼容"设置处理（见[`dev/gaming.md`](dev/gaming.md)），该设置尚未经实测。报告者使用修改过的安装包时，登录前有与网络连接有关的闪退，登录后约280秒退出；原版安装包停留在登录界面时没有出现前者，后者尚未经实测。使用Vulkan时进入大世界后实时3D画面全黑，原因是色调映射着色器依赖Inf的clamp被删去（见[`dev/gpu-turnip.md`](dev/gpu-turnip.md)），由`patches/mesa/0015`（上游修正的回移）修正，修正后的效果尚未在游戏中实测。
- **部分网站里后摄红蓝颠倒或无法打开**：需要网站名称和复现时的日志。
- **部分机器的后摄模组不受支持**：后摄有OV13B10与S5K3L6两种模组，本项目只支持OV13B10；S5K3L6尚无可用的驱动，尚未收到报告。前摄的两种模组（HI846、S5K4H7）都已支持，做法见[`dev/camera.md`](dev/camera.md)中“前摄的两种模组”一节，S5K3L6可以照此处理。
- **硬件解码的遗留问题**：拖动到更低分辨率后裁剪不更新；字节缓冲模式下分辨率变大时重新分配失败；`SYS_ERROR`之后出现`invalid uc_region`；HAL里POLLPRI空转；`venus_compat_gfmt=N`需要复测。
- **硬件编码的遗留问题**：熄屏而系统没有休眠时视频硬件会挂起，之后编码可能一直失败到重启；一次只能有一个编码会话（需要在CP非像素窗口上做IOVA分配）；`v4l2_codec2`把`MB_RC_ENABLE`和`GOP_SIZE`放在同一次调用里，gen1不认前者，于是IDR之间还有每30帧一个的I帧；休眠唤醒后偶发一次编码无输出，没有任何报错，原因不明。见[`dev/audio-video.md`](dev/audio-video.md)的“硬件视频编码”一节。
- **Wi-Fi芯片启动失败时泄漏内存**：`ath11k_mhi_start`失败后没有释放，每次泄漏约4 MB DMA内存（`mhi.c:446-450`）。

## 四、功能开发

- **按网络随机的Wi-Fi MAC地址**：需要同时做一次性迁移，避免老网络的设置突然生效。
- **GPU最高优先级的权限检查**：目前内核和Turnip都不检查调用者权限。ROM里的ANGLE看到Turnip声明了REALTIME，就会让每个进程都用它，所以用OpenGL ES的游戏和SurfaceFlinger同在ring 0，享受不到抢占。可以参照panthor的`group_priority_permit()`：内核只允许带`CAP_SYS_NICE`的进程或DRM master使用最高一档ring；Turnip在初始化时试建一个最高优先级的队列，建不成就不声明REALTIME。不能让队列创建直接失败，因为ROM的ANGLE遇到`VK_ERROR_NOT_PERMITTED`不会降级重试，GLES应用会无法启动。动手之前先确认两点：SurfaceFlinger在本ROM上带有`CAP_SYS_NICE`；常玩的GLES游戏在现有内核上确实有合成掉帧。代价是SystemUI等界面也会落到ring 2，游戏在前台时下拉通知栏，动画可能变卡。背景见[`dev/gpu-turnip.md`](dev/gpu-turnip.md)的“GPU抢占”一节。
- **120 Hz无缝降频**
- **平行视界的全屏播放（其余情况）**：右侧页面请求横屏时已能展开为整屏；左侧页面请求横屏、不切换为横屏的全屏，以及按窗口大小判断分屏的应用仍不能全屏显示（见[`dev/parallel-view.md`](dev/parallel-view.md)）。
- **Wi-Fi Direct**；海外地区的Wi-Fi频段设置（regdomain）
- **在设置里显示Android ID**，方便用户自己登记Google认证
- **收紧SELinux策略**：rproc-kick、hexagonrpcd、bootctl的规则收窄；做一个vendor版的tinymix，让audioroute不再需要`vendor_executes_system_violators`。
- **统一启动入口的后续**：不再依赖ESP上的派生文件；AVB校验。
- **otapreopt**

## 五、维护者事项

- 是否预装KernelSU管理器
- 下载与OTA的长期基础设施，包括国内可达的下载方式
- 下载服务器写密钥的轮换和域名自动续费
- 补传v0.7.1的内核源码附件；在v0.2到v0.5的发布页说明旧下载链接已失效
- 核对issue表单的显示效果
- 构建机上的设备树改为本仓库的checkout

## 六、搁置

- **recovery**：启动后立即复位、循环重启。恢复出厂和fastboot已改由统一启动入口承担。
- **原神1080×1728的渲染上限**：游戏按设备白名单决定画质档位，要突破只能伪装机型，有封号风险。
