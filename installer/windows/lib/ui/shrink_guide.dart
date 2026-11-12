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
        kind: BtnKind.secondary, icon: Icons.manage_search, onPressed: s.shrinkChecking ? null : s.analyzeShrink);
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
    if (s.shrinkChecking) {
      body = Row(children: [
        const SizedBox(width: 20, height: 20, child: CircularProgressIndicator(strokeWidth: 2)),
        const SizedBox(width: 12),
        Expanded(child: Text(l.shrinkAnalyzing, style: context.tt.bodyLarge)),
      ]);
    } else if (err != null) {
      body = NoteBox(warn: true, text: '${l.shrinkFailed}${errText(l, err)}');
    } else if (info != null) {
      // 压缩出来的空间与 C 盘之后已有的未分配空间连成一片，按两者之和比较
      final max = fmtMib(info.totalMib), need = fmtMib(s.minNeedMib);
      final file = info.file, kind = info.blocker;
      final fg = gk.onWarningContainer;
      if (info.totalMib >= s.minNeedMib) {
        body = NoteBox(text: l.shrinkEnough(max, need));
      } else if (info.limitedBySpace) {
        body = NoteBox(warn: true, text: l.shrinkFull(max, need, fmtMib(info.freeMib)));
      } else if (file == null || kind == null) {
        body = NoteBox(warn: true, text: l.shrinkNoFile(max, need));
      } else {
        final (title, how) = blockerText(l, kind);
        body = NoteBox(
          warn: true,
          text: l.shrinkNotEnough(max, need),
          child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
            SelectableText('${info.drive}$file',
                style: context.tt.bodyLarge!.copyWith(color: fg, fontWeight: FontWeight.w600)),
            const SizedBox(height: 12),
            Text(title, style: context.tt.titleSmall!.copyWith(color: fg)),
            const SizedBox(height: 4),
            SelectableText(how, style: context.tt.bodyLarge!.copyWith(color: fg)),
            const SizedBox(height: 12),
            Text(l.shrinkNext, style: context.tt.bodyMedium!.copyWith(color: fg)),
          ]),
        );
      }
    } else {
      return const SizedBox.shrink();
    }
    // 与检查项的文字对齐（状态图标 28 + 间距 16）
    return Padding(padding: const EdgeInsets.only(left: 44, top: 4, bottom: 8), child: body);
  }
}

/// 挡住压缩的那类文件：名称与处理方法
(String, String) blockerText(L10n l, ShrinkBlocker k) => switch (k) {
      ShrinkBlocker.pagefile => (l.sbPagefile, l.sbPagefileHow),
      ShrinkBlocker.hiberfil => (l.sbHiberfil, l.sbHiberfilHow),
      ShrinkBlocker.restorePoint => (l.sbRestore, l.sbRestoreHow),
      ShrinkBlocker.bitlocker => (l.sbBitlocker, l.sbBitlockerHow),
      ShrinkBlocker.usnJournal => (l.sbUsn, l.sbUsnHow),
      ShrinkBlocker.ntfsMeta => (l.sbNtfs, l.sbNtfsHow),
      ShrinkBlocker.memoryDump => (l.sbDump, l.sbDumpHow),
      ShrinkBlocker.other => (l.sbOther, l.sbOtherHow),
    };
