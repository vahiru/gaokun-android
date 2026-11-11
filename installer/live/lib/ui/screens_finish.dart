// 选项 →（分区大小）→ 确认 → 进度 → 完成 / 失败
import 'dart:async';

import 'package:flutter/material.dart';
import 'package:gk3_common/backend/backend.dart';
import 'package:gk3_common/backend/protocol.dart';
import 'package:gk3_common/ui/theme.dart';
import 'package:gk3_common/ui/widgets.dart';

import '../app.dart';
import '../model/model.dart';
import '../session.dart';
import 'messages.dart';
import 'save_logs.dart';
import 'screens_risk.dart';
import 'screens_start.dart';
import 'widgets.dart';

class OptsPage extends StatefulWidget {
  const OptsPage({super.key});
  @override
  State<OptsPage> createState() => _OptsPageState();
}

class _OptsPageState extends State<OptsPage> {
  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) {
      final s = context.session;
      if (!s.rescueUsable) s.rescue = false;
      s.setRescue(s.rescue);
    });
  }

  @override
  Widget build(BuildContext context) {
    final s = context.session, l = context.l, tt = Theme.of(context).textTheme;
    final p = s.plan;
    final re = s.mode == Mode.reinstall;
    final dataLine = p == null || s.planning
        ? '…'
        : !p.ok
            ? planErrorText(context, p)
            : re
                ? (p.keepData ? l.optsDataKept(fmtMib(p.userdataMib)) : l.optsDataWiped(fmtMib(p.userdataMib)))
                : (s.userdataMib == null ? l.optsData(fmtMib(p.userdataMib)) : l.optsDataFixed(fmtMib(p.userdataMib)));
    // 重新安装且清数据：这一行是"要丢东西"的提醒，用 error 色
    final dataBad = p != null && (!p.ok || (re && !p.keepData));
    return StepPage(
      step: Gk3Step.opts,
      title: l.optsTitle,
      subtitle: l.optsSub,
      onBack: () => Navigator.pop(context),
      // 重新安装不改分区表：分区大小没得调
      bottomLeft: re ? null : Btn(l.optsAdvanced, kind: BtnKind.secondary, icon: Icons.tune, onPressed: p?.ok == true ? () => go(context, const AdvPage()) : null),
      // 方案选定 → 写盘前的风险确认（强制阅读）→ 最终确认 → 开始（用户 2026-10-06）
      onNext: p?.ok == true && !s.planning ? () => RiskPage.open(context, RiskKind.install, const ConfirmPage()) : null,
      child: ListView(children: [
        if (re) ...[
          ChoiceCard(
            icon: Icons.folder_open,
            title: l.optsKeepTitle,
            body: l.optsKeepBody,
            trailing: Switch(value: s.keepData, onChanged: s.setKeepData),
            onTap: () => s.setKeepData(!s.keepData),
          ),
          const SizedBox(height: 14),
        ],
        ChoiceCard(
          icon: Icons.health_and_safety,
          title: l.optsRescueTitle,
          body: l.optsRescueBody,
          // 安装器就在目标盘上（免 U 盘安装）：默认关，写明为什么（GUI-17）
          note: s.disk?.medium == true && s.rescueUsable ? l.optsRescueSameDisk : null,
          reason: !s.rescueAvailable ? l.optsRescueMissing : (s.rescueUsable ? null : l.optsRescueNoPart),
          trailing: Switch(value: s.rescue && s.rescueUsable, onChanged: s.rescueUsable ? s.setRescue : null),
          onTap: s.rescueUsable ? () => s.setRescue(!s.rescue) : null,
        ),
        const SizedBox(height: 20),
        Text(dataLine, style: tt.titleMedium!.copyWith(color: dataBad ? context.cs.error : context.cs.onSurface)),
        // 新建分区时提醒 /data 太小（重新安装不改分区大小，提醒了也没法改）
        if (p != null && p.ok && !s.planning && !re) DataSizeWarning(p.userdataMib),
      ]),
    );
  }
}

/// PLANERR → 人话。msg 的取值在 installer-lib.sh 的 gk3_plan 里。
String planErrorText(BuildContext context, Plan p) {
  final l = context.l;
  final e = p.error;
  if (e == null) return l.errPlan('?');
  return switch (e['msg']) {
    'not-enough-space' => l.errNoSpace(fmtMib(e.intOf('avail_mib')), fmtMib(e.intOf('need_mib'))),
    'userdata-too-small' => l.errUserdataSmall(fmtMib(e.intOf('min_mib'))),
    'userdata-too-big' => l.errUserdataBig(fmtMib(e.intOf('max_mib'))),
    'mbr-disk' => l.errMbr,
    'partlabel-conflict' => l.modeWhyInstalled(e['names'].replaceAll(',', ', ')),
    'alongside-needs-existing-esp' || 'reinstall-needs-esp' => l.modeWhyNoEsp,
    'reinstall-missing' => l.errReinstallMissing(e['names'].replaceAll(',', ', ')),
    'reinstall-duplicate' => l.errReinstallDup(e['names'].replaceAll(',', ', ')),
    // 按 KiB 报（本机的 misc 只有 1007 KiB，按 MiB 取整是 0）
    'reinstall-part-small' => l.errReinstallSmall(e['name'], fmtKib(e.intOf('have_kib')), fmtKib(e.intOf('need_kib'))),
    final m => l.errPlan(m),
  };
}

// ── 专业分区（只有 /data 能改；其余由系统决定，改了也装不上）────────────────

class AdvPage extends StatefulWidget {
  const AdvPage({super.key});
  @override
  State<AdvPage> createState() => _AdvPageState();
}

class _AdvPageState extends State<AdvPage> {
  double? _v;
  Timer? _debounce;

  @override
  void dispose() {
    _debounce?.cancel();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final s = context.session, l = context.l, tt = Theme.of(context).textTheme;
    final p = s.plan;
    final max = s.maxUserdataMib ?? p?.userdataMib ?? 0;
    const min = 8192; // 滑块下限的显示值；真正的下限由 gk3_plan 判（PLANERR userdata-too-small）
    final cur = _v ?? (p?.userdataMib ?? max).toDouble();
    return StepPage(
      step: Gk3Step.opts,
      title: l.advTitle,
      subtitle: l.advSub,
      onBack: () => Navigator.pop(context),
      bottomLeft: Btn(l.advReset, kind: BtnKind.secondary, icon: Icons.restart_alt, onPressed: () {
        setState(() => _v = null);
        s.setUserdata(null);
      }),
      onNext: p?.ok == true ? () => Navigator.pop(context) : null,
      nextLabel: l.btnNext,
      child: Row(crossAxisAlignment: CrossAxisAlignment.start, children: [
        Expanded(
          child: ListView(children: [
            for (final part in p?.parts ?? <PlanPart>[])
              Padding(
                padding: const EdgeInsets.symmetric(vertical: 6),
                child: Row(children: [
                  SizedBox(width: 180, child: Text(part.name == 'userdata' ? l.advUserdata : part.name, style: tt.bodyLarge)),
                  Text(fmtMib(part.sizeMib), style: tt.bodyLarge!.copyWith(color: part.name == 'userdata' ? context.cs.primary : context.cs.onSurfaceVariant)),
                  if (part.name != 'userdata') ...[const SizedBox(width: 12), Flexible(child: Text(l.advFixed, style: tt.bodySmall, overflow: TextOverflow.ellipsis))],
                ]),
              ),
          ]),
        ),
        const SizedBox(width: 32),
        SizedBox(
          width: 460,
          child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
            Text(l.advUserdata, style: tt.titleMedium),
            Text(fmtMib(cur.round()), style: tt.headlineMedium),
            if (max > min)
              Slider(
                value: cur.clamp(min.toDouble(), max.toDouble()),
                min: min.toDouble(),
                max: max.toDouble(),
                onChanged: (v) {
                  setState(() => _v = (v / 1024).round() * 1024.0);
                  _debounce?.cancel();
                  _debounce = Timer(const Duration(milliseconds: 300), () => s.setUserdata(_v!.round() >= max ? null : _v!.round()));
                },
              ),
            if (p != null && p.ok)
              // ★ "已分配 / 共"读 PLANSUM 的 fixed_mib 与 avail_mib —— C 版这里是
              //   plan_userdata_mib + 13776 的硬编码（roadmap 欠账第 4 条）
              Text(l.advTotal(fmtMib(p.fixedMib + p.userdataMib), fmtMib(p.availMib)), style: tt.bodyLarge),
            if (p != null && !p.ok) Text(planErrorText(context, p), style: tt.bodyLarge!.copyWith(color: context.cs.error)),
            DataSizeWarning(cur.round()),
          ]),
        ),
      ]),
    );
  }
}

// ── 确认 ─────────────────────────────────────────────────────────────────────

class ConfirmPage extends StatelessWidget {
  const ConfirmPage({super.key});

  @override
  Widget build(BuildContext context) {
    final s = context.session, l = context.l, tt = Theme.of(context).textTheme;
    final d = s.disk!, p = s.plan!;
    final wipe = s.mode == Mode.wipe;
    final re = s.mode == Mode.reinstall;
    final a = s.along;
    // 将被删除的东西要全列出来；9 行以内不折叠（第一版折叠到 6 行，藏掉的正好是 WinRE）
    const show = 9;
    return StepPage(
      step: Gk3Step.confirm,
      title: l.confirmTitle,
      subtitle: l.confirmSub,
      bottom: Row(children: [
        Btn(l.btnBack, kind: BtnKind.text, icon: Icons.arrow_back, onPressed: () => Navigator.pop(context)),
        const SizedBox(width: 24),
        Expanded(
          child: HoldToConfirm(
            idle: l.confirmHoldIdle,
            busy: (x) => l.confirmHoldBusy('$x'),
            // U16：BitLocker + 要换 BOOTAA64 时，不勾"已拿到恢复密钥"就开始不了
            enabled: !s.needBitlockerKey || s.bitlockerKeyOk,
            onConfirmed: () => go(context, const RunPage(), replace: true),
          ),
        ),
      ]),
      child: Row(crossAxisAlignment: CrossAxisAlignment.start, children: [
        Expanded(
          child: ListView(children: [
            Text('${d.model.isNotEmpty ? d.model : (d.external ? l.diskExternal : l.diskInternal)} · ${d.path} · ${fmtMib(d.sizeMib)}', style: tt.titleMedium),
            const SizedBox(height: 10),
            diskBar(d, highlightFree: a is AlongOk && !wipe ? a.region : null),
            const SizedBox(height: 18),
            if (wipe) ...[
              Text(l.confirmWipeHead, style: tt.titleMedium!.copyWith(color: context.cs.error)),
              const SizedBox(height: 8),
              if (d.parts.isEmpty) Text(l.confirmNoParts, style: tt.bodyMedium),
              // ★ 必须把将被销毁的东西逐条列出来（stage7-live-installer.md §3 的界面流程）
              for (final part in d.parts.take(show))
                Padding(
                  padding: const EdgeInsets.symmetric(vertical: 3),
                  child: Row(children: [
                    Container(width: 12, height: 12, decoration: BoxDecoration(color: DiskBar.colorOf(context, part.os), shape: BoxShape.circle)),
                    const SizedBox(width: 10),
                    SizedBox(width: 90, child: Text(part.path.split('/').last, style: tt.bodyMedium)),
                    Expanded(
                      child: Text(
                        '${part.label}${part.fs.isNotEmpty ? '  (${part.fs})' : ''}${isOnekey(part) ? '  ·  ${l.confirmOnekey}' : ''}',
                        style: tt.bodyLarge!.copyWith(color: isOnekey(part) ? context.cs.error : null),
                      ),
                    ),
                    Text(fmtMib(part.sizeMib), style: tt.bodyMedium),
                  ]),
                ),
              if (d.parts.length > show) Text(l.confirmMore('${d.parts.length - show}'), style: tt.bodyMedium),
              // v1.0 计划 GUI-18：整盘清空连华为的一键恢复分区一起删 —— 写明后果
              if (d.parts.where(isOnekey).firstOrNull case final ok?) ...[
                const SizedBox(height: 10),
                Text(l.confirmOnekeyWarn(ok.label), style: tt.bodyMedium!.copyWith(color: context.cs.error)),
              ],
            ] else if (re) ...[
              // 重新安装：逐个列出要重写的分区与做什么 —— 格式化的那几个（数据会没）用 error 色
              Text(l.confirmReinstallHead, style: tt.titleMedium!.copyWith(color: p.keepData ? context.cs.onSurface : context.cs.error)),
              const SizedBox(height: 8),
              for (final r in p.reuse)
                Padding(
                  padding: const EdgeInsets.symmetric(vertical: 3),
                  child: Row(children: [
                    SizedBox(width: 90, child: Text(r.path.split('/').last, style: tt.bodyMedium)),
                    Expanded(child: Text(r.name, style: tt.bodyLarge)),
                    Text(
                      switch (r.action) { 'format' => l.actFormat, 'keep' => l.actKeep, _ => l.actWrite },
                      style: tt.bodyLarge!.copyWith(color: r.action == 'format' ? context.cs.error : context.cs.onSurfaceVariant),
                    ),
                    SizedBox(width: 90, child: Text(fmtKib(r.sizeKib), style: tt.bodyMedium, textAlign: TextAlign.end)),
                  ]),
                ),
            ] else ...[
              Text(l.confirmAlongHead, style: tt.titleMedium!.copyWith(color: context.gk.success)),
              const SizedBox(height: 8),
              Text(l.confirmAlongBody(fmtMib(p.totalNewMib), a is AlongOk ? a.esp.path : '?'), style: tt.bodyLarge),
              if (a is AlongOk && a.windows) ...[const SizedBox(height: 6), Text(l.confirmWindowsKept, style: tt.bodyLarge)],
            ],
            if (s.dual) ...[const SizedBox(height: 22), const DualOptions()],
          ]),
        ),
        const SizedBox(width: 36),
        SizedBox(
          width: 380,
          child: Container(
            padding: const EdgeInsets.all(20),
            decoration: BoxDecoration(color: context.cs.surfaceContainerHigh, borderRadius: BorderRadius.circular(16)),
            child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
              Text(re ? l.confirmReinstallRight : l.confirmNewLayout, style: tt.titleMedium),
              const SizedBox(height: 8),
              for (final part in p.parts)
                Padding(
                  padding: const EdgeInsets.symmetric(vertical: 2),
                  child: Row(children: [Expanded(child: Text(part.name, style: tt.bodyLarge)), Text(fmtMib(part.sizeMib), style: tt.bodyMedium)]),
                ),
              const Divider(height: 24),
              Text(l.confirmRescue(s.rescue && s.rescueUsable ? l.wordInstall : l.wordNoInstall), style: tt.bodyLarge),
              if (s.source == Source.net && s.variant != null) Text(s.variant!.nameFor(s.language), style: tt.bodyLarge),
            ]),
          ),
        ),
      ]),
    );
  }
}

/// 双系统的两件事（见 tools/gk3boot/README.md）：开机默认进哪个系统、BitLocker 恢复密钥。
/// 默认系统用大字写清楚"冷开机进这里选的、之后在哪里改"（预选 Android，但以 Windows 为主的人要看得见）
class DualOptions extends StatelessWidget {
  const DualOptions({super.key});

  @override
  Widget build(BuildContext context) {
    final s = context.session, l = context.l, tt = Theme.of(context).textTheme;
    final win = s.defaultOs == 'windows';
    return Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
      Text(l.confirmDefaultTitle, style: tt.titleLarge),
      const SizedBox(height: 6),
      Text(l.confirmDefaultLead, style: tt.titleMedium),
      const SizedBox(height: 12),
      Row(crossAxisAlignment: CrossAxisAlignment.start, children: [
        Expanded(
          child: ChoiceCard(
            icon: Icons.android,
            title: l.confirmDefaultAndroid,
            body: l.confirmDefaultAndroidBody,
            selected: !win,
            onTap: () => s.setDefaultOs('android'),
          ),
        ),
        const SizedBox(width: 12),
        Expanded(
          child: ChoiceCard(
            icon: Icons.desktop_windows,
            title: l.confirmDefaultWindows,
            body: l.confirmDefaultWindowsBody,
            selected: win,
            onTap: () => s.setDefaultOs('windows'),
          ),
        ),
      ]),
      if (win) ...[const SizedBox(height: 8), Text(l.confirmDefaultWindowsWhen, style: tt.bodyLarge)],
      const SizedBox(height: 8),
      Text(l.confirmMenu5, style: tt.bodyMedium),
      if (s.needBitlockerKey) ...[
        const SizedBox(height: 18),
        Text(l.confirmBitlockerHead, style: tt.titleMedium!.copyWith(color: context.cs.error)),
        const SizedBox(height: 6),
        Text(s.viaWindowsScript ? l.confirmBitlockerScript : l.confirmBitlockerUsb, style: tt.bodyLarge),
        CheckboxListTile(
          contentPadding: EdgeInsets.zero,
          controlAffinity: ListTileControlAffinity.leading,
          value: s.bitlockerKeyOk,
          onChanged: (v) => s.setBitlockerKeyOk(v ?? false),
          title: Text(l.confirmBitlockerCheck, style: tt.bodyLarge),
        ),
      ],
    ]);
  }
}

// ── 进度 ─────────────────────────────────────────────────────────────────────

class RunPage extends StatefulWidget {
  const RunPage({super.key, this.resume});

  /// 不是新开一次安装，而是接着跟一个还在跑的写盘任务（gk3_job_status 的 JOB 记录，GUI-11）
  final Gk3Record? resume;
  @override
  State<RunPage> createState() => _RunPageState();
}

class _RunPageState extends State<RunPage> {
  final _log = <String>[];
  final _scroll = ScrollController();
  int _pct = 0;
  Gk3Progress? _last;
  Gk3Record? _err, _planErr;
  bool _showLog = false;
  StreamSubscription<Gk3Event>? _sub;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) => _start());
  }

  void _start() {
    final s = context.session, job = widget.resume;
    _sub = (job == null ? s.install() : s.follow(job['id'])).listen((e) {
      if (!mounted) return;
      setState(() {
        switch (e) {
          case Gk3Progress():
            _pct = e.percent;
            _last = e;
            _log.add('[${e.percent}%] ${e.text}');
          case Gk3Log():
            _log.add(e.line);
          case Gk3Record():
            if (e.type == 'ERR') _err = e;
            if (e.type == 'PLANERR') _planErr = e;
            _log.add('${e.type} ${e.fields}');
          case Gk3Exit():
            _finish(e.code);
        }
      });
      if (_showLog && _scroll.hasClients) {
        WidgetsBinding.instance.addPostFrameCallback((_) {
          if (_scroll.hasClients) _scroll.jumpTo(_scroll.position.maxScrollExtent);
        });
      }
    });
  }

  void _finish(int code) {
    final s = context.session;
    final job = widget.resume;
    if (code == 0) {
      // 接着跟的是缩分区 / 调整磁盘：做完了就从头走一遍（盘变了，前面的判断都要重来）
      go(context, job == null || job['fn'] == 'gk3_apply' ? const DonePage() : const WelcomePage(), replace: true);
      return;
    }
    // 盘没动过的失败（下载阶段、取消、或者 gk3_apply 在第一次写盘之前的检查里停下 —— ERR touched=no）：
    // 可以返回、可以重试。写盘阶段的失败才是"盘可能写了一半"（v1.0 计划 GUI-3）
    final untouched = job == null && (s.stage == InstallStage.download || _err?['touched'] == 'no');
    go(
      context,
      FailPage(
        log: List.of(_log),
        code: code,
        err: _err,
        planErr: _planErr,
        untouched: untouched,
        download: s.stage == InstallStage.download,
        cancelled: s.cancelled,
      ),
      replace: true,
    );
  }

  /// 取消下载（GUI-5）：先问一句；问的这会儿要是已经下完、进了写盘，就什么都不做
  Future<void> _cancel() async {
    final l = context.l;
    final ok = await showDialog<bool>(
      context: context,
      builder: (c) => AlertDialog(
        title: Text(l.runCancelTitle),
        content: SizedBox(width: 520, child: Text(l.runCancelBody)),
        actions: [
          TextButton(onPressed: () => Navigator.pop(c, false), child: Text(l.runCancelKeep)),
          FilledButton(onPressed: () => Navigator.pop(c, true), child: Text(l.runCancel)),
        ],
      ),
    );
    if (ok == true && mounted) context.session.cancelDownload();
  }

  @override
  void dispose() {
    _sub?.cancel();
    _scroll.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final s = context.session, l = context.l, tt = Theme.of(context).textTheme;
    final last = _last;
    final step = last == null ? l.stepPrep : progressText(l, last);
    final rate = last == null ? null : downloadRate(l, last);
    final downloading = s.stage == InstallStage.download;
    return PopScope(
      canPop: false, // 装到一半不许返回
      child: StepPage(
        step: Gk3Step.install,
        title: widget.resume == null ? l.runTitle : l.runResumeTitle,
        subtitle: widget.resume == null ? l.runSub : l.runResumeSub,
        bottom: Row(children: [
          Btn(_showLog ? l.runHideLog : l.runShowLog, kind: BtnKind.secondary, icon: Icons.subject, onPressed: () => setState(() => _showLog = !_showLog)),
          const Spacer(),
          // 只在下载阶段有：盘还没动，停下来没有代价（写盘阶段停下就是半个盘 —— 不给这个按钮）
          if (downloading) Btn(l.runCancel, kind: BtnKind.text, icon: Icons.close, onPressed: _cancel),
        ]),
        child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
          const SizedBox(height: 20),
          Row(children: [
            Expanded(child: Text(step, style: tt.titleMedium)),
            Text('$_pct%', style: tt.headlineMedium),
          ]),
          const SizedBox(height: 16),
          LinearProgressIndicator(value: _pct / 100, minHeight: 8),
          if (rate != null) ...[
            const SizedBox(height: 10),
            Text(rate, style: tt.bodyLarge!.copyWith(color: context.cs.onSurfaceVariant)),
          ],
          const SizedBox(height: 24),
          if (_showLog) Expanded(child: LogView(_log, controller: _scroll)),
        ]),
      ),
    );
  }
}

class DonePage extends StatelessWidget {
  const DonePage({super.key});
  @override
  Widget build(BuildContext context) {
    final s = context.session, l = context.l, tt = Theme.of(context).textTheme;
    // 免 U 盘安装（安装器跑在内置盘上）：没有要拔的介质（v1.0 计划 GUI-17）
    final internalMedium = s.disks?.any((d) => d.medium && !d.external) ?? false;
    return PopScope(
      canPop: false,
      child: StepPage(
        step: Gk3Step.install,
        title: l.doneTitle,
        bottom: Row(children: [const Spacer(), Btn(l.btnReboot, icon: Icons.restart_alt, autofocus: true, onPressed: s.backend.reboot)]),
        child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
          Container(
            width: 96,
            height: 96,
            decoration: BoxDecoration(color: context.gk.successContainer, shape: BoxShape.circle),
            child: Icon(Icons.check, size: 56, color: context.gk.onSuccessContainer),
          ),
          const SizedBox(height: 24),
          Text(internalMedium ? l.doneBodyInternal : l.doneBody, style: tt.bodyLarge),
          // 双系统：开机默认进哪个、什么时候生效、在哪里改（设计稿 §4.7"完成页按所选的默认系统说明"）
          if (s.dual) ...[const SizedBox(height: 16), Text(s.defaultOs == 'windows' ? l.doneDualWindows : l.doneDualAndroid, style: tt.bodyLarge)],
          // 后端的提醒：EFI 变量 LoaderEntryDefault 删不掉 —— 开机仍会先进它指的那一项
          for (final n in s.notes.where((n) => n['code'] == 'loadervar-stuck')) ...[
            const SizedBox(height: 16),
            Text(l.doneNoteLoaderVar(n['value']), style: tt.bodyLarge!.copyWith(color: context.cs.error)),
          ],
          if (s.rescue && s.rescueUsable) ...[const SizedBox(height: 16), Text(l.doneRescue, style: tt.bodyLarge)],
        ]),
      ),
    );
  }
}

/// 失败页分两种（v1.0 计划 GUI-3）：
///   - 盘没动过（[untouched]：下载阶段失败 / 取消，或者 gk3_apply 在第一次写盘之前的检查里停下、ERR touched=no）：
///     给"重试"（/run 里下好的部分留着，gk3_net_fetch 接着续传）和"返回修改"（换网络 / 换版本 / 换选项）。
///   - 写盘阶段失败：盘可能停在中间状态 —— 不许返回、不给重试，让用户先看日志（分区表备份的还原命令在里面）。
/// 两种都能从侧栏重启 / 关机。
/// ★ 标题下面那句错误说明按 ERR 的代码查 l10n（ui/messages.dart）；后端的中文原话只在日志里（INST-10）。
///   日志是后端原样的输出、部分是中文 —— 英文界面里默认收起，点了才看（bug 报告要它，所以不能不给）。
class FailPage extends StatefulWidget {
  const FailPage({
    super.key,
    required this.log,
    required this.code,
    this.err,
    this.planErr,
    this.untouched = false,
    this.download = false,
    this.cancelled = false,
  });
  final List<String> log;
  final int code;
  final Gk3Record? err, planErr;
  final bool untouched, download, cancelled;

  @override
  State<FailPage> createState() => _FailPageState();
}

class _FailPageState extends State<FailPage> {
  bool? _showLog;

  @override
  Widget build(BuildContext context) {
    final l = context.l, tt = Theme.of(context).textTheme;
    final w = widget;
    final String msg;
    if (w.cancelled) {
      msg = l.runCancelBody;
    } else if (w.err != null) {
      msg = errText(l, w.err!);
    } else if (w.planErr != null) {
      msg = planErrorText(context, Plan(CallResult([w.planErr!], const [], w.code)));
    } else {
      final raw = w.log.lastWhere((x) => x.startsWith('!! '), orElse: () => '');
      msg = raw.isNotEmpty && l.localeName.startsWith('zh') ? raw.substring(3) : l.errNoCode('${w.code}');
    }
    final backedUp = !w.untouched && w.log.any((x) => x.contains('sgdisk --load-backup='));
    final title = w.cancelled ? l.failCancelTitle : (w.download ? l.failDlTitle : (w.untouched ? l.failUntouchedTitle : l.failTitle));
    final sub = w.download || w.cancelled ? l.failDlSub : (w.untouched ? l.failUntouchedSub : l.failSub);
    final showLog = _showLog ?? l.localeName.startsWith('zh');
    return PopScope(
      canPop: w.untouched,
      child: StepPage(
        step: Gk3Step.install,
        title: title,
        subtitle: sub,
        bottom: w.untouched
            ? Row(children: [
                // 回到选项页：跳过风险确认页（它在栈里、正好在失败页下面）—— "返回修改"是去改方案的
                Btn(l.failBackEdit, kind: BtnKind.text, icon: Icons.arrow_back, onPressed: () {
                  final self = ModalRoute.of(context);
                  Navigator.popUntil(context, (r) => r != self && r.settings.name != kRiskRoute);
                }),
                const SizedBox(width: 12),
                Btn(l.shellOpen, kind: BtnKind.secondary, icon: Icons.terminal, onPressed: () => openShell(context)),
                const SizedBox(width: 12),
                const SaveLogsButton(),
                const Spacer(),
                Btn(l.btnRetry, icon: Icons.refresh, autofocus: true, onPressed: () => go(context, const RunPage(), replace: true)),
              ])
            : Row(children: [
                Btn(l.shellOpen, kind: BtnKind.secondary, icon: Icons.terminal, onPressed: () => openShell(context)),
                const SizedBox(width: 12),
                const SaveLogsButton(),
                const SizedBox(width: 16),
                Expanded(child: Text(l.shellHint, style: tt.bodySmall)),
              ]),
        child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
          Text(msg, style: tt.titleMedium!.copyWith(color: w.cancelled ? context.cs.onSurface : context.cs.error)),
          if (backedUp) ...[const SizedBox(height: 8), Text(l.failBackup, style: tt.bodyMedium)],
          const SizedBox(height: 14),
          if (showLog) ...[
            Text(l.failLogTitle, style: tt.labelLarge),
            const SizedBox(height: 6),
            Expanded(child: LogView(w.log)),
          ] else
            Align(
              alignment: Alignment.centerLeft,
              child: Btn(l.failLogShow, kind: BtnKind.text, icon: Icons.subject, onPressed: () => setState(() => _showLog = true)),
            ),
        ]),
      ),
    );
  }
}

/// 华为的一键恢复分区（出厂镜像）。认它的卷标 Onekey —— 出厂布局里 p6 的 LABEL（见 docs/dev/hardware.md）。
/// 同一套恢复机制是否还用到 WINPE（p5，1 GiB FAT）尚未核实，所以只标这一个
bool isOnekey(Part p) => p.fslabel.toLowerCase() == 'onekey';
