import 'package:flutter/material.dart';
import 'package:gk3_common/ui/theme.dart';
import 'package:gk3_common/ui/widgets.dart';

import '../app.dart';
import '../model.dart';
import 'bits.dart';
import 'messages.dart';
import 'run_pages.dart';

/// 系统盘的分区条：Windows 的分区按 gk3w probe 给的 os 上色，空闲区画成虚线框，选中的那块高亮
Widget systemDiskBar(BuildContext context, {FreeSpan? highlight}) {
  final s = context.session, d = s.disk!;
  final free = s.freeSpans;
  return DiskBar(
    height: 28,
    parts: [for (final p in s.probe!.partsOf(d)) (start: p.start, sizeMib: p.sizeMib, os: p.os.isEmpty ? 'other' : p.os)],
    free: [for (final f in free) (start: f.start, sizeMib: f.sizeMib)],
    highlightFree: highlight == null ? null : free.indexOf(highlight),
  );
}

/// 第二步：装在哪块空闲区。每块能不能装、/data 能有多大都问过 gk3w plan
class RegionPage extends StatelessWidget {
  const RegionPage({super.key});

  @override
  Widget build(BuildContext context) {
    final s = context.session, l = context.l, d = s.disk!;
    return WinStepPage(
      step: WinStep.region,
      title: l.regionTitle,
      subtitle: l.regionSub(d.model, fmtMib(d.sizeMib)),
      onBack: () => Navigator.pop(context),
      onNext: s.region == null ? null : () => go(context, const ReleasePage()),
      child: ListView(children: [
        systemDiskBar(context, highlight: s.region),
        const SizedBox(height: 8),
        _Legend(),
        const SizedBox(height: 24),
        for (final f in s.freeSpans)
          Padding(
            padding: const EdgeInsets.only(bottom: 12),
            child: Builder(builder: (context) {
              final p = s.plans[f];
              final ok = p?.ok ?? false;
              return ChoiceCard(
                icon: Icons.crop_free,
                title: l.regionFree(fmtMib(f.sizeMib)),
                body: ok ? l.regionData(fmtMib(p!.userdataMib)) : null,
                reason: ok ? null : l.regionTooSmall(fmtMib(p?.needMib ?? s.minNeedMib)),
                selected: identical(f, s.region),
                onTap: ok ? () => s.selectRegion(f) : null,
              );
            }),
          ),
        const SizedBox(height: 8),
        NoteBox(text: l.regionNote),
      ]),
    );
  }
}

class _Legend extends StatelessWidget {
  @override
  Widget build(BuildContext context) {
    final l = context.l;
    Widget item(String os, String label) => Padding(
          padding: const EdgeInsets.only(right: 20),
          child: Row(mainAxisSize: MainAxisSize.min, children: [
            Container(width: 12, height: 12, decoration: BoxDecoration(color: DiskBar.colorOf(context, os), borderRadius: BorderRadius.circular(3))),
            const SizedBox(width: 6),
            Text(label, style: context.tt.bodySmall),
          ]),
        );
    return Wrap(children: [
      item('windows', 'Windows'),
      item('esp', l.legendEsp),
      item('winre', l.legendRecovery),
      item('android', 'Android'),
      item('other', l.legendOther),
    ]);
  }
}

/// 第三步：装哪个版本（发布清单，与 LiveCD 的网络安装同一份）
class ReleasePage extends StatefulWidget {
  const ReleasePage({super.key});
  @override
  State<ReleasePage> createState() => _ReleasePageState();
}

class _ReleasePageState extends State<ReleasePage> {
  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) => context.session.loadVariants());
  }

  @override
  Widget build(BuildContext context) {
    final s = context.session, l = context.l;
    return WinStepPage(
      step: WinStep.release,
      title: l.releaseTitle,
      subtitle: l.releaseSub,
      onBack: () => Navigator.pop(context),
      onNext: s.variant == null ? null : () => go(context, const OptionsPage()),
      child: s.loadingVariants
          ? Center(child: Column(mainAxisSize: MainAxisSize.min, children: [
              const CircularProgressIndicator(),
              const SizedBox(height: 16),
              Text(l.releaseLoading, style: context.tt.bodyLarge),
            ]))
          : ListView(children: [
              if (s.variants.isEmpty)
                NoteBox(
                  warn: true,
                  text: s.variantsErr == null ? l.releaseNone : errText(l, s.variantsErr!),
                  child: Btn(l.btnRetry, kind: BtnKind.secondary, onPressed: s.loadVariants),
                ),
              for (final v in s.variants)
                Padding(
                  padding: const EdgeInsets.only(bottom: 12),
                  child: ChoiceCard(
                    icon: Icons.android,
                    title: v.name,
                    body: l.releaseSize(fmtMib(v.sizeMib)),
                    good: v.latest ? l.releaseLatest : null,
                    selected: identical(v, s.variant),
                    onTap: () => s.selectVariant(v),
                  ),
                ),
              const SizedBox(height: 8),
              NoteBox(text: l.releaseNote),
            ]),
    );
  }
}

/// 第四步：开机默认进哪个系统
class OptionsPage extends StatelessWidget {
  const OptionsPage({super.key});

  @override
  Widget build(BuildContext context) {
    final s = context.session, l = context.l;
    return WinStepPage(
      step: WinStep.options,
      title: l.optionsTitle,
      subtitle: l.optionsSub,
      onBack: () => Navigator.pop(context),
      onNext: () => go(context, const ConfirmPage()),
      child: ListView(children: [
        ChoiceCard(
          icon: Icons.android,
          title: l.optDefaultAndroid,
          body: l.optDefaultAndroidBody,
          selected: s.defaultOs == 'android',
          onTap: () => s.setDefaultOs('android'),
        ),
        const SizedBox(height: 12),
        ChoiceCard(
          icon: Icons.window,
          title: l.optDefaultWindows,
          body: l.optDefaultWindowsBody,
          selected: s.defaultOs == 'windows',
          onTap: () => s.setDefaultOs('windows'),
        ),
      ]),
    );
  }
}

/// 第五步：写盘之前，用最醒目的方式说清楚会改什么；按住 2 秒才开始
class ConfirmPage extends StatelessWidget {
  const ConfirmPage({super.key});

  @override
  Widget build(BuildContext context) {
    final s = context.session, l = context.l, cs = context.cs;
    final r = s.region!, p = s.plans[r]!, v = s.variant!;
    final ready = !s.needBitlockerAck || s.bitlockerAck;
    return WinStepPage(
      step: WinStep.confirm,
      danger: true,
      title: l.confirmTitle,
      subtitle: l.confirmSub,
      bottom: Row(children: [
        Btn(l.btnBack, onPressed: () => Navigator.pop(context), kind: BtnKind.text, icon: Icons.arrow_back),
        const SizedBox(width: 24),
        Expanded(
          child: HoldToConfirm(
            idle: l.holdToInstall,
            busy: l.holdKeep,
            enabled: ready,
            onConfirmed: () => go(context, RunPage(task: RunTask.install, releaseDir: kReleaseDir), replace: true),
          ),
        ),
      ]),
      child: ListView(children: [
        Text(l.confirmWill, style: context.tt.titleMedium),
        const SizedBox(height: 4),
        Bullet(l.confirmParts(fmtMib(r.sizeMib), fmtMib(p.userdataMib))),
        Bullet(l.confirmDownload(v.name, fmtMib(v.sizeMib))),
        Bullet(l.confirmBoot),
        Bullet(s.defaultOs == 'android' ? l.confirmDefaultAndroid : l.confirmDefaultWindows),
        const SizedBox(height: 12),
        Bullet(l.confirmWindowsSafe, icon: Icons.shield_outlined, color: context.gk.success),
        if (s.needBitlockerAck) ...[
          const SizedBox(height: 16),
          NoteBox(
            warn: true,
            text: l.confirmBitlocker,
            child: CheckboxListTile(
              contentPadding: EdgeInsets.zero,
              controlAffinity: ListTileControlAffinity.leading,
              value: s.bitlockerAck,
              onChanged: (v) => s.setBitlockerAck(v ?? false),
              title: Text(l.confirmBitlockerAck, style: context.tt.bodyLarge!.copyWith(color: cs.onSurface)),
            ),
          ),
        ],
      ]),
    );
  }
}

/// 下载的发布文件放在这里（真后端接上时由它决定；演示数据里只是回显）
const kReleaseDir = r'C:\ProgramData\gaokun3\release';
