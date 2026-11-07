# 上游投稿补丁草稿（未发送）

本目录存放5份面向Linux主线的英文补丁草稿，由本仓[`patches/kernel/camera/0002`](../../../patches/kernel/camera/0002-clk-qcom-camcc-sc8280xp-mark-camnoc-ahb-rcgs-shared.patch)、[`patches/kernel/camera/0001`](../../../patches/kernel/camera/0001-clk-qcom-camcc-sc8280xp-gdsc-wait-vals.patch)、[`patches/kernel/camera/0003`](../../../patches/kernel/camera/0003-media-i2c-ov13b10-of-match-and-gaokun3-power-sequence.patch)（拆为OF匹配表与`get_selection`两份）及[`patches/kernel/camera/0004`](../../../patches/kernel/camera/0004-media-camss-continue-without-never-bound-sensors.patch)整理提交说明而来。

发送前需在干净的`linux-next`树上执行`git apply --check`，并补齐`dt-bindings`中`ovti,ov13b10`的binding说明（`0003`依赖该binding，主线目前尚未收录）。

| 文件 | 收件人（`get_maintainer.pl`） |
| --- | --- |
| `0001-clk-qcom-camcc-sc8280xp-Mark-CAMNOC-AHB-RCGs-shared.patch` | linux-clk, linux-arm-msm, Bjorn Andersson, Konrad Dybcio |
| `0002-clk-qcom-camcc-sc8280xp-Use-the-hardware-GDSC-wait-values.patch` | 同上 |
| `0003-media-i2c-ov13b10-Add-OF-device-ID-table.patch` | linux-media, Sakari Ailus, Arec Kao |
| `0004-media-qcom-camss-Complete-notifier-without-never-bound-sensors.patch`（**RFC**） | linux-media, linux-arm-msm, Robert Foss, Bryan O'Donoghue, Hans Verkuil |
| `0005-media-i2c-ov13b10-Implement-get_selection.patch` | linux-media, Sakari Ailus, Arec Kao |

说明：
- `0003`与`0005`的hunk从`patches/kernel/camera/0003`拆分而来，发送前须单独核对行号与上下文。
- `0004`为RFC补丁：当前通过模块参数`sensor_wait_ms`超时完成异步子设备注册，上游可能倾向在`v4l2-async`层或设备树层面表达同机二选一传感器拓扑。
