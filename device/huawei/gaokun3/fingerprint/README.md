# crDroid 指纹候选（FTE7001、GDIX5125）

面向本仓crDroid 17.0（Android 17）的AIDL fingerprint V4服务：录入（每指16次）、认证、
接触检测、枚举、删除、取消和用户切换，最多5根手指。取图经内核的fpcmd驱动和厂商TA，
解码、匹配和模板存储都在HAL里。FocalTech FTE7001和Goodix GDIX5125共用一个镜像，
型号在运行时判断。构建、部署和实机检查的经过见
[实机记录](../../../../docs/dev/crdroid-fingerprint-machine-check-2026-11-01.md)。

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
  采一帧底图；有手指的帧先减底图，再减7×7局部均值去掉按压造成的大面积明暗变化。
  手指压上去时，被盖住部分的固定图样也会变弱，相减后残留一小份固定图样，每帧都一样，
  会让不同手指的帧也相关起来；所以再按它在底图自身高通图样上的投影（最多一半）扣掉。
  然后按脊线幅度定增益。脊线幅度低于20个计数的帧直接拒绝：两帧无手指图相减只有约4个计数，
  有手指约100个，单靠百分位拉伸会把噪声拉成能通过质量门槛的“纹理”。
  GET_FINGER_STATUS在Goodix上表示手指是否在键上，抬起后读回0。
- Goodix底图在操作进行中更新：没有按压时每分钟重取一次。本次操作中读到过手指时，取底图（包括
  会话的第一帧和在按压期间到期的那次）要等键上没有手指满2秒：刚离开的手指会留下痕迹，底图带着它时，上面的扣除会连同脊线一起扣掉。新取的帧和
  原底图做同样的高通相减，只差噪声就直接替换（底图跟着温度慢慢漂移）；否则其中一帧带着
  手指，而手指会让读数变低，所以较亮的那帧留下。内核在INIT时取手指检测的参照，手指当时
  若按在键上，之后手指状态一直读0，HAL的第一帧底图就会带着手指，因此会话的第一帧底图
  5秒后重取；这期间的一次按压可能认不出。新帧连续被拒3次、跨度
  达到10秒时也直接替换，以免温度变化后底图一直换不掉。

匹配器的尺寸是按Goodix的图像定的，那里相邻脊线相距约7.5个样本。像素间距不同的传感器，
帧在提取特征前先按profile的`matchScale`重采样到这个比例（缩小时先做高斯预滤波）；模板仍存
传感器原始的样本，比例以后改了也不必重新录入。比例不对时，无关手指的得分会升高：把Goodix
帧放大1.15倍，其他手指最高到37.5，原样时是18.5。Goodix的比例是1。FTE7001的像素间距尚未用
真实手指测量，暂时也是1。

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

## 匹配

模板保存的是图像，匹配时重新提取特征，换匹配器不用改存储格式。窄条传感器上两帧
通常只有一部分重叠，位置和角度都未知（`gaokun3-matcher.c`）：

- 在带通后的脊线图上取DoG极值点，按局部脊线方向转正后计算梯度方向直方图描述子。
- 描述子最相近的点对各给出一个刚体位姿（旋转不超过±60°），按互相印证的点对数排序。
  拿平板的方式不同，手指碰到电源键的角度差别很大：只用录入帧做模板时，5根手指的212次
  按压在±40°内认出104次，±60°内124次，其他手指的得分都没有变化。再放宽几乎没有收益，
  偶然的高分却越来越多；完全不限角度时，其他手指的帧已经能达到接受阈值。
- 在两帧都有接触的区域上爬山，使脊线相关最大。证据是相关系数的Fisher z乘以重叠面积的
  平方根，再加上这个位姿能解释的互为最近的描述子点对数（每对计2）。两根不同手指的平行
  脊线在一小块区域上也能相关得很好，但其中有区分度的特征点对不上。
- 每根手指取得分最高的一帧；它达到`kCorroboratedScore`（22）时，再加上其余帧里得分最高、
  且和它不是同一处指面的一帧（两帧之间得分低于`kSameSpotScore`，25）。达到`kMatchThreshold`
  （37）即接受，并报告得分最高的手指。同一处指面的两帧会把一次偶然的对上算两遍，学到的帧都
  集中在解锁时按的那一处，尤其如此；两处各一次16到19分的偶然对上加起来也能接近阈值，所以
  最好的一帧本身要先超过偶然的水平（实采数据里其他手指的单帧最高20.7）。

每帧的特征（`FrameFeatures`）缓存在手指上：认证开始时`prepare()`补齐缺的；会话每次采集前
都会重读数据库，帧没变的手指沿用原来的缓存；学习只为新帧提取。探针帧的特征只提取一次。
手指按最近一次认证成功的先后排，一根一根比，每根手指的帧分给最多8个线程，有手指过阈值
就停：所有数据里其他手指对一根手指的得分都在20以下，第一个过阈值的就是得分最高的。成功时
先回调，学习和保存在其后。在Goodix机器上，一次判定从开始采集到出结果约180 ms，其中采集
约150 ms。

阈值高于离线比较中其他手指达到的最高分：一个人6根手指的Goodix采集（两次录入、两次按压
测试和旧模板库），用左右食指的帧组成16到32帧的模板，其他手指的帧去比，约2万次比较里
最高35.6。模板帧数越多，其他手指偶然撞上的机会越多，32帧时比16帧时约高2分，阈值按32帧
定。加上`kCorroboratedScore`后，扩大到8根手指、5根手指各自不超过48帧的模板（每根约2到3万
次比较），其他手指连25都没有达到。这只是一个人的数据，不足以估计误识率；FocalTech尚未用
真实图像测试。

## 模板

每用户 `/data/vendor_de/<user>/fpdata/gaokun3-templates.db`，目录 0700、文件 0600，
跟随 Android 用户数据删除。vold 需先准备对应用户的 vendor_de 父目录。
HAL 的命名 type_transition 为新 fpdata 目录赋予 fingerprint_vendor_data_file。
存储`GK3A` v4数据库：头部记录传感器型号和帧尺寸，与本机不符时拒绝加载；每根手指记录
自己的帧数。v3库（每指固定16帧）和FocalTech的v2库仍可读取，下次保存时写成v4。最多5个
有唯一正ID的录入、随机authenticatorId、SHA256损坏校验、临时文件fsync→rename→目录fsync。

录入每指16帧。纹理覆盖不到画面六成的帧不收，提示用力按住（`PARTIAL`）；和已收下的某一帧
得分达到`kRepeatScore`（60）的也不收，提示每次换个位置（`IMMOBILE`）；两种情况都要等手指
抬起再按。设置的侧边指纹录入页目前不显示这两条提示的文字。之后认证得分达到`kLearnThreshold`（40）时，这一帧补进该手指的模板，每指
最多48帧；满了就替换最早学到的一帧，录入的16帧不动。用一天里的全部按压学习，两根食指的
模板长到45和49帧，所以上限取48。侧边按键上，录入和日常解锁时手指
碰到的往往不是同一块，学到的帧让模板向解锁时用的区域扩展。门槛比接受阈值37高，接近
阈值的误识不会变成模板的一部分。学习还要求这一帧和录入时的16帧本身达到`kCorroboratedScore`
（22），即超过偶然的水平：否则万一有一次误识被学进去，同一根冒充手指以后的按压就会对上这一帧，
再被学进去，越积越多。和模板中某一帧得分达到`kRepeatScore`（60）的帧不学：
那是重复的一帧（同一次按压的帧之间三分之一达到60，不同按压之间只有0.3%），两帧几乎
一样时，一次偶然的对上会在两帧相加时算两次。

它不兼容 NixOS 的 `GK3F` 容器，需重新录入。FBE 保护存储的文件，数据库本身没有
TEE MAC、独立模板加密或防回滚；SHA256 不能防止有写权限者篡改。
不记录原始图像或模板日志。

## 验证

```sh
bash scripts/fingerprint/test-offline.sh
```

`test-offline.sh`用Clang `-Wall -Wextra -Werror`编译core和matcher的测试，并在
AddressSanitizer + UBSan下运行：

- 模板库：往返、权限、损坏、截断、符号链接，v2/v3兼容，学习帧的顺序与上限，传感器型号绑定；
- 解码：AES有效帧、平帧和越界帧，Goodix底图和固定图样；
- 匹配器：合成纹理转动±10°和±50°、平移后仍能对齐，转90°的视图和无关场景在偶然水平以下；
  重采样后的尺寸与周期；
- 特征缓存：与现提取的结果一致，学习时增量更新，重读数据库后沿用；
- 持久失败计数和锁定期限。

合成纹理只是回归测试，推不出真实手指的识别率或误识率。`test-transport.sh`在Linux主机上
检查真实Transport实现的GPIO持有和恢复、读回失败和INIT失败的清理、shutdown失败后的保留，
设备调用全部用模拟实现替换。会话回归模块`gaokun3-fingerprint-session-test`在设备上运行，
不采集图像也不访问模板，覆盖旧会话替换、HW_UNAVAILABLE回调内重试和close回调重入。
`test-integration.py`检查补丁能否打上、UAPI是否一致、V4接口覆盖、构建开关和输入准备。

现在的代码在Goodix机器上以框架的虚拟指纹实例实测过录入、解锁和拒绝其他手指。装进ROM的
正式服务、与之配套的SELinux策略，以及FTE7001机器上的匹配，都尚未验证。
