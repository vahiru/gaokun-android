import 'package:flutter/material.dart';
import 'package:gk3_common/ui/theme.dart';
import 'package:gk3_common/ui/widgets.dart';

import '../app.dart';
import '../model.dart';
import 'bits.dart';
import 'messages.dart';
import 'run_pages.dart';

/// 系统盘的分区条：已有的分区一律暗灰（muted），只突出空闲区、要装 Android 的那块与已装的 Android
Widget systemDiskBar(BuildContext context, {FreeSpan? highlight}) {
  final s = context.session, d = s.disk!;
  final free = s.freeSpans;
  return DiskBar(
    height: 28,
    parts: [for (final p in s.probe!.partsOf(d)) (start: p.start, sizeMib: p.sizeMib, os: p.os.isEmpty ? 'other' : p.os)],
    free: [for (final f in free) (start: f.start, sizeMib: f.sizeMib)],
    highlightFree: highlight == null ? null : free.indexOf(highlight),
    muted: true,
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
      nextLabel: l.btnNext,
      onNext: s.region == null ? null : () => go(context, const ReleasePage()),
      child: ListView(children: [
        systemDiskBar(context, highlight: s.region),
        const SizedBox(height: 8),
        const DiskLegend(target: true),
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

/// 分区条下面的图例（与 [systemDiskBar] 的 muted 配色对应）
class DiskLegend extends StatelessWidget {
  const DiskLegend({super.key, this.target = false, this.android = false});

  /// 有没有"要装 Android 的位置"（选位置页）、"已装的 Android"（管理页）
  final bool target, android;

  @override
  Widget build(BuildContext context) {
    final l = context.l, cs = context.cs, gk = context.gk;
    Widget item(Color c, String label) => Padding(
          padding: const EdgeInsets.only(right: 20),
          child: Row(mainAxisSize: MainAxisSize.min, children: [
            Container(width: 12, height: 12, decoration: BoxDecoration(color: c, borderRadius: BorderRadius.circular(3))),
            const SizedBox(width: 6),
            Text(label, style: context.tt.bodySmall),
          ]),
        );
    return Wrap(children: [
      item(cs.surfaceContainerHighest, l.legendExisting),
      if (target) ...[
        item(gk.success.withValues(alpha: 0.35), l.legendFree),
        item(gk.success, l.legendTarget),
      ],
      if (android) item(gk.success, 'Android'),
    ]);
  }
}

/// 第三步：装哪个版本、从哪来：GitHub 的 releases、R2 上的镜像，或者已经下载好的本地文件
class ReleasePage extends StatefulWidget {
  const ReleasePage({super.key});
  @override
  State<ReleasePage> createState() => _ReleasePageState();
}

class _ReleasePageState extends State<ReleasePage> {
  final _dir = TextEditingController();

  @override
  void initState() {
    super.initState();
    _dir.text = SessionScope.read(context).localDir;
    WidgetsBinding.instance.addPostFrameCallback((_) => context.session.loadVariants());
  }

  @override
  void dispose() {
    _dir.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final s = context.session, l = context.l;
    return WinStepPage(
      step: WinStep.release,
      title: l.releaseTitle,
      subtitle: s.source == ReleaseSource.local ? l.releaseSubLocal : l.releaseSub,
      onBack: () => Navigator.pop(context),
      nextLabel: l.btnNext,
      onNext: s.releaseReady ? () => go(context, const OptionsPage()) : null,
      child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
        Align(
          alignment: Alignment.centerLeft,
          child: SegmentedButton<ReleaseSource>(
            segments: [
              ButtonSegment(value: ReleaseSource.github, label: Text(l.srcGithub), icon: const Icon(Icons.cloud_download_outlined)),
              ButtonSegment(value: ReleaseSource.r2, label: Text(l.srcR2), icon: const Icon(Icons.cloud_outlined)),
              ButtonSegment(value: ReleaseSource.local, label: Text(l.srcLocal), icon: const Icon(Icons.folder_open_outlined)),
            ],
            selected: {s.source},
            showSelectedIcon: false,
            onSelectionChanged: (v) => s.setSource(v.first),
          ),
        ),
        const SizedBox(height: 20),
        Expanded(child: s.source == ReleaseSource.local ? _local(context) : _online(context)),
      ]),
    );
  }

  Widget _online(BuildContext context) {
    final s = context.session, l = context.l;
    if (s.loadingVariants) {
      return Center(child: Column(mainAxisSize: MainAxisSize.min, children: [
        const CircularProgressIndicator(),
        const SizedBox(height: 16),
        Text(l.releaseLoading, style: context.tt.bodyLarge),
      ]));
    }
    return ListView(children: [
      if (s.variants.isEmpty)
        NoteBox(
          warn: true,
          text: s.variantsErr == null ? l.releaseNone : errText(l, s.variantsErr!),
          child: Btn(l.btnRetry, kind: BtnKind.secondary, onPressed: () => s.loadVariants(force: true)),
        ),
      for (final v in s.variants)
        Padding(
          padding: const EdgeInsets.only(bottom: 12),
          child: ChoiceCard(
            dense: true,
            icon: Icons.android,
            title: v.name,
            body: l.releaseMeta(v.date, fmtMib(v.sizeMib)),
            good: v.latest ? l.releaseLatest : null,
            warn: v.pre && !v.latest ? l.releasePre : null,
            selected: identical(v, s.variant),
            onTap: () => s.selectVariant(v),
          ),
        ),
      const SizedBox(height: 8),
      NoteBox(text: s.source == ReleaseSource.r2 ? l.releaseNoteR2 : l.releaseNote),
    ]);
  }

  Widget _local(BuildContext context) {
    final s = context.session, l = context.l;
    return ListView(children: [
      TextField(
        controller: _dir,
        onChanged: s.setLocalDir,
        decoration: InputDecoration(
          labelText: l.localDirLabel,
          hintText: r'D:\Downloads\gaokun3-v1.0.0-rc.3',
          border: const OutlineInputBorder(),
          prefixIcon: const Icon(Icons.folder_outlined),
        ),
      ),
      const SizedBox(height: 20),
      Text(l.localNeed, style: context.tt.titleMedium),
      const SizedBox(height: 4),
      Bullet(l.localBoot, icon: Icons.insert_drive_file_outlined),
      Bullet(l.localSuper, icon: Icons.insert_drive_file_outlined),
      Bullet(l.localSums, icon: Icons.verified_outlined),
      const SizedBox(height: 12),
      NoteBox(text: l.localNote),
    ]);
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
    final r = s.region!, p = s.plans[r]!, v = s.variant;
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
            busy: (_) => l.holdKeep,
            enabled: ready,
            onConfirmed: () => go(context, RunPage(task: RunTask.install, releaseDir: kReleaseDir), replace: true),
          ),
        ),
      ]),
      child: ListView(children: [
        Text(l.confirmWill, style: context.tt.titleMedium),
        const SizedBox(height: 4),
        Bullet(l.confirmParts(fmtMib(r.sizeMib), fmtMib(p.userdataMib))),
        Bullet(s.source == ReleaseSource.local ? l.confirmLocal(s.localDir) : l.confirmDownload(v!.name, fmtMib(v.sizeMib))),
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
