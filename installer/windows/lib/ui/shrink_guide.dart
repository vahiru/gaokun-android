import 'package:flutter/material.dart';
import 'package:gk3_common/ui/theme.dart';
import 'package:gk3_common/ui/widgets.dart';

import '../app.dart';
import '../l10n/app_localizations.dart';
import '../model.dart';
import 'bits.dart';
import 'messages.dart';

/// "可用未分配空间"一行右侧的按钮：读取用户本次开机后最近一次在"磁盘管理"里压缩时停在哪个文件上（gk3w shrink-info：
/// 解读 Windows 那次分析记下的 259 号事件，不自己分析）
class ShrinkAnalyzeButton extends StatelessWidget {
  const ShrinkAnalyzeButton({super.key});

  @override
  Widget build(BuildContext context) {
    final s = context.session, l = context.l;
    final done = s.shrinkInfo != null || s.shrinkErr != null;
    return Btn(done ? l.shrinkReanalyze : l.shrinkAnalyze,
        kind: BtnKind.secondary, icon: Icons.manage_search, onPressed: s.shrinkChecking || s.mftMoving ? null : s.analyzeShrink);
  }
}

/// 读取的结果，放在"可用未分配空间"一行下面：停在哪个文件、这个文件怎么处理；可压缩多少由"压缩卷"对话框自己显示。
/// 文件路径与处理步骤可以选中复制（步骤里有要在终端里运行的命令）
class ShrinkGuide extends StatelessWidget {
  const ShrinkGuide({super.key});

  @override
  Widget build(BuildContext context) {
    final s = context.session, l = context.l, gk = context.gk;
    final info = s.shrinkInfo, err = s.shrinkErr;
    final Widget body;
    if (s.mftMoving) {
      body = Row(children: [
        const SizedBox(width: 20, height: 20, child: CircularProgressIndicator(strokeWidth: 2)),
        const SizedBox(width: 12),
        Expanded(child: Text(l.mftMoving, style: context.tt.bodyLarge)),
      ]);
    } else if (s.shrinkChecking) {
      body = Row(children: [
        const SizedBox(width: 20, height: 20, child: CircularProgressIndicator(strokeWidth: 2)),
        const SizedBox(width: 12),
        Expanded(child: Text(l.shrinkAnalyzing, style: context.tt.bodyLarge)),
      ]);
    } else if (err != null) {
      body = NoteBox(warn: true, text: '${l.shrinkFailed}${errText(l, err)}');
    } else if (info != null) {
      final dl = info.letter, file = info.file, kind = info.blocker;
      // 已经有够用的未分配空间（"可用未分配空间"一行上按的）：照样说明，只是不用警示的底色
      final warn = !s.hasRoom;
      final fg = warn ? gk.onWarningContainer : context.cs.onSurface;
      final Widget result;
      if (info.none) {
        result = NoteBox(text: l.shrinkNoEvent);
      } else if (info.limitedBySpace) {
        result = NoteBox(warn: warn, text: l.shrinkFull(dl, info.time));
      } else if (file == null || kind == null) {
        result = NoteBox(warn: warn, text: l.shrinkNoFile(dl, info.time));
      } else {
        result = NoteBox(warn: warn, text: l.shrinkBlocked(dl, info.time), child: _blocker(context, '${info.drive}$file', kind, dl, fg));
      }
      // 又读到了上次那条：先说清楚，免得用户以为处理没有用
      body = s.shrinkSame
          ? Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [NoteBox(warn: true, text: l.shrinkStale), const SizedBox(height: 8), result])
          : result;
    } else {
      return const SizedBox.shrink();
    }
    // 与检查项的文字对齐（状态图标 28 + 间距 16）
    return Padding(padding: const EdgeInsets.only(left: 44, top: 4, bottom: 8), child: body);
  }

  /// 挡住压缩的文件：路径、类别与处理方法（MFT 另有"移动 MFT"）。[fg] 跟着外面方框的底色
  Widget _blocker(BuildContext context, String path, ShrinkBlocker kind, String dl, Color fg) {
    final l = context.l;
    final (title, how) = blockerText(l, kind, dl);
    return Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
      SelectableText(path, style: context.tt.bodyLarge!.copyWith(color: fg, fontWeight: FontWeight.w600)),
      const SizedBox(height: 12),
      Text(title, style: context.tt.titleSmall!.copyWith(color: fg)),
      const SizedBox(height: 4),
      if (kind == ShrinkBlocker.mft)
        ..._mftBody(context, dl, fg)
      else ...[
        SelectableText(how, style: context.tt.bodyLarge!.copyWith(color: fg)),
        const SizedBox(height: 12),
        Text(l.shrinkNext, style: context.tt.bodyMedium!.copyWith(color: fg)),
      ],
    ]);
  }

  /// 压缩停在 MFT 上：没挪过就说明并提供"移动 MFT"；挪过了，说挪了多少，再按情况说明：
  /// MFT 已经只剩开头必须留在原处的部分（不能再多），还有放不下的部分（腾出前部的空间后可以再挪），
  /// 或者挪完了（请用户在"磁盘管理"里重新"压缩卷"、回来再查看：安装器不自己重新分析）
  List<Widget> _mftBody(BuildContext context, String dl, Color fg) {
    final s = context.session, l = context.l;
    final style = context.tt.bodyLarge!.copyWith(color: fg);
    final done = s.mftResult;
    // 校验恢复密钥（并暂停保护）期间不移动：它做完会重新检查
    final again = Btn(l.mftMoveBtn,
        kind: BtnKind.secondary, icon: Icons.low_priority, onPressed: s.bitlockerChecking ? null : () => confirmMoveMft(context));
    // 移动失败的提示写在这一块里（读取的结果仍然有效，照样显示）
    final failed = s.mftErr == null ? null : Text('${l.mftFailed}${errText(l, s.mftErr!)}', style: style);
    if (done == null) {
      return [if (failed != null) ...[failed, const SizedBox(height: 4)], Text(l.sbMftHow(dl), style: style), const SizedBox(height: 12), again];
    }
    final moved = done.intOf('moved_mib'), unplaced = done.intOf('unplaced_mib');
    final movedText = Text(l.mftMoved(fmtMib(moved), fmtMib(done.intOf('end_before_mib')), fmtMib(done.intOf('end_after_mib')), dl), style: style);
    // 中途失败：只说挪了多少与失败原因，不去猜别的原因；可以再试
    if (failed != null) {
      return [if (moved > 0) ...[movedText, const SizedBox(height: 4)], failed, const SizedBox(height: 12), again];
    }
    final stuck = mftStuck(done);
    return [
      moved > 0 ? movedText : Text(l.mftNothing, style: style),
      const SizedBox(height: 4),
      Text(stuck ? l.mftStuck(dl) : unplaced > 0 ? l.mftUnplaced(fmtMib(unplaced), dl) : l.mftDone(dl), style: style),
      if (!stuck && unplaced > 0) ...[const SizedBox(height: 12), again],
    ];
  }
}

/// 移动 MFT 之前问一句：会写那个分区，要几分钟
Future<void> confirmMoveMft(BuildContext context) async {
  final s = context.session, l = context.l;
  final go = await showDialog<bool>(
    context: context,
    builder: (c) => AlertDialog(
      title: Text(l.mftConfirmTitle),
      content: Text(l.mftConfirmBody(s.shrinkInfo?.letter ?? 'C')),
      actions: [
        TextButton(onPressed: () => Navigator.pop(c, false), child: Text(l.mftConfirmCancel)),
        TextButton(onPressed: () => Navigator.pop(c, true), child: Text(l.mftConfirmOk)),
      ],
    ),
  );
  if (go == true) await s.moveMft();
}

/// 挡住压缩的那类文件：名称与处理方法（[dl]：那个分区的盘符）
(String, String) blockerText(L10n l, ShrinkBlocker k, String dl) => switch (k) {
      ShrinkBlocker.pagefile => (l.sbPagefile, l.sbPagefileHow(dl)),
      ShrinkBlocker.hiberfil => (l.sbHiberfil, l.sbHiberfilHow(dl)),
      ShrinkBlocker.restorePoint => (l.sbRestore, l.sbRestoreHow(dl)),
      ShrinkBlocker.bitlocker => (l.sbBitlocker, l.sbBitlockerHow(dl)),
      ShrinkBlocker.usnJournal => (l.sbUsn, l.sbUsnHow(dl)),
      ShrinkBlocker.mft => (l.sbMft, l.sbMftHow(dl)),
      ShrinkBlocker.ntfsMeta => (l.sbNtfs, l.sbNtfsHow(dl)),
      ShrinkBlocker.memoryDump => (l.sbDump, l.sbDumpHow(dl)),
      ShrinkBlocker.other => (l.sbOther, l.sbOtherHow),
    };
