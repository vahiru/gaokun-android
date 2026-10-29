# R15 桌面短滑翻页检查

用户反馈一屏到二屏所需滑动距离太长。2026-10-30 在当前隔离 HyperOS、1600×2560/240 DPI、MIUI Home 前台进行注入对照：

- 从第一页向左300 px、180 ms：日志 pageChange screenId=1->2、ScrollEnd page=1.0，成功。
- 从第二页向右300 px、800 ms：日志 ScrollEnd page=0.0，成功。
- 从第一页向左150 px、200 ms：事后截图仍第一页；没有取得可靠对应 ScrollEnd 日志，不能只凭这次确定精确阈值。

证据目录 `/Users/vahiru/.cache/hos-r15-nav/paging/`。300 px 是屏宽18.75%；自动注入绕过电容检测/驱动跟踪，不能代替真实手指测试。当前证据排除“桌面始终要求横跨整屏才能切页”，不排除翻页判定仍有优化空间。

当前 MiuiHome APK 无DEX，含Flutter AOT libapp.so及Rust libapp_launcher.so。传统 ScreenView Java 参数调整方案不适用。字符串发现 NoSwipeRightAtFirstPagePhysics，但未找到经过验证的外部距离/速度配置入口。没有修改桌面APK、签名、屏幕密度、输入缩放或全局触摸阈值；不把本次检查称为已优化。

下一步若需要更短手势，应确认实际无干扰手指轨迹与桌面最终判定，定位此Flutter版本的分页physics实现或可用配置后再修改，避免用全局坐标放大破坏点击定位。
