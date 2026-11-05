# 归档：早期上游Venus补丁集（SC8280XP视频编解码）

> **状态说明**：本目录下的Venus补丁集（`0013`–`0020`）已于内核迁移至`qcom-iris`驱动（`patches/0053`–`0060`）时退役，不再由[`../../scripts/kernel-apply-patches.sh`](../../scripts/kernel-apply-patches.sh)应用。现役`qcom-iris`与`c2.qti.avc.decoder`视频编解码架构详见[`../../docs/dev/audio-video.md`](../../docs/dev/audio-video.md)。

## 背景与归档原因

1. **驱动演进（`venus` → `qcom-iris`）**：主线Linux 7.2+针对SM8250/SM8350/SC8280XP（IRIS2架构）引入了新一代`qcom-iris`状态机驱动，SC8280XP设备树节点通过`"qcom,sc8280xp-iris", "qcom,sm8250-venus"`回退匹配`sm8250_data`即可由`qcom-iris`驱动工作。
2. **早期Venus补丁的历史局限**：
   - `0017`/`0018`在资源结构中设置了`opp_pmdomain = {"mx", NULL}`但未设置`opp_pmdomain_num`，且未声明`mmcx`电源域。
   - `0019`在`sc8280xp.dtsi`中使用的compatible为`qcom,sm8350-venus`，且OPP频点未按`videocc`的`÷3`分频比换算。
   - 在主线7.2中启用Venus必须关闭`CONFIG_VIDEO_QCOM_IRIS`（否则`core.c`中的资源表被`#if !IS_ENABLED(IRIS)`剔除导致编译失败）。

## 目录补丁清单（仅作历史参考）

| 补丁 | 修改内容 |
|---|---|
| `0013` | `dt-bindings`文档更新 |
| `0014` | 移除`of_match`尾随逗号（主线已合并，跳过） |
| `0015` | `hfi_venus` presets位更新 |
| `0016` | 可选LLCC路径支持 |
| `0017` | SM8350 resource结构定义 |
| `0018` | SC8280XP resource结构定义 |
| `0019` | `sc8280xp.dtsi`添加`venus`节点 |
| `0020` | `sc8280xp-huawei-gaokun3.dts`启用`venus` |
