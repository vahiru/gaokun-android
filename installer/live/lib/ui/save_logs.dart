// "保存日志"（v1.0 计划 GUI-12 的前端）：失败页上的一个按钮。
//
// LiveCD 不自动保存日志（只在内存里，见 installer/backend/src/linux/lx.h），装失败时要由用户存到电脑上读得到的地方。
// 后端 gk3l log-targets 列出可移动盘上 Windows 会分配盘符的 FAT / exFAT 分区（另插的 U 盘在前、启动介质本身在后），
// gk3l save-logs --to <分区> 写一个 gaokun3-logs-<时间>/。这里只是让人挑一个、告诉人存到哪了。
import 'package:flutter/material.dart';
import 'package:gk3_common/backend/backend.dart';
import 'package:gk3_common/backend/protocol.dart';
import 'package:gk3_common/ui/widgets.dart';

import '../app.dart';
import '../model/model.dart';
import 'messages.dart';

class SaveLogsButton extends StatelessWidget {
  const SaveLogsButton({super.key});

  @override
  Widget build(BuildContext context) =>
      Btn(context.l.logsSave, kind: BtnKind.secondary, icon: Icons.save_alt, onPressed: () => showDialog<void>(context: context, builder: (_) => const SaveLogsDialog()));
}

class SaveLogsDialog extends StatefulWidget {
  const SaveLogsDialog({super.key});
  @override
  State<SaveLogsDialog> createState() => _SaveLogsDialogState();
}

class _SaveLogsDialogState extends State<SaveLogsDialog> {
  List<Gk3Record>? _targets;
  bool _busy = false;
  String? _result, _error;

  @override
  void initState() {
    super.initState();
    // 不能在 initState 里直接用 context.session（InheritedWidget 还没挂上）
    WidgetsBinding.instance.addPostFrameCallback((_) => _scan());
  }

  Future<void> _scan() async {
    setState(() {
      _targets = null;
      _error = null;
      _result = null;
    });
    final r = await context.session.backend.run('log-targets');
    if (mounted) setState(() => _targets = r.ofType('LOGTARGET').toList());
  }

  Future<void> _save(Gk3Record t) async {
    setState(() => _busy = true);
    final CallResult r = await context.session.backend.run('save-logs', ['--to', t['part']]);
    if (!mounted) return;
    final l = context.l;
    setState(() {
      _busy = false;
      final saved = r.first('LOGSAVED');
      if (r.ok && saved != null) {
        _result = l.logsSaved(_name(t), saved['dir']);
      } else {
        _error = callErrorText(l, r);
      }
    });
  }

  String _name(Gk3Record t) => '${t['label'].isNotEmpty ? t['label'] : t['part']} (${fmtMib(t.intOf('size_mib'))})';

  @override
  Widget build(BuildContext context) {
    final l = context.l, tt = Theme.of(context).textTheme;
    final ts = _targets;
    Widget body;
    if (_result != null) {
      body = Text(_result!, style: tt.bodyLarge);
    } else if (ts == null || _busy) {
      body = const Center(child: Padding(padding: EdgeInsets.all(16), child: CircularProgressIndicator()));
    } else if (ts.isEmpty) {
      body = Text(l.logsNoTarget, style: tt.bodyLarge);
    } else {
      body = Column(mainAxisSize: MainAxisSize.min, crossAxisAlignment: CrossAxisAlignment.stretch, children: [
        Text(l.logsPick, style: tt.bodyLarge),
        const SizedBox(height: 12),
        for (final t in ts) ...[
          ChoiceCard(
            icon: t.yes('removable') && !t.yes('medium') ? Icons.usb : Icons.sd_storage,
            title: _name(t),
            body: t['part'],
            dense: true,
            onTap: () => _save(t),
          ),
          const SizedBox(height: 8),
        ],
        if (_error != null) Text(_error!, style: tt.bodyMedium!.copyWith(color: Theme.of(context).colorScheme.error)),
      ]);
    }
    return AlertDialog(
      title: Text(l.logsTitle),
      content: SizedBox(width: 560, child: SingleChildScrollView(child: body)),
      actions: [
        if (_result == null) TextButton(onPressed: _busy ? null : _scan, child: Text(l.logsRescan)),
        FilledButton(onPressed: _busy ? null : () => Navigator.pop(context), child: Text(_result == null ? MaterialLocalizations.of(context).cancelButtonLabel : l.editDone)),
      ],
    );
  }
}
