# 待办清单

这份清单写给愿意参与这个项目的人，列出还没有完成的事，以及每件事的下一步。

已经完成的工作和过程记录不在这里，需要时可以在git历史中查看这份文件的旧版本。面向用户的已知限制见[`known-limitations.zh-CN.md`](known-limitations.zh-CN.md)，各子系统的技术说明见[`dev/`](dev/)。

想认领某一项，请先[开一个issue](https://github.com/vahiru/gaokun-android/issues/new/choose)说明，避免和别人重复。大部分条目需要一台MateBook E Go；标注"维护者"的条目涉及发布基础设施，只能由维护者处理。

## 一、1.0发布前要完成的事

- **新用户安装路径的实机测试**：用图形安装器在真机上分别走一遍清空整盘、双系统和"重新安装Android（清除数据）"，从U盘和从Windows各启动一次安装器，装好后完成首次开机向导，再做一次系统更新。Windows上的启动脚本需要确认在关闭Secure Boot之前检查并暂停BitLocker。全新安装后顺便核对几项默认设置：不监听5555端口，USB调试默认关闭且打开后开关状态在重启后保留，默认备份方式为Seedvault，默认时区为上海。
- **恢复出厂设置的实机测试**：在做好数据备份的前提下，验证"设置 → 清除所有数据"能否通过统一启动入口（`gk3boot`）正常清除`/data`并回到首次开机向导。
- **长时间连续运行观察**：在发布版的默认设置下，连续日常使用72小时，记录稳定性与出现的问题。
- **发版说明定稿**：以候选版说明（`relnotes/v1.0.0-rc.*.md`）为基础整理1.0正式版发版说明。
- **安装器与系统版本一致**：用1.0的启动镜像重新制作安装器，并让发布脚本核对两者一致。
- **固件存储里的崩溃记录**：从1.0.0-rc.3起，内核不再把崩溃日志写进固件存储，但已经写进去的记录要用户按[issue #56](https://github.com/vahiru/gaokun-android/issues/56)手动清理。是否在启动入口或Android开机时自动清理，由维护者决定。报告者日志里的那次内核崩溃（`sync_file_ioctl → drm_crtc_fence_get_timeline_name`）另需排查。

## 二、需要在实机上核对的改动

下面这些代码已经合入，但效果尚未经完整实测。如果你有这台机器，欢迎挑一项测试，并在issue里报告结果，无论成功与否。

- **BOE和CSOT屏幕自动识别**（#58）：在CSOT屏的机器上确认开机日志里是`panel ID pins: 0 0`，画面正常。
- **带麦克风的有线耳机录音**：确认插入3.5 mm四段式耳机后，`Wired Headset Mic`正常录音。
- **传感器DSP崩溃后的自动恢复**：触发一次SLPI恢复，确认`gaokun3_sscrecover`在约60秒内恢复加速度计与自动旋转。
- **fastboot执行端**：用电源键确认的操作和刷写功能。
- **银行、支付类应用与游戏**：常用银行和支付应用兼容性汇总；《英雄联盟手游》进入对局。
- **开关UBWC压缩的游戏帧率对比**：对比去掉`vendor.minigbm.debug=nocompression`前后的游戏帧率与稳定性。

## 三、已知问题

- **USB-C偶尔开机后无法工作**（[issue #102](https://github.com/vahiru/gaokun-android/issues/102)）：开机时UCSI初始化失败（`PPM init failed`，`/sys/class/typec`为空），重启后恢复；另有偶发的开机后电脑无法识别平板的情况。原因尚在排查。
- **插电脑时平板反过来供电**（[issue #103](https://github.com/vahiru/gaokun-android/issues/103)）：对方不支持USB PD时偶尔发生，目前只会弹通知提示重插。自动重新协商尚未实现。
- **音频与蓝牙在长时间运行后一起卡死**（[issue #104](https://github.com/vahiru/gaokun-android/issues/104)）：有用户报告，从未复现。看门狗会把现场记录到`/data/vendor/gaokun3/hangdump-*`，请遇到的人提供。
- **休眠期间大流量下载可能断网**（[issue #105](https://github.com/vahiru/gaokun-android/issues/105)）：息屏休眠期间若后台有持续大带宽下载，有较大概率被路由器踢下线，点亮屏幕后自动重连恢复。
- **GPU的SMMU中断号不对**（[issue #101](https://github.com/vahiru/gaokun-android/issues/101)）：硬件触发的是SPI 675/680，设备树声明的是678/679，所以context fault到不了CPU，目前用`smmu-nostall.sh`定时轮询作为替代。改错了不会有任何征兆，需要先确认正确的映射，见[`dev/gpu-turnip.md`](dev/gpu-turnip.md)。
- **无限暖暖启动即崩溃**（[issue #33](https://github.com/vahiru/gaokun-android/issues/33)）：推测虚幻引擎按ANGLE报告的渲染器名称匹配机型时选了GLES，而游戏只带了Vulkan着色器。下一步：请报告者用`debug.angle.gl_renderer`和`debug.angle.gl_vendor`覆盖渲染器名称后对比。
- **部分网站里后摄红蓝颠倒或无法打开**：需要网站名称和复现时的日志。
- **硬件解码的遗留问题**：拖动到更低分辨率后裁剪不更新；字节缓冲模式下分辨率变大时重新分配失败；`SYS_ERROR`之后出现`invalid uc_region`；HAL里POLLPRI空转；`venus_compat_gfmt=N`需要复测。
- **Wi-Fi芯片启动失败时泄漏内存**：`ath11k_mhi_start`失败后没有释放，每次泄漏约4 MB DMA内存（`mhi.c:446-450`）。

## 四、功能开发

- **指纹**：驱动和HAL在[PR #92](https://github.com/vahiru/gaokun-android/pull/92)（草稿），支持FocalTech和Goodix两种传感器，匹配效果还需要改进。
- **硬件视频编码**（[issue #106](https://github.com/vahiru/gaokun-android/issues/106)）：除上层`v4l2_codec2`编码组件不支持`RGBX`到`NV12`转换外，底层固件的OX编码路径在收到首个输出缓冲（`FTB`）时会直接触发整板硬复位（Windows驱动使用的是固件的DX架构接口）。在内核中支持编码需要调查`PERSIST`安全缓冲区或实现DX架构编码流；任何编码测试都会导致整机复位，测试前请做好准备。
- **蓝牙通话**（[issue #107](https://github.com/vahiru/gaokun-android/issues/107)）：音频策略里没有蓝牙SCO通路，蓝牙耳机的麦克风在通话中无法使用。
- **相机**（[issue #108](https://github.com/vahiru/gaokun-android/issues/108)）：变焦和曝光补偿；软件ISP降噪；闪光灯照片负向曝光补偿；手电筒亮度档位；libcamera补丁和传感器参数提交到上游。
- **按网络随机的Wi-Fi MAC地址**：需要同时做一次性迁移，避免老网络的设置突然生效。
- **120 Hz无缝降频**
- **Wi-Fi Direct**；海外地区的Wi-Fi频段设置（regdomain）
- **在设置里显示Android ID**，方便用户自己登记Google认证
- **收紧SELinux策略**：rproc-kick、hexagonrpcd、bootctl的规则收窄；做一个vendor版的tinymix，让audioroute不再需要`vendor_executes_system_violators`。
- **统一启动入口的后续**：不再依赖ESP上的派生文件；AVB校验。
- **开启Secure Boot安装**：借用发行版带微软签名的arm64 shim，由用户在首次开机时登记本项目的密钥（MOK），再用这把密钥给systemd-boot、gk3boot和内核签名，这样安装时就不必关闭Secure Boot，双系统时BitLocker也不会要求输入恢复密钥。第一步是读取固件的`db`变量，确认其中是否有微软第三方UEFI CA（2011或2023版），以及BIOS中是否有相关开关。其他需要确认的问题：Secure Boot开启时systemd-boot能否加载单独的设备树，或者是否需要改用统一内核镜像（UKI）；Windows更新推送的SBAT吊销规则会使旧shim失效，附带的shim需要持续更新。这项改动不能让Play Integrity通过；要真正提高安全性，还需要dm-verity、救援系统的登录限制等配合。
- **otapreopt**

## 五、维护者事项

- 是否预装KernelSU管理器
- 下载与OTA的长期基础设施，包括国内可达的下载方式
- 下载服务器写密钥的轮换和域名自动续费
- 补传v0.7.1的内核源码附件；在v0.2到v0.5的发布页说明旧下载链接已失效
- 核对issue表单的显示效果
- 构建机上的设备树改为本仓库的checkout
- 提交到上游：`drm_crtc`的`BUG_ON`问题（`patches/kernel/drm/0001`）、[`patches/kernel/camera/0001..0004`](../patches/kernel/camera/)里的相机与`camcc`补丁、libcamera补丁

## 六、搁置

- **recovery**：启动后立即复位、循环重启。恢复出厂和fastboot已改由统一启动入口承担。
- **原神1080×1728的渲染上限**：游戏按设备白名单决定画质档位，要突破只能伪装机型，有封号风险。
