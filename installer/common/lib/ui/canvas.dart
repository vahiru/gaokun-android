import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import 'theme.dart';

/// 固定 1280×800 的逻辑画布，整体缩放到屏幕上（C 版 README："逻辑坐标固定 1280×800，
/// 输出时整体缩放"）。这也让 golden 出图与真机上的布局一模一样。
/// ★ Esc = 返回（roadmap 第 1 步：Tab/方向键移动焦点、回车确认、Esc 返回）。
class LogicalCanvas extends StatelessWidget {
  const LogicalCanvas({super.key, required this.child});
  final Widget child;

  @override
  Widget build(BuildContext context) {
    final mq = MediaQuery.of(context);
    return ColoredBox(
      color: Theme.of(context).colorScheme.surface,
      child: Center(
        child: FittedBox(
          child: SizedBox.fromSize(
            size: kCanvas,
            child: MediaQuery(
              data: mq.copyWith(size: kCanvas, padding: EdgeInsets.zero, viewInsets: EdgeInsets.zero, textScaler: TextScaler.noScaling),
              child: Shortcuts(
                shortcuts: const {SingleActivator(LogicalKeyboardKey.escape): DismissIntent()},
                child: child,
              ),
            ),
          ),
        ),
      ),
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
