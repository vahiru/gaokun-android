import 'package:flutter/material.dart';
import 'package:gk3_common/ui/theme.dart';

/// 检查页一行的状态
enum CheckState { ok, warn, fail, info }

/// 检查页的一行：左边状态图标，右边标题与说明（说明里写清楚怎么处理）
class CheckRow extends StatelessWidget {
  const CheckRow({super.key, required this.state, required this.title, this.detail});
  final CheckState state;
  final String title;
  final String? detail;

  @override
  Widget build(BuildContext context) {
    final cs = context.cs, gk = context.gk, tt = context.tt;
    final (icon, color) = switch (state) {
      CheckState.ok => (Icons.check_circle, gk.success),
      CheckState.warn => (Icons.warning_amber_rounded, gk.warning),
      CheckState.fail => (Icons.cancel, cs.error),
      CheckState.info => (Icons.info_outline, cs.primary),
    };
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 8),
      child: Row(crossAxisAlignment: CrossAxisAlignment.start, children: [
        Icon(icon, color: color, size: 28),
        const SizedBox(width: 16),
        Expanded(
          child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
            Text(title, style: tt.titleMedium),
            if (detail != null) ...[
              const SizedBox(height: 2),
              Text(detail!, style: tt.bodyMedium!.copyWith(color: cs.onSurfaceVariant)),
            ],
          ]),
        ),
      ]),
    );
  }
}

/// 一条要点（确认页的"会做什么"）
class Bullet extends StatelessWidget {
  const Bullet(this.text, {super.key, this.icon = Icons.circle, this.color});
  final String text;
  final IconData icon;
  final Color? color;

  @override
  Widget build(BuildContext context) => Padding(
        padding: const EdgeInsets.symmetric(vertical: 6),
        child: Row(crossAxisAlignment: CrossAxisAlignment.start, children: [
          Padding(
            padding: const EdgeInsets.only(top: 4),
            child: Icon(icon, size: icon == Icons.circle ? 8 : 20, color: color ?? context.cs.onSurfaceVariant),
          ),
          SizedBox(width: icon == Icons.circle ? 16 : 12),
          Expanded(child: Text(text, style: context.tt.bodyLarge)),
        ]),
      );
}

/// 一块有底色的说明（警告 / 提示）
class NoteBox extends StatelessWidget {
  const NoteBox({super.key, required this.text, this.warn = false, this.child});
  final String text;
  final bool warn;
  final Widget? child;

  @override
  Widget build(BuildContext context) {
    final cs = context.cs, gk = context.gk;
    final bg = warn ? gk.warningContainer : cs.surfaceContainerHigh;
    final fg = warn ? gk.onWarningContainer : cs.onSurface;
    return Container(
      padding: const EdgeInsets.fromLTRB(20, 16, 20, 16),
      decoration: BoxDecoration(color: bg, borderRadius: BorderRadius.circular(16)),
      child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
        Text(text, style: context.tt.bodyLarge!.copyWith(color: fg)),
        if (child != null) ...[const SizedBox(height: 8), child!],
      ]),
    );
  }
}
