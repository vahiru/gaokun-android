# crDroid指纹HAL（FTE7001、GDIX5125）

面向本仓crDroid 17.0（Android 17）的AIDL fingerprint V4服务：录入（每指16次）、认证、
接触检测、枚举、删除、取消和用户切换，最多5根手指。取图经内核的fpcmd驱动和厂商TA，
解码、匹配和模板存储都在HAL里。FocalTech FTE7001和Goodix GDIX5125共用一个镜像，
型号在运行时判断。构建、部署和实机检查的经过见
[实机记录](../../../../docs/dev/crdroid-fingerprint-machine-check-2026-11-01.md)。

## 能力边界

传感器经厂商QSEE TA取图，解密和匹配运行在普通Android用户态。服务向框架报告`STRONG`（Class 3）：
认证成功时签发HardwareAuthToken（见“令牌”一节），可用于锁屏解锁、应用内的指纹验证（BiometricPrompt）
以及绑定指纹的Keystore密钥。

报告`STRONG`是本项目的取舍，不是CDD认证结论。CDD要求Class 2、Class 3在隔离执行环境中匹配，这里达不到。
采用`STRONG`的依据：

- 本机的KeyMint和Gatekeeper都是软件实现（`device.mk`），PIN的校验和Keystore密钥本来就在普通世界，
  取得root或内核权限即可获取。HAL在用户态签发令牌，不会让密钥的保护比PIN更弱。
- 同一机型装FocalTech传感器的机器，在Windows上由用户态的WinBio引擎适配器匹配
  （[设计文档](../../../../docs/dev/fingerprint-driver-design.md)第1节），Windows Hello照常可用于登录以外的验证。

指纹本身的局限如实说明：匹配器的误识率只有一个人的数据作为依据（见“匹配”一节），不足以估计CDD要求的
1/50000量级；没有防伪（假指纹）检测；模板库没有TEE保护，有root权限即可读写。

仍无法使用的场景：微信支付、支付宝等应用的指纹支付一般依赖厂商在TEE中预置的密钥和机型白名单；检查硬件
密钥证明或Play Integrity的应用会因KeyMint是软件实现而拒绝。

录入要求本HAL生成、仍在有效期内的challenge，以及Gatekeeper为它签发、MAC校验通过的PIN令牌；`resetLockout`
要求10分钟内签发、MAC校验通过的PIN令牌。HAL持久化失败计数：每5次失败锁定30秒，20次永久锁定，
resetLockout成功时清零。单调时钟计时；进程重启在timed边界重新等待完整30秒，不缩短锁定。

challenge按用户保存在服务里，不随会话销毁：每个用户最多16个，生成后10分钟内有效。传感器出错时HAL报
`HW_UNAVAILABLE`并退役当前会话，框架新建会话后，设置用原来的授权令牌重试录入，新会话因此要能识别这个challenge。

HAL随镜像构建。

本HAL报告`STRONG`（Class 3）。SystemUI在Class 3指纹临时锁定期间只检测接触，不再请求认证；锁定期满时HAL发出
`onLockoutCleared`，SystemUI随之恢复认证。

## 令牌

签发和校验令牌用的HMAC-SHA256密钥与KeyMint、Gatekeeper的相同。keystore2启动时主持共享密钥协商：VINTF中
声明的每个`ISharedSecret`实例（本机为KeyMint的`default`和Gatekeeper的`gatekeeper`）给出seed和nonce，
各方用自己的协商密钥对排序后的全部参数做CKDF（`keymaster::ckdf`）。两个软件实现的协商密钥都是公开的
32字节零（AOSP `security/keymint/aidl/default/ta/soft.rs`的`kak()`、`gatekeeper/aidl/software/SharedSecret.cpp`），
各方的nonce在其进程启动后第一次被查询时生成，此后不变。HAL第一次需要密钥时向各方读取参数，用同样的算法推导出
这把密钥，之后缓存到进程退出（`SharedSecretKeySource`、`deriveAuthKey`）；读取失败时这次操作以`UNABLE_TO_PROCESS`
结束，下次重试。KeyMint或Gatekeeper的进程重启后会换新的nonce，keystore2却只在自己启动时协商，那时它们自己的
令牌也已无法校验；若keystore2随后也重启并重新协商，HAL要等自己重启后才会用上新密钥。

HAL不作为第三个参与方注册：keystore2要等VINTF中声明的全部参与方上线后才协商，且只协商一次。作为参与方时，
指纹HAL注册失败（或在开机协商期间退出）会让协商无法完成，Keystore的全部令牌校验随之失效；协商之后重启则会
丢失自己的密钥，此后签发的令牌都无法通过校验。自行推导没有这些依赖，HAL重启后重新推导即可。代价是依赖
“协商密钥公开”这一前提：若改用TEE中的KeyMint，须改为作为参与方注册，Gatekeeper届时也要随之更换。推导的
密钥不对时（例如多出HAL无权查找的参与方：servicemanager列出实例时会略去调用方无权查找的），录入和
`resetLockout`会以`MAC mismatch`拒绝PIN令牌。

SELinux：两个`ISharedSecret`服务只允许KeyMint、Gatekeeper的客户端查找（`hal_attribute_service`中的neverallow），
所以HAL声明为这两者的客户端（`hal_client_domain`），写法与`hal_identity_default`相同。这同时允许它调用
KeyMint的其他服务（IKeyMintDevice、ISecureClock等）、IGatekeeper和`computeSharedSecret`（后者会让对方换用新密钥）；HAL只调用
`getSharedSecretParameters`，这些权限的风险与HAL本身被攻破、可以伪造令牌相比并不更大。

签发的令牌：challenge为认证的operationId（锁屏为0），userId为用户的SID，authenticatorId为本HAL当前的值，
类型为指纹，时间戳为`CLOCK_BOOTTIME`毫秒，与KeyMint、Gatekeeper相同。Keystore中同时接受PIN和指纹的密钥，
以及设为不随新录入失效的指纹密钥，都绑定在SID上。SID取自校验通过的录入或`resetLockout`令牌（已录入指纹时，每次输入PIN后
框架都会调用`resetLockout`），存在模板库旁的`gaokun3-secure-user-id`里（见“模板”一节）；升级后用户输入一次
PIN之前SID为0，这两类密钥暂时无法用指纹解锁。

authenticatorId在录入新手指和`invalidateAuthenticatorId`时更换，删除手指时不变，没有手指时为0
（`ISession#getAuthenticatorId`）。绑定指纹且随新录入失效的密钥绑定在它上面。

## 传感器型号

同一个镜像要同时支持装FocalTech FTE7001和装Goodix GDIX5125的机器，型号只能在运行时判断。
服务启动时读一次板级strap（TLMM GPIO61/62，与Windows DSDT里`\_SB.SPBA._HID`的选法相同）：
0/1是FocalTech，1/0是Goodix，0/0和1/1没有可用传感器，这时`getSensorProps`返回空列表。
读strap只把这两根线短暂申请为输入，不改偏置，读完立即释放。

选定的型号决定一套profile（`Core.h`的`SensorProfile`）：帧尺寸、CAPTURE字节数和解码方式。
匹配器不再写死尺寸，帧结构自带宽高（最大40×196），两种尺寸的合成回归都在`test_matcher.cpp`里。

- FocalTech：40×196，TA返回AES加密的LE16样本，HAL解密后按百分位归一化，与原来相同。
  内核默认就是FocalTech，HAL不调用`SET_SENSOR`，旧内核也能用。
  GET_FINGER_STATUS在FocalTech上表示传感器里有一帧待读，读出之前始终为1，手指抬起也不会复位。
  认证读到1就取图；录入和检测要等手指抬起，这期间读到1时HAL读出并丢弃这一帧，否则状态位
  不会变回0，此后的按压均无响应。
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
[`patches/kernel/fingerprint/`](../../../../patches/kernel/fingerprint/)下的3个补丁（`0001` QSEECOM、`0002`常驻驱动、`0003` Goodix分支）随`kernel-apply-patches.sh`一并打入。`CONFIG_GAOKUN3_FPCMD`在Kconfig中默认关闭，由[`kernel-config-android.sh`](../../../../scripts/kernel-config-android.sh)连同`QCOM_QSEECOM`和`GPIO_CDEV`一起打开，并断言为`=y`；按[`prebuilt-boot/README.md`](../prebuilt-boot/README.md)的常规流程编译即可，不需要单独的指纹内核。内核缺`0003`时，Goodix机器上的HAL会在日志里报`Fingerprint kernel driver has no Goodix support`。

`0002` 与原交付版本的差异：TA 默认路径 `/vendor/firmware/fingerpr.mbn`；
驱动编为 built-in；同一时刻只允许一个 open；HAL 崩溃后 release 请求 shutdown；
shutdown 一旦失败，保留 DMA 缓冲且拒绝重试和重新 open，重启后才能恢复。
单次 secure call 仍可能不返回，取消只在两次 ioctl 之间生效，不会异步复位传感器。
没有 debugfs 和任意 QSEE 命令入口，正常构建不输出图像缓冲。

## 本地输入和 Android 构建

TA和Goodix模板是专有二进制，不入库，放在设备树的`firmware/`（git忽略）中，来源见
[`firmware/README.md`](../firmware/README.md)第三、四节。下面的准备脚本只离线写本地目录，不连接构建机或真机：
它核对TA哈希（`--ta`给出驱动包里的`fingerpr.mbn`；不给时从私有交接包读取并核验整份交接包），
再从`gfspi.dll`导出Goodix模板。可先用`--device-tree /tmp/fingerprint-candidate`输出到独立目录检查。

```sh
python3 scripts/fingerprint/prepare-crdroid.py --ta /path/to/fingerpr.mbn --gfspi /path/to/gfspi.dll
```

TA SHA256：`b081543c7b6ae4360969007836747601e8c48ec48a8335a5479e45ec0e34355a`；
模板SHA256：`273537378e759cc19b829c6c5a3b5226da603b55f96be6bcdf9a2e66c6c26725`。
含有TA的原交接ZIP同样不入库。

`scripts/sync-device-tree.sh`在第2a步把本机的这两份带到构建机（本机没有时保留构建机上的那份），
第3a步按sha256断言构建机上的两份，并从构建机上`prebuilt-boot/vmlinuz.efi`嵌入的配置确认
`CONFIG_GAOKUN3_FPCMD=y`。在已经具备完整crDroid编译环境的构建机上：

```sh
# 从本仓同步（脚本只连接指定构建机，不访问平板）：
bash scripts/sync-device-tree.sh BUILD_HOST
# 进入构建机的 crDroid 源码目录：
source build/envsetup.sh
lunch lineage_gaokun3-cp2a-userdebug
m android.hardware.biometrics.fingerprint-service.gaokun3 selinux_policy
# 单编与策略通过后，再按现有流程构建镜像。
```

TA或Goodix模板缺失、哈希不符时，fingerprint.mk直接报错。内核不带驱动时Android构建本身不会失败，
由上面的同步检查和`scripts/release.sh`对`boot.img`的检查拦下。
不经`sync-device-tree.sh`直接同步设备树时，也要带上这两个不入库的文件。
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

用户的SID存在同一目录的`gaokun3-secure-user-id`里（`GK3S`、SID、SHA256，写法与模板库相同），不放进模板库：
模板库格式不变，切换到另一个槽位上的旧版本时，旧版HAL照常读取模板库，只是不读这个文件。这个文件缺失、
损坏或权限不对时SID按0处理，不影响模板库的载入，用户下次输入PIN时重新写入；写入失败也不影响模板库的保存，
下次保存时重试。

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

- 模板库：往返、权限、损坏、截断、符号链接，v2/v3兼容，学习帧的顺序与上限，传感器型号绑定；SID文件的
  保存，以及缺失、损坏、权限不对、符号链接时按0处理；
- 解码：AES有效帧、平帧和越界帧，Goodix底图和固定图样；
- 匹配器：合成纹理转动±10°和±50°、平移后仍能对齐，转90°的视图和无关场景在偶然水平以下；
  重采样后的尺寸与周期；
- 特征缓存：与现提取的结果一致，学习时增量更新，重读数据库后沿用；
- 持久失败计数和锁定期限；
- 令牌：CKDF、令牌密钥的推导（与参数顺序无关）、令牌MAC及各字段的篡改，期望值由独立实现
  （Python的CMAC和HMAC）算出。

合成纹理只是回归测试，推不出真实手指的识别率或误识率。`test-transport.sh`在Linux主机上
检查真实Transport实现的GPIO持有和恢复、读回失败和INIT失败的清理、shutdown失败后的保留，
设备调用全部用模拟实现替换。会话回归模块`gaokun3-fingerprint-session-test`在设备上运行，
不采集图像，只使用临时目录中的合成模板，覆盖旧会话替换、HW_UNAVAILABLE回调内重试、close回调重入、
退役会话的challenge在新会话中仍然有效、FocalTech等待抬起期间读出并丢弃待读的帧，以及令牌：录入拒绝伪造、
错误密钥、非密码类型或没有SID的PIN令牌且不启动传感器；resetLockout拒绝过期、晚于当前时间、伪造、错误密钥
或非密码类型的令牌，接受后清零失败计数并保存SID；认证成功时签发带operationId、SID和authenticatorId的令牌，
取不到密钥时不启动传感器；录入完成时更换authenticatorId并保存SID；删除手指不更换authenticatorId，invalidate
更换。
`test-integration.py`检查补丁能否打上、UAPI是否一致、V4接口覆盖、镜像总是装入HAL与固件、缺失或改动的输入被拒绝，以及输入准备。

录入、解锁和拒绝其他手指已在Goodix机器上随ROM构建的正式服务与SELinux策略下实测过；FTE7001机器上的匹配尚未实测。
令牌的签发与校验在Goodix机器上实测过：录入时校验Gatekeeper的令牌、锁屏解锁、加密认证（BiometricPrompt）、
绑定authenticatorId和绑定SID的限时密钥、指纹解锁后的“设备已解锁”密钥，以及输入PIN后的`resetLockout`。
随ROM构建的这一版HAL及其SELinux规则（`hal_client_domain`）尚未上机测试，规则已通过完整的策略编译检查。
