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
      onNext: s.checking || t == null ? null : () => go(context, t.usb ? const ReleasePage(media: true) : const MediaRegionPage()),
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
                title: l.mediaInternalTitle,
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

/// 写入本机时（仍是"目标"这一步）：GK3LIVE 建在哪块未分配空间的末尾（理由见 Session.mediaRegion）。
/// 分区条上标出它的位置，与双系统安装的选位置页相同
class MediaRegionPage extends StatelessWidget {
  const MediaRegionPage({super.key});

  @override
  Widget build(BuildContext context) {
    final s = context.session, l = context.l, r = s.mediaRegion;
    final need = fmtMib(kMediaNeedMib);
    return WinStepPage(
      step: MediaStep.target,
      title: l.mediaRegionTitle,
      subtitle: l.mediaRegionSub(need),
      onBack: () => Navigator.pop(context),
      nextLabel: l.btnNext,
      onNext: r == null ? null : () => go(context, const ReleasePage(media: true)),
      child: ListView(children: [
        _liveDiskBar(context, r),
        const SizedBox(height: 8),
        const DiskLegend(live: true),
        const SizedBox(height: 24),
        for (final f in s.freeSpans)
          Padding(
            padding: const EdgeInsets.only(bottom: 12),
            child: ChoiceCard(
              icon: Icons.crop_free,
              title: l.regionFree(fmtMib(f.sizeMib)),
              body: s.mediaFits(f) ? l.mediaRegionRest(fmtMib(f.sizeMib - kMediaNeedMib)) : null,
              reason: s.mediaFits(f) ? null : l.regionTooSmall(need),
              selected: identical(f, r),
              onTap: s.mediaFits(f) ? () => s.selectMediaRegion(f) : null,
            ),
          ),
      ]),
    );
  }

  /// 系统盘的分区条：所选空闲区的末尾画成将要创建的 GK3LIVE，前面的部分仍是空闲区
  Widget _liveDiskBar(BuildContext context, FreeSpan? r) {
    final s = context.session, d = s.disk!;
    final restSectors = r == null ? 0 : (r.sizeMib - kMediaNeedMib) * 1024 * 1024 ~/ d.sector;
    return DiskBar(
      height: 28,
      muted: true,
      parts: [
        for (final p in s.probe!.partsOf(d)) (start: p.start, sizeMib: p.sizeMib, os: p.os.isEmpty ? 'other' : p.os),
        if (r != null) (start: r.start + restSectors, sizeMib: kMediaNeedMib, os: DiskBar.planned),
      ],
      free: [
        for (final f in s.freeSpans)
          if (!identical(f, r))
            (start: f.start, sizeMib: f.sizeMib)
          else if (f.sizeMib > kMediaNeedMib)
            (start: f.start, sizeMib: f.sizeMib - kMediaNeedMib),
      ],
    );
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
          Bullet(l.mediaConfirmPart(fmtMib(s.mediaRegion!.sizeMib), fmtMib(kMediaNeedMib))),
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
