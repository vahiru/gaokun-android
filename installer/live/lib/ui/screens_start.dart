// 欢迎 / 预检 → 方式（内置硬盘的分区布局与安装方式在同一页）→ （删除分区）
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
        Btn(l.btnStart, autofocus: true, onPressed: checks == null || s.blocked ? null : () => go(context, const ModePage())),
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

// ── 方式 ─────────────────────────────────────────────────────────────────────

class ModePage extends StatefulWidget {
  const ModePage({super.key});
  @override
  State<ModePage> createState() => _ModePageState();
}

enum _Pick { wipe, along, reinstall, rmlive, edit }

class _ModePageState extends State<ModePage> {
  _Pick? _pick;

  /// 进入本页时重新探测并评估内置硬盘（Session.probeInternal）；完成之前不显示上一次留下的结果
  bool _loading = true;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) async {
      await context.session.probeInternal();
      if (mounted) setState(() => _loading = false);
    });
  }

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
      // 删除分区、删除安装器分区同样先过风险确认页（RiskPage 等那一页 pop 了才跟着 pop，所以这里 await 的仍是"做完了没有"）
      case _Pick.edit || _Pick.rmlive:
        await RiskPage.open(context, _pick == _Pick.edit ? RiskKind.edit : RiskKind.removeInstaller,
            _pick == _Pick.edit ? const DiskEditPage() : const RemoveInstallerPage());
        // 删过分区的盘要重新评估：几种方式能不能走，都可能变了
        if (mounted && s.disk != null) {
          setState(() => _pick = null);
          await s.assess(s.disk!);
        }
    }
  }

  /// 页面顶部：内置硬盘的名称与容量、分区布局条与图例（与 Windows 安装器的选位置页相同）。
  /// 选中"保留现有系统"时在条上突出 Android 将要使用的那块未分配空间
  Widget _diskHead(Disk d, {FreeRegion? target}) {
    final l = context.l, tt = context.tt, cs = context.cs;
    final name = d.model.isNotEmpty ? d.model : l.diskInternal;
    // 只突出这块盘上确实有的那块空闲区（按对象认，与 diskBar 相同）
    final hit = target != null && d.free.any((f) => identical(f, target)) ? target : null;
    return Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
      Row(children: [
        Icon(Icons.storage, color: cs.onSurfaceVariant),
        const SizedBox(width: 12),
        Expanded(child: Text('$name · ${fmtMib(d.sizeMib)}', style: tt.titleMedium)),
        Text(d.path, style: tt.bodyMedium!.copyWith(color: cs.onSurfaceVariant)),
      ]),
      // 没有可列出的分区与空闲区（读不出分区表、空白盘、MBR）时不画布局条：画出来是一整条灰色，看着像全是现有分区
      if (d.parts.isNotEmpty || d.free.isNotEmpty) ...[
        const SizedBox(height: 12),
        diskBar(d, muted: true, height: 28, highlightFree: hit),
        const SizedBox(height: 8),
        DiskLegend(
          existing: d.parts.any((p) => p.os != 'android'),
          free: d.free.any((f) => !identical(f, hit)),
          target: hit != null,
          android: d.parts.any((p) => p.os == 'android'),
        ),
      ],
      const SizedBox(height: 24),
    ]);
  }

  @override
  Widget build(BuildContext context) {
    final s = context.session, l = context.l;
    final a = s.along;
    // 安装介质上没有系统文件（gaokun3/payload/）：三种安装方式都不能选，只剩删除分区
    final noRel = !(s.usbRelease?.installable ?? false);
    final reason = noRel ? l.modeWhyNoRelease : _alongReason(a);
    StepPage page(Widget child) => StepPage(
          step: Gk3Step.mode,
          title: l.modeTitle,
          subtitle: l.modeSub,
          // 探测与评估进行中不能返回：返回后马上再进本页会让两次评估同时写同一份 Session
          onBack: _loading || s.assessing ? null : () => Navigator.pop(context),
          onNext: _pick == null ? null : _next,
          // 重新安装默认也会清掉 /data —— 和整盘清空一样用 error 色的"下一步"
          nextDanger: _pick == _Pick.wipe || _pick == _Pick.reinstall,
          child: child,
        );
    if (_loading || s.assessing) return page(const Center(child: CircularProgressIndicator()));
    final d = s.internalDisk;
    // 只安装到内置硬盘：没有它，或读不出它的分区表时，没有可选的方式
    if (d == null) return page(Text(l.diskNone, style: context.tt.bodyLarge));
    if (d.unreadable || s.disk == null) {
      return page(ListView(children: [_diskHead(d), Text(l.diskUnreadable, style: context.tt.bodyLarge!.copyWith(color: context.cs.error))]));
    }
    // 分区布局固定在顶部，只滚动下面的选项：选中靠下的选项时仍看得到布局条上突出的位置。选项只有几项，全部直接构建
    return page(Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
      _diskHead(s.disk!, target: _pick == _Pick.along && a is AlongOk ? a.region : null),
      Expanded(
        child: SingleChildScrollView(
          child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
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
              // Android 已装好、安装器从这块盘上的安装器分区运行：可以单独删掉它（装的时候选了保留，或者删除没成功）
              if (s.installerPart case final lv?) ...[
                ChoiceCard(
                  icon: Icons.delete_sweep_outlined,
                  title: l.modeRmLiveTitle,
                  body: [
                    l.modeRmLiveBody,
                    if (r.rmlive case final rm?) rm.merge ? l.rmLiveMerge(lv.label, fmtMib(lv.sizeMib)) : l.rmLiveFree(lv.label, fmtMib(lv.sizeMib)),
                  ].join('\n'),
                  selected: _pick == _Pick.rmlive,
                  onTap: () => setState(() => _pick = _Pick.rmlive),
                ),
                const SizedBox(height: 14),
              ],
            ],
            ChoiceCard(
              icon: Icons.delete_forever,
              danger: true,
              title: l.modeWipeTitle,
              warn: l.modeWipeWarn,
              body: l.modeWipeBody,
              // 安装器从这块盘上运行：读进了内存（安装器分区）就照常清除、保留那个分区；否则清除不了
              reason: noRel ? l.modeWhyNoRelease : (s.disk!.medium && s.installerPart == null ? l.modeWhyMedium : null),
              note: s.installerPart == null ? null : l.confirmWipeKeepsLive(s.installerPart!.label),
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
        ),
      ),
    ]));
  }
}
