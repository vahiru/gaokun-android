# 设置页面触摸后显示异常：现场保存

2026-10-30 用户明确报告设置显示不稳定、触摸后再次异常。此前几张静态截图未辨认出异常，不能解释为问题消失，也不能宣称已修复。

已直接截图当前页面，未先切页或重启应用；同时从 DCIM/Screenshots 取回用户00:27:26、39、42、51保存的四张设置截图，仅读取与本次报告相关图片。当前焦点为 DeepLinkHomepageActivity，内容“全部参数与信息”。

证据目录 `/Users/vahiru/.cache/hos-r15-nav/`：
- settings-render-abnormal.png / settings-render-abnormal-log.txt
- user-settings-abnormal-first.jpg、user-settings-39.jpg、user-settings-42.jpg、user-settings-abnormal.jpg
- settings-render-surface.txt、settings-render-input-display.txt

显示和输入视口读回1600×2560、rotation0、240 DPI，fontScale1.25。暂未找到足以定位用户所述异常的具体图像区域；已请用户指出区域，或确认截图与肉眼显示是否不同。日志中的DolbyVision metadata错误本身不足以定因。本轮未更改渲染参数、密度、字体、旋转或应用数据。

## 用户澄清与按压分帧（后续）

用户明确：触摸时卡片边缘消失，松手恢复；另报控制中心比例过小。不是单纯静态配色问题。

临时修改 secure background_blur_enable=0未获得改善证据，已恢复1（派生persist.sys.background_blur_setting=1）；临时contrast_level=1无卡片配色改善，已删除恢复原本不存在状态。未修改亮度。

DOWN/MOVE/UP截屏显示MOVE时最后一行白色变为约244灰、页面底色243灰，像素圆角形状仍保留，视觉上卡片底边与背景融合。这是按压状态的一条证据，并非完整根因结论。

尝试shell fabricated覆盖 Settings color/miuix_preference_item_tint_color_light为蓝色，资源查询生效，但用户反馈仍会消失。force-stop Settings重开后“更多连接”仍灰。已禁用 com.android.shell:HosSettingsTouchTint，读回原黑色。静态候选 tools/hos/overlays/settings-touch 标记未接受部署，未集成镜像。后续代码确认首页HeaderAdapter走recyclerview tint，不是preference tint，不能由这次失败推断所有按压资源都无效。

控制中心另有独立、数值闭环的面板物理尺寸错轴问题，见hyperos-r15-controlcenter-density-audit.md及hyperos-r16-display-physical-density.md；不能用它代替设置按压问题的验收。
