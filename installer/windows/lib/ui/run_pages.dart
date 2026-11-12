import 'package:flutter/material.dart';
import 'package:gk3_common/ui/theme.dart';
import 'package:gk3_common/ui/widgets.dart';

import '../app.dart';
import '../l10n/app_localizations.dart';
import '../model.dart';
import '../session.dart';
import 'bits.dart';
import 'messages.dart';

/// 侧栏标在哪一步：双系统安装与卸载在"安装"，制作安装介质在"写入"
Enum _railStep(WriteTask t) => t == WriteTask.media ? MediaStep.write : WinStep.install;

/// 正在写盘：进度条、当前在做什么、日志。跑完换成完成页或失败页（不能"返回"到确认页）
class RunPage extends StatefulWidget {
  const RunPage({super.key, required this.task, this.releaseDir = ''});
  final WriteTask task;
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
      final ok = switch (widget.task) {
        WriteTask.install => await s.install(releaseDir: widget.releaseDir),
        WriteTask.uninstall => await s.uninstall(),
        WriteTask.media => await s.writeMedia(releaseDir: widget.releaseDir),
      };
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
    final s = context.session, l = context.l, task = widget.task;
    // 卸载不报百分比（gk3w uninstall），进度条不定长
    final pct = task != WriteTask.uninstall;
    final p = s.lastProgress;
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (_scroll.hasClients) _scroll.jumpTo(_scroll.position.maxScrollExtent);
    });
    return PopScope(
      canPop: false,
      child: WinStepPage(
        step: _railStep(task),
        title: runTitle(l, task),
        subtitle: l.runSub,
        bottom: const SizedBox(height: 40),
        child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
          Text(p == null ? l.runStarting : progressText(l, p), style: context.tt.titleMedium),
          const SizedBox(height: 12),
          ClipRRect(
            borderRadius: BorderRadius.circular(8),
            child: LinearProgressIndicator(minHeight: 12, value: pct ? s.percent / 100 : null),
          ),
          if (pct) ...[
            const SizedBox(height: 6),
            Text('${s.percent}%', style: context.tt.bodySmall),
          ],
          const SizedBox(height: 20),
          NoteBox(text: switch (task) {
            WriteTask.install => l.runSleepHintInstall,
            WriteTask.uninstall => l.runSleepHintUninstall,
            WriteTask.media => (s.mediaWritten?.usb ?? false) ? '${l.runSleepHintMedia}${l.runMediaUsbHint}' : l.runSleepHintMedia,
          }),
          const SizedBox(height: 16),
          Expanded(child: LogView(s.log, controller: _scroll)),
        ]),
      ),
    );
  }
}

/// 装好了 / 卸载好了 / 安装介质做好了
class DonePage extends StatelessWidget {
  const DonePage({super.key, required this.task});
  final WriteTask task;

  @override
  Widget build(BuildContext context) {
    final s = context.session, l = context.l;
    // U 盘做好了不必重启：只有"返回首页"
    final usb = task == WriteTask.media && (s.mediaWritten?.usb ?? false);
    return WinStepPage(
      step: _railStep(task),
      title: switch (task) {
        WriteTask.install => l.doneInstallTitle,
        WriteTask.uninstall => l.doneUninstallTitle,
        WriteTask.media => l.doneMediaTitle,
      },
      subtitle: switch (task) {
        WriteTask.install => l.doneInstallSub,
        WriteTask.uninstall => l.doneUninstallSub(fmtMib(s.freedMib)),
        WriteTask.media => usb ? l.doneMediaSubUsb : l.doneMediaSubInternal,
      },
      bottom: Row(children: [
        if (!usb) Btn(l.btnLater, onPressed: () => restart(context), kind: BtnKind.text),
        const Spacer(),
        usb
            ? Btn(l.btnStartOver, icon: Icons.home_outlined, onPressed: () => restart(context))
            : Btn(l.btnRebootNow, icon: Icons.restart_alt, onPressed: () => s.backend.reboot()),
      ]),
      child: ListView(children: switch (task) {
        WriteTask.install => [
            Bullet(l.doneInstallMenu, icon: Icons.menu_open),
            Bullet(l.doneInstallFirstBoot, icon: Icons.hourglass_bottom),
            Bullet(l.doneInstallWindows, icon: Icons.window),
          ],
        WriteTask.uninstall => [
            Bullet(l.doneUninstallExtend, icon: Icons.open_in_full),
            Bullet(l.doneUninstallBoot, icon: Icons.window),
          ],
        WriteTask.media => [
            Bullet(l.doneMediaInstall, icon: Icons.verified_user_outlined),
            if (usb) Bullet(l.doneMediaSecureBoot, icon: Icons.security)
            else Bullet(l.doneMediaStays, icon: Icons.menu_open),
          ],
      }),
    );
  }
}

/// 没做完：说清楚原因、盘有没有被动过、接下来怎么办；日志留在页面上
class FailPage extends StatelessWidget {
  const FailPage({super.key, required this.task});
  final WriteTask task;

  @override
  Widget build(BuildContext context) {
    final s = context.session, l = context.l;
    final err = s.result?.err;
    final why = err == null ? (s.result?.error ?? l.errUnknown) : errText(l, err);
    return WinStepPage(
      step: _railStep(task),
      title: switch (task) {
        WriteTask.install => l.failInstallTitle,
        WriteTask.uninstall => l.failUninstallTitle,
        WriteTask.media => l.failMediaTitle,
      },
      subtitle: why,
      bottom: Row(children: [
        const Spacer(),
        Btn(l.btnStartOver, icon: Icons.refresh, onPressed: () => restart(context)),
      ]),
      child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
        NoteBox(
          warn: s.touched != Touched.no,
          text: switch (s.touched) {
            Touched.no => l.failUntouched,
            // gk3w 没报原因就退出了（被结束、崩溃）：不知道动没动过盘，让用户重新检查看实际状态
            Touched.unknown => l.failUnknownTouched,
            Touched.yes => switch (task) {
                WriteTask.install => l.failTouchedInstall,
                WriteTask.uninstall => l.failTouchedUninstall,
                WriteTask.media => (s.mediaWritten?.usb ?? false) ? l.failTouchedMediaUsb : l.failTouchedMediaInternal,
              },
          },
        ),
        const SizedBox(height: 16),
        Expanded(child: LogView(s.log)),
      ]),
    );
  }
}

/// 进度页的标题（原生窗口阻止睡眠时的说明也用它）
String runTitle(L10n l, WriteTask t) => switch (t) {
      WriteTask.install => l.runInstallTitle,
      WriteTask.uninstall => l.runUninstallTitle,
      WriteTask.media => l.runMediaTitle,
    };
