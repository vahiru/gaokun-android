// 删除分区：安装方式页最后一项。用来腾出空间，例如删掉 Windows 恢复分区、华为一键恢复分区（"磁盘管理"删不了它们）。
//
// ★ 只做删除。缩小分区要回 Windows 用"压缩卷"，新建、格式化分区不是安装 Android 需要的操作。
// ★ 每次删除单独确认（按住 2 秒）、立即执行，与 Windows 安装程序的分区页一样 —— 攒到最后一起做，
//   中途失败时留下的中间状态讲不清楚。
// ★ 界面只负责"让人选、让人确认"。能不能删由后端判（gk3l delete，installer/backend/src/linux/edit.c）：
//   ESP 不动、挂着的不动，后端会自己再验一遍；界面上不给按钮只是先把原因说出来。
import 'package:flutter/material.dart';
import 'package:gk3_common/backend/protocol.dart';
import 'package:gk3_common/ui/theme.dart';
import 'package:gk3_common/ui/widgets.dart';

import '../app.dart';
import '../model/model.dart';
import 'messages.dart';
import 'screens_finish.dart' show isOnekey;
import 'widgets.dart';

class DiskEditPage extends StatefulWidget {
  const DiskEditPage({super.key});
  @override
  State<DiskEditPage> createState() => _DiskEditPageState();
}

class _DiskEditPageState extends State<DiskEditPage> {
  /// 选中的分区：按起始扇区记（每删一次都重新探测，对象都是新的，分区号也可能变）
  int? _selStart;

  /// 点过"删除"、正在等按住确认
  bool _confirming = false;
  bool _busy = false;
  int _pct = 0;
  String? _msg, _err;

  /// 分区与空闲区，按盘上的顺序
  List<Object> _rows(Disk d) => [...d.parts, ...d.free]..sort((a, b) => _start(a).compareTo(_start(b)));
  int _start(Object o) => o is Part ? o.start : (o as FreeRegion).start;

  void _pick(Part p) => setState(() {
        _selStart = p.start;
        _confirming = false;
        _msg = null;
        _err = null;
      });

  Future<void> _delete(Part p) async {
    final s = context.session;
    setState(() {
      _busy = true;
      _pct = 0;
      _msg = null;
      _err = null;
    });
    final r = await s.deletePartition(p.path, (e) {
      if (e is Gk3Progress && mounted) setState(() => _pct = e.percent);
    });
    if (!mounted) return;
    setState(() {
      _busy = false;
      _confirming = false;
      if (r.ok) {
        _msg = context.l.editOk(p.label);
        _selStart = null; // 分区号、边界都可能变了：让人重新选
      } else {
        _err = context.l.editFailed(callErrorText(context.l, r));
      }
    });
  }

  @override
  Widget build(BuildContext context) {
    final s = context.session, l = context.l, cs = context.cs, tt = context.tt;
    final d = s.disk!;
    final rows = _rows(d);
    final sel = d.parts.where((p) => p.start == _selStart).firstOrNull;
    return StepPage(
      step: Gk3Step.mode,
      title: l.editTitle,
      subtitle: l.editSub,
      onBack: _busy ? null : () => Navigator.pop(context),
      nextLabel: l.editDone,
      onNext: _busy ? null : () => Navigator.pop(context),
      child: Row(crossAxisAlignment: CrossAxisAlignment.start, children: [
        Expanded(
          child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
            Text('${d.path} · ${fmtMib(d.sizeMib)}', style: tt.titleMedium),
            const SizedBox(height: 10),
            diskBar(d),
            const SizedBox(height: 16),
            Expanded(
              child: ListView.separated(
                itemCount: rows.length,
                separatorBuilder: (_, _) => const SizedBox(height: 6),
                itemBuilder: (_, i) {
                  final o = rows[i];
                  if (o is Part) {
                    return ChoiceCard(
                      title: '${o.label}  ·  ${o.path.split('/').last}',
                      body: [if (o.fs.isNotEmpty) o.fs.toUpperCase(), fmtKib(o.sizeKib)].join(' · '),
                      trailing: Container(width: 14, height: 14, decoration: BoxDecoration(color: DiskBar.colorOf(context, o.os), shape: BoxShape.circle)),
                      selected: o.start == _selStart,
                      dense: true,
                      onTap: _busy ? null : () => _pick(o),
                    );
                  }
                  // 未分配空间只是显示出来（删掉一个分区后能看到空间并到了哪里），不能选
                  final f = o as FreeRegion;
                  return ChoiceCard(title: l.editFree, body: fmtMib(f.sizeMib), selected: false, dense: true, onTap: null);
                },
              ),
            ),
          ]),
        ),
        const SizedBox(width: 24),
        SizedBox(
          width: 380,
          child: Container(
            padding: const EdgeInsets.all(20),
            decoration: BoxDecoration(color: cs.surfaceContainerHigh, borderRadius: BorderRadius.circular(16)),
            child: SingleChildScrollView(
              child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
                if (sel == null) Text(l.editPick, style: tt.bodyLarge),
                if (sel != null) ..._partPanel(sel),
                if (_busy) ...[const SizedBox(height: 16), LinearProgressIndicator(value: _pct / 100, minHeight: 8)],
                if (_msg != null) ...[const SizedBox(height: 16), Text(_msg!, style: tt.bodyLarge!.copyWith(color: context.gk.success))],
                if (_err != null) ...[const SizedBox(height: 16), Text(_err!, style: tt.bodyMedium!.copyWith(color: cs.error))],
              ]),
            ),
          ),
        ),
      ]),
    );
  }

  Widget _why(String t) => Row(crossAxisAlignment: CrossAxisAlignment.start, children: [
        Icon(Icons.info_outline, size: 20, color: context.cs.onSurfaceVariant),
        const SizedBox(width: 8),
        Expanded(child: Text(t, style: context.tt.bodyMedium)),
      ]);

  List<Widget> _partPanel(Part p) {
    final l = context.l, tt = context.tt, cs = context.cs;
    final head = [
      Text(p.label, style: tt.titleLarge),
      Text([p.path, if (p.fs.isNotEmpty) p.fs.toUpperCase(), fmtKib(p.sizeKib)].join(' · '), style: tt.bodyMedium),
      const SizedBox(height: 16),
    ];
    // 这两种不给按钮：直接说为什么（后端也会拒绝，这里只是先说出来）
    if (p.isEsp) return [...head, _why(l.editWhyEsp)];
    if (p.medium) return [...head, _why(l.editWhyMedium)];
    // 删掉之后少了什么：Windows 恢复环境、华为一键恢复
    final consequence = [
      if (p.os == 'winre') l.editDeleteWinre,
      if (isOnekey(p)) l.editDeleteOnekey,
    ];
    return [
      ...head,
      Align(
        alignment: Alignment.centerLeft,
        child: Btn(l.editDelete, kind: _confirming ? BtnKind.danger : BtnKind.secondary, icon: Icons.delete_outline,
            onPressed: _busy ? null : () => setState(() => _confirming = true)),
      ),
      if (_confirming) ...[
        const SizedBox(height: 16),
        for (final t in [l.editDeleteWarn, ...consequence]) ...[
          Text(t, style: tt.bodyMedium!.copyWith(color: cs.error)),
          const SizedBox(height: 8),
        ],
        const SizedBox(height: 4),
        HoldToConfirm(
          idle: '${l.editDelete} · ${l.holdIdle}',
          busy: (x) => l.confirmHoldBusy('$x'),
          enabled: !_busy,
          onConfirmed: () => _delete(p),
        ),
      ],
    ];
  }
}
