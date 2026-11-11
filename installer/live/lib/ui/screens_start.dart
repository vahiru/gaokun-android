// 欢迎 / 预检 → 选盘 → 方式 → （缩分区）
import 'dart:math' as math;

import 'package:flutter/material.dart';

import '../app.dart';
import '../backend/protocol.dart';
import '../model/model.dart';
import '../session.dart';
import 'screens_edit.dart';
import 'screens_finish.dart';
import 'screens_risk.dart';
import 'messages.dart';
import 'screens_source.dart';
import 'theme.dart';
import 'widgets.dart';

class WelcomePage extends StatefulWidget {
  const WelcomePage({super.key});
  @override
  State<WelcomePage> createState() => _WelcomePageState();
}

class _WelcomePageState extends State<WelcomePage> {
  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) async {
      final s = context.session;
      if (s.checks != null) return;
      await s.start();
      // 写盘任务还在跑（界面刚才崩过又起来了）：直接接上去看它（GUI-11）
      final job = s.runningJob;
      if (job != null && mounted) go(context, RunPage(resume: job));
    });
  }

  String _checkName(String id) {
    final l = context.l;
    return switch (id) {
      'model' => l.checkModel,
      'bios' => l.checkBios,
      'secureboot' => l.checkSecureboot,
      'uefi' => l.checkUefi,
      'root' => l.checkRoot,
      'tools' => l.checkTools,
      'power' => l.checkPower,
      _ => id,
    };
  }

  String? _failText(Check c) {
    final l = context.l;
    return switch (c.id) {
      'model' => l.checkModelBad(c.value),
      'secureboot' => l.checkSecurebootBad,
      'uefi' => l.checkUefiBad,
      'root' => l.checkRootBad,
      'tools' => c.pkgs.isEmpty ? l.checkToolsBad(c.missing.replaceAll(',', ', ')) : l.checkToolsBadPkgs(c.missing.replaceAll(',', ', '), c.pkgs.replaceAll(',', ' ')),
      'power' => l.checkPowerBad(c.min),
      _ => null,
    };
  }

  @override
  Widget build(BuildContext context) {
    final s = context.session, l = context.l, tt = Theme.of(context).textTheme;
    final checks = s.checks;
    return StepPage(
      step: Gk3Step.welcome,
      title: l.welcomeTitle,
      subtitle: l.welcomeSub,
      bottom: Row(children: [
        Btn(l.btnQuit, kind: BtnKind.secondary, icon: Icons.terminal, onPressed: () => openShell(context)),
        const SizedBox(width: 16),
        Btn(l.btnLanguage, kind: BtnKind.secondary, icon: Icons.translate, onPressed: s.toggleLanguage),
        const Spacer(),
        Btn(l.btnStart, autofocus: true, onPressed: checks == null || s.blocked ? null : () => go(context, const DiskPage())),
      ]),
      child: Row(crossAxisAlignment: CrossAxisAlignment.start, children: [
        Expanded(
          flex: 5,
          child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
            Text(l.welcomeBody, style: tt.bodyLarge),
            const SizedBox(height: 24),
            Container(
              padding: const EdgeInsets.all(20),
              decoration: BoxDecoration(color: context.cs.surfaceContainerLow, borderRadius: BorderRadius.circular(16)),
              child: Text(l.welcomeNote, style: tt.bodyMedium),
            ),
          ]),
        ),
        const SizedBox(width: 40),
        Expanded(
          flex: 4,
          child: Container(
            padding: const EdgeInsets.all(22),
            decoration: BoxDecoration(color: context.cs.surfaceContainerLow, borderRadius: BorderRadius.circular(20), border: Border.all(color: s.blocked ? context.cs.error : context.cs.outlineVariant)),
            child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
              Text(l.checkTitle, style: tt.titleMedium),
              const SizedBox(height: 14),
              if (checks == null)
                Row(children: [const SizedBox.square(dimension: 22, child: CircularProgressIndicator(strokeWidth: 3)), const SizedBox(width: 12), Text(l.checkRunning, style: tt.bodyMedium)])
              else ...[
                for (final c in checks)
                  _CheckRow(
                    name: _checkName(c.id),
                    check: c,
                    // 后端的取值是给机器看的（disabled/enabled），界面上说人话
                    shown: switch (c.id) {
                      'secureboot' => switch (c.value) { 'disabled' => l.checkSecurebootOff, 'enabled' => l.checkSecurebootOn, _ => c.value },
                      'power' => c.value.isEmpty ? '' : (c.ac ? l.checkPowerShownAc(c.value) : l.checkPowerShown(c.value)),
                      _ => c.value,
                    },
                    failText: _failText(c),
                    unknownText: l.checkUnknown,
                  ),
                if (s.blocked) ...[const SizedBox(height: 10), Text(l.checkBlocked, style: tt.bodyLarge!.copyWith(color: context.cs.error))],
              ],
            ]),
          ),
        ),
      ]),
    );
  }
}

class _CheckRow extends StatelessWidget {
  const _CheckRow({required this.name, required this.check, required this.shown, this.failText, required this.unknownText});
  final String name, unknownText, shown;
  final Check check;
  final String? failText;

  @override
  Widget build(BuildContext context) {
    final tt = Theme.of(context).textTheme;
    final (icon, color) = switch (check.state) {
      CheckState.ok => (Icons.check_circle, context.gk.success),
      CheckState.fail => (Icons.cancel, context.cs.error),
      CheckState.unknown => (Icons.help, context.gk.warning),
    };
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 5),
      child: Row(crossAxisAlignment: CrossAxisAlignment.start, children: [
        Icon(icon, color: color, size: 22),
        const SizedBox(width: 10),
        Expanded(
          child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
            Row(children: [
              Flexible(child: Text(name, style: tt.bodyLarge, overflow: TextOverflow.ellipsis)),
              if (shown.isNotEmpty) ...[const SizedBox(width: 10), Flexible(child: Text(shown, style: tt.bodyMedium, overflow: TextOverflow.ellipsis))],
            ]),
            if (check.state == CheckState.fail && failText != null) Text(failText!, style: tt.bodyMedium!.copyWith(color: context.cs.error)),
            if (check.state == CheckState.unknown) Text(unknownText, style: tt.bodySmall),
          ]),
        ),
      ]),
    );
  }
}

// ── 选盘 ─────────────────────────────────────────────────────────────────────

class DiskPage extends StatefulWidget {
  const DiskPage({super.key});
  @override
  State<DiskPage> createState() => _DiskPageState();
}

class _DiskPageState extends State<DiskPage> {
  Disk? _picked;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) => context.session.probe());
  }

  Future<void> _next() async {
    final s = context.session;
    await s.assess(_picked!);
    if (mounted) go(context, const ModePage());
  }

  @override
  Widget build(BuildContext context) {
    final s = context.session, l = context.l, tt = Theme.of(context).textTheme;
    final disks = s.disks;
    Widget body;
    if (disks == null) {
      body = const Center(child: CircularProgressIndicator());
    } else if (disks.isEmpty) {
      body = Text(l.diskNone, style: tt.bodyLarge);
    } else {
      // 安装 U 盘排最后、禁用但写明原因（C 版是直接不显示 —— 用户会以为盘没识别）。
      // ★ 安装器跑在【内置盘】上时（免 U 盘安装）那块盘照样能选：只是不能整盘清空（选方式那一页拦），
      //   双系统照常 —— 那正是免 U 盘装双系统的目标流程（用户 2026-09-25）。
      bool blocked(Disk d) => (d.medium && d.external) || d.unreadable;
      final sorted = [...disks]..sort((a, b) => (blocked(a) ? 1 : 0) - (blocked(b) ? 1 : 0));
      body = ListView.separated(
        itemCount: sorted.length,
        separatorBuilder: (_, _) => const SizedBox(height: 14),
        itemBuilder: (_, i) {
          final d = sorted[i];
          final name = d.model.isNotEmpty ? d.model : (d.external ? l.diskExternal : l.diskInternal);
          final free = d.largestFree;
          return ChoiceCard(
            icon: d.external ? Icons.usb : Icons.storage,
            title: '$name · ${fmtMib(d.sizeMib)}',
            body: [
              d.path,
              // 读不出分区表的盘没有"几个分区"可说（说"没有分区"就是 S1 原来的错）
              if (!d.unreadable) d.parts.isEmpty ? l.diskNoParts : l.diskParts('${d.parts.length}'),
              if (free != null) l.diskFree(fmtMib(free.sizeMib)),
            ].join('   ·   '),
            reason: d.unreadable ? l.diskUnreadable : (blocked(d) ? l.diskMedium : null),
            note: d.medium && !d.external ? l.diskMediumInternal : null,
            selected: identical(_picked, d),
            onTap: () => setState(() => _picked = d),
            extra: DiskBar(disk: d),
          );
        },
      );
    }
    return StepPage(
      step: Gk3Step.disk,
      title: l.diskTitle,
      subtitle: l.diskSub,
      onBack: () => Navigator.pop(context),
      onNext: _picked == null || s.assessing ? null : _next,
      child: body,
    );
  }
}

// ── 方式 ─────────────────────────────────────────────────────────────────────

class ModePage extends StatefulWidget {
  const ModePage({super.key});
  @override
  State<ModePage> createState() => _ModePageState();
}

enum _Pick { wipe, along, shrink, reinstall, edit }

class _ModePageState extends State<ModePage> {
  _Pick? _pick;

  String? _alongReason(Along? a) {
    final l = context.l;
    return switch (a) {
      null || AlongOk() => null,
      AlongNoEsp() => l.modeWhyNoEsp,
      AlongEspSmall(:final freeMib, :final needMib) => l.modeWhyEspSmall('$freeMib', '$needMib'),
      AlongEspTooSmall(:final sizeMib) => l.modeWhyEspTooSmall('$sizeMib'),
      AlongHibernated() => l.modeWhyHibernated,
      AlongInstalled(:final names) => names.isEmpty ? l.modeWhyEspOurs : l.modeWhyInstalled(names.replaceAll(',', ', ')),
      AlongNoRoom(:final haveMib, :final needMib) => haveMib == 0
          ? l.modeWhyNoFree(needMib == null ? '?' : fmtMib(needMib))
          : l.modeWhyNoRoom(needMib == null ? '?' : fmtMib(needMib), fmtMib(haveMib)),
      AlongMbr() => l.errMbr,
      AlongError(:final msg) => l.errPlan(msg),
    };
  }

  Future<void> _next() async {
    final s = context.session;
    switch (_pick!) {
      case _Pick.wipe:
        s.setMode(Mode.wipe);
        go(context, const SourcePage());
      case _Pick.along:
        s.setMode(Mode.alongside);
        go(context, const SourcePage());
      case _Pick.reinstall:
        s.setMode(Mode.reinstall);
        go(context, const SourcePage());
      // 缩分区与手动调整同样先过风险确认页（RiskPage 等那一页 pop 了才跟着 pop，所以这里 await 的仍是"做完了没有"）
      case _Pick.edit:
        await RiskPage.open(context, RiskKind.edit, const DiskEditPage());
        // 改过的盘要重新评估：几种方式能不能走，都可能变了
        if (mounted && s.disk != null) {
          setState(() => _pick = null);
          await s.assess(s.disk!);
        }
      case _Pick.shrink:
        await RiskPage.open(context, RiskKind.shrink, const ShrinkPage());
        // 缩完回来：assess 已经重跑过，双系统多半可行了 —— 替用户选上
        if (mounted && context.session.along is AlongOk) setState(() => _pick = _Pick.along);
    }
  }

  @override
  Widget build(BuildContext context) {
    final s = context.session, l = context.l;
    final a = s.along;
    final reason = _alongReason(a);
    if (s.assessing || s.disk == null) {
      return StepPage(
      step: Gk3Step.mode,title: l.modeTitle, subtitle: l.modeSub, child: const Center(child: CircularProgressIndicator()));
    }
    return StepPage(
      step: Gk3Step.mode,
      title: l.modeTitle,
      subtitle: l.modeSub,
      onBack: () => Navigator.pop(context),
      onNext: _pick == null ? null : _next,
      // 重新安装默认也会清掉 /data —— 和整盘清空一样用 error 色的"下一步"
      nextDanger: _pick == _Pick.wipe || _pick == _Pick.reinstall,
      child: ListView(children: [
        // ★ 盘上已经有我们的 Android 时，另外两项多半都走不通（整盘清空会锯掉从这块盘跑的安装器、
        //   双系统会建出第二套同名分区）—— 所以重新安装排最前面
        if (s.reinstall case final r?) ...[
          ChoiceCard(
            icon: Icons.system_update_alt,
            title: l.modeReinstallTitle,
            warn: l.modeReinstallWarn,
            body: l.modeReinstallBody,
            // U18：Windows 在休眠时重新安装同样要写共用的 EFI 分区 —— 一样拦住
            reason: !r.ok ? planErrorText(context, r) : (s.espInfo?.winHibernated ?? false) ? l.modeWhyHibernated : null,
            selected: _pick == _Pick.reinstall,
            onTap: () => setState(() => _pick = _Pick.reinstall),
          ),
          const SizedBox(height: 14),
        ],
        ChoiceCard(
          icon: Icons.delete_forever,
          danger: true,
          title: l.modeWipeTitle,
          warn: l.modeWipeWarn,
          body: l.modeWipeBody,
          reason: s.disk!.medium ? l.modeWhyMedium : null,
          selected: _pick == _Pick.wipe,
          onTap: () => setState(() => _pick = _Pick.wipe),
        ),
        const SizedBox(height: 14),
        ChoiceCard(
          icon: Icons.call_split,
          title: l.modeAlongTitle,
          good: reason == null ? l.modeAlongOk : null,
          body: a is AlongOk ? l.modeAlongInfo(fmtMib(a.region.sizeMib), a.esp.path) : null,
          reason: reason,
          selected: _pick == _Pick.along,
          onTap: () => setState(() => _pick = _Pick.along),
          extra: a is AlongOk ? DiskBar(disk: s.disk!, highlightFree: a.region) : null,
        ),
        if (a is AlongNoRoom && s.canShrink) ...[
          const SizedBox(height: 14),
          ChoiceCard(
            icon: Icons.compress,
            title: l.modeShrinkTitle,
            body: l.modeShrinkBody,
            selected: _pick == _Pick.shrink,
            onTap: () => setState(() => _pick = _Pick.shrink),
          ),
        ],
        // ★ 最后一项：手动调整（Ubuntu 的"安装类型"也是把手动分区放在最后）。总是可选 ——
        //   上面几项都走不通的时候，这是用户自己能做点什么的地方
        const SizedBox(height: 14),
        ChoiceCard(
          icon: Icons.construction,
          title: l.editEntryTitle,
          body: l.editEntryBody,
          // MBR 盘上删 / 建 / 改大小都要写 GPT 分区表，后端一律拒绝（ERR mbr-disk）—— 这里先说
          reason: s.disk!.mbr ? l.errMbr : null,
          selected: _pick == _Pick.edit,
          onTap: () => setState(() => _pick = _Pick.edit),
        ),
      ]),
    );
  }
}

// ── 缩分区 ───────────────────────────────────────────────────────────────────

class ShrinkPage extends StatefulWidget {
  const ShrinkPage({super.key});
  @override
  State<ShrinkPage> createState() => _ShrinkPageState();
}

class _ShrinkPageState extends State<ShrinkPage> {
  Shrinkable? _pick;
  double _target = 0;
  bool _running = false;
  final _log = <String>[];
  int _pct = 0;
  String? _error;

  /// 默认让 /data 有多大（v1.0 计划 GUI-20）。与 Windows 脚本 D: 加密时的默认（"回车 = 64"）同一个口径
  static const _dataDefaultMib = 64 * 1024;

  /// 双系统除 /data 之外的固定开销（gk3_plan 在 PLANERR fixed_mib= 里报的；旧后端没有就退回 need_mib，只会多腾）。
  /// 兜底的 20 GiB 只决定滑块的【初始位置】与 /data 的估计，不参与任何判定 —— 判定在 gk3_shrink 与 gk3_plan 里。
  int get _fixedMib {
    final a = context.session.along;
    return a is AlongNoRoom ? (a.fixedMib ?? a.needMib ?? 20 * 1024) : 20 * 1024;
  }

  void _select(Shrinkable s) {
    // 默认：腾出"固定开销 + 64 GiB 的 /data"，但不低于 gk3_shrink 自己的下限。
    // ⚠️ 原先是"至少要的（固定开销 + 8 GiB 下限）再多 8 GiB" —— 按默认操作的双系统用户 /data 只有约 16 GiB，
    //    一个大型手游都装不下（v1.0 计划 GUI-20）
    final want = s.curMib - _fixedMib - _dataDefaultMib;
    setState(() {
      _pick = s;
      _target = want.clamp(s.floorMib, s.curMib - 1024).toDouble();
    });
  }

  Future<void> _go() async {
    final s = context.session;
    setState(() {
      _running = true;
      _error = null;
      _log.clear();
    });
    final r = await s.shrink(_pick!, _target.round(), (e) {
      if (!mounted) return;
      setState(() {
        if (e is Gk3Progress) {
          _pct = e.percent;
          _log.add('[${e.percent}%] ${progressText(context.l, e)}');
        } else if (e is Gk3Log) {
          _log.add(e.line);
        }
      });
    });
    if (!mounted) return;
    if (r.ok) {
      Navigator.pop(context);
    } else {
      setState(() {
        _running = false;
        _error = callErrorText(context.l, r);
      });
    }
  }

  String? _why(Shrinkable s) {
    final l = context.l;
    if (s.can) return null;
    return switch (s.why) {
      'ntfs-dirty' => l.shrinkWhyDirty,
      'ntfs-hibernated' => l.shrinkWhyHibernated,
      'bitlocker' => l.shrinkWhyBitlocker,
      'mounted' => l.shrinkWhyMounted,
      _ => l.shrinkWhyFs,
    };
  }

  @override
  Widget build(BuildContext context) {
    final s = context.session, l = context.l, tt = Theme.of(context).textTheme;
    final list = (s.shrinkables ?? []).where((x) => x.fs == 'ntfs' || x.fs.startsWith('ext') || x.why == 'ntfs-dirty' || x.why == 'ntfs-hibernated' || x.why == 'bitlocker').toList()
      ..sort((a, b) => b.curMib.compareTo(a.curMib));
    final labels = {for (final p in s.disk?.parts ?? <Part>[]) p.path: p.label};
    if (_running) {
      return StepPage(
      step: Gk3Step.mode,
        title: l.shrinkRunning,
        subtitle: l.shrinkWarn,
        bottom: const SizedBox(height: kTouch),
        child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
          LinearProgressIndicator(value: _pct / 100, minHeight: 8),
          const SizedBox(height: 16),
          Expanded(child: LogView(_log)),
        ]),
      );
    }
    final p = _pick;
    return StepPage(
      step: Gk3Step.mode,
      title: l.shrinkTitle,
      subtitle: l.shrinkSub,
      onBack: () => Navigator.pop(context),
      bottom: p == null
          ? null
          : Row(children: [
              Btn(l.btnBack, kind: BtnKind.text, icon: Icons.arrow_back, onPressed: () => Navigator.pop(context)),
              const SizedBox(width: 24),
              Expanded(
                // 不用 confirmHoldIdle（"按住 2 秒以开始安装"）：这一步是缩分区，不是开始安装
                child: HoldToConfirm(idle: '${l.shrinkGo} · ${l.holdIdle}', busy: (x) => l.confirmHoldBusy('$x'), onConfirmed: _go),
              ),
            ]),
      child: list.isEmpty
          ? Text(l.shrinkNone, style: tt.bodyLarge)
          : Row(crossAxisAlignment: CrossAxisAlignment.start, children: [
              Expanded(
                child: ListView.separated(
                  itemCount: list.length,
                  separatorBuilder: (_, _) => const SizedBox(height: 12),
                  itemBuilder: (_, i) {
                    final x = list[i];
                    return ChoiceCard(
                      title: '${labels[x.part] ?? x.part}  ·  ${x.part.split('/').last}',
                      body: l.shrinkRow(x.fs.toUpperCase(), fmtMib(x.curMib), fmtMib(x.minMib)),
                      reason: _why(x),
                      selected: identical(p, x),
                      onTap: () => _select(x),
                    );
                  },
                ),
              ),
              if (p != null) ...[
                const SizedBox(width: 28),
                SizedBox(
                  width: 440,
                  child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                    Text(l.shrinkSizeHint, style: tt.titleMedium),
                    const SizedBox(height: 8),
                    Text(fmtMib(_target.round()), style: tt.headlineMedium),
                    Slider(
                      value: _target,
                      min: p.floorMib.toDouble(),
                      max: (p.curMib - 1024).toDouble(),
                      // 取整到 GiB 之后再夹回范围：两端不一定是整 GiB，取整会越界（Slider 断言 value <= max）
                      onChanged: (v) => setState(() => _target = ((v / 1024).round() * 1024.0).clamp(p.floorMib.toDouble(), (p.curMib - 1024).toDouble())),
                    ),
                    Text(l.shrinkFreed(fmtMib(p.curMib - _target.round())), style: tt.bodyLarge!.copyWith(color: context.gk.success)),
                    // /data 的估计：腾出来的减去固定开销（腾出来的那段要是紧挨着已有的空闲，实际会更大一点）
                    Text(l.shrinkData(fmtMib(math.max(0, p.curMib - _target.round() - _fixedMib))), style: tt.bodyLarge),
                    DataSizeWarning(p.curMib - _target.round() - _fixedMib),
                    const SizedBox(height: 18),
                    Text(l.shrinkWarn, style: tt.bodyMedium!.copyWith(color: context.gk.warning)),
                    if (_error != null) ...[const SizedBox(height: 12), Text(_error!, style: tt.bodyMedium!.copyWith(color: context.cs.error))],
                  ]),
                ),
              ],
            ]),
    );
  }
}
