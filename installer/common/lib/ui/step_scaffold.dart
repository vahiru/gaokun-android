import 'package:flutter/material.dart';

import 'theme.dart';
import 'widgets.dart';

/// 左侧步骤栏里的一项：标签与两个图标（还没到 / 当前）
class RailStep {
  const RailStep(this.label, this.icon, this.iconCurrent);
  final String label;
  final IconData icon, iconCurrent;
}

/// 每一步的骨架（MD3 大屏布局）：左侧导航栏标出步骤；右侧标题、说明、内容、底栏（返回 / 下一步）。
/// 步骤栏只是指示，不可点：导航只走 返回 / 下一步（Navigator 的栈）。
/// 各应用传入自己的步骤、侧栏副标题与侧栏底部（LiveCD：重启 / 关机按钮与版本号）。
class StepScaffold extends StatelessWidget {
  const StepScaffold({
    super.key,
    required this.steps,
    required this.current,
    this.railTitle = 'gaokun3',
    this.railIcon = Icons.android,
    required this.railSubtitle,
    this.railFooter = const [],
    required this.backLabel,
    required this.defaultNextLabel,
    this.nextLabel,
    required this.title,
    this.subtitle,
    required this.child,
    this.onBack,
    this.onNext,
    this.nextDanger = false,
    this.bottomLeft,
    this.bottom,
    this.danger = false,
  });

  final List<RailStep> steps;

  /// 当前是第几步（steps 的下标）
  final int current;

  /// 侧栏顶上的主标题、副标题与左边圆里的图标（null = 不要图标，文字照原来的边距靠左）
  final String railTitle;
  final IconData? railIcon;
  final String railSubtitle;

  /// 侧栏底部（在一个 Spacer 之后，按顺序排）
  final List<Widget> railFooter;
  final String backLabel;

  /// 有 onNext 而没给 nextLabel 时，"下一步"按钮的文字
  final String defaultNextLabel;

  /// 下一步按钮的文字；给了文字而没有 onNext 时画一个禁用的按钮
  final String? nextLabel;
  final String title;
  final String? subtitle;
  final Widget child;
  final VoidCallback? onBack, onNext;
  final bool nextDanger;
  final Widget? bottomLeft;

  /// 替换整条底栏（确认页的"按住 2 秒"、进度页的空底栏）
  final Widget? bottom;

  /// 标题换成 error-container 的警示横幅（大警告图标 + 标题 + 说明）：写盘前的风险确认页
  final bool danger;

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      body: Row(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          _StepRail(steps: steps, current: current, title: railTitle, icon: railIcon, subtitle: railSubtitle, footer: railFooter),
          Expanded(
            child: Padding(
              padding: const EdgeInsets.fromLTRB(48, 40, 48, 28),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.stretch,
                children: [
                  if (danger)
                    _DangerBanner(title: title, subtitle: subtitle)
                  else ...[
                    Text(title, style: context.tt.headlineMedium),
                    if (subtitle != null) ...[
                      const SizedBox(height: 8),
                      Text(subtitle!, style: context.tt.bodyLarge!.copyWith(color: context.cs.onSurfaceVariant)),
                    ],
                  ],
                  SizedBox(height: danger ? 20 : 28),
                  Expanded(child: child),
                  const SizedBox(height: 20),
                  bottom ??
                      Row(
                        children: [
                          if (onBack != null) Btn(backLabel, onPressed: onBack, kind: BtnKind.text, icon: Icons.arrow_back),
                          if (bottomLeft != null) ...[const SizedBox(width: 12), bottomLeft!],
                          const Spacer(),
                          if (nextLabel != null || onNext != null)
                            Btn(nextLabel ?? defaultNextLabel, onPressed: onNext, kind: nextDanger ? BtnKind.danger : BtnKind.primary),
                        ],
                      ),
                ],
              ),
            ),
          ),
        ],
      ),
    );
  }
}

/// 风险确认页的标题：MD3 的 error-container 横幅，左边一个 error 色的圆、里面是大号警告图标。
/// 整页最醒目的东西 —— 用户 2026-10-06："用最醒目的方式告诉用户，这玩意儿可能会损坏数据"
class _DangerBanner extends StatelessWidget {
  const _DangerBanner({required this.title, this.subtitle});
  final String title;
  final String? subtitle;

  @override
  Widget build(BuildContext context) {
    final cs = context.cs, tt = context.tt;
    return Container(
      padding: const EdgeInsets.fromLTRB(20, 18, 24, 18),
      decoration: BoxDecoration(color: cs.errorContainer, borderRadius: BorderRadius.circular(24)),
      child: Row(children: [
        Container(
          width: 72,
          height: 72,
          decoration: BoxDecoration(color: cs.error, shape: BoxShape.circle),
          child: Icon(Icons.warning_amber_rounded, size: 44, color: cs.onError),
        ),
        const SizedBox(width: 20),
        Expanded(
          child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
            Text(title, style: tt.headlineMedium!.copyWith(color: cs.onErrorContainer)),
            if (subtitle != null) ...[
              const SizedBox(height: 6),
              Text(subtitle!, style: tt.bodyLarge!.copyWith(color: cs.onErrorContainer)),
            ],
          ]),
        ),
      ]),
    );
  }
}

/// 左侧的步骤栏：MD3 标准导航抽屉的样子（当前项是 secondary-container 的胶囊指示），但不可点
class _StepRail extends StatelessWidget {
  const _StepRail({required this.steps, required this.current, required this.title, required this.icon, required this.subtitle, required this.footer});
  final List<RailStep> steps;
  final int current;
  final String title;
  final IconData? icon;
  final String subtitle;
  final List<Widget> footer;

  @override
  Widget build(BuildContext context) {
    final cs = context.cs, tt = context.tt;
    return Container(
      width: 272,
      color: cs.surfaceContainerLow,
      padding: const EdgeInsets.fromLTRB(12, 32, 12, 24),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          Padding(
            padding: const EdgeInsets.symmetric(horizontal: 16),
            child: Row(
              children: [
                if (icon != null) ...[
                  Container(
                    width: 44,
                    height: 44,
                    decoration: BoxDecoration(color: cs.primaryContainer, shape: BoxShape.circle),
                    child: Icon(icon, color: cs.onPrimaryContainer),
                  ),
                  const SizedBox(width: 14),
                ],
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Text(title, style: tt.titleMedium),
                      Text(subtitle, style: tt.bodySmall),
                    ],
                  ),
                ),
              ],
            ),
          ),
          const SizedBox(height: 28),
          for (var i = 0; i < steps.length; i++) _RailItem(step: steps[i], state: i.compareTo(current)),
          const Spacer(),
          ...footer,
        ],
      ),
    );
  }
}

class _RailItem extends StatelessWidget {
  const _RailItem({required this.step, required this.state});
  final RailStep step;

  /// < 0 已完成，0 当前，> 0 还没到
  final int state;

  @override
  Widget build(BuildContext context) {
    final cs = context.cs, tt = context.tt;
    final cur = state == 0, done = state < 0;
    final fg = cur ? cs.onSecondaryContainer : (done ? cs.onSurface : cs.onSurfaceVariant);
    return Container(
      height: 56,
      margin: const EdgeInsets.only(bottom: 2),
      padding: const EdgeInsets.symmetric(horizontal: 16),
      decoration: ShapeDecoration(shape: const StadiumBorder(), color: cur ? cs.secondaryContainer : Colors.transparent),
      child: Row(
        children: [
          Icon(done ? Icons.check_circle : (cur ? step.iconCurrent : step.icon), size: 24, color: done ? cs.primary : fg),
          const SizedBox(width: 12),
          Expanded(
            child: Text(step.label, style: tt.labelLarge!.copyWith(color: fg)),
          ),
        ],
      ),
    );
  }
}
