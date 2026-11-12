// Windows 上的端到端：真界面 + 真后端（gk3w.exe、下载），对着一块 VHD 走完 安装 → 管理 → 卸载。
// 测试模式（GK3_TEST_DISK）：gk3w 不带 --real-disk，只肯写 VHD；检查页不因这台 PC 自己的检查项挡住；不重启。
// 要管理员权限（程序清单 requireAdministrator），从已提权的终端跑：
//
//   set GK3_TEST_DISK=<VHD 的磁盘号>
//   set GK3_TEST_RELEASE=<本地发布目录>     可选：给了就选"本地文件"；不给就从 GitHub 下载默认选中的版本
//   flutter test integration_test\vhd_test.dart -d windows
//
// 后端文件：程序目录的 backend\（CMake 打包），或 GK3W 指向 gk3w.exe —— 同目录要有 mke2fs.exe、mke2fs.conf、
// systemd-bootaa64.efi（kBackendFiles，缺一个就是 MissingBackend）。
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:gk3_windows/app.dart';
import 'package:gk3_windows/backend/platform.dart';
import 'package:gk3_windows/backend/win_backend.dart';
import 'package:gk3_windows/l10n/app_localizations.dart';
import 'package:gk3_windows/session.dart';
import 'package:gk3_windows/ui/run_pages.dart';
import 'package:gk3_windows/ui/title_bar.dart';
import 'package:gk3_common/ui/widgets.dart';
import 'package:integration_test/integration_test.dart';

final l = lookupL10n(const Locale('zh'));

/// 真时间里等一个控件出现（检查、下载、写盘都是真的，要等几分钟）
Future<void> waitFor(WidgetTester t, Finder f, {Duration timeout = const Duration(minutes: 2)}) async {
  final end = DateTime.now().add(timeout);
  while (f.evaluate().isEmpty) {
    if (DateTime.now().isAfter(end)) fail('timed out waiting for $f');
    await Future<void>.delayed(const Duration(milliseconds: 250));
    await t.pump();
  }
}

Future<void> tap(WidgetTester t, Finder f) async {
  await waitFor(t, f);
  await t.tap(f.first);
  await t.pump(const Duration(milliseconds: 400));
}

Future<void> hold(WidgetTester t) async {
  final g = await t.startGesture(t.getCenter(find.byType(HoldToConfirm)));
  for (var i = 0; i < 30; i++) {
    await Future<void>.delayed(const Duration(milliseconds: 100));
    await t.pump();
  }
  await g.up();
  await t.pump();
}

/// BitLocker 已开启（或读不到）时勾选"我已确认可获取恢复密钥"。先看一眼再点：勾着的再点一次就取消了，按钮变灰
Future<void> ackBitlocker(WidgetTester t, Session s) async {
  if (s.needBitlockerAck && !s.bitlockerAck) await tap(t, find.text(l.confirmBitlockerAck));
  expect(!s.needBitlockerAck || s.bitlockerAck, isTrue);
}

/// 失败时把日志区的内容带进失败信息
String logOf(Session s) => s.log.join('\n');

void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();

  testWidgets('VHD：检查 → 位置 → 版本 → 选项 → 确认 → 安装完成 → 管理 → 卸载完成', (t) async {
    final disk = requestedTestDisk();
    expect(disk, isNotNull, reason: 'set GK3_TEST_DISK to the disk number of a test VHD');
    final backend = locateRealBackend();
    expect(backend, isA<WinBackend>(), reason: 'gk3w.exe not found next to the app or via GK3W');
    final session = Session(backend!, language: 'zh', testDisk: disk);
    attachWindow(session);
    await t.pumpWidget(WinInstallerApp(session: session));
    await session.check();
    await t.pump();
    expect(session.checkErr, isNull, reason: '${session.checkErr}');
    expect(session.disk?.num, disk);
    expect(session.installed, isFalse, reason: 'the VHD already has Android on it');
    expect(session.region, isNotNull, reason: 'no free space on the VHD');
    await waitFor(t, find.textContaining(l.chkRoom('')));

    await tap(t, find.text(l.btnNext)); // 位置
    await tap(t, find.text(l.btnNext)); // 版本
    final local = Platform.environment['GK3_TEST_RELEASE'];
    if (local != null && local.isNotEmpty) {
      await tap(t, find.text(l.srcLocal));
      await t.enterText(find.byType(TextField), local);
      await t.pump();
    } else {
      await waitFor(t, find.text(l.releaseLatest), timeout: const Duration(minutes: 1));
    }
    await tap(t, find.text(l.btnNext)); // 选项
    await tap(t, find.text(l.btnNext)); // 确认
    await ackBitlocker(t, session);
    await hold(t);
    await waitFor(t, find.byWidgetPredicate((w) => w is DonePage || w is FailPage), timeout: const Duration(minutes: 40));
    expect(find.byType(DonePage), findsOneWidget, reason: 'install failed: ${session.result?.err}\n${logOf(session)}');
    expect(session.result!.ok, isTrue);

    // 稍后重启 → 首页重新检查 → 已安装 → 管理 → 卸载
    await tap(t, find.text(l.btnLater));
    await waitFor(t, find.text(l.chkInstalled), timeout: const Duration(minutes: 1));
    expect(session.installed, isTrue);
    await tap(t, find.text(l.btnManage));
    await tap(t, find.text(l.manageUninstall));
    await ackBitlocker(t, session);
    await hold(t);
    await waitFor(t, find.byWidgetPredicate((w) => w is DonePage || w is FailPage), timeout: const Duration(minutes: 10));
    expect(find.byType(DonePage), findsOneWidget, reason: 'uninstall failed: ${session.result?.err}\n${logOf(session)}');
    expect(session.freedMib, greaterThan(0));

    // 卸载后：首页又能装，空闲区回来了
    await tap(t, find.text(l.btnLater));
    await waitFor(t, find.text(l.btnNext));
    await session.check();
    expect(session.installed, isFalse);
    expect(session.region, isNotNull);
  }, timeout: const Timeout(Duration(minutes: 60)));
}
