# Gravity NaN 与探针误判

2026-10-29：旧Main的sensor段只用events>0判PASS，没有检查registerListener返回值、数值finite或时间戳；因此77次NaN也打印PASS。R11历史 `app-probe-results.txt:8` 为78次有限值[1.4515306,-1.2026088,9.623782]，不能说历来都是NaN，也不能把旧PASS一律解释成正确。

本轮首次Gravity NaN，亮屏解锁再跑得到有限值[3.145713,-1.4177469,9.17959]，不能因此关闭此问题。旧probe依次监听四类，每类1.6秒后注销，accel末值与gravity末值相隔数秒，不是同步样本。屏幕锁定时的麦克风零/相机策略拒绝同样应保留测试前置条件，不直接计硬件故障。

AOSP main源码可解释的一条候选路径：

- GravitySensor::process取FUSION_NOMAG旋转矩阵第三列乘GRAVITY_EARTH，hasEstimate只反映初始化位标志，不检查finite。
- Fusion::checkInitComplete对最初accel做d/length(d)，没有先判断零长度/非有限值；零向量可产生NaN。handleAcc的free-fall阈值检查发生在checkInitComplete之后，不能保护首次归一化；NaN与阈值比较也不会自动被拒绝。
- SensorFusion输入时间差有gyro<50ms、accel<100ms门槛，但没有覆盖值的finite检查。
- Gravity与game-rotation共用NOMAG融合器；从无client变为有client时init重置。旧probe顺序注销/注册可能在两类间重置，因此Gravity坏、后来game rotation好并不矛盾；若其他client仍持有融合器则不会发生该重置。

参考是AOSP main，未证明donor二进制与其逐字相同；未捕获HAL首次输入零向量，所以这只是值得验证的机制，不能当最终硬件/HAL根因。不要先把NaN替换为0“修复”输出。

新增严格探针位于tools/hos/probe，详见其README。最小下一证据：同时记录accel/gyro/gravity/game-rotation，2s预热、5s采样、3轮订阅；每个事件finite、timestamp正且严格递增、成功注册且有采样。预热NaN也永久标FAIL并记录首次有效延迟，区分“启动出现非法输出后恢复”和“完全没有非法输出”。静止机身时比较同窗accel与gravity均值/方向，并记录四元数范数；无运动条件才谈静态误差，不把经验阈值冒充硬件规格。

严格探针目前校验finite/timestamp/到达性，**不以gravity模长接近9.80665直接宣布融合准确**：GravitySensor实现本就从姿态矩阵乘该常数，方向误差仍可能存在。采样期间应确保设备亮屏解锁、记录静止/移动状态。旋转准确度与实际自动旋转需要另行物理姿态测试。

## 2026-10-30：实际HyperOS二进制与完整参考代码重放

R19FP启动后，未加载任何指纹模块时，严格探针三轮均记录原始accel/gyro有限，但
gravity/game rotation全NaN。故不能把故障归咎于某次指纹SMC；父线程正在相同
userdata/姿态下切回旧内核作A/B，本文未参与设备操作。

实际拉取的592160字节 `libsensorservice.so` 与R18 system.erofs中提取文件逐字节
一致，SHA256 `a4ba29620bc241c8cc4ab07b9dee05fd3e29e170855a94f0bddbd8ab8594dcaf`。
该库保留Fusion动态符号，实际ARM64反汇编证据：

- `Fusion::checkInitComplete` VA0x567d4，ACC分支0x56824开始计算平方和，
  0x56858为fsqrt，0x5685c直接计算1/norm，0x56864/56868乘回输入。
  分支中没有先检查零norm或finite。
- `Fusion::handleAcc` VA0x57064，0x57094先调用初始化路径；只有后面
  0x570c4才与freefall阈值比较。这与参考代码的保护顺序一致。
- `getOrthogonal` VA0x56aec亦通过fsqrt/fdiv直接归一化，没有零向量保护。

因此“实际库也具有零输入污染机制”现在有二进制证据；但本轮是否真的收到过零输入
仍未证明，不能把机制直接当作已定位的设备根因。

从CICD只读下载的完整参考Fusion.cpp SHA256为
`f0913a82cd63a4cc026bce6181bea5b185080860af531b96052abd747ec6836e`。
未修改算法源码，仅提供Errors/Log头替身，在主机用ASan/UBSan编译，结果：

| 输入序列 | 初始化 | 后续1000步正常输入 | init后正常新输入 |
|---|---|---|---|
| 正常acc先 / gyro先 | finite | finite，q范数1 | finite |
| 首acc为有限零向量，任一顺序 | NaN | 持续NaN | 恢复finite |
| 初始gyro dT为0，acc正常 | finite | finite | finite |

正常输入使用本次实际均值附近的acc `[-0.185,-0.123,9.873]` 与gyro
`[-0.003,-0.0008,-0.024]`，dT=20ms。测试不生成设备数据、不注入真实传感器，
也没有用软件值覆盖NaN。

P初始化为零不是该正常输入下NaN的充分条件：update仍有非零观测噪声矩阵R。
参考 `checkState` 只重置协方差；其中PSD检查的比较也不是finite检查，不能指望
它修复已经NaN的四元数。实际错误处理是否完全相同需另做二进制核对。

`init(mode)`清采样计数/初始化位，但不立即清x0；disabled/0clients时dump保留
旧NaN并不证明仍在执行融合。正常新样本完成初始化才会覆盖x0。严格探针目前把
有限零向量算作finite，因此后续证据应增加最初事件、最小acc norm、零向量计数，
同时保留完整时间顺序。应用层未见零不等于融合器从未收到零；不能据此改HAL输出。

主机证据：`~/.cache/hos-r19-fusion-audit/` 中的原始源码、
`reference-provenance.json`、`probe.cpp`、`host-results.txt` 与提取的实际库。
