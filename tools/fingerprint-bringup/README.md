# 指纹TA加载冒烟测试

本目录包含用于验证gaokun3指纹（FocalTech `FTE7001`）可信应用加载路径的内核测试模块：通过高通QSEECOM `APP_START` SMC将华为签名的指纹TA（`fingerpr.mbn`，单一secelf，TA名`fingerprint`）加载进QSEE并获取`app_id`。架构与设计说明见[`docs/dev/fingerprint-driver-design.md`](../../docs/dev/fingerprint-driver-design.md)与[`docs/dev/fingerprint/`](../../docs/dev/fingerprint/README.md)。

> **说明**：测试使用的是厂商QSEECOM SMC加载路径与厂商签名的TA镜像，TrustZone仍逐段验签，性质与GPU/DSP/Wi-Fi固件加载相同。

## 依赖

* 内核需打入`patches/kernel/fingerprint/0001`（`qcom_scm_qseecom_app_load/_shutdown` + 32位TZ内存约束）。上游主线仅包含`LOOKUP`/`SEND`，不包含`LOAD`。

## 模块行为

`qcom_qseecom_fptest.c`在`insmod`后创建debugfs触发文件，模块加载本身不发送任何SMC。向触发文件写入`load`后依次执行：读取`/data/local/tmp/fingerpr.mbn` → 分配tzmem（4 MiB对齐） → 校验物理地址位于32位空间内 → 调用`APP_START`加载 → 打印`app_id` → 调用`LOOKUP`复核 → 调用`APP_SHUTDOWN`卸载。

## 注意事项

* **需有人在设备旁可长按电源键恢复**：SMC参数异常或TZ拒绝加载时存在整机挂死风险；物理地址检查仅能拦截地址截断问题。

## 运行步骤

```sh
# 1) 启动带0050补丁的测试内核（写boot分区或单次启动前需用户确认）
# 2) 推送模块与TA镜像
adb push qcom_qseecom_fptest.ko /data/local/tmp/
# fingerpr.mbn放在/data/local/tmp/（取自uup-drivers发布包，见device/huawei/gaokun3/firmware/README.md）
# 3) 触发测试
adb shell su -c 'insmod /data/local/tmp/qcom_qseecom_fptest.ko'
adb shell su -c 'echo load > /sys/kernel/debug/gaokun3_fptest/trigger'
adb shell su -c 'dmesg | grep fptest | tail'
```

预期`dmesg`输出`LOAD 成功！app_id=N`。

## 构建（外部模块，指向带0050补丁的内核树）

```sh
make -C <kernel-tree> M=$PWD ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules
```
