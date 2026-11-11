// 手动调整磁盘（用户 2026-09-25："能给的都给"）：删除 / 新建 / 格式化 / 缩小 / 扩大。
//
// ★ 参考别的安装器：入口学 Ubuntu（"安装类型"最后一项进手动分区），执行学 Windows 安装程序
//   （每个操作单独确认、立即生效）—— 攒到最后一起做，中途失败时的中间状态讲不清楚。
// ★ 界面只负责"让人选、让人确认"。能不能做、做到多大，都由后端判（installer-lib.sh 的 gk3_part_*）：
//   ESP 不动、挂着的不动、越界的不做，后端会自己再验一遍；界面上的禁用只是先把原因说出来。
import 'package:flutter/material.dart';

import '../app.dart';
import '../backend/protocol.dart';
import '../model/model.dart';
import 'messages.dart';
import 'theme.dart';
import 'widgets.dart';

enum _Op { resize, format, delete, create }

class DiskEditPage extends StatefulWidget {
  const DiskEditPage({super.key});
  @override
  State<DiskEditPage> createState() => _DiskEditPageState();
}

class _DiskEditPageState extends State<DiskEditPage> {
  /// 选中的是哪一行：按起始扇区记（每做完一步重新探测，对象都是新的，分区号也可能变）
  int? _selStart;
  _Op? _op;
  String _fs = 'ext4';
  double _size = 0;
  bool _busy = false;
  int _pct = 0;
  String? _msg, _err;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) => context.session.scanShrink());
  }

  /// 分区与空闲区，按盘上的顺序
  List<Object> _rows(Disk d) => [...d.parts, ...d.free]..sort((a, b) => _start(a).compareTo(_start(b)));
  int _start(Object o) => o is Part ? o.start : (o as FreeRegion).start;

  void _pick(Object o) => setState(() {
        _selStart = _start(o);
        _op = null;
        _msg = null;
        _err = null;
      });

  /// 调整大小的范围：下限 = 文件系统自己报的最小值 + gk3_shrink 的余量（缩不了就是现在的大小）；
  /// 上限 = 现在的大小 + 紧挨在后面的那段空闲（只能并紧挨着的，见 gk3__grow）
  (int, int) _range(Disk d, Part p) {
    final sh = context.session.shrinkables?.where((x) => x.part == p.path).firstOrNull;
    final min = sh != null && sh.can ? sh.floorMib : p.sizeMib;
    final after = d.free.where((f) => f.start == p.end + 1).firstOrNull;
    return (min.clamp(0, p.sizeMib), p.sizeMib + (after?.sizeMib ?? 0));
  }

  bool _resizable(Part p) => p.fs == 'ntfs' || p.fs.startsWith('ext');

  void _choose(_Op op, {int? size}) => setState(() {
        _op = op;
        _msg = null;
        _err = null;
        if (size != null) _size = size.toDouble();
        if (op == _Op.format || op == _Op.create) _fs = 'ext4';
      });

  Future<void> _run(String fn, List<String> args, String what) async {
    final s = context.session;
    setState(() {
      _busy = true;
      _pct = 0;
      _msg = null;
      _err = null;
    });
    final r = await s.editDisk(fn, args, (e) {
      if (e is Gk3Progress && mounted) setState(() => _pct = e.percent);
    });
    if (!mounted) return;
    setState(() {
      _busy = false;
      _op = null;
      if (r.ok) {
        _msg = context.l.editOk(what);
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
    final sel = rows.where((o) => _start(o) == _selStart).firstOrNull;
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
            DiskBar(disk: d),
            const SizedBox(height: 16),
            Expanded(
              child: ListView.separated(
                itemCount: rows.length,
                separatorBuilder: (_, _) => const SizedBox(height: 6),
                itemBuilder: (_, i) {
                  final o = rows[i];
                  final selected = _start(o) == _selStart;
                  if (o is Part) {
                    return ChoiceCard(
                      title: '${o.label}  ·  ${o.path.split('/').last}',
                      body: [if (o.fs.isNotEmpty) o.fs.toUpperCase(), fmtKib(o.sizeKib)].join(' · '),
                      trailing: Container(width: 14, height: 14, decoration: BoxDecoration(color: DiskBar.colorOf(context, o.os), shape: BoxShape.circle)),
                      selected: selected,
                      dense: true,
                      onTap: _busy ? null : () => _pick(o),
                    );
                  }
                  final f = o as FreeRegion;
                  return ChoiceCard(
                    title: l.editFree,
                    body: fmtMib(f.sizeMib),
                    trailing: Icon(Icons.add_circle_outline, color: cs.primary),
                    selected: selected,
                    dense: true,
                    onTap: _busy ? null : () => _pick(f),
                  );
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
                if (sel is Part) ..._partPanel(d, sel),
                if (sel is FreeRegion) ..._freePanel(d, sel),
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

  Widget _fsPicker({required bool allowNone}) => SegmentedButton<String>(
        segments: [
          const ButtonSegment(value: 'ext4', label: Text('ext4')),
          const ButtonSegment(value: 'vfat', label: Text('FAT32')),
          const ButtonSegment(value: 'ntfs', label: Text('NTFS')),
          if (allowNone) ButtonSegment(value: 'none', label: Text(context.l.editFsNone)),
        ],
        selected: {_fs},
        showSelectedIcon: false,
        onSelectionChanged: _busy ? null : (v) => setState(() => _fs = v.first),
      );

  Widget _slider(int min, int max) => Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
        Text(context.l.editNewSize, style: context.tt.titleSmall),
        Text(fmtMib(_size.round()), style: context.tt.headlineSmall),
        Slider(
          value: _size.clamp(min.toDouble(), max.toDouble()),
          min: min.toDouble(),
          max: max.toDouble(),
          // 按 1 GiB 走，两头保留精确值（最小、最大都能取到）
          onChanged: _busy ? null : (v) => setState(() => _size = v <= min + 512 ? min.toDouble() : (v >= max - 512 ? max.toDouble() : (v / 1024).round() * 1024.0)),
        ),
      ]);

  List<Widget> _partPanel(Disk d, Part p) {
    final l = context.l, tt = context.tt;
    final head = [
      Text(p.label, style: tt.titleLarge),
      Text([p.path, if (p.fs.isNotEmpty) p.fs.toUpperCase(), fmtKib(p.sizeKib)].join(' · '), style: tt.bodyMedium),
      const SizedBox(height: 16),
    ];
    // 这两种不给任何操作：直接说为什么（后端也会拒绝，这里只是先说出来）
    if (p.isEsp) return [...head, _why(l.editWhyEsp)];
    if (p.medium) return [...head, _why(l.editWhyMedium)];
    final (min, max) = _range(d, p);
    final canResize = _resizable(p) && max > min;
    return [
      ...head,
      Wrap(spacing: 8, runSpacing: 8, children: [
        Btn(l.editResize, kind: _op == _Op.resize ? BtnKind.primary : BtnKind.tonal, icon: Icons.open_in_full,
            onPressed: _busy || !canResize ? null : () => _choose(_Op.resize, size: p.sizeMib)),
        Btn(l.editFormat, kind: _op == _Op.format ? BtnKind.primary : BtnKind.tonal, icon: Icons.cleaning_services_outlined,
            onPressed: _busy ? null : () => _choose(_Op.format)),
        Btn(l.editDelete, kind: _op == _Op.delete ? BtnKind.danger : BtnKind.secondary, icon: Icons.delete_outline,
            onPressed: _busy ? null : () => _choose(_Op.delete)),
      ]),
      if (!_resizable(p)) ...[const SizedBox(height: 10), _why(l.editWhyNoResizeFs)],
      if (_resizable(p) && max == p.sizeMib && min < max) ...[const SizedBox(height: 10), _why(l.editOnlyShrink)],
      const SizedBox(height: 16),
      if (_op == _Op.resize) ...[
        _slider(min, max),
        Text(l.editResizeWarn, style: tt.bodyMedium!.copyWith(color: context.gk.warning)),
        const SizedBox(height: 12),
        HoldToConfirm(
          idle: '${l.editResize} · ${l.holdIdle}',
          busy: (x) => l.confirmHoldBusy('$x'),
          enabled: !_busy && _size.round() != p.sizeMib,
          onConfirmed: () => _run('gk3_part_resize', [p.path, '${_size.round()}'], '${l.editResize} ${p.label} → ${fmtMib(_size.round())}'),
        ),
      ],
      if (_op == _Op.format) ...[
        Text(l.editFsLabel, style: tt.titleSmall),
        const SizedBox(height: 8),
        _fsPicker(allowNone: false),
        const SizedBox(height: 12),
        Text(l.editFormatWarn, style: tt.bodyMedium!.copyWith(color: context.cs.error)),
        const SizedBox(height: 12),
        HoldToConfirm(
          idle: '${l.editFormat} · ${l.holdIdle}',
          busy: (x) => l.confirmHoldBusy('$x'),
          enabled: !_busy,
          onConfirmed: () => _run('gk3_part_format', [p.path, _fs], '${l.editFormat} ${p.label}'),
        ),
      ],
      if (_op == _Op.delete) ...[
        Text(l.editDeleteWarn, style: tt.bodyMedium!.copyWith(color: context.cs.error)),
        const SizedBox(height: 12),
        HoldToConfirm(
          idle: '${l.editDelete} · ${l.holdIdle}',
          busy: (x) => l.confirmHoldBusy('$x'),
          enabled: !_busy,
          onConfirmed: () => _run('gk3_part_delete', [p.path], '${l.editDelete} ${p.label}'),
        ),
      ],
    ];
  }

  List<Widget> _freePanel(Disk d, FreeRegion f) {
    final l = context.l, tt = context.tt;
    const min = 16; // 与 gk3__emit_free 报空闲区的下限一致：更小的碎片不值得建分区
    // ⚠️ 后端把起点向上对齐到 1 MiB（gk3_part_create）：空闲区起点没对齐时（比如 GPT 表之后那段从第 34 扇区起），
    //    按整段大小传过去会越界、被后端拒绝 —— 最大值按对齐后的起点算
    final aligned = (f.start + 2047) ~/ 2048 * 2048;
    final max = (f.end - aligned + 1) ~/ 2048;
    return [
      Text(l.editFree, style: tt.titleLarge),
      Text(fmtMib(f.sizeMib), style: tt.bodyMedium),
      const SizedBox(height: 16),
      Btn(l.editCreate, kind: _op == _Op.create ? BtnKind.primary : BtnKind.tonal, icon: Icons.add,
          onPressed: _busy || max < min ? null : () => _choose(_Op.create, size: max)),
      const SizedBox(height: 16),
      if (_op == _Op.create) ...[
        _slider(min, max),
        Text(l.editFsLabel, style: tt.titleSmall),
        const SizedBox(height: 8),
        _fsPicker(allowNone: true),
        const SizedBox(height: 12),
        HoldToConfirm(
          idle: '${l.editCreate} · ${l.holdIdle}',
          busy: (x) => l.confirmHoldBusy('$x'),
          enabled: !_busy,
          onConfirmed: () => _run(
            'gk3_part_create',
            ['--disk', d.path, '--start', '${f.start}', '--size-mib', '${_size.round()}', '--fs', _fs],
            '${l.editCreate} ${fmtMib(_size.round())}',
          ),
        ),
      ],
    ];
  }
}
