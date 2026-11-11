// "保存日志"（v1.0 计划 GUI-12 的前端）：失败页上的一个按钮。
//
// 安装 U 盘只有一个 EFI 类型的分区，Windows / macOS 默认不挂它 —— 装失败的人把 U 盘插回电脑看不到日志。
// 后端 gk3_log_targets 列出能写的 FAT / exFAT（另插的 U 盘在前、启动介质最后并标 esp=yes），
// gk3_save_logs <分区> 写一个 gaokun3-logs-<时间>/（不含 WiFi 配置）。这里只是让人挑一个、告诉人存到哪了。
import 'package:flutter/material.dart';

import '../app.dart';
import '../backend/backend.dart';
import '../backend/protocol.dart';
import '../model/model.dart';
import 'messages.dart';
import 'widgets.dart';

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
    final r = await context.session.backend.run('gk3_log_targets');
    if (mounted) setState(() => _targets = r.ofType('LOGTARGET').toList());
  }

  Future<void> _save(Gk3Record t) async {
    setState(() => _busy = true);
    final CallResult r = await context.session.backend.run('gk3_save_logs', [t['part']]);
    if (!mounted) return;
    final l = context.l;
    setState(() {
      _busy = false;
      final saved = r.first('LOGSAVED');
      if (r.ok && saved != null) {
        _result = [l.logsSaved(_name(t), saved['dir']), if (saved.yes('esp')) l.logsSavedEsp].join('\n');
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
            note: t.yes('esp') ? l.logsEspNote : null,
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
