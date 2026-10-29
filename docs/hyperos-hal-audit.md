# HyperOS donor 框架依赖审计（R8 初步）

输入为 piano OS4.0.5.0.XPYCNXM 的 R4 system/system_ext 镜像中提取的
framework.jar、services.jar、miui-framework.jar、miui-services.jar。输入 SHA-256 位于
`out/hyperos-r8/framework/inputs.json`，专有 jar 和反编译输出仅存本地 out。

本轮仅反编译 miui-framework/miui-services，并单独查看 services 的 LightsService。
前两份 JADX 输出共 26 个错误；Java 伪源码仅供定位，涉及补丁的结论还须核对 DEX 指令。
25 个等待调用位置、所在文件哈希及行号记录在 `framework/wait-sites.json`。
该清单不覆盖所有 framework、JNI、APK、反射及 native 调用，不能作为启动准出证明。

## 已区分的等待路径

| 路径 | 本轮证据 | 后续处理 |
|---|---|---|
| `miui.util.ITouchFeature` | `waitForService` 在 binderDied 中；首次服务探测走 getService，不能据此判为必然卡首次启动 | 继续检查 HIDL 探测、各调用者空值/错误路径 |
| `MiuiDeviceLockRpmbManager` | initHal 调 doInitHal(false)，初始走 checkService；显式等待发生在名为 DeviceLock-HalWait 的 daemon 线程及死亡重连 | 不能把后台长期等待等同 system_server 主线程阻塞；继续审计上层状态与异常路径 |
| `DeviceGuardManagerImpl` | initMiGuardService 初始不等待；等待分支用于重连；有国际版、cust feature、persist 属性三重门控 | 核对最终属性与 cust 配置，不伪造 TEE 返回 |
| `DeviceStateChecker.isMiGuardServiceRunning` | 直接 waitForService 后才尝试 HIDL；方法名虽为检查，却可能阻塞 | 调用线程及入口仍待追踪 |
| `PayJoyAccessManagerStubImpl.initPayjoyService` | isConfigEnable 门控后，AIDL 分支直接等待；startService 另有 RPMB 标志门控，不能混作同一道门 | 追踪配置来源与 initPayjoyService 调用时机 |
| `VibratorManagerServiceImpl.getHal` | 只有 sys.haptic.motor 为 linear/zlinear 才等待 vibrator 服务 | 核对最终属性，不从 piano vendor 导入该能力声明 |
| `ScoutHelper`、`MiuiSentinelUtils` | 通过 Future 包装 waitForService，调用方使用 2 秒 timeout | 核验超时后的 worker 生命周期；不能称无限阻塞调用方 |
| Iris AIDL | isDeclared 探测和 waitForDeclaredService 路径；HIDL 有 getService(true) | 区分未声明直接返回与已声明但缺服务的等待，查调用门控 |
| SeaAudio | waitForDeclaredService 后，部分方法直接访问 mService，异常捕获仅 RemoteException | 未声明服务可能转为空指针风险，不能用“不阻塞”证明安全 |
| LightsService | 构造时通过 waitForDeclaredService 获取 ILights；无 HAL 时不填充 light 表，getLight 返回 SparseArray 查询值 | 继续审计消费者是否判空；当前查看的 ShutdownThreadImpl 对 light 有判空，不能外推全部调用者 |

## 已落实的挂载修复依据

FeatureParser.readOnce 按 Build.DEVICE 选择 `<device>.xml`，找不到时尝试设备名 MD5 文件；
目录依次为 `/product/etc/device_features`、`/mi_ext/product/etc/device_features`、
`/opcust/etc/device_features`。另外会读取 `/mi_ext/product/etc/miext_removable_apk_info.xml`。
因此 R7 只挂载 `/mnt/vendor/mi_ext` 并不完整，R8 已增加 `/mi_ext` 只读 bind。
donor system 根的 `/mi_ext` 已用 dump.erofs 确认为目录。

donor system 属性将 `ro.product.property_source_order` 指定为
`odm,vendor,product,system_ext,system`。没有 donor odm 时，应结合我方 vendor 的 device 值
决定最终 Build.DEVICE，不能凭 donor product 的 miproduct 或 mod_device=piano 推断。
gaokun3 device_features 尚未生成或集成，运行时属性也尚未验证。

mi_ext 镜像根只有 etc/product/system/system_ext，没有 root。init.miui.ext.rc 中四条
userdebug 挂载引用 `/mnt/vendor/mi_ext/root/…`，其中一条还带主线不支持的 override_creds。
R8 对精确匹配的这四条 mount 作注释，保留其他 action；mi_ext 自身 rc 的两条 boot overlay
已由 first-stage fstab 提前建立，R8 去掉重复 mount。生成器遇到额外/不同命令即拒绝处理。

## 未完成范围

尚未实现完整 HAL 桩或框架能力门控；本轮没有更改 DEX。需要继续覆盖直接/间接等待、
未声明服务的空值处理、HIDL 客户端、Lights 所有消费者、Millet/Greeze 与私有 native 服务。
最终必须以隔离基线及 HyperOS enforcing 启动日志验收；不能以字符串搜索或反编译清单替代。

## R8 后续：设备配置和 Lights DEX 复核

从实际实验 vendor 镜像回读 `/build.prop` 确认 `ro.product.vendor.device=gaokun3`；结合 donor
system 的 vendor 优先顺序，新增 `tools/hos/config/gaokun3.xml`，共 44 个不重复条目。
保留平板、qcom 标识和现有设备树已记录的 60/120 Hz 模式；关闭尚无实现的小米私有键盘、
触摸/显示、笔手势和 TEE 能力。`support_usb_keyboard=false` 仅用于关闭 MiuiKeyboardManager
的专有协议路径，不代表禁止 Android 通用 USB 键盘。FeatureParser 的最终选择仍待上机核验。

该配置尚待写入 product，添加时以同目录 piano.xml 为元数据模板；已完成合成 EROFS 演练，
新文件的 uid/gid、mode、SELinux xattr 和内容全部回读一致。完整 donor mi_ext 内未发现
`product/etc/device_features` 目录，因此目前没有该路径下的 gaokun3 配置覆盖层。

services.jar 完整 JADX 反编译记录了 73 个错误。对 Lights 风险另用 apkanalyzer 直接反汇编
实际 DEX，证据位于 `out/hyperos-r8/framework/lights-dex-audit.json` 及其引用 smali：

- BatteryService.Led.updateLightsLocked 在灯对象为空时立即返回。
- LocalDisplayAdapter.BacklightAdapter 两个 setBacklight 重载均在调用前检查 mBacklight。
- PowerManagerService.setAttentionLightInternal 调用 setFlashing 前检查对象为空的分支。
- NotificationAttentionHelper 调 pulse 前的 DEX 确有 if-eqz 判空。JADX 生成的同一方法含一段
  看似不判空的重复控制流，不能据此认定存在 NPE，更不能基于该伪源码盲目修补。

这四条已核验路径不需要针对“空灯对象必崩”的补丁。仍未证明所有 Lights 调用、异常路径和
运行时行为安全；也未验证现有灯 HAL、背光和 HWC 在 HyperOS 下的实际工作状态。
