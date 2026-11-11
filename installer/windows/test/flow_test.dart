// 各演示场景从头走到尾（回放 testdata/ 里真 gk3w 的录制）
import 'package:flutter_test/flutter_test.dart';
import 'package:gk3_windows/ui/run_pages.dart';

import 'helpers.dart';

void main() {
  testWidgets('有空闲区：检查 → 位置 → 版本 → 选项 → 确认 → 安装完成', (t) async {
    final (rec, s) = await pumpApp(t, 'free');
    await see(t, find.text(l.chkModel));
    await see(t, find.text(l.chkRoom('40 GiB')));
    expect(enabled(t, l.btnNext), isTrue);
    // 至少要多大、每块空闲区的方案都是问 gk3w plan 的
    expect(rec.calls.where((c) => c.first == 'plan').length, 2);

    await tap(t, find.text(l.btnNext));
    await see(t, find.text(l.regionData('27.8 GiB')));
    await tap(t, find.text(l.btnNext));
    await see(t, find.textContaining('crDroid'));
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

    await tap(t, find.text(l.btnRebootNow));
    expect(rec.reboots, 1);
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
    await see(t, find.textContaining('crDroid'));
    await tap(t, find.text(l.btnNext));
    await tap(t, find.text(l.btnNext));
    await hold(t, l.holdToInstall);
    await waitFor(t, find.byType(FailPage), tries: 400);
    expect(find.text(l.errRelease), findsOneWidget);
    expect(find.text(l.failUntouched), findsOneWidget);
    expect(s.touched, isFalse);
  });

  testWidgets('英文界面：检查页上没有中文', (t) async {
    await pumpApp(t, 'blocked', language: 'en');
    await see(t, find.text(en.chkModelBad));
    expect(cjkOnScreen(t), isEmpty);
  });
}
