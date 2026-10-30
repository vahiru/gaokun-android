# R22 设置按压动画逐帧复核

未应用新的视觉覆盖或修改Settings代码。设备在R22 super＋原R17内核时保存原配色截图及6秒有界录屏；其后指纹测试切换内核不影响这份历史采样的身份。证据位于~/.cache/hos-r22-settings-check/。

## 观测

原始截图1600×2560。首页左栏“更多连接”行固定DOWN/MOVE/CANCEL：采样白底255→灰234→取消动画灰228→稳定白255，圆角在这些离散截图中保留。取消后0.4秒截图仍处于动画，不能把它误认成已经稳定的UP状态。

录屏输出800×1280，共38帧；screenrecord设6秒，实际视频帧PTS0–4.488秒，不能声称覆盖120Hz显示的每一帧。没有录音，临时设备视频已删除。分析脚本保留所有解码帧而不补帧，用空白内部patch(x240..260,y752..768)与左外侧背景(x2..8,y752..768)的RGB中位数比较：

- frame11/PTS1.459356：内外均242，视频中卡片下缘肉眼难辨。
- frame23/PTS3.029533、frame24/PTS3.160578：内外最大通道差仅1，取消过渡再次接近背景。
- 稳定态及按压较深阶段差值恢复约13–15。

这是有损编码后的图像测量，不能将242当成未经编码的精确surface像素值。已纠正分析时最初选在右边x312..318的背景patch：该区域会出现滚动条/阴影，原比较受到污染，不用于最终结论。当前animation-pixels.json及contact-sheet以左背景patch生成。

## 与实际代码对应及界限

APK既有反编译显示CardStateDrawable.draw重建roundRect路径，仅动画Paint alpha。白色组底叠黑色foreground、按压alpha0.12会经过约(255−243)/255的背景同色点；录屏确实复现该动态边缘融合，不是把用户报告简化为静态配色偏好。

同时ViewOutlineHelper有拖动时跳过更新和延迟更新路径；本轮未读取运行时clipToOutline/provider/radii，不能排除用户其他操作还触发实际裁剪异常。该视频不能证明所有触摸异常只有一个原因。

下一候选保留原白底/黑tint/alpha动画，在组外轮廓加入独立细描边。先核对背景drawable替换是否兼容两栏group clipping/radius/inset及深色模式；不直接把foreground包装成LayerDrawable，因为右侧adapter依赖CardStateDrawable类型。未部署、不宣称设置渲染已修复。


## 背景RRO方案审查结论

没有生成或部署不完整的背景覆盖。两栏背景原为ColorDrawable，虽然支持替换普通Drawable，但BaseDecoration.clipDrawableRoundRect只传矩形bounds并做外层clip，不把动态上下圆角传给GradientDrawable。矩形stroke在圆角处会被裁掉；固定圆角stroke在partial group会出现错误切角/横边，左右栏半径来源也不同。可靠的描边必须复用现有动态group Path，不能仅用静态背景形状替换。

实际Settings APK签名为MIUI，证书SHA256 c9009d01ebf9f5d0302bc71b2fe9aa9a47a432bba17308a3111b75d7b2149025；当前没有可用的匹配签名构建路径。未为尝试描边修改DEX、重签系统包或放宽签名检查。继续寻找原有可配置接口或可验证的框架适配方式，保留当前原配色与动画；问题仍开放。
