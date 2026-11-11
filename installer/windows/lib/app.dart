import 'package:flutter/material.dart';
import 'package:flutter_localizations/flutter_localizations.dart';
import 'package:gk3_common/ui/canvas.dart';
import 'package:gk3_common/ui/step_scaffold.dart';
import 'package:gk3_common/ui/theme.dart';

import 'l10n/app_localizations.dart';
import 'session.dart';
import 'ui/check_page.dart';
import 'ui/title_bar.dart';
import 'ui/media_pages.dart';

const kAppVersion = '0.1.0-proto';

/// 给子树提供 Session，并在它变化时重建依赖它的部分
class SessionScope extends InheritedNotifier<Session> {
  const SessionScope({super.key, required Session session, required super.child}) : super(notifier: session);

  static Session of(BuildContext context) => context.dependOnInheritedWidgetOfExactType<SessionScope>()!.notifier!;

  /// 不建立依赖的读取（initState 里用）
  static Session read(BuildContext context) => context.getInheritedWidgetOfExactType<SessionScope>()!.notifier!;
}

extension SessionX on BuildContext {
  Session get session => SessionScope.of(this);
  L10n get l => L10n.of(this);
}

/// 跳到下一步。导航就是 Navigator 的栈：返回 = 回到真正来的那一页
Future<T?> go<T>(BuildContext context, Widget page, {bool replace = false}) {
  final route = PageRouteBuilder<T>(
    pageBuilder: (_, _, _) => page,
    transitionDuration: const Duration(milliseconds: 220),
    transitionsBuilder: (_, a, _, child) => FadeTransition(
      opacity: a,
      child: SlideTransition(position: Tween(begin: const Offset(0.03, 0), end: Offset.zero).animate(a), child: child),
    ),
  );
  final nav = Navigator.of(context);
  return replace ? nav.pushReplacement(route) : nav.push(route);
}

/// 回到第一页并重新检查（失败页、完成页的"重新开始"）
void restart(BuildContext context) {
  Navigator.of(context).popUntil((r) => r.isFirst);
  context.session.check();
}

class WinInstallerApp extends StatelessWidget {
  const WinInstallerApp({super.key, required this.session, this.home});
  final Session session;

  /// 测试时直接从某一页开始
  final Widget? home;

  @override
  Widget build(BuildContext context) {
    return SessionScope(
      session: session,
      child: ListenableBuilder(
        listenable: session,
        builder: (context, _) => MaterialApp(
          debugShowCheckedModeBanner: false,
          onGenerateTitle: (c) => L10n.of(c).appTitle,
          theme: buildTheme(),
          locale: Locale(session.language),
          supportedLocales: L10n.supportedLocales,
          localizationsDelegates: const [
            L10n.delegate,
            GlobalMaterialLocalizations.delegate,
            GlobalWidgetsLocalizations.delegate,
            GlobalCupertinoLocalizations.delegate,
          ],
          // 窗口没有系统标题栏：画布最上面一行用来拖动窗口，右上角是最小化与关闭（ui/title_bar.dart）
          builder: (context, child) => LogicalCanvas(
            child: Stack(children: [
              child!,
              const Positioned(left: 0, top: 0, right: 0, height: DragStrip.height, child: DragStrip()),
              if (session.backend.demoLabel != null)
                Positioned(
                  right: WindowButtons.width + 12,
                  top: 12,
                  child: DemoBadge(L10n.of(context).fixtureBadge(session.backend.demoLabel!)),
                ),
              const Positioned(right: 0, top: 0, child: WindowButtons()),
            ]),
          ),
          home: home ?? const CheckPage(),
        ),
      ),
    );
  }
}

/// 双系统安装的六步（左侧步骤栏只是指示）
enum WinStep { check, region, release, options, confirm, install }

/// 制作独立安装介质的五步（侧栏底部的入口进入）：把 LiveCD 写入 U 盘或本机磁盘，安装方式在 LiveCD 里选
enum MediaStep { about, target, release, confirm, write }

/// 每一步的骨架：gk3_common 的 StepScaffold，填上所在流程的步骤（[WinStep] 或 [MediaStep]）、
/// 侧栏底部的安装介质入口、语言切换与版本号
class WinStepPage extends StatelessWidget {
  const WinStepPage({
    super.key,
    required this.step,
    required this.title,
    this.subtitle,
    required this.child,
    this.onBack,
    this.onNext,
    this.nextLabel,
    this.nextDanger = false,
    this.bottomLeft,
    this.bottom,
    this.danger = false,
  });

  /// [WinStep] 或 [MediaStep]：决定侧栏显示哪个流程的步骤
  final Enum step;
  final String title;
  final String? subtitle;
  final Widget child;
  final VoidCallback? onBack, onNext;
  final String? nextLabel;
  final bool nextDanger;
  final Widget? bottomLeft, bottom;
  final bool danger;

  @override
  Widget build(BuildContext context) {
    assert(step is WinStep || step is MediaStep);
    final l = context.l, s = context.session;
    final media = step is MediaStep;
    final steps = media
        ? [
            RailStep(l.railAbout, Icons.info_outline, Icons.info),
            RailStep(l.railTarget, Icons.storage_outlined, Icons.storage),
            RailStep(l.railRelease, Icons.download_outlined, Icons.download),
            RailStep(l.railConfirm, Icons.verified_user_outlined, Icons.verified_user),
            RailStep(l.railWrite, Icons.album_outlined, Icons.album),
          ]
        : [
            RailStep(l.railCheck, Icons.fact_check_outlined, Icons.fact_check),
            RailStep(l.railRegion, Icons.storage_outlined, Icons.storage),
            RailStep(l.railRelease, Icons.download_outlined, Icons.download),
            RailStep(l.railOptions, Icons.tune_outlined, Icons.tune),
            RailStep(l.railConfirm, Icons.verified_user_outlined, Icons.verified_user),
            RailStep(l.railInstall, Icons.install_desktop_outlined, Icons.install_desktop),
          ];
    final footerButton = TextButton.styleFrom(alignment: Alignment.centerLeft, padding: const EdgeInsets.symmetric(horizontal: 16));
    return StepScaffold(
      steps: steps,
      current: step.index,
      railTitle: l.railTitle,
      railIcon: null,
      railSubtitle: l.railSub,
      railFooter: [
        // 制作独立安装介质的入口：写盘期间、已在这个流程里时不可用
        TextButton.icon(
          style: footerButton,
          onPressed: s.writing || media ? null : () => go(context, const MediaAboutPage()),
          icon: const Icon(Icons.album_outlined),
          label: Text(l.railMedia),
        ),
        TextButton.icon(
          style: footerButton,
          onPressed: s.writing ? null : () => s.setLanguage(s.language == 'zh' ? 'en' : 'zh'),
          icon: const Icon(Icons.translate),
          label: Text(l.railLanguage),
        ),
        const SizedBox(height: 16),
        Padding(
          padding: const EdgeInsets.symmetric(horizontal: 16),
          child: Text(l.railFooter(kAppVersion), style: context.tt.bodySmall),
        ),
      ],
      backLabel: l.btnBack,
      defaultNextLabel: l.btnNext,
      title: title,
      subtitle: subtitle,
      onBack: onBack,
      onNext: onNext,
      nextLabel: nextLabel,
      nextDanger: nextDanger,
      bottomLeft: bottomLeft,
      bottom: bottom,
      danger: danger,
      child: child,
    );
  }
}
