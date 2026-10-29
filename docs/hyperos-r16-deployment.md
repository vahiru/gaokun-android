# R16 显示物理尺寸修复部署

2026-10-30 已启动，boot ID `ae3caf81-773d-49a6-b95d-2e90f6eef8d6`，`ro.boot.hos_build=r16-display`，boot_completed=1。

- 新EFI SHA256 `cf1685272eb0dfa6b09a679c0d7a448e4012595f4d4ed4b9365448be6a8092b0`。
- 仅交换BOE/CSOT面板物理尺寸为166×266毫米，时序、分辨率、触摸映射不改。配置/cpio/版本与R15相同，完整对象审计见cache/hos-r16-display/kernel。
- 在Android隔离系统校验guard后挂载ESP，新建R16目录和入口；安装前实际ESP有约43MiB空闲，无需删除R15。R15 EFI/入口、DTB/ramdisk及生产loader.conf均核对保留。通过单次启动进入R16。
- 安装前发现旧R6 Image与R9 HyperOS入口已不在ESP，目录时间00:20；本轮未执行其删除，未自行恢复这些旧入口。当前有效回退为R15及原Ubuntu救援入口。最初包含R6 Image哈希的安装检查因此停止，尚未写入新目录；核对R15及其DTB/ramdisk后才执行安装。
- 未修改super_hos，仍为R15镜像；设置按压颜色实验尚未持久部署。

启动后SurfaceFlinger两显示模式均上报244.82×244.82 DPI（此前152.78），wm density仍240。控制中心截图从约410px宽恢复约658px，和AutoDensity推导一致。设置等使用MIUI自动密度的页面也随真实PPI变化，用户字体比例未改。

截图证据：`/Users/vahiru/.cache/hos-r16-display/control-center-after.png`、`settings-after-kernel.png`，原图在hos-r15-nav/control-center-current.png。构建VM已确认deallocated。

设置触摸背景另行处理：旧preference tint候选用户未验收，已撤销。R16当前临时开启shell fabricated HosSettingsCardTint，仅覆盖Settings `miuix_recyclerview_card_tint_color_light` 为蓝色，force-stop重开后已实际截到首页“更多连接”蓝色按压及圆角。用户验收待回；这不是镜像内永久修复，重启可能消失。
