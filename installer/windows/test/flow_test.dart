// 各演示场景从头走到尾（回放 testdata/ 里真 gk3w 的录制）
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:gk3_common/backend/protocol.dart';
import 'package:gk3_windows/app.dart';
import 'package:gk3_windows/session.dart';
import 'package:gk3_windows/ui/media_pages.dart';
import 'package:gk3_windows/ui/run_pages.dart';

import 'helpers.dart';

void main() {
  // 安装介质的入口在 0.1.0 里不注册（app.dart 的 mediaEntryEnabled）；页面照样测
  setUpAll(() => mediaEntryEnabled = true);
  testWidgets('有空闲区：检查 → 位置 → 版本 → 选项 → 确认 → 安装完成', (t) async {
    // 原生窗口的通道：记下安装期间的 setBusy（阻止关机与睡眠）
    final window = <MethodCall>[];
    t.binding.defaultBinaryMessenger.setMockMethodCallHandler(const MethodChannel('gk3/window'), (c) async {
      window.add(c);
      return null;
    });
    addTearDown(() => t.binding.defaultBinaryMessenger.setMockMethodCallHandler(const MethodChannel('gk3/window'), null));
    final (rec, s) = await pumpApp(t, 'free');
    await see(t, find.text(l.chkModel));
    await see(t, find.text(l.chkRoom('40 GiB')));
    expect(enabled(t, l.btnNext), isTrue);
    // 至少要多大、每块空闲区的方案都是问 gk3w plan 的
    expect(rec.calls.where((c) => c.first == 'plan').length, 2);

    await tap(t, find.text(l.btnNext));
    await see(t, find.text(l.regionData('27.8 GiB')));
    await tap(t, find.text(l.btnNext));
    await see(t, find.textContaining('v1.0.0-rc.3'));
    await tap(t, find.text(l.btnNext));
    await see(t, find.text(l.optDefaultAndroidBody));
    await tap(t, find.text(l.optDefaultWindows));
    await tap(t, find.text(l.btnNext));
    await see(t, find.text(l.confirmDefaultWindows));
    await see(t, find.text(l.confirmParts('40 GiB', '27.8 GiB')));

    await hold(t, l.holdToInstall);
    await waitFor(t, find.byType(DonePage), tries: 400);
    expect(find.byType(DonePage), findsOneWidget);
    expect(find.text(l.doneInstallTitle), findsOneWidget);
    final apply = rec.calls.singleWhere((c) => c.first == 'apply');
    expect(apply, containsAllInOrder(['--disk', '2', '--region-start', '874383360', '--region-end', '958269439']));
    expect(apply, containsAllInOrder(['--default-os', 'windows', '--real-disk']));
    expect(apply, isNot(contains('--bitlocker-key')));
    expect(s.percent, 100);
    // 下载期间只保持唤醒、不挡关闭与关机；gk3w 写盘时才挡；结束后都撤销（审查 11-12 第 2 条）
    final busy = [for (final c in window) if (c.method == 'setBusy') c.arguments as Map];
    expect(busy.firstWhere((a) => a['awake'] == true), {'busy': false, 'awake': true, 'power': l.runInstallTitle, 'shutdown': l.shutdownReasonInstall});
    expect(busy.where((a) => a['busy'] == true).single, {'busy': true, 'awake': true, 'power': l.runInstallTitle, 'shutdown': l.shutdownReasonInstall});
    expect(busy.last, containsPair('busy', false));
    expect(busy.last, containsPair('awake', false));

    await tap(t, find.text(l.btnRebootNow));
    expect(rec.reboots, 1);
  });

  testWidgets('几块空闲区：不够的那块不能选，默认选最大的，可以换', (t) async {
    final (rec, s) = await pumpApp(t, 'multi');
    await tap(t, find.text(l.btnNext));
    await see(t, find.text(l.regionTooSmall('20.2 GiB')));
    expect(find.text(l.regionFree('15 GiB')), findsOneWidget);
    expect(s.region!.sizeMib, 26624);
    await tap(t, find.text(l.regionFree('25 GiB')));
    expect(s.region!.sizeMib, 25600);
    // 点不够的那块不起作用
    await tap(t, find.text(l.regionFree('15 GiB')));
    expect(s.region!.sizeMib, 25600);
    expect(rec.calls.where((c) => c.first == 'plan').length, 4);
  });

  testWidgets('本地文件：不下载，直接用文件夹里的安装文件', (t) async {
    final (rec, s) = await pumpApp(t, 'free');
    await tap(t, find.text(l.btnNext));
    await tap(t, find.text(l.btnNext));
    await see(t, find.textContaining('v1.0.0-rc.3'));
    await tap(t, find.text(l.srcLocal));
    expect(enabled(t, l.btnNext), isFalse);
    await t.enterText(find.byType(TextField), r'D:\gk3\rc3');
    await settle(t);
    await tap(t, find.text(l.btnNext));
    await tap(t, find.text(l.btnNext));
    await see(t, find.text(l.confirmLocal(r'D:\gk3\rc3')));
    await hold(t, l.holdToInstall);
    await waitFor(t, find.byType(DonePage), tries: 400);
    expect(find.byType(DonePage), findsOneWidget);
    expect(rec.calls.where((c) => c.first == 'download'), isEmpty);
    expect(rec.calls.singleWhere((c) => c.first == 'apply'), containsAllInOrder(['--release', r'D:\gk3\rc3']));
  });

  testWidgets('R2 镜像：只有最新的一个版本', (t) async {
    final (rec, s) = await pumpApp(t, 'free');
    await tap(t, find.text(l.btnNext));
    await tap(t, find.text(l.btnNext));
    await tap(t, find.text(l.srcR2));
    await see(t, find.text('crDroid 13.0'));
    expect(s.variants.length, 1);
    expect(rec.calls.where((c) => c.first == 'releases').map((c) => c.last), ['github', 'r2']);
  });

  testWidgets('已经装了：检查 → 管理 → 卸载完成', (t) async {
    final (rec, s) = await pumpApp(t, 'installed');
    await see(t, find.text(l.chkInstalled));
    expect(rec.calls.where((c) => c.first == 'plan'), isEmpty);
    await tap(t, find.text(l.btnManage));
    await see(t, find.text(l.manageUninstall));
    await see(t, find.textContaining('gk3rescue'));
    await tap(t, find.text(l.manageUninstall));
    await see(t, find.text(l.uninstallParts(7, '64.1 GiB')));
    await hold(t, l.holdToUninstall);
    await waitFor(t, find.byType(DonePage), tries: 400);
    expect(find.text(l.doneUninstallSub('40 GiB')), findsOneWidget);
    expect(rec.calls.singleWhere((c) => c.first == 'uninstall'), containsAllInOrder(['--disk', '0', '--real-disk']));
  });

  testWidgets('系统盘没有空闲区：说明要压缩出多少，不能往下走', (t) async {
    await pumpApp(t, 'nospace');
    await see(t, find.text(l.chkNoRoom('20.2 GiB')));
    expect(find.textContaining(l.chkNoRoomNone), findsOneWidget);
    expect(enabled(t, l.btnNext), isFalse);
  });

  testWidgets('检查不过：机型、Secure Boot、快速启动标红，BitLocker 提醒', (t) async {
    await pumpApp(t, 'blocked');
    await see(t, find.text(l.chkModelBad));
    expect(find.text(l.chkSecureBootOn), findsOneWidget);
    expect(find.text(l.chkFastStartupOn), findsOneWidget);
    expect(find.text(l.chkBitlockerOn), findsOneWidget);
    expect(enabled(t, l.btnNext), isFalse);
  });

  testWidgets('发布文件损坏：失败页说清楚原因，盘没动过', (t) async {
    final (_, s) = await pumpApp(t, 'bad-release');
    await tap(t, find.text(l.btnNext));
    await tap(t, find.text(l.btnNext));
    await see(t, find.textContaining('v1.0.0-rc.3'));
    await tap(t, find.text(l.btnNext));
    await tap(t, find.text(l.btnNext));
    await hold(t, l.holdToInstall);
    await waitFor(t, find.byType(FailPage), tries: 400);
    expect(find.text(l.errRelease), findsOneWidget);
    expect(find.text(l.failUntouched), findsOneWidget);
    expect(s.touched, Touched.no);
  });

  testWidgets('安装介质写入U盘：侧栏入口 → 说明 → 目标 → 版本 → 确认；gk3w 还没有 media 命令，写入这一步按失败处理', (t) async {
    final window = <MethodCall>[];
    t.binding.defaultBinaryMessenger.setMockMethodCallHandler(const MethodChannel('gk3/window'), (c) async {
      window.add(c);
      return null;
    });
    addTearDown(() => t.binding.defaultBinaryMessenger.setMockMethodCallHandler(const MethodChannel('gk3/window'), null));
    final (rec, s) = await pumpApp(t, 'usb');
    await see(t, find.text(l.chkRoom('40 GiB')));
    expect(enabled(t, l.railMedia), isTrue);
    await tap(t, find.text(l.railMedia));
    await see(t, find.text(l.mediaUseWipe));
    // 已在这个流程里：入口不可用
    expect(enabled(t, l.railMedia), isFalse);
    await tap(t, find.text(l.btnNext));
    await see(t, find.text(l.mediaUsbTitle('Generic Flash Disk', '29.4 GiB')));
    expect(find.text(l.mediaInternalTitle), findsOneWidget);
    expect(enabled(t, l.btnNext), isFalse);
    await tap(t, find.text(l.mediaUsbTitle('Generic Flash Disk', '29.4 GiB')));
    expect(s.mediaTarget!.usb, isTrue);
    await tap(t, find.text(l.btnNext));
    await see(t, find.text(l.releaseSubMedia));
    await see(t, find.text(l.releaseLiveCd('0.2.0-rc.2', '225 MiB')));
    await tap(t, find.text(l.btnNext));
    await see(t, find.text(l.mediaConfirmSubUsb('Generic Flash Disk', '29.4 GiB')));
    expect(find.text(l.mediaConfirmUsb), findsOneWidget);
    expect(find.text(l.mediaConfirmLiveDl('0.2.0-rc.2', '225 MiB')), findsOneWidget);
    expect(find.text(l.confirmBoot), findsNothing);

    await hold(t, l.holdToWrite);
    await waitFor(t, find.byType(FailPage), tries: 400);
    expect(find.text(l.failMediaTitle), findsOneWidget);
    // 回放数据里没有 gk3w media：退出码 127、没有 ERR —— 与 gk3w 被结束时一样，按"不知道动没动过盘"说
    expect(find.text(l.failUnknownTouched), findsOneWidget);
    expect(rec.calls.where((c) => c.first == 'download').length, 1);
    final media = rec.calls.singleWhere((c) => c.first == 'media');
    expect(media, containsAllInOrder(['--target', 'usb', '--disk', '3', '--release', r'C:\ProgramData\gaokun3\release']));
    expect(media, containsAllInOrder(['--livecd', 'https://github.com/vahiru/gaokun-android/releases/download/v1.0.0-rc.2/gaokun3-installer-0.2.0-rc.2-windows.zip']));
    expect(media, isNot(contains('--real-disk')));
    final busy = [for (final c in window) if (c.method == 'setBusy') c.arguments as Map];
    expect(busy.where((a) => a['busy'] == true).single, {'busy': true, 'awake': true, 'power': l.runMediaTitle, 'shutdown': l.shutdownReasonMedia});
    expect(busy.last, containsPair('busy', false));
    // 返回首页后，入口又能用了；重新检查清掉了之前选的目标
    await tap(t, find.text(l.btnStartOver));
    await see(t, find.text(l.chkRoom('40 GiB')));
    expect(enabled(t, l.railMedia), isTrue);
    expect(s.mediaTarget, isNull);
  });

  testWidgets('安装介质写入本机磁盘：没有U盘时提示，写到未分配空间', (t) async {
    final (rec, s) = await pumpApp(t, 'free');
    await tap(t, find.text(l.railMedia));
    await tap(t, find.text(l.btnNext));
    await see(t, find.text(l.mediaUsbNone));
    await tap(t, find.text(l.mediaInternalTitle));
    expect(s.mediaTarget!.usb, isFalse);
    await tap(t, find.text(l.btnNext));
    // 位置页：GK3LIVE 建在所选未分配空间的开头
    await see(t, find.text(l.mediaRegionTitle));
    expect(find.text(l.mediaRegionRest('38 GiB')), findsOneWidget);
    expect(find.byType(MediaRegionPage), findsOneWidget);
    await tap(t, find.text(l.btnNext));
    await see(t, find.textContaining('v1.0.0-rc.3'));
    await tap(t, find.text(l.btnNext));
    await see(t, find.text(l.mediaConfirmPart('40 GiB', '2 GiB')));
    expect(find.text(l.confirmBoot), findsOneWidget);
    expect(find.text(l.mediaConfirmEntry), findsOneWidget);
    await hold(t, l.holdToWrite);
    await waitFor(t, find.byType(FailPage), tries: 400);
    final media = rec.calls.singleWhere((c) => c.first == 'media');
    expect(media, containsAllInOrder(['--target', 'internal', '--disk', '2', '--region-start', '874383360', '--region-end', '958269439']));
    expect(media, contains('--real-disk'));
  });

  testWidgets('安装介质写入本机磁盘：几块未分配空间时默认选最大的一块（LiveCD 保留现有系统时也装进最大的），可以换', (t) async {
    final (rec, s) = await pumpApp(t, 'multi');
    await tap(t, find.text(l.railMedia));
    await tap(t, find.text(l.btnNext));
    await tap(t, find.text(l.mediaInternalTitle));
    await tap(t, find.text(l.btnNext));
    await see(t, find.text(l.mediaRegionTitle));
    expect(s.mediaRegion!.sizeMib, 26624);
    expect(find.text(l.mediaRegionRest('24 GiB')), findsOneWidget);
    await tap(t, find.text(l.regionFree('15 GiB')));
    expect(s.mediaRegion!.sizeMib, 15360);
    await tap(t, find.text(l.btnNext));
    await see(t, find.textContaining('v1.0.0-rc.3'));
    await tap(t, find.text(l.btnNext));
    await see(t, find.text(l.mediaConfirmPart('15 GiB', '2 GiB')));
    await hold(t, l.holdToWrite);
    await waitFor(t, find.byType(FailPage), tries: 400);
    final media = rec.calls.singleWhere((c) => c.first == 'media');
    expect(media, containsAllInOrder(['--region-start', '${s.mediaRegion!.start}', '--region-end', '${s.mediaRegion!.end}']));
  });

  testWidgets('安装介质：本机不满足条件时本机磁盘不能选，列出未满足的条件', (t) async {
    final (_, s) = await pumpApp(t, 'blocked');
    await tap(t, find.text(l.railMedia));
    await tap(t, find.text(l.btnNext));
    await see(t, find.textContaining(l.mediaInternalBlocked));
    expect(find.textContaining(l.chkModelBad), findsOneWidget);
    expect(find.textContaining(l.chkSecureBootOn), findsOneWidget);
    await tap(t, find.text(l.mediaInternalTitle));
    expect(s.mediaTarget, isNull);
    expect(enabled(t, l.btnNext), isFalse);
  });

  testWidgets('安装介质：R2 也有 LiveCD；本地文件时目录里还要有 LiveCD', (t) async {
    final (_, s) = await pumpApp(t, 'usb');
    await tap(t, find.text(l.railMedia));
    await tap(t, find.text(l.btnNext));
    await tap(t, find.text(l.mediaUsbTitle('Generic Flash Disk', '29.4 GiB')));
    await tap(t, find.text(l.btnNext));
    await tap(t, find.text(l.srcR2));
    await see(t, find.text('crDroid 13.0'));
    expect(s.liveCd!.url, startsWith('https://ota.072172.xyz/installer/0.2.0-rc.2/'));
    await tap(t, find.text(l.srcLocal));
    await see(t, find.text(l.localLiveCd));
  });

  testWidgets('测试模式：本机检查不过也能继续（角上标出）；BitLocker 已开启时要勾选才能按住开始', (t) async {
    // blocked 的 preflight 是一台 PC 的真实结果：机型不对、Secure Boot、快速启动、BitLocker 都开着
    final (rec, s) = await pumpApp(t, 'blocked', testDisk: 2);
    await see(t, find.text(l.chkModelBad));
    expect(find.text(l.testBadge('2')), findsOneWidget);
    expect(find.text(l.chkBlockedNote), findsNothing);
    expect(enabled(t, l.btnNext), isTrue);
    await tap(t, find.text(l.btnNext));
    await tap(t, find.text(l.btnNext));
    await see(t, find.textContaining('v1.0.0-rc.3'));
    await tap(t, find.text(l.btnNext));
    await tap(t, find.text(l.btnNext));
    await see(t, find.text(l.confirmBitlockerAck));
    // 没勾选：按住不起作用
    await hold(t, l.holdToInstall);
    expect(find.byType(DonePage), findsNothing);
    expect(rec.calls.where((c) => c.first == 'apply'), isEmpty);
    await tap(t, find.text(l.confirmBitlockerAck));
    expect(s.bitlockerAck, isTrue);
    await hold(t, l.holdToInstall);
    await waitFor(t, find.byType(DonePage), tries: 400);
    expect(find.byType(DonePage), findsOneWidget);
    expect(rec.calls.singleWhere((c) => c.first == 'apply'), containsAllInOrder(['--bitlocker-key', 'yes']));
  });

  testWidgets('gk3w 写盘时没报原因就退出（被结束、崩溃）：失败页不说"磁盘未被修改"，而是请用户重新检查', (t) async {
    final (rec, s) = await pumpApp(t, 'free');
    rec.replace['apply'] = (args) => Stream.fromIterable([const Gk3Log('PROGRESS 30 write-super'), const Gk3Exit(1)]);
    await tap(t, find.text(l.btnNext));
    await tap(t, find.text(l.btnNext));
    await see(t, find.textContaining('v1.0.0-rc.3'));
    await tap(t, find.text(l.srcLocal));
    await t.enterText(find.byType(TextField), r'D:\gk3\rc3');
    await settle(t);
    await tap(t, find.text(l.btnNext));
    await tap(t, find.text(l.btnNext));
    await hold(t, l.holdToInstall);
    await waitFor(t, find.byType(FailPage), tries: 400);
    expect(s.touched, Touched.unknown);
    expect(find.text(l.failUnknownTouched), findsOneWidget);
    expect(find.text(l.failUntouched), findsNothing);
    expect(s.writing, isFalse);
    expect(s.diskBusy, isFalse);
  });

  testWidgets('已安装时 Secure Boot / 快速启动开着也能去管理、卸载；没装时挡住', (t) async {
    sbOn(Gk3Event e) => e is Gk3Record && e.type == 'PREFLIGHT'
        ? Gk3Record('PREFLIGHT', {...e.fields, 'secure_boot': 'on', 'fast_startup': 'on'})
        : e;
    final (rec, s) = await pumpApp(t, 'installed', map: {'preflight': sbOn});
    await see(t, find.text(l.chkInstalled));
    expect(find.text(l.chkSecureBootOn), findsOneWidget);
    expect(enabled(t, l.btnManage), isTrue);
    expect(rec.calls.first.first, 'preflight');
  });

  testWidgets('没装时 Secure Boot 开着照样挡住', (t) async {
    final (_, s) = await pumpApp(t, 'free', map: {
      'preflight': (e) => e is Gk3Record && e.type == 'PREFLIGHT' ? Gk3Record('PREFLIGHT', {...e.fields, 'secure_boot': 'on'}) : e,
    });
    await see(t, find.text(l.chkSecureBootOn));
    expect(s.installed, isFalse);
    expect(enabled(t, l.btnNext), isFalse);
  });

  testWidgets('检查调用失败却没报原因：检查页挡住并说明（不再空着页面放行）', (t) async {
    final (rec, s) = await pumpApp(t, 'free', replaceFirst: {'preflight': (_) => Stream.fromIterable([const Gk3Exit(1)])});
    await see(t, find.text(l.chkFailed));
    expect(s.checkErr!['code'], 'backend-failed');
    expect(find.text(l.errBackendFailed), findsOneWidget);
    expect(enabled(t, l.btnNext), isFalse);
  });


  testWidgets('英文界面：检查页上没有中文', (t) async {
    await pumpApp(t, 'blocked', language: 'en');
    await see(t, find.text(en.chkModelBad));
    expect(cjkOnScreen(t), isEmpty);
  });
}
