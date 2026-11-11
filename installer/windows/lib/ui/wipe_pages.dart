import 'package:flutter/material.dart';
import 'package:gk3_common/ui/theme.dart';
import 'package:gk3_common/ui/widgets.dart';

import '../app.dart';
import '../model.dart';
import '../session.dart';
import 'bits.dart';
import 'check_page.dart';
import 'install_pages.dart';
import 'run_pages.dart';

// 整盘安装（侧栏底部的入口）：Windows 端只做准备 —— 在未分配空间里建 GK3LIVE、写入 LiveCD 与所选版本的
// 安装文件、配置下一次开机进入 LiveCD；清除整个磁盘与安装 Android 在 LiveCD 里进行，那边还会再确认一次。
// 设计与待定事项：experiments/wininstall-proto-20261111/notes.md（工作区，不在仓库里）。

/// 第一步：说明整盘安装会怎样进行、会清除什么
class WipeAboutPage extends StatelessWidget {
  const WipeAboutPage({super.key});

  @override
  Widget build(BuildContext context) {
    final l = context.l;
    return WinStepPage(
      step: WipeStep.about,
      title: l.wipeAboutTitle,
      subtitle: l.wipeAboutSub,
      onBack: () => Navigator.pop(context),
      onNext: () => go(context, const WipeCheckPage()),
      child: ListView(children: [
        Text(l.wipeAboutSteps, style: context.tt.titleMedium),
        const SizedBox(height: 4),
        Bullet(l.wipeAboutStep1, icon: Icons.album_outlined),
        Bullet(l.wipeAboutStep2, icon: Icons.restart_alt),
        Bullet(l.wipeAboutStep3, icon: Icons.delete_sweep_outlined),
        const SizedBox(height: 16),
        NoteBox(warn: true, text: l.wipeAboutWarn),
        const SizedBox(height: 12),
        NoteBox(text: l.wipeAboutKeepWindows),
      ]),
    );
  }
}

/// 第二步：与双系统安装相同的检查项，外加放 GK3LIVE 的未分配空间
class WipeCheckPage extends StatelessWidget {
  const WipeCheckPage({super.key});

  @override
  Widget build(BuildContext context) {
    final s = context.session, l = context.l, d = s.disk;
    final (rows, blockedByPreflight) = preflightRows(context);
    var blocked = blockedByPreflight;
    final need = fmtMib(kWipeNeedMib);
    if (!s.checking && s.probe != null) {
      final r = s.wipeRegion;
      if (d == null) {
        rows.add(CheckRow(state: CheckState.fail, title: l.chkNoSystemDisk));
        blocked = true;
      } else if (r == null) {
        final biggest = s.freeSpans.fold<int>(0, (m, f) => f.sizeMib > m ? f.sizeMib : m);
        rows.add(CheckRow(
          state: CheckState.fail,
          title: l.chkNoRoom(need),
          detail: '${biggest == 0 ? l.chkNoRoomNone : l.chkNoRoomBiggest(fmtMib(biggest))}\n${l.chkNoRoomHow(need)}',
        ));
        blocked = true;
      } else {
        rows.add(CheckRow(state: CheckState.ok, title: l.chkRoom(fmtMib(r.sizeMib)), detail: l.chkWipeRoomDetail(need)));
      }
    }
    return WinStepPage(
      step: WipeStep.check,
      title: l.checkTitle,
      subtitle: l.wipeCheckSub,
      onBack: () => Navigator.pop(context),
      bottomLeft: Btn(l.btnRecheck, onPressed: s.checking ? null : s.check, kind: BtnKind.secondary, icon: Icons.refresh),
      nextLabel: l.btnNext,
      onNext: s.checking || blocked || s.probe == null ? null : () => go(context, const ReleasePage(wipe: true)),
      child: s.checking
          ? Center(
              child: Column(mainAxisSize: MainAxisSize.min, children: [
                const CircularProgressIndicator(),
                const SizedBox(height: 16),
                Text(l.checking, style: context.tt.bodyLarge),
              ]),
            )
          : ListView(children: [
              ...rows,
              if (blocked) ...[const SizedBox(height: 12), NoteBox(text: l.chkBlockedNote, warn: true)],
            ]),
    );
  }
}

/// 第四步：写盘之前说清楚这一步改什么、重启之后会清除什么；按住 2 秒才开始
class WipeConfirmPage extends StatelessWidget {
  const WipeConfirmPage({super.key});

  @override
  Widget build(BuildContext context) {
    final s = context.session, l = context.l, cs = context.cs;
    final v = s.variant;
    final ready = !s.needBitlockerAck || s.bitlockerAck;
    return WinStepPage(
      step: WipeStep.confirm,
      danger: true,
      title: l.wipeConfirmTitle,
      subtitle: l.wipeConfirmSub,
      bottom: Row(children: [
        Btn(l.btnBack, onPressed: () => Navigator.pop(context), kind: BtnKind.text, icon: Icons.arrow_back),
        const SizedBox(width: 24),
        Expanded(
          child: HoldToConfirm(
            idle: l.holdToPrepare,
            busy: (_) => l.holdKeep,
            enabled: ready,
            onConfirmed: () => go(context, const RunPage(task: WriteTask.prepare, releaseDir: kReleaseDir), replace: true),
          ),
        ),
      ]),
      child: ListView(children: [
        Text(l.wipeConfirmNow, style: context.tt.titleMedium),
        const SizedBox(height: 4),
        Bullet(s.source == ReleaseSource.local ? l.confirmLocal(s.localDir) : l.confirmDownload(v!.name, fmtMib(v.sizeMib))),
        Bullet(l.wipeConfirmPart(fmtMib(kWipeNeedMib))),
        Bullet(l.confirmBoot),
        Bullet(l.wipeConfirmOnce),
        const SizedBox(height: 16),
        Text(l.wipeConfirmAfter, style: context.tt.titleMedium),
        const SizedBox(height: 4),
        Bullet(l.wipeConfirmErase, icon: Icons.delete_forever_outlined, color: cs.error),
        Bullet(l.wipeConfirmCancel, icon: Icons.undo, color: context.gk.success),
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
              title: Text(l.confirmBitlockerAck, style: context.tt.bodyLarge!.copyWith(color: cs.onSurface)),
            ),
          ),
        ],
      ]),
    );
  }
}
