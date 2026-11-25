import 'dart:async';

// 各演示场景从头走到尾（回放 testdata/ 里真 gk3w 的录制）
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:gk3_common/backend/protocol.dart';
import 'package:gk3_common/ui/widgets.dart';
import 'package:gk3_windows/app.dart';
import 'package:gk3_windows/model.dart';
import 'package:gk3_windows/session.dart';
import 'package:gk3_windows/ui/check_page.dart';
import 'package:gk3_windows/ui/media_pages.dart';
import 'package:gk3_windows/ui/messages.dart';
import 'package:gk3_windows/ui/run_pages.dart';
import 'package:gk3_windows/ui/title_bar.dart';

import 'helpers.dart';

void main() {
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
    // 下载期间只保持唤醒、不挡关闭与关机；gk3w 写盘时才挡；结束后都撤销
    final busy = [for (final c in window) if (c.method == 'setBusy') c.arguments as Map];
    // 下载期间关闭先问（confirmClose），写盘时不许关（busy）
    expect(busy.firstWhere((a) => a['awake'] == true),
        {'busy': false, 'awake': true, 'confirmClose': true, 'power': l.runInstallTitle, 'shutdown': l.shutdownReasonInstall});
    expect(busy.where((a) => a['busy'] == true).single,
        {'busy': true, 'awake': true, 'confirmClose': false, 'power': l.runInstallTitle, 'shutdown': l.shutdownReasonInstall});
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

  testWidgets('只装最新版本：GitHub 有多个版本时也只显示最新的一个，不能另选', (t) async {
    final (_, s) = await pumpApp(t, 'free');
    await tap(t, find.text(l.btnNext));
    await tap(t, find.text(l.btnNext));
    await see(t, find.text(l.releaseLatest));
    expect(s.variants.length, greaterThan(1));
    expect(s.variant!.id, 'v1.0.0-rc.3');
    expect(find.text(s.variant!.name), findsOneWidget);
    expect(find.text(s.variants[1].name), findsNothing);
    expect(find.byType(ChoiceCard), findsOneWidget);
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

  testWidgets('已经装了、系统盘上还有 gk3w 写的安装器分区：卸载时一并删除，确认页写明', (t) async {
    // installed 的 GK3LIVE（disk0p4）是旧版脚本建的 Basic data 类型：改成 gk3w media 建的 XBOOTLDR 类型
    Gk3Event xbootldr(Gk3Event e) => e is Gk3Record && e.type == 'PART' && e['path'] == 'disk0p4'
        ? Gk3Record('PART', {...e.fields, 'type': kXbootldrType, 'name': 'GK3LIVE', 'fslabel': ''})
        : e;
    final (rec, s) = await pumpApp(t, 'installed', map: {'probe': xbootldr});
    await tap(t, find.text(l.btnManage));
    await tap(t, find.text(l.manageUninstall));
    await see(t, find.text(l.uninstallLive('2.3 GiB')));
    await hold(t, l.holdToUninstall);
    await waitFor(t, find.byType(DonePage), tries: 400);
    expect(rec.calls.singleWhere((c) => c.first == 'uninstall'), isNot(contains('--installer-only')));
  });

  testWidgets('旧版脚本建的 GK3LIVE（Basic data 类型）：卸载确认页不说要删它', (t) async {
    await pumpApp(t, 'installed');
    await tap(t, find.text(l.btnManage));
    await tap(t, find.text(l.manageUninstall));
    await see(t, find.text(l.uninstallParts(7, '64.1 GiB')));
    expect(find.text(l.uninstallLive('2.3 GiB')), findsNothing);
  });

  testWidgets('空间不够：读取最近一次压缩停在哪个文件，给出处理方法；重新检查后清掉', (t) async {
    final (rec, _) = await pumpApp(t, 'nospace');
    await see(t, find.text(l.shrinkAnalyze));
    await tap(t, find.text(l.shrinkAnalyze));
    await waitFor(t, find.text(l.sbPagefile), tries: 400);
    expect(find.text(l.shrinkBlocked('C', '14:05')), findsOneWidget);
    expect(find.text(r'C:\pagefile.sys'), findsOneWidget);
    expect(find.text(l.sbPagefileHow('C')), findsOneWidget);
    expect(find.text(l.shrinkNext), findsOneWidget);
    expect(find.text(l.shrinkReanalyze), findsOneWidget);
    // 不指定分区：gk3w 取用户最近一次在"磁盘管理"里压缩的那个
    expect(rec.calls.where((c) => c.first == 'shrink-info').single, ['shrink-info']);
    await tap(t, find.text(l.btnRecheck));
    expect(find.text(l.sbPagefile), findsNothing);
    expect(find.text(l.shrinkAnalyze), findsOneWidget);
  });

  testWidgets('还没压缩过、分区太满、找不到文件、D 盘、又读到同一条、读取失败的写法', (t) async {
    Stream<Gk3Event> shrink(Map<String, String> f) => Stream.fromIterable([Gk3Record('SHRINK', f), const Gk3Exit(0)]);
    var reply = shrink({'event': 'none'});
    await pumpApp(t, 'nospace', respond: {'shrink-info': (args, input) => reply});
    await tap(t, find.text(l.shrinkAnalyze));
    await see(t, find.text(l.shrinkNoEvent));
    // 压缩停在已用空间上（分区太满）：请用户清理这个分区，不去怪事件里的文件
    reply = shrink({'drive': 'C:', 'record': '11', 'time': '09:12', 'free_mib': '4096', 'file': r'\pagefile.sys::$DATA', 'limit': 'space'});
    await tap(t, find.text(l.shrinkReanalyze));
    await see(t, find.text(l.shrinkFull('C', '09:12')));
    expect(find.text(l.sbPagefile), findsNothing);
    reply = shrink({'drive': 'C:', 'record': '12', 'time': '09:20', 'free_mib': '4096'});
    await tap(t, find.text(l.shrinkReanalyze));
    await see(t, find.text(l.shrinkNoFile('C', '09:20')));
    expect(find.text(l.shrinkStale), findsNothing);
    // 用户在"磁盘管理"里压缩的是 D 盘：结果、文件路径与处理步骤都写 D 盘
    reply = shrink({'drive': 'D:', 'record': '13', 'time': '09:31', 'free_mib': '300000',
        'file': r'\System Volume Information\{11111111-2222-3333-4444-555555555555}{3808876b-c176-4e48-b7ae-04046e6cc752}', 'limit': 'file'});
    await tap(t, find.text(l.shrinkReanalyze));
    await see(t, find.text(l.shrinkBlocked('D', '09:31')));
    expect(find.text(l.sbRestoreHow('D')), findsOneWidget);
    expect(find.textContaining(r'D:\System Volume Information\'), findsOneWidget);
    // 处理之后没重新"压缩卷"就来查看：还是那一条，先说清楚
    await tap(t, find.text(l.shrinkReanalyze));
    await see(t, find.text(l.shrinkStale));
    expect(find.text(l.shrinkBlocked('D', '09:31')), findsOneWidget);
    reply = Stream.fromIterable([const Gk3Record('ERR', {'code': 'shrink-query'}), const Gk3Exit(1)]);
    await tap(t, find.text(l.shrinkReanalyze));
    await see(t, find.text('${l.shrinkFailed}${l.errShrinkQuery}'));
  });

  testWidgets('压缩停在 MFT 上：确认后移动 MFT（写盘期间挡住关闭），不自己重新分析', (t) async {
    final window = <MethodCall>[];
    t.binding.defaultBinaryMessenger.setMockMethodCallHandler(const MethodChannel('gk3/window'), (c) async {
      window.add(c);
      return null;
    });
    addTearDown(() => t.binding.defaultBinaryMessenger.setMockMethodCallHandler(const MethodChannel('gk3/window'), null));
    final (rec, s) = await pumpApp(t, 'nospace-mft');
    // 同 main.dart：启动 gk3w 之前等原生窗口挡住关闭（一帧）
    s.beforeDiskWrite = WindowControl.settleBusy;
    await tap(t, find.text(l.shrinkAnalyze));
    await waitFor(t, find.text(l.mftMoveBtn), tries: 400);
    expect(find.text(l.sbMft), findsOneWidget);
    expect(find.text(r'C:\$Mft'), findsOneWidget);
    // 取消：不动盘
    await tapShown(t, find.text(l.mftMoveBtn));
    await tap(t, find.text(l.mftConfirmCancel));
    expect(rec.calls.where((c) => c.first == 'mft-move'), isEmpty);
    await tapShown(t, find.text(l.mftMoveBtn));
    await tap(t, find.text(l.mftConfirmOk));
    await waitFor(t, find.text(l.mftStuck('C')), tries: 400);
    expect(rec.calls.singleWhere((c) => c.first == 'mft-move'), ['mft-move', '--execute', '--drive', 'C:']);
    expect(rec.calls.where((c) => c.first == 'shrink-info'), hasLength(1));
    expect(find.text(l.mftMoved('78 MiB', '755 MiB', '677 MiB', 'C')), findsOneWidget);
    expect(find.text(l.mftMoveBtn), findsNothing);
    final busy = [for (final c in window) if (c.method == 'setBusy') c.arguments as Map];
    expect(busy.where((a) => a['busy'] == true && a['power'] == l.mftMoving), isNotEmpty);
    expect(busy.last['busy'], isFalse);
    expect(s.mftMoving, isFalse);
    // 用户重新"压缩卷"后再查看（回放换到 nospace-mft-after）：仍停在同一分区的 MFT 上，"已达上限"照样显示，不再建议移动
    await tapShown(t, find.text(l.shrinkReanalyze));
    expect(rec.calls.where((c) => c.first == 'shrink-info'), hasLength(2));
    await see(t, find.text(l.shrinkBlocked('C', '21:20')));
    expect(find.text(l.shrinkStale), findsNothing);
    expect(find.text(l.mftStuck('C')), findsOneWidget);
    expect(find.text(l.mftMoveBtn), findsNothing);
    // 没再"压缩卷"就又来查看：还是那一条，先说清楚
    await tapShown(t, find.text(l.shrinkReanalyze));
    await see(t, find.text(l.shrinkStale));
    expect(find.text(l.mftStuck('C')), findsOneWidget);
  });

  testWidgets('MFT 已挪到头：重新"压缩卷"后的新记录仍停在同一分区的 MFT 上时，不再建议移动', (t) async {
    var record = '21';
    final (rec, _) = await pumpApp(t, 'nospace-mft', respond: {
      'shrink-info': (args, input) => Stream.fromIterable([
            Gk3Record('SHRINK', {'drive': 'C:', 'record': record, 'time': '10:00', 'file': r'\$Mft::$DATA', 'limit': 'file'}),
            const Gk3Exit(0),
          ]),
    });
    await tap(t, find.text(l.shrinkAnalyze));
    await tapShown(t, find.text(l.mftMoveBtn));
    await tap(t, find.text(l.mftConfirmOk));
    await waitFor(t, find.text(l.mftStuck('C')), tries: 400);
    record = '22';
    await tapShown(t, find.text(l.shrinkReanalyze));
    expect(rec.calls.where((c) => c.first == 'shrink-info'), hasLength(2));
    await see(t, find.text(l.shrinkBlocked('C', '10:00')));
    expect(find.text(l.shrinkStale), findsNothing);
    expect(find.text(l.mftStuck('C')), findsOneWidget);
    expect(find.text(l.mftMoveBtn), findsNothing);
    expect(rec.calls.where((c) => c.first == 'mft-move'), hasLength(1));
  });

  testWidgets('移动 MFT 成功但前部空间不够：说明还有多少没挪，可以清理后再挪', (t) async {
    final (_, s) = await pumpApp(t, 'nospace-mft', respond: {
      'mft-move': (args, input) => Stream.fromIterable([
            const Gk3Record('MFTMOVE', {'drive': 'C:', 'execute': 'yes', 'end_before_mib': '755', 'end_after_mib': '720', 'moved_mib': '35',
                'steps': '3', 'unplaced_mib': '43', 'fixed_end_mib': '678'}),
            const Gk3Exit(0),
          ]),
    });
    await t.runAsync(s.analyzeShrink);
    await t.runAsync(s.moveMft);
    await settle(t);
    expect(find.text(l.mftMoved('35 MiB', '755 MiB', '720 MiB', 'C')), findsOneWidget);
    expect(find.text(l.mftUnplaced('43 MiB', 'C')), findsOneWidget);
    expect(find.text(l.mftMoveBtn), findsOneWidget);
  });

  testWidgets('移动 MFT 失败：说明原因、可以再试；中途失败时显示已移动的部分', (t) async {
    var reply = Stream<Gk3Event>.fromIterable([const Gk3Record('ERR', {'code': 'mft-open'}), const Gk3Exit(1)]);
    final (rec, s) = await pumpApp(t, 'nospace-mft', respond: {'mft-move': (args, input) => reply});
    await t.runAsync(s.analyzeShrink);
    await t.runAsync(s.moveMft);
    await settle(t);
    // 一步都没挪：失败原因与"移动MFT"按钮都在
    expect(find.text('${l.mftFailed}${l.errMftRead}'), findsOneWidget);
    expect(find.text(l.mftMoveBtn), findsOneWidget);
    expect(rec.calls.where((c) => c.first == 'shrink-info'), hasLength(1));
    // 重新查看：失败提示清掉
    await t.runAsync(s.analyzeShrink);
    await settle(t);
    expect(find.textContaining(l.mftFailed), findsNothing);
    // 挪了一部分后连续失败：报已挪的部分、失败原因，还能再挪；不自己重新分析
    reply = Stream<Gk3Event>.fromIterable([
      const Gk3Record('MFTMOVE', {'drive': 'C:', 'execute': 'yes', 'end_before_mib': '755', 'end_after_mib': '720', 'moved_mib': '35',
          'steps': '3', 'unplaced_mib': '43', 'fixed_end_mib': '678'}),
      const Gk3Record('ERR', {'code': 'mft-move-failed'}),
      const Gk3Exit(1),
    ]);
    await t.runAsync(s.moveMft);
    await settle(t);
    expect(rec.calls.where((c) => c.first == 'shrink-info'), hasLength(2));
    expect(find.text(l.mftMoved('35 MiB', '755 MiB', '720 MiB', 'C')), findsOneWidget);
    expect(find.text('${l.mftFailed}${l.errMftFailed}'), findsOneWidget);
    // 失败时不去猜别的原因（不说前部空间不足）
    expect(find.text(l.mftUnplaced('43 MiB', 'C')), findsNothing);
    expect(find.text(l.mftMoveBtn), findsOneWidget);
  });

  testWidgets('空间够用时也能查看：挡住的文件照样给出处理方法', (t) async {
    final (rec, _) = await pumpApp(t, 'free');
    await see(t, find.text(l.chkRoom('40 GiB')));
    await tap(t, find.text(l.shrinkAnalyze));
    await waitFor(t, find.text(l.sbPagefile), tries: 400);
    expect(find.text(l.shrinkBlocked('C', '14:05')), findsOneWidget);
    expect(find.text(l.sbPagefileHow('C')), findsOneWidget);
    expect(rec.calls.where((c) => c.first == 'shrink-info'), hasLength(1));
    // 下一步照常可以按
    expect(enabled(t, l.btnNext), isTrue);
  });

  testWidgets('移动 MFT 期间不能往下走、不能重新检查，也不能校验恢复密钥', (t) async {
    final gate = StreamController<Gk3Event>();
    final (_, s) = await pumpApp(t, 'free', respond: {
      'shrink-info': (args, input) => Stream.fromIterable([
            const Gk3Record('SHRINK', {'drive': 'C:', 'free_mib': '100', 'file': r'\$Mft::$DATA', 'limit': 'file'}),
            const Gk3Exit(0),
          ]),
      'mft-move': (args, input) => gate.stream,
    });
    expect(enabled(t, l.btnNext), isTrue);
    await tap(t, find.text(l.shrinkAnalyze));
    await tapShown(t, find.text(l.mftMoveBtn));
    await tap(t, find.text(l.mftConfirmOk));
    expect(s.mftMoving, isTrue);
    expect(enabled(t, l.btnNext), isFalse);
    expect(enabled(t, l.btnRecheck), isFalse);
    expect(await s.verifyBitlockerKey(kGoodKey), isFalse);
    expect(await s.install(releaseDir: r'C:\x'), isFalse);
    expect(s.result!.err!['code'], 'internal');
    gate
      ..add(const Gk3Record('MFTMOVE', {'drive': 'C:', 'execute': 'yes', 'end_before_mib': '900', 'end_after_mib': '100', 'moved_mib': '800',
          'steps': '50', 'unplaced_mib': '0', 'fixed_end_mib': '3'}))
      ..add(const Gk3Exit(0));
    await gate.close();
    await settle(t, 40);
    expect(s.mftMoving, isFalse);
    expect(enabled(t, l.btnNext), isTrue);
    // 挪完了（没有放不下的部分）：请用户重新"压缩卷"、回来再查看
    expect(find.text(l.mftDone('C')), findsOneWidget);
    expect(find.text(l.mftMoveBtn), findsNothing);
    // 新的记录仍停在 MFT 上（还没挪到头）：移动的结果清掉，可以再挪
    await tapShown(t, find.text(l.shrinkReanalyze));
    await see(t, find.text(l.mftMoveBtn));
    expect(find.text(l.mftDone('C')), findsNothing);
  });

  testWidgets('测试模式不读取这台电脑的压缩记录', (t) async {
    await pumpApp(t, 'nospace', testDisk: 2);
    await see(t, find.text(l.chkNoRoom('20.2 GiB')));
    expect(find.text(l.shrinkAnalyze), findsNothing);
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
    // 不是这台平板：不提供暂停这台电脑的 BitLocker 保护
    expect(find.text(l.chkBitlockerSuspendBox), findsNothing);
    expect(find.text(l.chkSecureBootHow), findsOneWidget);
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

  testWidgets('写入中途被安全软件拒绝访问：失败页提示允许 gk3w.exe，并先卸载未完成的安装', (t) async {
    final (_, s) = await pumpApp(t, 'free', respond: {
      'apply': (args, input) => Stream.fromIterable([
            const Gk3Record('ERR', {'code': 'disk-write', 'part': 'boot_a', 'winerr': '5', 'touched': 'yes'}),
            const Gk3Exit(1),
          ]),
    });
    await tap(t, find.text(l.btnNext));
    await tap(t, find.text(l.btnNext));
    await see(t, find.textContaining('v1.0.0-rc.3'));
    await tap(t, find.text(l.btnNext));
    await tap(t, find.text(l.btnNext));
    await hold(t, l.holdToInstall);
    await waitFor(t, find.byType(FailPage), tries: 400);
    expect(find.text(l.errWriteDenied), findsOneWidget);
    expect(find.text(l.failTouchedInstall), findsOneWidget);
    expect(s.touched, Touched.yes);
    // 别的 Windows 错误照常按写入失败说；不是写入类的错误码不提安全软件
    expect(errText(l, const Gk3Record('ERR', {'code': 'disk-write', 'winerr': '1117'})), l.errDiskWrite);
    expect(errText(l, const Gk3Record('ERR', {'code': 'esp-verify', 'winerr': '5'})), l.errDiskVerify);
    expect(errText(l, const Gk3Record('ERR', {'code': 'esp-write', 'winerr': '5'})), l.errWriteDenied);
  });

  testWidgets('安装介质写入U盘：侧栏入口 → 说明 → 目标 → 版本 → 确认 → 写入完成', (t) async {
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
    await see(t, find.text(l.releaseLiveCd('0.2.0', '181 MiB')));
    await tap(t, find.text(l.btnNext));
    await see(t, find.text(l.mediaConfirmSubUsb('Generic Flash Disk', '29.4 GiB')));
    expect(find.text(l.mediaConfirmUsb), findsOneWidget);
    expect(find.text(l.mediaConfirmLiveDl('0.2.0', '181 MiB')), findsOneWidget);
    expect(find.text(l.confirmBoot), findsNothing);

    await hold(t, l.holdToWrite);
    await waitFor(t, find.byType(DonePage), tries: 400);
    expect(find.text(l.doneMediaTitle), findsOneWidget);
    expect(find.text(l.doneMediaSubUsb), findsOneWidget);
    // 下载时一并下载 LiveCD 镜像（与核对它的 SHA256SUMS），gk3w media 拿到的是下载好的 ISO
    final dl = rec.calls.singleWhere((c) => c.first == 'download');
    expect(dl.sublist(dl.length - 2), [
      'https://github.com/vahiru/gaokun-android/releases/download/live-installer-v0.2.0/gaokun3-live-0.2.0.iso',
      'https://github.com/vahiru/gaokun-android/releases/download/live-installer-v0.2.0/SHA256SUMS',
    ]);
    final media = rec.calls.singleWhere((c) => c.first == 'media');
    expect(media, containsAllInOrder(['--target', 'usb', '--disk', '3', '--expect-mib', '30108', '--release', r'C:\ProgramData\gaokun3\release']));
    expect(media, containsAllInOrder(['--livecd', r'C:\ProgramData\gaokun3\release\gaokun3-live-0.2.0.iso']));
    expect(media, isNot(contains('--real-disk')));
    final busy = [for (final c in window) if (c.method == 'setBusy') c.arguments as Map];
    expect(busy.where((a) => a['busy'] == true).single,
        {'busy': true, 'awake': true, 'confirmClose': false, 'power': l.runMediaTitle, 'shutdown': l.shutdownReasonMedia});
    expect(busy.last, containsPair('busy', false));
    // 返回首页后，入口又能用了；重新检查清掉了之前选的目标
    await tap(t, find.text(l.btnStartOver));
    await see(t, find.text(l.chkRoom('40 GiB')));
    expect(enabled(t, l.railMedia), isTrue);
    expect(s.mediaTarget, isNull);
  });

  testWidgets('安装介质写入本机磁盘：没有U盘时提示，写到未分配空间，完成页说明从启动菜单进入 LiveCD', (t) async {
    final (rec, s) = await pumpApp(t, 'free', overrides: const {'media': 'media-internal.txt'});
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
    // 回放的是 PC 上 VHDX 真跑 gk3w media --target internal 的输出（testdata/common/media-internal.txt）
    await waitFor(t, find.byType(DonePage), tries: 400);
    expect(find.text(l.doneMediaTitle), findsOneWidget);
    expect(find.text(l.doneMediaSubInternal), findsOneWidget);
    final media = rec.calls.singleWhere((c) => c.first == 'media');
    expect(media, containsAllInOrder(['--target', 'internal', '--disk', '2', '--region-start', '874383360', '--region-end', '958269439']));
    expect(media, contains('--real-disk'));
  });

  testWidgets('安装介质写入本机磁盘：几块未分配空间时默认选最大的一块（LiveCD 保留现有系统时也装进最大的），可以换', (t) async {
    final (rec, s) = await pumpApp(t, 'multi', overrides: const {'media': 'media-internal.txt'});
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
    await waitFor(t, find.byType(DonePage), tries: 400);
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

  testWidgets('安装介质：系统盘上已有 GK3LIVE 时本机磁盘不能选，写明原因', (t) async {
    final (_, s) = await pumpApp(t, 'installed');
    await tap(t, find.text(l.railMedia));
    await tap(t, find.text(l.btnNext));
    await see(t, find.textContaining(l.chkLiveExists));
    await tap(t, find.text(l.mediaInternalTitle));
    expect(s.mediaTarget, isNull);
  });

  testWidgets('安装介质：gk3w 建的 GK3LIVE（GPT 名为 GK3LIVE、没有卷标）同样拦住', (t) async {
    Gk3Event live(Gk3Event e) => e is Gk3Record && e.type == 'PART' && e['path'] == 'disk2p5'
        ? Gk3Record('PART', {...e.fields, 'name': 'GK3LIVE', 'fslabel': ''})
        : e;
    final (_, s) = await pumpApp(t, 'free', map: {'probe': live});
    await tap(t, find.text(l.railMedia));
    await tap(t, find.text(l.btnNext));
    await see(t, find.textContaining(l.chkLiveExists));
    await tap(t, find.text(l.mediaInternalTitle));
    expect(s.mediaTarget, isNull);
  });

  testWidgets('删除本机磁盘上的安装器分区：没装 Android 时 ESP 一并还原，按住后只删它（--installer-only）', (t) async {
    final (rec, s) = await pumpApp(t, 'live');
    await tap(t, find.text(l.railMedia));
    await tap(t, find.text(l.btnNext));
    await see(t, find.textContaining(l.chkLiveExists));
    await see(t, find.text(l.mediaRmLiveBody('2 GiB')));
    await tap(t, find.text(l.mediaRmLiveTitle));
    await see(t, find.text(l.rmLiveTitle));
    expect(find.text(l.rmLivePart('2 GiB')), findsOneWidget);
    expect(find.text(l.rmLiveBoot), findsOneWidget);
    await hold(t, l.holdToRmLive);
    await waitFor(t, find.byType(DonePage), tries: 400);
    expect(find.text(l.doneRmLiveTitle), findsOneWidget);
    // 释放的是安装器分区本身的 2 GiB（FREE 记录是与前面的未分配空间连成的 40 GiB），可以合并回 C 盘
    expect(find.text(l.doneUninstallSub('2 GiB')), findsOneWidget);
    expect(find.text(l.doneUninstallExtend), findsOneWidget);
    // 不必重启：只有"返回首页"
    expect(find.text(l.btnRebootNow), findsNothing);
    expect(rec.calls.singleWhere((c) => c.first == 'uninstall'), containsAllInOrder(['--disk', '2', '--installer-only', '--real-disk']));
  });

  testWidgets('删除安装器分区：已装 Android 时不动 EFI 分区，确认页不说还原引导，完成页不建议合并回 C 盘', (t) async {
    Gk3Event xbootldr(Gk3Event e) => e is Gk3Record && e.type == 'PART' && e['path'] == 'disk0p4'
        ? Gk3Record('PART', {...e.fields, 'type': kXbootldrType, 'name': 'GK3LIVE', 'fslabel': ''})
        : e;
    await pumpApp(t, 'installed', map: {'probe': xbootldr});
    await tap(t, find.text(l.railMedia));
    await tap(t, find.text(l.btnNext));
    await tap(t, find.text(l.mediaRmLiveTitle));
    await see(t, find.text(l.rmLivePart('2.3 GiB')));
    expect(find.text(l.rmLiveBoot), findsNothing);
    await hold(t, l.holdToRmLive);
    await waitFor(t, find.byType(DonePage), tries: 400);
    expect(find.text(l.doneUninstallSub('2.3 GiB')), findsOneWidget);
    // 空出的空间在 /data 后面：合并不回 C 盘
    expect(find.text(l.doneUninstallExtend), findsNothing);
  });

  testWidgets('旧版脚本建的 GK3LIVE（Basic data 类型）：不提供删除', (t) async {
    await pumpApp(t, 'installed');
    await tap(t, find.text(l.railMedia));
    await tap(t, find.text(l.btnNext));
    await see(t, find.textContaining(l.chkLiveExists));
    expect(find.text(l.mediaRmLiveTitle), findsNothing);
  });

  testWidgets('安装介质：R2 上还没有 LiveCD（不能往下走）；本地文件时目录里还要有 LiveCD 镜像', (t) async {
    final (_, s) = await pumpApp(t, 'usb');
    await tap(t, find.text(l.railMedia));
    await tap(t, find.text(l.btnNext));
    await tap(t, find.text(l.mediaUsbTitle('Generic Flash Disk', '29.4 GiB')));
    await tap(t, find.text(l.btnNext));
    await tap(t, find.text(l.srcR2));
    await see(t, find.text('crDroid 13.0'));
    expect(s.liveCd, isNull);
    await see(t, find.text(l.releaseNoLiveCd));
    expect(enabled(t, l.btnNext), isFalse);
    await tap(t, find.text(l.srcLocal));
    await see(t, find.text(l.localLiveCd));
  });

  testWidgets('测试模式：本机检查不过也能继续（角上标出）；不写本机系统盘，不要 BitLocker 恢复密钥', (t) async {
    // blocked 的 preflight 是一台 PC 的真实结果：机型不对、Secure Boot、快速启动、BitLocker 都开着
    final (rec, s) = await pumpApp(t, 'blocked', testDisk: 2);
    await see(t, find.text(l.chkModelBad));
    expect(find.text(l.testBadge('2')), findsOneWidget);
    expect(find.text(l.chkBlockedNote), findsNothing);
    expect(find.text(l.chkBitlockerSuspendBox), findsNothing);
    expect(enabled(t, l.btnNext), isTrue);
    await tap(t, find.text(l.btnNext));
    await tap(t, find.text(l.btnNext));
    await see(t, find.textContaining('v1.0.0-rc.3'));
    await tap(t, find.text(l.btnNext));
    await tap(t, find.text(l.btnNext));
    await see(t, find.text(l.confirmAntivirus));
    expect(find.text(l.confirmBitlocker), findsNothing);
    await hold(t, l.holdToInstall);
    await waitFor(t, find.byType(DonePage), tries: 400);
    expect(find.byType(DonePage), findsOneWidget);
    final i = rec.calls.indexWhere((c) => c.first == 'apply');
    expect(rec.calls[i], isNot(contains('--bitlocker-key')));
    expect(rec.inputs[i], isNull);
    expect(rec.calls.where((c) => c.first == 'bitlocker-key'), isEmpty);
  });

  testWidgets('BitLocker 保护开着：恢复密钥通过校验后才能按住开始；密钥经标准输入交给 apply，用过即清除', (t) async {
    final (rec, s) = await pumpApp(t, 'free', map: {'preflight': bitlockerOn()}, respond: {'bitlocker-key': fakeBitlockerKey});
    await see(t, find.text(l.chkBitlockerOn));
    expect(find.textContaining('3F2A9C41'), findsWidgets);
    await tap(t, find.text(l.btnNext));
    await tap(t, find.text(l.btnNext));
    await see(t, find.textContaining('v1.0.0-rc.3'));
    await tap(t, find.text(l.btnNext));
    await tap(t, find.text(l.btnNext));
    await see(t, find.text(l.confirmBitlocker));
    expect(find.text(l.confirmAntivirus), findsOneWidget);
    // 位数不够：校验按钮不能按
    await t.enterText(find.byType(TextField), '123456-123456');
    await settle(t);
    expect(enabled(t, l.blVerify), isFalse);
    // 对不上：说明原因，按住不起作用
    await t.enterText(find.byType(TextField), kOtherKey);
    await tapShown(t, find.text(l.blVerify));
    await see(t, find.text(l.errBitlockerWrong));
    await hold(t, l.holdToInstall);
    expect(rec.calls.where((c) => c.first == 'apply'), isEmpty);
    // 正确的（空格分组）：通过；之后改了内容就要重新校验
    await t.enterText(find.byType(TextField), kGoodKey.replaceAll('-', ' '));
    await tapShown(t, find.text(l.blVerify));
    await see(t, find.text(l.blOk));
    // 输入框自动整理成连字符分组
    expect(t.widget<TextField>(find.byType(TextField)).controller!.text, kGoodKey);
    await t.enterText(find.byType(TextField), kGoodKey.substring(0, 20));
    await settle(t);
    expect(s.bitlockerVerified, isFalse);
    expect(find.text(l.blOk), findsNothing);
    await t.enterText(find.byType(TextField), kGoodKey);
    await tapShown(t, find.text(l.blVerify));
    await see(t, find.text(l.blOk));
    await hold(t, l.holdToInstall);
    await waitFor(t, find.byType(DonePage), tries: 400);
    expect(find.byType(DonePage), findsOneWidget);
    final i = rec.calls.indexWhere((c) => c.first == 'apply');
    expect(rec.calls[i], containsAllInOrder(['--real-disk', '--bitlocker-key', '-']));
    expect(rec.inputs[i], '$kGoodKey\n');
    // 校验的调用也只经标准输入传密钥
    for (final c in rec.calls.where((c) => c.first == 'bitlocker-key')) {
      expect(c, ['bitlocker-key']);
    }
    expect(s.bitlockerVerified, isFalse);
  });

  testWidgets('恢复密钥某一组校验不过：指出是第几组', (t) async {
    final (_, s) = await pumpApp(t, 'free', map: {'preflight': bitlockerOn()}, respond: {
      'bitlocker-key': (args, input) => Stream.fromIterable([const Gk3Record('ERR', {'code': 'bitlocker-key-format', 'group': '3'}), const Gk3Exit(1)]),
    });
    await t.runAsync(() => s.verifyBitlockerKey(kGoodKey));
    expect(errText(l, s.bitlockerErr!), l.errBitlockerGroup('3'));
  });

  testWidgets('出厂状态（Secure Boot 与 BitLocker 都开着）：先校验恢复密钥并暂停保护，再去关闭 Secure Boot', (t) async {
    final (rec, s) = await pumpApp(t, 'bitlocker');
    await see(t, find.text(l.chkSecureBootHowBitlocker));
    expect(find.text(l.chkBitlockerSuspendBox), findsOneWidget);
    expect(enabled(t, l.btnNext), isFalse);
    await t.enterText(find.byType(TextField), kGoodKey);
    await tapShown(t, find.text(l.blVerifySuspend));
    await see(t, find.text(l.chkBitlockerSuspended));
    final i = rec.calls.indexWhere((c) => c.first == 'bitlocker-key');
    expect(rec.calls[i], ['bitlocker-key', '--suspend', '2']);
    expect(rec.inputs[i], '$kGoodKey\n');
    // 暂停之后重新检查过：Secure Boot 一项回到普通的说明，不再要密钥
    expect(rec.calls.where((c) => c.first == 'preflight'), hasLength(2));
    expect(find.text(l.chkSecureBootHow), findsOneWidget);
    expect(find.text(l.chkBitlockerSuspendBox), findsNothing);
    expect(s.needBitlockerKey, isFalse);
    expect(enabled(t, l.btnNext), isFalse);
  });

  testWidgets('正在加密或解密系统盘：标红挡住，不要密钥', (t) async {
    await pumpApp(t, 'free', map: {
      'preflight': (e) => e is Gk3Record && e.type == 'PREFLIGHT' ? Gk3Record('PREFLIGHT', {...e.fields, 'bitlocker': 'converting'}) : e,
    });
    await see(t, find.text(l.chkBitlockerConverting));
    expect(enabled(t, l.btnNext), isFalse);
    expect(find.text(l.chkBitlockerSuspendBox), findsNothing);
  });

  testWidgets('校验并暂停保护期间不能重新检查', (t) async {
    final gate = StreamController<Gk3Event>();
    final (_, s) = await pumpApp(t, 'bitlocker', respond: {'bitlocker-key': (args, input) => gate.stream});
    await t.enterText(find.byType(TextField), kGoodKey);
    await tapShown(t, find.text(l.blVerifySuspend));
    expect(s.bitlockerChecking, isTrue);
    expect(enabled(t, l.btnRecheck), isFalse);
    gate
      ..add(const Gk3Record('ERR', {'code': 'bitlocker-key-wrong'}))
      ..add(const Gk3Exit(1));
    await gate.close();
    await settle(t);
    expect(s.bitlockerChecking, isFalse);
    expect(enabled(t, l.btnRecheck), isTrue);
  });

  testWidgets('系统盘没有恢复密钥：无从校验，标红挡住', (t) async {
    await pumpApp(t, 'free', map: {'preflight': bitlockerOn(ids: '')});
    await see(t, find.text(l.chkBitlockerNoKey));
    expect(find.text(l.chkBitlockerNoKeyHow), findsOneWidget);
    expect(enabled(t, l.btnNext), isFalse);
  });

  testWidgets('卸载也要恢复密钥：通过之后经标准输入交给 uninstall', (t) async {
    final (rec, _) = await pumpApp(t, 'installed', map: {'preflight': bitlockerOn()}, respond: {'bitlocker-key': fakeBitlockerKey});
    await tap(t, find.text(l.btnManage));
    await tap(t, find.text(l.manageUninstall));
    await see(t, find.text(l.confirmBitlocker));
    expect(find.text(l.confirmAntivirus), findsOneWidget);
    await hold(t, l.holdToUninstall);
    expect(rec.calls.where((c) => c.first == 'uninstall'), isEmpty);
    await t.enterText(find.byType(TextField), kGoodKey);
    await tapShown(t, find.text(l.blVerify));
    await see(t, find.text(l.blOk));
    await hold(t, l.holdToUninstall);
    await waitFor(t, find.byType(DonePage), tries: 400);
    final i = rec.calls.indexWhere((c) => c.first == 'uninstall');
    expect(rec.calls[i], containsAllInOrder(['--real-disk', '--bitlocker-key', '-']));
    expect(rec.inputs[i], '$kGoodKey\n');
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


  testWidgets('下载中途按关闭：先问；继续下载就不关，确认退出就停止下载、再关窗口', (t) async {
    final window = <MethodCall>[];
    t.binding.defaultBinaryMessenger.setMockMethodCallHandler(const MethodChannel('gk3/window'), (c) async {
      window.add(c);
      return null;
    });
    addTearDown(() => t.binding.defaultBinaryMessenger.setMockMethodCallHandler(const MethodChannel('gk3/window'), null));
    final (rec, s) = await pumpApp(t, 'free');
    final deleted = <String>[];
    s.deleteFiles = deleted.add;
    // 一直下不完的下载；记下它有没有被取消
    var cancelled = false;
    rec.replace['download'] = (_) {
      final c = StreamController<Gk3Event>();
      c.onCancel = () => cancelled = true;
      c.add(Gk3Progress(3, 'dl name=super.img.zst pct=10', code: 'dl', fields: const {'name': 'super.img.zst', 'pct': '10'}));
      return c.stream;
    };
    for (var i = 0; i < 4; i++) {
      await tap(t, find.text(l.btnNext));
    }
    await hold(t, l.holdToInstall);
    await waitFor(t, find.byType(RunPage));
    await settle(t);
    expect(s.downloading, isTrue);
    expect([for (final c in window) if (c.method == 'setBusy') c.arguments as Map].last,
        containsPair('confirmClose', true));

    await tap(t, find.bySemanticsLabel(l.winClose));
    await see(t, find.text(l.quitDownloadTitle));
    await tap(t, find.text(l.quitDownloadContinue));
    expect(find.text(l.quitDownloadTitle), findsNothing);
    expect(cancelled, isFalse);
    expect(deleted, isEmpty);
    expect(window.where((c) => c.method == 'forceClose' || c.method == 'close'), isEmpty);

    await tap(t, find.bySemanticsLabel(l.winClose));
    await tap(t, find.text(l.quitDownloadExit));
    await settle(t);
    expect(cancelled, isTrue);
    // 撤回：删掉这次下载的目录里的文件，再关
    expect(deleted, [rec.calls.singleWhere((c) => c.first == 'download').last]);
    expect(window.where((c) => c.method == 'forceClose'), hasLength(1));
    expect(rec.calls.where((c) => c.first == 'apply'), isEmpty);
  });

  /// 开始安装，停在下载这一步（下载由测试结束）；返回原生通道收到的调用与结束下载的办法
  Future<(Rec, Session, List<MethodCall>, void Function())> startPendingDownload(WidgetTester t) async {
    final window = <MethodCall>[];
    t.binding.defaultBinaryMessenger.setMockMethodCallHandler(const MethodChannel('gk3/window'), (c) async {
      window.add(c);
      return null;
    });
    addTearDown(() => t.binding.defaultBinaryMessenger.setMockMethodCallHandler(const MethodChannel('gk3/window'), null));
    final (rec, s) = await pumpApp(t, 'free');
    final c = StreamController<Gk3Event>();
    var dir = '';
    rec.replace['download'] = (args) {
      dir = args.last;
      c.add(Gk3Progress(3, 'dl name=super.img.zst pct=10', code: 'dl', fields: const {'name': 'super.img.zst', 'pct': '10'}));
      return c.stream;
    };
    for (var i = 0; i < 4; i++) {
      await tap(t, find.text(l.btnNext));
    }
    await hold(t, l.holdToInstall);
    await waitFor(t, find.byType(RunPage));
    await settle(t);
    expect(s.downloading, isTrue);
    return (
      rec,
      s,
      window,
      () {
        c
          ..add(Gk3Record('RELEASE', {'dir': dir, 'source': 'net'}))
          ..add(const Gk3Exit(0));
        c.close();
      }
    );
  }

  testWidgets('确认框开着时下载完了：先不写盘、等答案；选退出就删掉刚下完的文件，不写盘，关窗口', (t) async {
    final (rec, s, window, finish) = await startPendingDownload(t);
    final deleted = <String>[];
    s.deleteFiles = deleted.add;
    await tap(t, find.bySemanticsLabel(l.winClose));
    await see(t, find.text(l.quitDownloadTitle));
    finish();
    await settle(t);
    expect(s.downloading, isFalse);
    expect(rec.calls.where((c) => c.first == 'apply'), isEmpty);
    // 框还开着：原生那边仍然先问（Alt+F4 不会直接关掉）
    expect([for (final c in window) if (c.method == 'setBusy') c.arguments as Map].last, containsPair('confirmClose', true));
    await tap(t, find.text(l.quitDownloadExit));
    await settle(t);
    expect(rec.calls.where((c) => c.first == 'apply'), isEmpty);
    expect(deleted, [rec.calls.singleWhere((c) => c.first == 'download').last]);
    expect(window.where((c) => c.method == 'forceClose'), hasLength(1));
  });

  testWidgets('确认框开着时下载完了：选继续就接着安装', (t) async {
    final (rec, s, window, finish) = await startPendingDownload(t);
    final deleted = <String>[];
    s.deleteFiles = deleted.add;
    await tap(t, find.bySemanticsLabel(l.winClose));
    finish();
    await settle(t);
    expect(rec.calls.where((c) => c.first == 'apply'), isEmpty);
    await tap(t, find.text(l.quitDownloadContinue));
    await waitFor(t, find.byType(DonePage), tries: 400);
    expect(rec.calls.where((c) => c.first == 'apply'), hasLength(1));
    expect(deleted, isEmpty);
    expect(window.where((c) => c.method == 'forceClose' || c.method == 'close'), isEmpty);
  });

  testWidgets('选了退出、还在停止下载时再按关闭：不再弹框，收尾完关窗口一次', (t) async {
    final window = <MethodCall>[];
    t.binding.defaultBinaryMessenger.setMockMethodCallHandler(const MethodChannel('gk3/window'), (c) async {
      window.add(c);
      return null;
    });
    addTearDown(() => t.binding.defaultBinaryMessenger.setMockMethodCallHandler(const MethodChannel('gk3/window'), null));
    final (rec, s) = await pumpApp(t, 'free');
    final deleted = <String>[];
    s.deleteFiles = deleted.add;
    final gate = Completer<void>();
    rec.replace['download'] = (_) {
      final c = StreamController<Gk3Event>();
      // 停止要过一会儿（例如正在算 sha256）
      c.onCancel = () => gate.future;
      return c.stream;
    };
    for (var i = 0; i < 4; i++) {
      await tap(t, find.text(l.btnNext));
    }
    await hold(t, l.holdToInstall);
    await waitFor(t, find.byType(RunPage));
    await settle(t);
    await tap(t, find.bySemanticsLabel(l.winClose));
    await tap(t, find.text(l.quitDownloadExit));
    expect(s.downloading, isTrue);
    await tap(t, find.bySemanticsLabel(l.winClose));
    unawaited(handleCloseRequest(kNavigatorKey.currentContext!, s));
    await settle(t);
    expect(find.text(l.quitDownloadTitle), findsNothing);
    expect(window.where((c) => c.method == 'forceClose'), isEmpty);
    gate.complete();
    await settle(t);
    expect(deleted, hasLength(1));
    expect(window.where((c) => c.method == 'forceClose'), hasLength(1));
  });

  testWidgets('确认框只弹一个：按了关闭键之后又按 Alt+F4（原生转来 closeRequested）', (t) async {
    final (_, s, window, _) = await startPendingDownload(t);
    await tap(t, find.bySemanticsLabel(l.winClose));
    await see(t, find.text(l.quitDownloadTitle));
    unawaited(handleCloseRequest(kNavigatorKey.currentContext!, s));
    await settle(t);
    expect(find.text(l.quitDownloadTitle), findsOneWidget);
    await tap(t, find.text(l.quitDownloadContinue));
    expect(find.text(l.quitDownloadTitle), findsNothing);
    expect(s.downloading, isTrue);
    expect(window.where((c) => c.method == 'forceClose'), isEmpty);
  });

  testWidgets('两次取消下载同时进行：第二次也等文件删完才返回（之后才关窗口）', (t) async {
    final (rec, s) = await pumpApp(t, 'free');
    final deleted = <String>[];
    s.deleteFiles = deleted.add;
    final gate = Completer<void>();
    rec.replace['download'] = (_) {
      final c = StreamController<Gk3Event>();
      // 取消要过一会儿才收拾完（真的下载要断开连接、关掉 .part）
      c.onCancel = () => gate.future;
      return c.stream;
    };
    for (var i = 0; i < 4; i++) {
      await tap(t, find.text(l.btnNext));
    }
    await hold(t, l.holdToInstall);
    await waitFor(t, find.byType(RunPage));
    await settle(t);
    expect(s.downloading, isTrue);
    var a = false, b = false;
    unawaited(s.cancelDownload().then((_) => a = true));
    unawaited(s.cancelDownload().then((_) => b = true));
    await settle(t, 5);
    expect((a, b), (false, false));
    expect(deleted, isEmpty);
    gate.complete();
    await settle(t, 5);
    expect((a, b), (true, true));
    expect(deleted, hasLength(1));
  });

  testWidgets('最大化按钮：调原生的 maximize；原生报告最大化之后换成"向下还原"', (t) async {
    final window = <MethodCall>[];
    t.binding.defaultBinaryMessenger.setMockMethodCallHandler(const MethodChannel('gk3/window'), (c) async {
      window.add(c);
      return null;
    });
    addTearDown(() {
      t.binding.defaultBinaryMessenger.setMockMethodCallHandler(const MethodChannel('gk3/window'), null);
      WindowControl.maximized.value = false;
    });
    await pumpApp(t, 'free');
    await tap(t, find.bySemanticsLabel(l.winMaximize));
    expect(window.where((c) => c.method == 'maximize'), hasLength(1));
    WindowControl.maximized.value = true;
    await settle(t);
    expect(find.bySemanticsLabel(l.winRestore), findsOneWidget);
    // 画出来之后告诉原生标题栏在哪（让系统拖动：鼠标、触摸、手写笔）
    expect(window.where((c) => c.method == 'setCaption').map((c) => c.arguments),
        anyElement(equals({'canvasW': 1280.0, 'canvasH': 800.0, 'strip': 32.0, 'buttons': 138.0})));
  });


  testWidgets('窗口不是 16:10（最大化后的工作区）：画布顺着多出来的方向加宽、铺满窗口，两边不留空', (t) async {
    await pumpApp(t, 'free');
    // 平板 200 % 缩放下最大化：工作区 1280×752 逻辑像素
    t.view.physicalSize = const Size(2560, 1504);
    t.view.devicePixelRatio = 2;
    await settle(t);
    expect(t.getTopRight(find.byType(WindowButtons)).dx, closeTo(1280, 0.5));
    expect(t.getTopLeft(find.byType(DragStrip)).dx, closeTo(0, 0.5));
    expect(t.getBottomLeft(find.byType(CheckPage)).dy, closeTo(752, 0.5));
    expect(t.takeException(), isNull);
    // 100 % 缩放的 1920×1080 显示器上最大化：工作区比画布大，放大到铺满（与原生 InCaption 的算法相同：
    // s = min(1920/1280, 1032/800)，标题栏是顶上 32·s 高的一行，按钮在最右边 138·s 宽）
    t.view.physicalSize = const Size(1920, 1032);
    t.view.devicePixelRatio = 1;
    await settle(t);
    const s = 1032 / 800;
    expect(t.getTopRight(find.byType(WindowButtons)).dx, closeTo(1920, 0.5));
    expect(t.getTopLeft(find.byType(WindowButtons)).dx, closeTo(1920 - WindowButtons.width * s, 0.5));
    expect(t.getTopLeft(find.byType(DragStrip)).dy, closeTo(0, 0.5));
    expect(t.getBottomLeft(find.byType(DragStrip)).dy, closeTo(DragStrip.height * s, 0.5));
    expect(t.getBottomLeft(find.byType(CheckPage)).dy, closeTo(1032, 0.5));
    expect(t.takeException(), isNull);
  });

  testWidgets('下载到"下载"文件夹的 Gaokun Android\\<版本>；装好后勾选"退出时删除"，关闭窗口时先删再关', (t) async {
    final window = <MethodCall>[];
    t.binding.defaultBinaryMessenger.setMockMethodCallHandler(const MethodChannel('gk3/window'), (c) async {
      window.add(c);
      return null;
    });
    addTearDown(() => t.binding.defaultBinaryMessenger.setMockMethodCallHandler(const MethodChannel('gk3/window'), null));
    final (rec, s) = await pumpApp(t, 'free');
    final deleted = <String>[];
    s
      ..downloadsDir = (() async => r'D:\Users\me\Downloads')
      ..deleteFiles = deleted.add;
    for (var i = 0; i < 4; i++) {
      await tap(t, find.text(l.btnNext));
    }
    await hold(t, l.holdToInstall);
    await waitFor(t, find.byType(DonePage), tries: 400);
    expect(rec.calls.singleWhere((c) => c.first == 'download').last, r'D:\Users\me\Downloads\Gaokun Android\v1.0.0-rc.3');
    // 默认不删
    final box = find.text(l.doneDeleteDownloads('1.3 GiB'));
    await see(t, box);
    expect(s.deleteOnExit, isFalse);
    await tap(t, box);
    expect(s.deleteOnExit, isTrue);
    expect([for (final c in window) if (c.method == 'setBusy') c.arguments as Map].last, containsPair('confirmClose', true));
    // "稍后重启"回到首页：还没退出，不删
    await tap(t, find.text(l.btnLater));
    expect(deleted, isEmpty);
    // 关闭窗口：先删，再关
    await tap(t, find.bySemanticsLabel(l.winClose));
    // 删的是下载报告的 RELEASE 目录（真后端就是传进去的那个；回放数据里是录制时的目录）
    expect(deleted, [r'C:\ProgramData\gaokun3\release']);
    expect(window.where((c) => c.method == 'forceClose'), hasLength(1));
  });

  testWidgets('装好后不勾选：关闭窗口时什么都不删，直接关', (t) async {
    final window = <MethodCall>[];
    t.binding.defaultBinaryMessenger.setMockMethodCallHandler(const MethodChannel('gk3/window'), (c) async {
      window.add(c);
      return null;
    });
    addTearDown(() => t.binding.defaultBinaryMessenger.setMockMethodCallHandler(const MethodChannel('gk3/window'), null));
    final (_, s) = await pumpApp(t, 'free');
    final deleted = <String>[];
    s.deleteFiles = deleted.add;
    for (var i = 0; i < 4; i++) {
      await tap(t, find.text(l.btnNext));
    }
    await hold(t, l.holdToInstall);
    await waitFor(t, find.byType(DonePage), tries: 400);
    await tap(t, find.bySemanticsLabel(l.winClose));
    expect(deleted, isEmpty);
    expect(window.where((c) => c.method == 'forceClose'), hasLength(1));
    expect(window.where((c) => c.method == 'close'), isEmpty);
  });

  testWidgets('勾了"退出时删除"后按立即重启却没有重启（被别的程序拦下、测试模式）：文件已删，原生不再先问，关窗口直接关', (t) async {
    final window = <MethodCall>[];
    t.binding.defaultBinaryMessenger.setMockMethodCallHandler(const MethodChannel('gk3/window'), (c) async {
      window.add(c);
      return null;
    });
    addTearDown(() => t.binding.defaultBinaryMessenger.setMockMethodCallHandler(const MethodChannel('gk3/window'), null));
    final (_, s) = await pumpApp(t, 'free');
    final deleted = <String>[];
    s.deleteFiles = deleted.add;
    for (var i = 0; i < 4; i++) {
      await tap(t, find.text(l.btnNext));
    }
    await hold(t, l.holdToInstall);
    await waitFor(t, find.byType(DonePage), tries: 400);
    await tap(t, find.text(l.doneDeleteDownloads('1.3 GiB')));
    // 回放后端的 reboot 什么都不做：就是"没有重启"
    await tap(t, find.text(l.btnRebootNow));
    expect(deleted, hasLength(1));
    expect([for (final c in window) if (c.method == 'setBusy') c.arguments as Map].last, containsPair('confirmClose', false));
    await tap(t, find.bySemanticsLabel(l.winClose));
    expect(deleted, hasLength(1));
    expect(window.where((c) => c.method == 'forceClose'), hasLength(1));
    expect(window.where((c) => c.method == 'close'), isEmpty);
  });


  testWidgets('英文界面：检查页上没有中文', (t) async {
    await pumpApp(t, 'blocked', language: 'en');
    await see(t, find.text(en.chkModelBad));
    expect(cjkOnScreen(t), isEmpty);
  });
}
