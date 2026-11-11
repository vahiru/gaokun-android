import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_localizations/flutter_localizations.dart';

import 'l10n/app_localizations.dart';
import 'session.dart';
import 'ui/screens_start.dart';
import 'ui/theme.dart';

/// 给子树提供 Session，并在它变化时重建依赖它的部分
class SessionScope extends InheritedNotifier<Session> {
  const SessionScope({super.key, required Session session, required super.child}) : super(notifier: session);

  static Session of(BuildContext context) => context.dependOnInheritedWidgetOfExactType<SessionScope>()!.notifier!;

  /// 不在 SessionScope 下面时为 null（侧栏的重启 / 关机据此决定画不画）
  static Session? maybeOf(BuildContext context) => context.dependOnInheritedWidgetOfExactType<SessionScope>()?.notifier;
}

extension SessionX on BuildContext {
  Session get session => SessionScope.of(this);
  L10n get l => L10n.of(this);
}

/// 跳到下一步。导航就是 Navigator 的栈：返回 = 回到真正来的那一页。
/// C 版用 screen++/screen-- 时，分支屏插在枚举中间，"来源"页按返回会掉进
/// 一个用户没走过的"缩分区"页。
///
/// [name] 给路由起名，后面要按名字跳过它时用（失败页"返回修改"跳过风险确认页，见 screens_risk.dart 的 kRiskRoute）
Future<T?> go<T>(BuildContext context, Widget page, {bool replace = false, String? name}) {
  final route = PageRouteBuilder<T>(
    settings: RouteSettings(name: name),
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

/// "退出到终端"：切到 tty2；切不过去时用 SnackBar 说清楚原因、给个替代办法（MD3 的做法：轻量的反馈，不打断）
Future<void> openShell(BuildContext context) async {
  final err = await context.session.backend.openShell();
  if (err != null && context.mounted) {
    ScaffoldMessenger.of(context).showSnackBar(SnackBar(
      content: Text(context.l.shellFailed(err)),
      behavior: SnackBarBehavior.floating,
      width: 760,
      duration: const Duration(seconds: 8),
    ));
  }
}

class InstallerApp extends StatelessWidget {
  const InstallerApp({super.key, required this.session, this.home});
  final Session session;

  /// 测试 / 出图时直接从某一页开始
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
          builder: (context, child) => LogicalCanvas(
            child: Stack(children: [
              child!,
              if (session.backend.demoLabel != null)
                Positioned(right: 16, top: 12, child: _DemoBadge(session.backend.demoLabel!)),
            ]),
          ),
          home: home ?? const WelcomePage(),
        ),
      ),
    );
  }
}

/// 固定 1280×800 的逻辑画布，整体缩放到屏幕上（C 版 README："逻辑坐标固定 1280×800，
/// 输出时整体缩放"）。这也让 golden 出图与真机上的布局一模一样。
/// ★ Esc = 返回（roadmap 第 1 步：Tab/方向键移动焦点、回车确认、Esc 返回）。
class LogicalCanvas extends StatelessWidget {
  const LogicalCanvas({super.key, required this.child});
  final Widget child;

  @override
  Widget build(BuildContext context) {
    final mq = MediaQuery.of(context);
    return ColoredBox(
      color: Theme.of(context).colorScheme.surface,
      child: Center(
        child: FittedBox(
          child: SizedBox.fromSize(
            size: kCanvas,
            child: MediaQuery(
              data: mq.copyWith(size: kCanvas, padding: EdgeInsets.zero, viewInsets: EdgeInsets.zero, textScaler: TextScaler.noScaling),
              child: Shortcuts(
                shortcuts: const {SingleActivator(LogicalKeyboardKey.escape): DismissIntent()},
                child: child,
              ),
            ),
          ),
        ),
      ),
    );
  }
}

class _DemoBadge extends StatelessWidget {
  const _DemoBadge(this.scenario);
  final String scenario;
  @override
  Widget build(BuildContext context) => IgnorePointer(
        // MD3 的小标签（tertiary-container，8 dp 圆角 —— 与 chip 同一个形状）
        child: Material(
          color: context.cs.tertiaryContainer,
          borderRadius: BorderRadius.circular(8),
          child: Padding(
            padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 6),
            child: Text(L10n.of(context).fixtureBadge(scenario), style: context.tt.labelLarge!.copyWith(color: context.cs.onTertiaryContainer)),
          ),
        ),
      );
}
