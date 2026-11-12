import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import 'theme.dart';

/// 固定 1280×800 的逻辑画布，整体缩放到屏幕上（C 版 README："逻辑坐标固定 1280×800，
/// 输出时整体缩放"）。这也让 golden 出图与真机上的布局一模一样。
/// ★ Esc = 返回（roadmap 第 1 步：Tab/方向键移动焦点、回车确认、Esc 返回）。
class LogicalCanvas extends StatelessWidget {
  const LogicalCanvas({super.key, required this.child, this.extend = false});
  final Widget child;

  /// 屏幕（窗口）不是 16:10 时：false = 等比缩放、居中，两边空着；true = 同样按 1280×800 放得下的比例缩放，
  /// 多出来的那个方向把画布加宽（或加高），界面铺满（Windows 安装器的窗口可以最大化，工作区不是 16:10）
  final bool extend;

  @override
  Widget build(BuildContext context) {
    if (!extend) return _scaled(context, kCanvas);
    return LayoutBuilder(builder: (context, c) {
      final s = c.maxWidth / kCanvas.width < c.maxHeight / kCanvas.height ? c.maxWidth / kCanvas.width : c.maxHeight / kCanvas.height;
      if (!s.isFinite || s <= 0) return _scaled(context, kCanvas);
      return _scaled(context, Size(c.maxWidth / s, c.maxHeight / s), fill: true);
    });
  }

  /// [fill]：画布与可用空间同比例，放大缩小都铺满（Center 给的是宽松约束，FittedBox 只会缩小、不会放大）
  Widget _scaled(BuildContext context, Size size, {bool fill = false}) {
    final mq = MediaQuery.of(context);
    final fitted = FittedBox(
          child: SizedBox.fromSize(
            size: size,
            child: MediaQuery(
              data: mq.copyWith(size: size, padding: EdgeInsets.zero, viewInsets: EdgeInsets.zero, textScaler: TextScaler.noScaling),
              child: Shortcuts(
                shortcuts: const {SingleActivator(LogicalKeyboardKey.escape): DismissIntent()},
                child: child,
              ),
            ),
          ),
        );
    return ColoredBox(
      color: Theme.of(context).colorScheme.surface,
      child: fill ? SizedBox.expand(child: fitted) : Center(child: fitted),
    );
  }
}

/// 角落里"演示数据：<场景>"的小标签（后端是 FixtureBackend 时）。文字由应用按自己的 l10n 拼好传进来
class DemoBadge extends StatelessWidget {
  const DemoBadge(this.text, {super.key});
  final String text;
  @override
  Widget build(BuildContext context) => IgnorePointer(
        // MD3 的小标签（tertiary-container，8 dp 圆角 —— 与 chip 同一个形状）
        child: Material(
          color: context.cs.tertiaryContainer,
          borderRadius: BorderRadius.circular(8),
          child: Padding(
            padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 6),
            child: Text(text, style: context.tt.labelLarge!.copyWith(color: context.cs.onTertiaryContainer)),
          ),
        ),
      );
}
