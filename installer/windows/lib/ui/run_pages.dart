import 'package:flutter/material.dart';
import 'package:gk3_common/ui/theme.dart';
import 'package:gk3_common/ui/widgets.dart';

import '../app.dart';
import '../model.dart';
import 'bits.dart';
import 'messages.dart';

enum RunTask { install, uninstall }

/// 正在写盘：进度条、当前在做什么、日志。跑完换成完成页或失败页（不能"返回"到确认页）
class RunPage extends StatefulWidget {
  const RunPage({super.key, required this.task, this.releaseDir = ''});
  final RunTask task;
  final String releaseDir;
  @override
  State<RunPage> createState() => _RunPageState();
}

class _RunPageState extends State<RunPage> {
  final _scroll = ScrollController();

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) async {
      final s = context.session;
      final ok = widget.task == RunTask.install ? await s.install(releaseDir: widget.releaseDir) : await s.uninstall();
      if (!mounted) return;
      go(context, ok ? DonePage(task: widget.task) : FailPage(task: widget.task), replace: true);
    });
  }

  @override
  void dispose() {
    _scroll.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final s = context.session, l = context.l;
    final install = widget.task == RunTask.install;
    final p = s.lastProgress;
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (_scroll.hasClients) _scroll.jumpTo(_scroll.position.maxScrollExtent);
    });
    return PopScope(
      canPop: false,
      child: WinStepPage(
        step: WinStep.install,
        title: install ? l.runInstallTitle : l.runUninstallTitle,
        subtitle: l.runSub,
        bottom: const SizedBox(height: 40),
        child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
          Text(p == null ? l.runStarting : progressText(l, p), style: context.tt.titleMedium),
          const SizedBox(height: 12),
          ClipRRect(
            borderRadius: BorderRadius.circular(8),
            child: LinearProgressIndicator(minHeight: 12, value: install ? s.percent / 100 : null),
          ),
          if (install) ...[
            const SizedBox(height: 6),
            Text('${s.percent}%', style: context.tt.bodySmall),
          ],
          const SizedBox(height: 20),
          NoteBox(text: install ? l.runSleepHintInstall : l.runSleepHintUninstall),
          const SizedBox(height: 16),
          Expanded(child: LogView(s.log, controller: _scroll)),
        ]),
      ),
    );
  }
}

/// 装好了 / 卸载好了
class DonePage extends StatelessWidget {
  const DonePage({super.key, required this.task});
  final RunTask task;

  @override
  Widget build(BuildContext context) {
    final s = context.session, l = context.l;
    final install = task == RunTask.install;
    return WinStepPage(
      step: WinStep.install,
      title: install ? l.doneInstallTitle : l.doneUninstallTitle,
      subtitle: install ? l.doneInstallSub : l.doneUninstallSub(fmtMib(s.freedMib)),
      bottom: Row(children: [
        Btn(l.btnLater, onPressed: () => restart(context), kind: BtnKind.text),
        const Spacer(),
        Btn(l.btnRebootNow, icon: Icons.restart_alt, onPressed: () => s.backend.reboot()),
      ]),
      child: ListView(children: [
        if (install) ...[
          Bullet(l.doneInstallMenu, icon: Icons.menu_open),
          Bullet(l.doneInstallFirstBoot, icon: Icons.hourglass_bottom),
          Bullet(l.doneInstallWindows, icon: Icons.window),
        ] else ...[
          Bullet(l.doneUninstallExtend, icon: Icons.open_in_full),
          Bullet(l.doneUninstallBoot, icon: Icons.window),
        ],
      ]),
    );
  }
}

/// 没做完：说清楚原因、盘有没有被动过、接下来怎么办；日志留在页面上
class FailPage extends StatelessWidget {
  const FailPage({super.key, required this.task});
  final RunTask task;

  @override
  Widget build(BuildContext context) {
    final s = context.session, l = context.l;
    final err = s.result?.err;
    final why = err == null ? (s.result?.error ?? l.errUnknown) : errText(l, err);
    return WinStepPage(
      step: WinStep.install,
      title: task == RunTask.install ? l.failInstallTitle : l.failUninstallTitle,
      subtitle: why,
      bottom: Row(children: [
        const Spacer(),
        Btn(l.btnStartOver, icon: Icons.refresh, onPressed: () => restart(context)),
      ]),
      child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
        NoteBox(
          warn: s.touched,
          text: s.touched ? (task == RunTask.install ? l.failTouchedInstall : l.failTouchedUninstall) : l.failUntouched,
        ),
        const SizedBox(height: 16),
        Expanded(child: LogView(s.log)),
      ]),
    );
  }
}
