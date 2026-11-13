// 欢迎 / 预检 → 选盘 → 方式 → （删除分区）
import 'package:flutter/material.dart';
import 'package:gk3_common/ui/theme.dart';
import 'package:gk3_common/ui/widgets.dart';

import '../app.dart';
import '../model/model.dart';
import '../session.dart';
import 'screens_edit.dart';
import 'screens_finish.dart';
import 'screens_risk.dart';
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

  /// 一项检查 → 一行：通过时标题说明是什么、下面是取值；没通过时标题说问题、下面说怎么处理
  _CheckRow _row(Check c) {
    final l = context.l;
    return switch (c.id) {
      'model' => switch (c.state) {
          CheckState.ok => _CheckRow(state: c.state, title: l.checkModelOk, detail: c.value),
          CheckState.fail => _CheckRow(state: c.state, title: l.checkModelBadTitle, detail: l.checkModelBad(c.value)),
          CheckState.unknown => _CheckRow(state: c.state, title: l.checkModel, detail: l.checkUnknown),
        },
      'power' => switch (c.state) {
          CheckState.ok => _CheckRow(state: c.state, title: l.checkPower, detail: c.ac ? l.checkPowerShownAc(c.value) : l.checkPowerShown(c.value)),
          CheckState.fail => _CheckRow(state: c.state, title: l.checkPowerLow, detail: l.checkPowerBad(c.min)),
          CheckState.unknown => _CheckRow(state: c.state, title: l.checkPower, detail: l.checkUnknown),
        },
      // 后端多报了一项（新后端）：照样列出来，不当成失败以外的东西
      _ => _CheckRow(state: c.state, title: c.id, detail: c.value.isEmpty ? null : c.value),
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
      // 与 Windows 安装器的检查页同一种排法：说明在上，检查结果逐行列在下面
      child: ListView(children: [
        Text(l.welcomeBody, style: tt.bodyLarge),
        const SizedBox(height: 24),
        Text(l.checkTitle, style: tt.titleMedium),
        const SizedBox(height: 8),
        if (checks == null)
          Padding(
            padding: const EdgeInsets.symmetric(vertical: 8),
            child: Row(children: [
              const SizedBox.square(dimension: 28, child: CircularProgressIndicator(strokeWidth: 3)),
              const SizedBox(width: 16),
              Text(l.checkRunning, style: tt.bodyLarge),
            ]),
          )
        else ...[
          for (final c in checks) _row(c),
          if (s.blocked) ...[const SizedBox(height: 8), Text(l.checkBlocked, style: tt.bodyLarge!.copyWith(color: context.cs.error))],
        ],
      ]),
    );
  }
}

/// 检查结果的一行：左边状态图标，右边标题与说明（样式同 Windows 安装器的 CheckRow）
class _CheckRow extends StatelessWidget {
  const _CheckRow({required this.state, required this.title, this.detail});
  final CheckState state;
  final String title;
  final String? detail;

  @override
  Widget build(BuildContext context) {
    final cs = context.cs, tt = context.tt;
    final (icon, color) = switch (state) {
      CheckState.ok => (Icons.check_circle, context.gk.success),
      CheckState.fail => (Icons.cancel, cs.error),
      CheckState.unknown => (Icons.warning_amber_rounded, context.gk.warning),
    };
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 8),
      child: Row(crossAxisAlignment: CrossAxisAlignment.start, children: [
        Icon(icon, color: color, size: 28),
        const SizedBox(width: 16),
        Expanded(
          child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
            Text(title, style: tt.titleMedium),
            if (detail != null) ...[
              const SizedBox(height: 2),
              Text(detail!, style: tt.bodyMedium!.copyWith(color: cs.onSurfaceVariant)),
            ],
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
            extra: diskBar(d),
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

enum _Pick { wipe, along, reinstall, edit }

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
      AlongBadTable() => l.errNoGpt,
      AlongError(:final msg) => l.errPlan(msg),
    };
  }

  Future<void> _next() async {
    final s = context.session;
    switch (_pick!) {
      case _Pick.wipe:
        s.setMode(Mode.wipe);
        go(context, const OptsPage());
      case _Pick.along:
        s.setMode(Mode.alongside);
        go(context, const OptsPage());
      case _Pick.reinstall:
        s.setMode(Mode.reinstall);
        go(context, const OptsPage());
      // 删除分区同样先过风险确认页（RiskPage 等那一页 pop 了才跟着 pop，所以这里 await 的仍是"做完了没有"）
      case _Pick.edit:
        await RiskPage.open(context, RiskKind.edit, const DiskEditPage());
        // 删过分区的盘要重新评估：几种方式能不能走，都可能变了
        if (mounted && s.disk != null) {
          setState(() => _pick = null);
          await s.assess(s.disk!);
        }
    }
  }

  @override
  Widget build(BuildContext context) {
    final s = context.session, l = context.l;
    final a = s.along;
    // 安装介质上没有系统文件（gaokun3/payload/）：三种安装方式都不能选，只剩删除分区
    final noRel = !(s.usbRelease?.installable ?? false);
    final reason = noRel ? l.modeWhyNoRelease : _alongReason(a);
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
            reason: noRel
                ? l.modeWhyNoRelease
                : !r.ok
                    ? planErrorText(context, r)
                    : (s.espInfo?.winHibernated ?? false)
                        ? l.modeWhyHibernated
                        : null,
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
          reason: noRel ? l.modeWhyNoRelease : (s.disk!.medium ? l.modeWhyMedium : null),
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
          extra: a is AlongOk ? diskBar(s.disk!, highlightFree: a.region) : null,
        ),
        // ★ 最后一项：删除分区（例如 Windows 恢复分区，"磁盘管理"删不了它们）。总是可选 ——
        //   上面几项都走不通的时候，这是用户在安装器里能做的；缩小分区要回 Windows 压缩卷
        const SizedBox(height: 14),
        ChoiceCard(
          icon: Icons.delete_outline,
          title: l.editEntryTitle,
          body: l.editEntryBody,
          // MBR 盘、主 GPT 坏了的盘上删除分区要写 GPT 分区表，后端拒绝（ERR mbr-disk / disk-not-gpt）—— 这里先说
          reason: s.disk!.mbr ? l.errMbr : (s.disk!.damaged ? l.errNoGpt : null),
          selected: _pick == _Pick.edit,
          onTap: () => setState(() => _pick = _Pick.edit),
        ),
      ]),
    );
  }
}
