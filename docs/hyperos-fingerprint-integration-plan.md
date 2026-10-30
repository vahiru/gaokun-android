# HyperOS 指纹接口整合候选

候选已通过独立一次性入口启动HyperOS，并在R21系统上完成空载及用户接触对照。通信可用，持续状态查询窗口现已取得接触触发帧并通过基础解码；不把crDroid实验结果当成HyperOS验收。

## 固定基线

从R17独立构建树 `hos-r17-kernel-thermal` 复制，基线EFI SHA256为
`d22e364af4fb403a65e03b1545e847765865c5c28012473779dd6b42cf3b60bc`。
只加入0050涉及的SCM C文件与头文件；保留原配置、ADC_TM5内建、Himax物理尺寸及坐标修复、USB device-only相关内核改动。不得直接用旧crDroid指纹实验EFI替换HyperOS：旧树ADC_TM5仍为module，不能替代R17基线。

只读审计已验证：原R17两文件应用0050后与已实测指纹实验树对应文件逐字节一致；tzmem与scm-smc底层文件在两树中本来相同。实际构建还需要候选源码/配置/DTB/产物审计，不能仅凭patch可应用判可启动。

## 全局影响与上线验收

0050收窄的是共享scm->dev的coherent DMA mask，影响后续TZMEM池增长及SCM扩展参数分配，并非指纹私有设置。已有池不会迁移。低4GiB内存压力及共享分配行为需要观察，旧注释“无副作用”不能作为证据。

listener循环改变全部QSEECOM调用的INCOMPLETE处理，uefisecapp与指纹共享全局调用锁；256轮限制不能中断一次不返回的SMC。正常SUCCESS帧保持不变仍需真机回归。当前配置只有uefisecapp这个in-tree QSEE客户端，新增TEE分支未启用。

后续部署仅使用独立oneshot项并保留默认系统和原EFI。进入HyperOS后先核实super_hos隔离参数、实际dmesg内核标识（uname可能伪装）、SELinux、USB device-only和无线ADB持久配置；随后读取UEFI变量并验证温控HAL合约、真实skin温区和四类IMU数据。不要为验收启用仍存在DSP崩溃风险的ALS请求。

指纹测试需要重新对候选编译模块，确认完整32位物理地址、TA LOAD/SHUTDOWN与共享资源恢复。只把真实88/index1d低位作为MCU帧可用门槛，空载应拒绝导出旧帧；87空ACK及固定20/a55a不能当作配置或ready验收。真实手指→新帧→图像有效性→录入/认证仍须分别证明；当前尚无Android指纹HAL。

## 早期RPC日志准备

`tools/hos/collect-rpc-cold-boot.py`只采集日志，不重启/刷写。它在ADB出现后直接带super_hos guard启动logcat，保留缓冲中的早期事件（无-T/-t），包含INFO级拒写日志，再核对build、daemon SHA和SELinux以及末尾boot ID。错误系统现场拒绝采集（0字节），模拟ADB的完整窗口/早期事件/身份检查通过。R19只读RPC镜像此前仅离线完成，尚未执行冷启动诊断。


## R21系统上测试后的回归

2026-10-30，boot e4417420-7d4d-4235-a981-01a173145e74，入口标签r19-fingerprint-kernel仅标内核，不代表super回退。完成四次LOAD/SHUTDOWN及GPIO恢复后，原始accel/gyro/gravity/game rotation三轮12项有限值/时间戳/交付检查通过；未观察到NaN，无方向精度保证。gyro仍有accuracy0启动瞬态，属于另行修复项。

Native thermal合约RESULT PASS failures=0，真实skin约27.03°C。USBdevice、持久ADB5555、SELinux Enforcing、同boot与空crash buffer确认。模块已卸载，临时thermal探针已删除，传感器APK已停止。证据`~/.cache/hos-r21-fingerprint-contact/`。指纹接触对照失败详情见机器检查记录末尾，尚无图像或Android HAL；本轮没有模板读写、录入或认证验证。


## 首帧门槛已跨过，录入/认证尚未实现

在R22 super＋匹配指纹内核下，用户接触确认后第121笔1d状态变1，取得15680B新编码帧；7840个LE16样本min69/max503、无>4096样本且非均匀，基础解码检查通过。空载150轮保持合法无帧，说明持续查询窗口比单纯idle等待适合此固件。详见机器检查记录最新一节。后续仍需图像布局/质量、可靠接触与抬起生命周期，以及Android HAL、录入和认证；不得把首帧成功升级为解锁可用。


## 客户端命令面闭合后的整合调整（2026-10-31）

Windows 产品驱动（SHA256 `90dfeb2c…0fb0`）只发 `0x8020/0x8021/0x8029/0x8031`；`0x8026/0x8027/0x802B` 不可达，且 TA 算法工作区 `base+0x950000` 超出 1 MiB 初始分配。TA 只在 SFS 存 16 字节模板元数据，完整模板与 enroll/identify 在 `ftWbioEngineAdapter.dll` 用户态完成。

因此 HyperOS 侧把「TA 只做取图 + MCU 状态」作为固定边界：Android 的算法与模板存储另行实现，不再等 TA listener 或 0x8026。该主机侧匹配链没有 TEE 支撑，不能声明 STRONG/HAT；功能 bring-up 与安全等级验收分开。详见[设计文档](fingerprint-driver-design.md) §4.1。
