import 'package:flutter/material.dart';
import 'package:gk3_common/ui/theme.dart';
import 'package:gk3_common/ui/widgets.dart';

import '../app.dart';
import '../model.dart';
import '../session.dart';
import 'bits.dart';
import 'install_pages.dart';
import 'run_pages.dart';

// 制作独立安装介质（侧栏底部的入口）：把最新的 LiveCD 与所选版本的安装文件写入 U 盘或本机磁盘。
// 安装方式（清除整个磁盘 / 保留 Windows / 重新安装）在 LiveCD 里选，这边不向 LiveCD 传任何状态。
// 写到本机磁盘时，LiveCD 要先载入内存才能清除它所在的磁盘（LiveCD 端尚未实现）。
// 设计与待定事项：experiments/wininstall-proto-20261111/notes.md（工作区，不在仓库里）。

/// 第一步：安装介质能做什么、可以写到哪里
class MediaAboutPage extends StatelessWidget {
  const MediaAboutPage({super.key});

  @override
  Widget build(BuildContext context) {
    final l = context.l;
    return WinStepPage(
      step: MediaStep.about,
      title: l.mediaAboutTitle,
      subtitle: l.mediaAboutSub,
      onBack: () => Navigator.pop(context),
      onNext: () => go(context, const MediaTargetPage()),
      child: ListView(children: [
        Text(l.mediaAboutUses, style: context.tt.titleMedium),
        const SizedBox(height: 4),
        Bullet(l.mediaUseWipe, icon: Icons.delete_sweep_outlined),
        Bullet(l.mediaUseAlong, icon: Icons.splitscreen_outlined),
        Bullet(l.mediaUseReinstall, icon: Icons.build_outlined),
        const SizedBox(height: 16),
        Text(l.mediaAboutTargets, style: context.tt.titleMedium),
        const SizedBox(height: 4),
        Bullet(l.mediaAboutUsb, icon: Icons.usb),
        Bullet(l.mediaAboutInternal, icon: Icons.storage_outlined),
        const SizedBox(height: 16),
        NoteBox(text: l.mediaAboutNote),
      ]),
    );
  }
}

/// 第二步：写到哪个 U 盘，或本机磁盘的未分配空间。本机磁盘要满足双系统安装的检查项（首页）
class MediaTargetPage extends StatelessWidget {
  const MediaTargetPage({super.key});

  @override
  Widget build(BuildContext context) {
    final s = context.session, l = context.l;
    final need = fmtMib(kMediaNeedMib);
    final t = s.mediaTarget;
    final blockers = _internalBlockers(context);
    return WinStepPage(
      step: MediaStep.target,
      title: l.mediaTargetTitle,
      subtitle: l.mediaTargetSub(need),
      onBack: () => Navigator.pop(context),
      bottomLeft: Btn(l.btnRedetect, onPressed: s.checking ? null : s.check, kind: BtnKind.secondary, icon: Icons.refresh),
      nextLabel: l.btnNext,
      onNext: s.checking || t == null ? null : () => go(context, const ReleasePage(media: true)),
      child: s.checking
          ? Center(
              child: Column(mainAxisSize: MainAxisSize.min, children: [
                const CircularProgressIndicator(),
                const SizedBox(height: 16),
                Text(l.checking, style: context.tt.bodyLarge),
              ]),
            )
          : ListView(children: [
              Text(l.mediaUsbHeader, style: context.tt.titleMedium),
              const SizedBox(height: 8),
              if (s.usbDisks.isEmpty) NoteBox(text: l.mediaUsbNone),
              for (final d in s.usbDisks)
                Padding(
                  padding: const EdgeInsets.only(bottom: 12),
                  child: ChoiceCard(
                    icon: Icons.usb,
                    title: l.mediaUsbTitle(d.model, fmtMib(d.sizeMib)),
                    body: d.sizeMib >= kMediaNeedMib ? l.mediaUsbBody : null,
                    reason: d.sizeMib >= kMediaNeedMib ? null : l.mediaUsbTooSmall(need),
                    selected: t != null && t.same(MediaTarget.usb(d)),
                    onTap: d.sizeMib >= kMediaNeedMib ? () => s.selectMediaTarget(MediaTarget.usb(d)) : null,
                  ),
                ),
              const SizedBox(height: 16),
              Text(l.mediaInternalHeader, style: context.tt.titleMedium),
              const SizedBox(height: 8),
              ChoiceCard(
                icon: Icons.storage_outlined,
                title: s.mediaRegion == null ? l.mediaInternalTitleNone : l.mediaInternalTitle(fmtMib(s.mediaRegion!.sizeMib)),
                body: blockers.isEmpty ? l.mediaInternalBody : null,
                reason: blockers.isEmpty ? null : '${l.mediaInternalBlocked}\n${blockers.join('\n')}',
                selected: t != null && t.same(const MediaTarget.internal()),
                onTap: blockers.isEmpty ? () => s.selectMediaTarget(const MediaTarget.internal()) : null,
              ),
            ]),
    );
  }

  /// 写到本机磁盘之前不满足的条件（与首页检查同一套，另需放得下 GK3LIVE 的未分配空间）；空 = 可以写
  List<String> _internalBlockers(BuildContext context) {
    final s = context.session, l = context.l, pf = s.preflight;
    if (s.checkErr != null || pf == null || s.probe == null) return [l.chkFailed];
    return [
      if (!pf.modelOk) l.chkModelBad,
      if (pf.secureBoot == 'on') l.chkSecureBootOn,
      if (pf.fastStartup == 'on') l.chkFastStartupOn,
      if (pf.lowBattery) l.chkBatteryLow,
      if (s.disk == null) l.chkNoSystemDisk else if (s.mediaRegion == null) l.chkNoRoom(fmtMib(kMediaNeedMib)),
    ];
  }
}

/// 第四步：写盘之前说清楚会改什么；按住 2 秒才开始。U 盘整个清除，本机只动未分配空间与 EFI 分区
class MediaConfirmPage extends StatelessWidget {
  const MediaConfirmPage({super.key});

  @override
  Widget build(BuildContext context) {
    final s = context.session, l = context.l, cs = context.cs;
    final t = s.mediaTarget!, v = s.variant, lc = s.liveCd;
    final local = s.source == ReleaseSource.local;
    final ready = t.usb || !s.needBitlockerAck || s.bitlockerAck;
    return WinStepPage(
      step: MediaStep.confirm,
      danger: true,
      title: l.mediaConfirmTitle,
      subtitle: t.usb ? l.mediaConfirmSubUsb(t.disk!.model, fmtMib(t.disk!.sizeMib)) : l.mediaConfirmSubInternal,
      bottom: Row(children: [
        Btn(l.btnBack, onPressed: () => Navigator.pop(context), kind: BtnKind.text, icon: Icons.arrow_back),
        const SizedBox(width: 24),
        Expanded(
          child: HoldToConfirm(
            idle: l.holdToWrite,
            busy: (_) => l.holdKeep,
            enabled: ready,
            onConfirmed: () => go(context, const RunPage(task: WriteTask.media, releaseDir: kReleaseDir), replace: true),
          ),
        ),
      ]),
      child: ListView(children: [
        Text(l.confirmWill, style: context.tt.titleMedium),
        const SizedBox(height: 4),
        if (local)
          Bullet(l.confirmLocal(s.localDir))
        else ...[
          Bullet(l.confirmDownload(v!.name, fmtMib(v.sizeMib))),
          Bullet(l.mediaConfirmLiveDl(lc!.id, fmtMib(lc.sizeMib))),
        ],
        if (t.usb)
          Bullet(l.mediaConfirmUsb)
        else ...[
          Bullet(l.mediaConfirmPart(fmtMib(kMediaNeedMib))),
          Bullet(l.confirmBoot),
          Bullet(l.mediaConfirmEntry),
          const SizedBox(height: 12),
          Bullet(l.confirmWindowsSafe, icon: Icons.shield_outlined, color: context.gk.success),
        ],
        if (!t.usb && s.needBitlockerAck) ...[
          const SizedBox(height: 16),
          NoteBox(
            warn: true,
            text: l.confirmBitlocker,
            child: CheckboxListTile(
              contentPadding: EdgeInsets.zero,
              controlAffinity: ListTileControlAffinity.leading,
              value: s.bitlockerAck,
              onChanged: (v) => s.setBitlockerAck(v ?? false),
              title: Text(l.confirmBitlockerAck, style: context.tt.bodyLarge!.copyWith(color: cs.onSurface)),
            ),
          ),
        ],
      ]),
    );
  }
}
