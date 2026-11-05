# 早期后摄实验补丁归档（仅存档，不应用）

> 本目录下的`media-i2c-s5k3l6xx.patch`为早期针对Samsung S5K3L6XX传感器的探索归档，**不在`scripts/kernel-apply-patches.sh`的`KPATCHES`列表中，也不应用于内核构建**。

后经Windows驱动包电源表逆向与CCI总线实测扫描证实，本机（`gaokun3`）搭载的后置摄像头传感器为**OmniVision OV13B10**（I2C地址`0x36`），而非`S5K3L6XX`（`0x10`）。现行完整的前后摄驱动、板级供电（`LDO2_B` 2.8V共轨投票）与软件ISP架构见[`docs/dev/camera.md`](../../docs/dev/camera.md)。
