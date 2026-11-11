import 'package:flutter/material.dart';
import 'package:gk3_common/ui/theme.dart';
import 'package:gk3_common/ui/widgets.dart';

import '../app.dart';
import '../l10n/app_localizations.dart';
import '../model/model.dart';
import '../version.dart';

/// 安装流程的七步。左侧导航按它标出"到哪了"（只是指示，不可点：导航只走 返回 / 下一步，
/// 那是 Navigator 的栈 —— C 版 screen++/screen-- 掉进没走过的页的教训，见 app.dart 的 go）。
enum Gk3Step { welcome, disk, mode, source, opts, confirm, install }

extension on Gk3Step {
  (IconData, IconData) get icons => switch (this) {
    Gk3Step.welcome => (Icons.waving_hand_outlined, Icons.waving_hand),
    Gk3Step.disk => (Icons.storage_outlined, Icons.storage),
    Gk3Step.mode => (Icons.call_split_outlined, Icons.call_split),
    Gk3Step.source => (Icons.download_outlined, Icons.download),
    Gk3Step.opts => (Icons.tune_outlined, Icons.tune),
    Gk3Step.confirm => (Icons.fact_check_outlined, Icons.fact_check),
    Gk3Step.install => (Icons.install_desktop_outlined, Icons.install_desktop),
  };
  String label(L10n l) => switch (this) {
    Gk3Step.welcome => l.railWelcome,
    Gk3Step.disk => l.railDisk,
    Gk3Step.mode => l.railMode,
    Gk3Step.source => l.railSource,
    Gk3Step.opts => l.railOpts,
    Gk3Step.confirm => l.railConfirm,
    Gk3Step.install => l.railInstall,
  };
}

/// 每一步的骨架（MD3 大屏布局）：左侧导航栏标出步骤；右侧标题、说明、内容、底栏（返回 / 下一步）
class StepPage extends StatelessWidget {
  const StepPage({
    super.key,
    required this.step,
    required this.title,
    this.subtitle,
    required this.child,
    this.onBack,
    this.onNext,
    this.nextLabel,
    this.nextDanger = false,
    this.bottomLeft,
    this.bottom,
    this.danger = false,
  });

  final Gk3Step step;
  final String title;
  final String? subtitle;
  final Widget child;
  final VoidCallback? onBack, onNext;
  final String? nextLabel;
  final bool nextDanger;
  final Widget? bottomLeft;

  /// 替换整条底栏（确认页的"按住 2 秒"、进度页的空底栏）
  final Widget? bottom;

  /// 标题换成 error-container 的警示横幅（大警告图标 + 标题 + 说明）：写盘前的风险确认页（screens_risk.dart）
  final bool danger;

  @override
  Widget build(BuildContext context) {
    final l = L10n.of(context);
    return Scaffold(
      body: Row(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          _StepRail(current: step),
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
                          if (onBack != null) Btn(l.btnBack, onPressed: onBack, kind: BtnKind.text, icon: Icons.arrow_back),
                          if (bottomLeft != null) ...[const SizedBox(width: 12), bottomLeft!],
                          const Spacer(),
                          if (nextLabel != null || onNext != null)
                            Btn(nextLabel ?? l.btnNext, onPressed: onNext, kind: nextDanger ? BtnKind.danger : BtnKind.primary),
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
  const _StepRail({required this.current});
  final Gk3Step current;

  @override
  Widget build(BuildContext context) {
    final l = L10n.of(context), cs = context.cs, tt = context.tt;
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
                Container(
                  width: 44,
                  height: 44,
                  decoration: BoxDecoration(color: cs.primaryContainer, shape: BoxShape.circle),
                  child: Icon(Icons.android, color: cs.onPrimaryContainer),
                ),
                const SizedBox(width: 14),
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Text('gaokun3', style: tt.titleMedium),
                      Text(l.railSub, style: tt.bodySmall),
                    ],
                  ),
                ),
              ],
            ),
          ),
          const SizedBox(height: 28),
          for (final s in Gk3Step.values) _RailItem(step: s, state: s.index.compareTo(current.index)),
          const Spacer(),
          const _PowerButtons(),
          const SizedBox(height: 16),
          Padding(
            padding: const EdgeInsets.symmetric(horizontal: 16),
            child: Text(l.railFooter(kInstallerVersion), style: tt.bodySmall),
          ),
        ],
      ),
    );
  }
}

class _RailItem extends StatelessWidget {
  const _RailItem({required this.step, required this.state});
  final Gk3Step step;

  /// < 0 已完成，0 当前，> 0 还没到
  final int state;

  @override
  Widget build(BuildContext context) {
    final cs = context.cs, tt = context.tt;
    final (outline, filled) = step.icons;
    final cur = state == 0, done = state < 0;
    final fg = cur ? cs.onSecondaryContainer : (done ? cs.onSurface : cs.onSurfaceVariant);
    return Container(
      height: 56,
      margin: const EdgeInsets.only(bottom: 2),
      padding: const EdgeInsets.symmetric(horizontal: 16),
      decoration: ShapeDecoration(shape: const StadiumBorder(), color: cur ? cs.secondaryContainer : Colors.transparent),
      child: Row(
        children: [
          Icon(done ? Icons.check_circle : (cur ? filled : outline), size: 24, color: done ? cs.primary : fg),
          const SizedBox(width: 12),
          Expanded(
            child: Text(step.label(L10n.of(context)), style: tt.labelLarge!.copyWith(color: fg)),
          ),
        ],
      ),
    );
  }
}

/// 侧栏底部常驻的"重新启动 / 关机"（v1.0 计划 GUI-3）：失败页、缩分区页让用户"回 Windows 关掉快速启动"，
/// 以前整个安装器只有完成页能重启 —— 别的时候只能长按电源键。
/// 正在写盘（Session.writing）时禁用并写明原因；点了先弹确认，不是一碰就关。
class _PowerButtons extends StatelessWidget {
  const _PowerButtons();

  Future<void> _ask(BuildContext context, String title, Future<void> Function() act) async {
    final l = L10n.of(context);
    final ok = await showDialog<bool>(
      context: context,
      builder: (c) => AlertDialog(
        title: Text(title),
        content: SizedBox(width: 520, child: Text(l.powerBody)),
        actions: [
          TextButton(onPressed: () => Navigator.pop(c, false), child: Text(MaterialLocalizations.of(c).cancelButtonLabel)),
          FilledButton(onPressed: () => Navigator.pop(c, true), child: Text(title.replaceAll(RegExp(r'[？?]$'), ''))),
        ],
      ),
    );
    if (ok == true) await act();
  }

  @override
  Widget build(BuildContext context) {
    final s = SessionScope.maybeOf(context);
    if (s == null) return const SizedBox.shrink();
    final l = L10n.of(context);
    final busy = s.writing;
    // 一行一个、左对齐（与上面的步骤项对齐）：两个并排时 272 宽的侧栏放不下"重新启动"四个字
    final st = TextButton.styleFrom(alignment: Alignment.centerLeft, padding: const EdgeInsets.symmetric(horizontal: 16));
    return Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
      TextButton.icon(
        style: st,
        onPressed: busy ? null : () => _ask(context, l.powerRebootTitle, s.backend.reboot),
        icon: const Icon(Icons.restart_alt),
        label: Text(l.railReboot),
      ),
      TextButton.icon(
        style: st,
        onPressed: busy ? null : () => _ask(context, l.powerPoweroffTitle, s.backend.poweroff),
        icon: const Icon(Icons.power_settings_new),
        label: Text(l.railPoweroff),
      ),
      if (busy)
        Padding(
          padding: const EdgeInsets.symmetric(horizontal: 16),
          child: Text(l.railBusy, style: context.tt.bodySmall!.copyWith(color: context.cs.onSurfaceVariant)),
        ),
    ]);
  }
}

/// /data 低于这个值时提醒（v1.0 计划 GUI-20：一个大型手游就要 20–40 GiB，而项目的目标就是跑手游）。
/// 只是提醒、不拦：真正的下限仍由 gk3_plan 判（GK3_USERDATA_MIN_MIB）
const kDataWarnMib = 32 * 1024;

/// 黄色（warning 角色）的一行提醒；/data 够大时什么都不画
class DataSizeWarning extends StatelessWidget {
  const DataSizeWarning(this.mib, {super.key});
  final int mib;

  @override
  Widget build(BuildContext context) {
    if (mib >= kDataWarnMib) return const SizedBox.shrink();
    return Padding(
      padding: const EdgeInsets.only(top: 10),
      child: Text(L10n.of(context).dataSmallWarn(fmtMib(mib < 0 ? 0 : mib)), style: context.tt.bodyMedium!.copyWith(color: context.gk.warning)),
    );
  }
}

/// 一块盘的布局条（gk3_common 的 DiskBar）：把 [Disk] 的分区与空闲区交给它。
/// [highlightFree] 按对象认（与 disk.free 里的同一个 FreeRegion），不按起点比
DiskBar diskBar(Disk disk, {FreeRegion? highlightFree}) => DiskBar(
      parts: [for (final p in disk.parts) (start: p.start, sizeMib: p.sizeMib, os: p.os)],
      free: [for (final f in disk.free) (start: f.start, sizeMib: f.sizeMib)],
      highlightFree: highlightFree == null ? null : disk.free.indexWhere((f) => identical(f, highlightFree)),
    );

/// 信号格数（dBm 数字对用户没有意义，roadmap 欠账第 1 条）—— 用 MD3 自带的 WiFi 图标
class SignalBars extends StatelessWidget {
  const SignalBars(this.bars, {super.key, this.secure = false});
  final int bars;
  final bool secure;
  @override
  Widget build(BuildContext context) {
    final icon = switch (bars.clamp(0, 4)) {
      0 => Icons.signal_wifi_0_bar,
      1 => Icons.network_wifi_1_bar,
      2 => Icons.network_wifi_2_bar,
      3 => Icons.network_wifi_3_bar,
      _ => Icons.signal_wifi_4_bar,
    };
    return Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        if (secure) Icon(Icons.lock_outline, size: 18, color: context.cs.onSurfaceVariant),
        const SizedBox(width: 4),
        Icon(icon, color: context.cs.onSurfaceVariant),
      ],
    );
  }
}
