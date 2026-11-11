import 'package:flutter/material.dart';
import 'package:gk3_common/ui/theme.dart';
import 'package:gk3_common/ui/widgets.dart';

import '../app.dart';
import '../model.dart';
import '../session.dart';
import 'bits.dart';
import 'install_pages.dart';
import 'run_pages.dart';

/// 已经装了 Android：显示装在哪、占多大；可以卸载（重新安装 = 先卸载再装）
class ManagePage extends StatelessWidget {
  const ManagePage({super.key});

  @override
  Widget build(BuildContext context) {
    final s = context.session, l = context.l, d = s.disk!;
    final parts = s.probe!.androidOn(d);
    final total = parts.fold<int>(0, (a, p) => a + p.sizeMib);
    return WinStepPage(
      step: WinStep.check,
      title: l.manageTitle,
      subtitle: l.manageSub(fmtMib(total)),
      onBack: () => Navigator.pop(context),
      child: ListView(children: [
        systemDiskBar(context),
        const SizedBox(height: 8),
        const DiskLegend(android: true),
        const SizedBox(height: 20),
        Text(l.manageParts, style: context.tt.titleMedium),
        const SizedBox(height: 8),
        Wrap(spacing: 10, runSpacing: 10, children: [
          for (final p in parts) Chip(label: Text('${p.name}  ${fmtMib(p.sizeMib)}')),
        ]),
        const SizedBox(height: 24),
        ChoiceCard(
          icon: Icons.delete_outline,
          danger: true,
          title: l.manageUninstall,
          body: l.manageUninstallBody,
          trailing: const Icon(Icons.chevron_right),
          onTap: () => go(context, const UninstallConfirmPage()),
        ),
        const SizedBox(height: 12),
        NoteBox(text: l.manageReinstallNote),
      ]),
    );
  }
}

/// 卸载之前：删哪些分区、引导怎么还原、数据全丢；按住 2 秒才开始
class UninstallConfirmPage extends StatelessWidget {
  const UninstallConfirmPage({super.key});

  @override
  Widget build(BuildContext context) {
    final s = context.session, l = context.l, d = s.disk!;
    final parts = s.probe!.androidOn(d);
    final total = parts.fold<int>(0, (a, p) => a + p.sizeMib);
    final ready = !s.needBitlockerAck || s.bitlockerAck;
    return WinStepPage(
      step: WinStep.confirm,
      danger: true,
      title: l.uninstallTitle,
      subtitle: l.uninstallSub,
      bottom: Row(children: [
        Btn(l.btnBack, onPressed: () => Navigator.pop(context), kind: BtnKind.text, icon: Icons.arrow_back),
        const SizedBox(width: 24),
        Expanded(
          child: HoldToConfirm(
            idle: l.holdToUninstall,
            busy: (_) => l.holdKeep,
            enabled: ready,
            onConfirmed: () => go(context, const RunPage(task: WriteTask.uninstall), replace: true),
          ),
        ),
      ]),
      child: ListView(children: [
        Text(l.confirmWill, style: context.tt.titleMedium),
        const SizedBox(height: 4),
        Bullet(l.uninstallParts(parts.length, fmtMib(total))),
        Bullet(l.uninstallBoot),
        Bullet(l.uninstallFree),
        const SizedBox(height: 12),
        Bullet(l.confirmWindowsSafe, icon: Icons.shield_outlined, color: context.gk.success),
        if (s.needBitlockerAck) ...[
          const SizedBox(height: 16),
          NoteBox(
            warn: true,
            text: l.confirmBitlocker,
            child: CheckboxListTile(
              contentPadding: EdgeInsets.zero,
              controlAffinity: ListTileControlAffinity.leading,
              value: s.bitlockerAck,
              onChanged: (v) => s.setBitlockerAck(v ?? false),
              title: Text(l.confirmBitlockerAck, style: context.tt.bodyLarge),
            ),
          ),
        ],
      ]),
    );
  }
}
