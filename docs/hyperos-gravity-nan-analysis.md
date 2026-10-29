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
