import 'package:flutter/material.dart';
import 'package:gk3_common/ui/theme.dart';
import 'package:gk3_common/ui/widgets.dart';

import '../app.dart';
import '../l10n/app_localizations.dart';
import '../model.dart';
import 'bits.dart';
import 'messages.dart';

/// "可用未分配空间"一行右侧的按钮：分析 C 盘最多还能压缩出多少、压缩停在哪个文件（gk3w shrink-info）
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

/// 分析的进度与结果，放在"可用未分配空间"一行下面：够不够、停在哪个文件、这个文件怎么处理。
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
      // 压缩出来的空间与 C 盘之后已有的未分配空间连成一片，按两者之和比较
      final total = fmtMib(info.totalMib), need = fmtMib(s.minNeedMib);
      final file = info.file, kind = info.blocker;
      // 已经有够用的未分配空间（"可用未分配空间"一行上按的）：说还能再压缩出多少，挡住的文件处理后可以更多
      final more = s.hasRoom;
      final head = !more
          ? null
          : info.adjacentMib > 0
              ? l.shrinkMoreAdjacent(total, fmtMib(info.adjacentMib))
              : l.shrinkMore(total);
      final fg = more ? context.cs.onSurface : gk.onWarningContainer;
      if (!more && info.totalMib >= s.minNeedMib) {
        body = NoteBox(text: l.shrinkEnough(total, need));
      } else if (info.limitedBySpace) {
        body = NoteBox(
            warn: !more, text: more ? '$head\n${l.shrinkMoreFull(fmtMib(info.freeMib))}' : l.shrinkFull(total, need, fmtMib(info.freeMib)));
      } else if (file == null || kind == null) {
        body = more ? NoteBox(text: head!) : NoteBox(warn: true, text: l.shrinkNoFile(total, need));
      } else {
        body = NoteBox(
          warn: !more,
          text: more ? '$head\n${l.shrinkMoreLimit}' : l.shrinkNotEnough(total, need),
          child: _blocker(context, '${info.drive}$file', kind, fg),
        );
      }
    } else {
      return const SizedBox.shrink();
    }
    // 与检查项的文字对齐（状态图标 28 + 间距 16）
    return Padding(padding: const EdgeInsets.only(left: 44, top: 4, bottom: 8), child: body);
  }

  /// 挡住压缩的文件：路径、类别与处理方法（MFT 另有"移动 MFT"）。[fg] 跟着外面方框的底色
  Widget _blocker(BuildContext context, String path, ShrinkBlocker kind, Color fg) {
    final l = context.l;
    final (title, how) = blockerText(l, kind);
    return Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
      SelectableText(path, style: context.tt.bodyLarge!.copyWith(color: fg, fontWeight: FontWeight.w600)),
      const SizedBox(height: 12),
      Text(title, style: context.tt.titleSmall!.copyWith(color: fg)),
      const SizedBox(height: 4),
      if (kind == ShrinkBlocker.mft)
        ..._mftBody(context, fg)
      else ...[
        SelectableText(how, style: context.tt.bodyLarge!.copyWith(color: fg)),
        const SizedBox(height: 12),
        Text(l.shrinkNext, style: context.tt.bodyMedium!.copyWith(color: fg)),
      ],
    ]);
  }

  /// 压缩停在 MFT 上：没挪过就说明并提供"移动 MFT"；挪过了，说挪了多少，再按情况说明：
  /// MFT 已经只剩开头必须留在原处的部分（不能再多），还有放不下的部分（腾出前部的空间后可以再挪），
  /// 或者别的原因仍停在它上面（例如挪动期间 MFT 又变大了：可以再挪一次）
  List<Widget> _mftBody(BuildContext context, Color fg) {
    final s = context.session, l = context.l;
    final style = context.tt.bodyLarge!.copyWith(color: fg);
    final done = s.mftResult;
    final again = Btn(l.mftMoveBtn, kind: BtnKind.secondary, icon: Icons.low_priority, onPressed: () => confirmMoveMft(context));
    // 移动失败的提示写在这一块里（分析结果仍然有效，照样显示）
    final failed = s.mftErr == null ? null : Text('${l.mftFailed}${errText(l, s.mftErr!)}', style: style);
    if (done == null) {
      return [if (failed != null) ...[failed, const SizedBox(height: 4)], Text(l.sbMftHow, style: style), const SizedBox(height: 12), again];
    }
    final moved = done.intOf('moved_mib'), unplaced = done.intOf('unplaced_mib');
    final stuck = unplaced == 0 && done.intOf('end_after_mib') <= done.intOf('fixed_end_mib');
    return [
      Text(
          moved > 0
              ? l.mftMoved(fmtMib(moved), fmtMib(done.intOf('end_before_mib')), fmtMib(done.intOf('end_after_mib')))
              : l.mftNothing,
          style: style),
      if (failed != null) ...[const SizedBox(height: 4), failed],
      const SizedBox(height: 4),
      Text(stuck ? l.mftStuck : unplaced > 0 ? l.mftUnplaced(fmtMib(unplaced)) : l.mftAgain, style: style),
      if (!stuck) ...[const SizedBox(height: 12), again],
    ];
  }
}

/// 移动 MFT 之前问一句：会写 C 盘，要几分钟
Future<void> confirmMoveMft(BuildContext context) async {
  final s = context.session, l = context.l;
  final go = await showDialog<bool>(
    context: context,
    builder: (c) => AlertDialog(
      title: Text(l.mftConfirmTitle),
      content: Text(l.mftConfirmBody),
      actions: [
        TextButton(onPressed: () => Navigator.pop(c, false), child: Text(l.mftConfirmCancel)),
        TextButton(onPressed: () => Navigator.pop(c, true), child: Text(l.mftConfirmOk)),
      ],
    ),
  );
  if (go == true) await s.moveMft();
}

/// 挡住压缩的那类文件：名称与处理方法
(String, String) blockerText(L10n l, ShrinkBlocker k) => switch (k) {
      ShrinkBlocker.pagefile => (l.sbPagefile, l.sbPagefileHow),
      ShrinkBlocker.hiberfil => (l.sbHiberfil, l.sbHiberfilHow),
      ShrinkBlocker.restorePoint => (l.sbRestore, l.sbRestoreHow),
      ShrinkBlocker.bitlocker => (l.sbBitlocker, l.sbBitlockerHow),
      ShrinkBlocker.usnJournal => (l.sbUsn, l.sbUsnHow),
      ShrinkBlocker.mft => (l.sbMft, l.sbMftHow),
      ShrinkBlocker.ntfsMeta => (l.sbNtfs, l.sbNtfsHow),
      ShrinkBlocker.memoryDump => (l.sbDump, l.sbDumpHow),
      ShrinkBlocker.other => (l.sbOther, l.sbOtherHow),
    };
