# crDroid 指纹候选（FTE7001、GDIX5125）

2026-11-01：接入交接包的固定 ioctl 采集链和 DoG/RANSAC 主机匹配器，
目标为本仓 crDroid 17.0 / Android 17。提供 AIDL fingerprint V4 服务、
每指 16 帧录入（提示抬起并换位置）、最多 5 指、枚举/删除、认证、接触检测、取消和用户切换。
实际 AIDL 服务/策略/内核和 target-files 构建已通过，FP2 已经 boot/vendor 部分 OTA
进入真机 B 槽，Enforcing 下驱动节点、AIDL 与框架服务注册正常。修复 FP1 的
会话重试软重启与 GPIO 初始化问题，修复版通过 300 次会话回归。后续临时候选已
完成一指真实录入、同指解锁及未录入手指拒绝；按电源键唤醒的首帧修复仍待连续真机
验证，永久更新及重启后模板恢复也尚未验证。详见
[实机记录](../../../../docs/crdroid-fingerprint-machine-check-2026-11-01.md)。

## 能力边界

传感器经厂商 QSEE TA 取图，解密和匹配运行在普通 Android 用户态。
服务固定报告 `CONVENIENCE`，拒绝非零 operationId 的加密认证，成功回调携带
空 HardwareAuthToken。没有 HAT 签名密钥，不提供 KeyMint/Keystore 解锁能力。
Class 1 的应用可见性和锁屏行为需要实际 crDroid 框架验证。

录入仅检查 challenge、有效期、SID 非零和 MAC 长度，**不能验证 HAT 的 MAC**；
因此目前不满足 ISession 的完整录入授权契约。`resetLockout` 也不验证 HAT；
HAL 持久化失败计数：每 5 次失败锁定 30 秒，20 次永久锁定，
框架调用 resetLockout 时清除计数，但 HAL 无法确认所携带的 PIN 授权真实有效。
单调时钟计时；进程重启在 timed 边界
重新等待完整 30 秒，不缩短锁定。完整行为仍须上机验收。
没有 FAR/FRR、防伪、TEE 模板保护或防回滚证明，不声明已达到 CDD Class 1/2/3
认证标准。`CONVENIENCE` 是开发阶段的保守能力报告，不是安全认证结论。

默认不装入发布镜像。`GAOKUN3_FINGERPRINT=true` 显式启用实验候选；
不得仅改成 WEAK/STRONG 来绕过框架限制。正式接入仍需可信录入授权以及安全等级评估。

## 传感器型号

同一个镜像要同时支持装FocalTech FTE7001和装Goodix GDIX5125的机器，型号只能在运行时判断。
服务启动时读一次板级strap（TLMM GPIO61/62，与Windows DSDT里`\_SB.SPBA._HID`的选法相同）：
0/1是FocalTech，1/0是Goodix，0/0和1/1没有可用传感器，这时`getSensorProps`返回空列表。
读strap只把这两根线短暂申请为输入，不改偏置，读完立即释放。

选定的型号决定一套profile（`Core.h`的`SensorProfile`）：帧尺寸、CAPTURE字节数和解码方式。
匹配器不再写死尺寸，帧结构自带宽高（最大40×196），两种尺寸的合成回归都在`test_matcher.cpp`里。

- FocalTech：40×196，TA返回AES加密的LE16样本，HAL解密后按百分位归一化，与原来相同。
  内核默认就是FocalTech，HAL不调用`SET_SENSOR`，旧内核也能用。
- Goodix：36×160，内核返回已解包的12位样本。HAL在INIT前调用`GAOKUN3_FPCMD_IOC_SET_SENSOR`。
  Goodix原图有很强的固定图样（逐像素的偏移，幅度和脊线相当），所以HAL在手指状态为0时
  采一帧底图；有手指的帧先减底图，再减7×7局部均值去掉按压造成的大面积明暗变化，
  按脊线幅度定增益。脊线幅度低于20个计数的帧直接拒绝：两帧无手指图相减只有约4个计数，
  有手指约100个，单靠百分位拉伸会把噪声拉成能通过质量门槛的“纹理”。
  GET_FINGER_STATUS在Goodix上表示手指是否在键上，抬起后读回0。
- Goodix底图在操作进行中更新：每次抬起后重取一次，没有按压时每分钟重取一次。新取的帧和
  原底图做同样的高通相减，只差噪声就直接替换（底图跟着温度慢慢漂移）；否则其中一帧带着
  手指，而手指会让读数变低，所以较亮的那帧留下。内核在INIT时取手指检测的参照，手指当时
  若按在键上，之后手指状态一直读0，HAL的第一帧底图就会带着手指，因此会话的第一帧底图
  5秒后重取，下一次抬起后也会换掉；这期间的一次按压可能认不出。新帧连续被拒3次、跨度
  达到10秒时也直接替换，以免温度变化后底图一直换不掉。

Goodix初始化需要`gfspi.dll`里的类型14传感器配置模板。它不进内核源码，作为固件文件
`/vendor/firmware/gaokun3-goodix-cfg.bin`提供；内核只接受这一份模板，并按每台机器自己的
OTP修补DAC等寄存器。模板和TA来自同一个Windows驱动包（uup-drivers-sc8280xp的200.0.10.0，
`gfspi.cab`），由`prepare-crdroid.py --gfspi`导出。

## 内核

沿用 Android 自己的内核基线、配置、DTB 和补丁链，不使用 NixOS/HyperOS 内核替代。
交接包 QSEE 补丁与本仓 0050 完全相同；0212 添加常驻驱动，0213 在它上面加 Goodix 分支，默认不开启：

```sh
bash /path/to/gaokun-android/scripts/kernel-apply-patches.sh /path/to/kernel --with-fp
# 在内核源码目录，先跑现有 Android 配置流程，然后：
bash /path/to/gaokun-android/scripts/fingerprint/configure-kernel.sh /path/to/kernel-output
make O=/path/to/kernel-output ARCH=arm64 -j8 vmlinuz.efi
```

继续使用已有 ARM64 编译器/CROSS_COMPILE 设置。配置脚本只运行 olddefconfig，
不编译、不插入模块。0050、0212 和 0213 均留在 `--with-fp` 实验补丁链。
内核缺 0213 时，Goodix 机器上的 HAL 会在日志里报 `Fingerprint kernel driver has no Goodix support`。
大构建开始前必须核对实际输出文件系统空间，预留镜像、验证副本及至少 2 GiB。

0212 与原交付版本的差异：TA 默认路径 `/vendor/firmware/fingerpr.mbn`；
驱动编为 built-in；同一时刻只允许一个 open；HAL 崩溃后 release 请求 shutdown；
shutdown 一旦失败，保留 DMA 缓冲且拒绝重试和重新 open，重启后才能恢复。
单次 secure call 仍可能不返回，取消只在两次 ioctl 之间生效，不会异步复位传感器。
没有 debugfs 和任意 QSEE 命令入口，正常构建不输出图像缓冲。

## 本地输入和 Android 构建

下面的准备脚本只离线写本地目录，不连接构建机或真机。
它先核验整份交接包及固定 TA 哈希，再从候选 EFI 的嵌入 IKCONFIG 验证驱动配置。
首次替换现有预置内核时保留 `vmlinuz.efi.before-fingerprint`；已存在回退副本时
拒绝覆盖。可先用 `--device-tree /tmp/fingerprint-candidate` 输出到独立目录检查。
使用已有设备树时要同时保留与该 Android 内核配套的 DTB。

```sh
python3 scripts/fingerprint/prepare-crdroid.py \
  --gfspi /path/to/gfspi.dll \
  --kernel-tree /path/to/kernel \
  --kernel-image /path/to/kernel-output/arch/arm64/boot/vmlinuz.efi
```

不传内核参数时仅准备TA和Goodix模板（模板SHA256：
`273537378e759cc19b829c6c5a3b5226da603b55f96be6bcdf9a2e66c6c26725`）。
固件在既有 firmware 忽略目录中，专有二进制和
含有它的原交接 ZIP 都不入库。TA SHA256：
`b081543c7b6ae4360969007836747601e8c48ec48a8335a5479e45ec0e34355a`。

在已经具备完整 crDroid 编译环境的构建机上：

```sh
# 从本仓同步（现有脚本只连接指定构建机，不访问平板）：
GAOKUN3_FINGERPRINT=true bash scripts/sync-device-tree.sh BUILD_HOST
# 进入构建机的 crDroid 源码目录：
source build/envsetup.sh
export GAOKUN3_FINGERPRINT=true
lunch lineage_gaokun3-cp2a-userdebug
m android.hardware.biometrics.fingerprint-service.gaokun3 selinux_policy
# 单编与策略通过后，再按现有流程构建镜像。
```

fingerprint.mk 将拒绝缺失/错误的 TA或Goodix模板，或缺少 fingerprint-kernel.sha256、
与该摘要不匹配的预置 EFI。直接同步设备树时也要带上这两个不入库的输入。
关闭构建变量后 HAL、VINTF fragment、固件和 feature XML 都不会安装。
新增节点标签和 ueventd 权限为 system:system 0600；SELinux 仅给指纹 HAL 访问。
GPIO chip 固定核对 `f100000.pinctrl` / 230 lines，使用 line 185，输出加上拉，
读回确认高 10ms→低后等待 400ms，并在 TA 使用期间持有 GPIO。成功 shutdown 后
恢复输入/下拉再释放；关闭失败则保留所有权。本机初始化探测已通过，不能据此声明录入成功。
GPIO line FD 使用内核的 [gpiolib cdev 接口](https://github.com/torvalds/linux/blob/master/drivers/gpio/gpiolib-cdev.c)。

## 模板

每用户 `/data/vendor_de/<user>/fpdata/gaokun3-templates.db`，目录 0700、文件 0600，
跟随 Android 用户数据删除。vold 需先准备对应用户的 vendor_de 父目录。
HAL 的命名 type_transition 为新 fpdata 目录赋予 fingerprint_vendor_data_file。
存储`GK3A` v3数据库：头部记录传感器型号和帧尺寸，与本机不符时拒绝加载（FocalTech的v2库
仍可读取，下次保存时写成v3）；最多 5 个有唯一正 ID 的录入、每指 16 帧、
随机 authenticatorId、SHA256 损坏校验、临时文件 fsync→rename→目录 fsync。
它不兼容 NixOS 的 `GK3F` 容器，需重新录入。FBE 保护文件落盘，数据库本身没有
TEE MAC、独立模板加密或防回滚；SHA256 不能防止有写权限者篡改。
不记录原始图像或模板日志。

## 验证

```sh
bash scripts/fingerprint/test-offline.sh
```

2026-11-01 本机结果：Clang `-Wall -Wextra -Werror` 编译 core/matcher，
AddressSanitizer + UBSan 通过；模板往返、权限/损坏/截断/符号链接拒绝、
合成同图/异图匹配、AES 有效图像解码及平帧/越界帧拒绝通过。同图 100、异图约 1.85，**仅为合成输入回归，
不能推导误识率**。0212 在参考 Kconfig/Makefile 上可应用，UAPI 字节一致；
V4 方法覆盖和 XML 检查、交接包损坏拒绝、构建开关/内核摘要门禁及 shell 语法检查通过。
静态 API 检查不等于 Android 编译。

后续验收：Soong/SELinux/neverallow/VINTF；开机服务发现；空载无帧；真实按下/抬起；
录入、取消和超时；同指/异指与重复帧；重启后恢复；删除和用户删除/切换；
锁屏、PIN 首次解锁、framework 锁定策略；HAL 崩溃和 shutdown 失败后的闭锁；
共享 SCM 的 UEFI/温控/IMU/USB 回归。没有执行 adb、刷写、重启或其他真机操作。


会话回归模块 `gaokun3-fingerprint-session-test` 在设备上运行，不采集图像或访问模板；
覆盖旧会话替换、HW_UNAVAILABLE 回调内重试、close 回调重入。首个会话修复构建已通过
100 轮共 300 次测试，system_server 未重启。Linux 主机可执行
`bash scripts/fingerprint/test-transport.sh`，检查真实 Transport 实现的 GPIO 持有/恢复、
读回失败/INIT 失败清理与 shutdown 失败保留，设备 syscall 全部替换为模拟实现。


2026-11-02 会话复用与状态坏帧重试候选已在同一 B 槽启动中真实录入 1 指，
框架显示 accept=1/reject=2，用户确认未录入手指被拒绝、已录入手指可解锁。
录入时每次抬起后略换位置，覆盖不同指腹区域；质量不足的帧仍拒绝，不降低门槛。
仅对状态查询的 EPROTO/EBADMSG 做最多 5 次额外轮询，不复位、不把坏帧当抬起，
持续坏帧与真实 I/O 错误仍结束会话。认证开始时允许处理已就绪的帧，避免按电源键
时忽略首帧；该修改通过模拟测试，连续真机验证尚未完成。初版 FP3 包未刷写，需将
此修复纳入后重新构建，永久安装与重启恢复仍待验证。

2026-11-02 用户进一步反馈为“有反应但经常提示不匹配”，真实同指拒绝率仍未解决。
离线发现匹配器 RANSAC 检查了缩放比例，却只应用单位尺度的旋转；已补上比例，使
两对采样点定义的相似变换实际用于全部候选点。阈值 12、比例范围 0.77～1.3 和
2.5 像素容差保留。新增几何与缩放图回归及 992 次无关合成纹理对照，随
`test-offline.sh` 在 ASan/UBSan 下通过；尚未 Android 编译或部署，不能据此宣称
真实识别率改善。无需修改现有模板格式。详细结果见实机记录的新一节。

2026-11-03 临时启用缩放候选后仍出现同指拒绝和间歇初始化失败。进一步修正认证
窗口结束时将 latched 置 true 的行为：ready 是帧可用标志，拒绝后不能要求它先
清零。新增真实 Session 主循环模拟回归，旧代码出现“后续有效匹配帧仍被跳过”。
匹配器的新候选同时对齐空间采样网格和梯度方向，采用插值方向直方图、双线性采样、
空间/方向插值和归一化，避免仅对梯度做粗方向量化。既有模板保存图像帧，可重新
提取特征；存储格式不变。扩展合成测试在 32 个独立纹理场景的 64 个 ±10° 视角中
接受 57 个，1984 次跨场景比较全部拒绝；另一组 992 次异图比较也全部拒绝。
ASan/UBSan 通过；这些结果不代表真实同指/异指表现，新候选部署结果见实机记录。
