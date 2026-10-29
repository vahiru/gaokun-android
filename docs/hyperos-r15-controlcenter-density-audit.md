# R15 控制中心尺寸审计

状态：只读审计，无真机修改。APK 与反编译缓存位于 `~/.cache/hos-r15-controlcenter/`，未修改 out。

## 结论

控制中心内容约 410px 宽可由物理 DPI 错误完整解释：屏幕竖屏模式为 1600×2560，但面板驱动给出横屏的宽高毫米 266×166。HWC 据此上报 152.78195 DPI，插件 AutoDensity 从全局 240 DPI 改算为 200 DPI，328dp 内容宽度实际仅 410px。右对齐则是平板布局选择，与尺寸错误是两个行为。

## 证据链

1. 真机 `dumpsys display`：1600×2560，logical density 240，xdpi=ydpi=152.78195。
2. 真机日志：`ControlCenterWindowViewController: updateCustomDensity: false, 200, 240`。
3. `refs/gaokun-buildbot/drivers/panel-hx83121a/panel-himax-hx83121a.c:687` 和 `:706`：BOE/CSOT 两个 panel_desc 都是 width_mm=266、height_mm=166。
4. `refs/a17/external/drm_hwcomposer/hwc/HwcDisplay.cpp:337`：无 EDID 有效物理尺寸时返回 `{configs_.mm_width, -1}`。
5. `refs/a17/external/drm_hwcomposer/hwc3/ComposerClient.cpp:360`：xdpi=横向像素×25.4/物理宽毫米；height<=0 时 ydpi=xdpi。因此 `1600×25.4/266=152.7819549` 精确对应运行值。
6. 插件 `miuix/autodensity/DisplayDensityConfig.java:163` 从 xdpi/ydpi 计算 PPI；`:377` 使用 PPI×1.1398963928×设备 scale，平板 `AutoDensityPolicy.calcPadScale` 最高 1.15。当前 round(152.78195×1.1398964×1.15)=200，匹配日志。
7. 插件 `MainPanelController.java:697` 使用 `control_center_universal_4_rows_with_margin_size` (342dp)，并非 `control_center_panel_width` (325dp)。可见内容资源 `control_center_universal_4_rows_size` 是 328dp，328×200/160=410px。面板含 margin 则约428px。
8. `CommonUtils.getForceVertical()` 对平板返回 true，`MainPanelStyleController.updateStyle()` 选择 WIDE_VERTICAL；不能单凭右对齐判断它误用了手机布局。

## 可逆修复建议

首选修复真实物理尺寸：在独立候选内核中，将该竖屏原生面板 width_mm 改为166、height_mm改为266，保留原 EFI 与槽位回滚。先核对生产内核源与描述符确实相同；不要直接修改历史产物。

在现有 HWC 回退下，预期 xdpi=ydpi≈244.82，插件目标密度约321，328dp 内容宽约658px。应保持全局240 DPI，验证控制中心、设置、横竖屏及触摸坐标；毫米尺寸修正本身不应修改像素分辨率或触摸坐标。

若需要更快隔离，可构造只针对该内置面板的 HWC 物理尺寸候选覆盖并保留原 vendor 回滚，但不应全局改变外接显示的物理尺寸行为。不要把 `ro.sf.lcd_density` 当作此问题的唯一修复，它不覆盖 AutoDensity 的物理 PPI 计算。

不推荐先改插件面板宽度：tile、文字、间距仍跟随200 DPI，单改 panel_width 也没有控制主面板实际宽度。

插件存在调试属性 `log.tag.autodensity.debug.enable=-1` 禁用 AutoDensity，但 `getDebugScale()` 仅在 `RootUtil` 认定 test-keys 或 `/system/bin/su`、`/system/xbin/su` 存在时使用。当前 release-keys，两个路径只读检查无结果，因此不能假定 KernelSU/adbd root 会让此开关生效。该属性还可能影响其他 MIUIX 进程，不是优先的局部修复。

## 尚未完成

本轮不包含候选编译、刷入或修复后的截图验证。上面的预期尺寸是按实际源码公式计算，不能代替部署后测量。
