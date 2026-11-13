// 流程测试：用真后端录的 fixture 把每条路从头点到尾，并核对最后发给后端的调用。
import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:gk3_common/backend/fixture_backend.dart';
import 'package:gk3_common/backend/protocol.dart';
import 'package:gk3_common/ui/widgets.dart';
import 'package:gk3_installer/l10n/app_localizations.dart';
import 'package:gk3_installer/model/model.dart';
import 'package:gk3_installer/session.dart';
import 'package:gk3_installer/ui/messages.dart';
import 'package:gk3_installer/ui/screens_risk.dart';

import 'helpers.dart';

void main() {
  testWidgets('windows-free：双系统一路装到完成，apply 的参数对', (t) async {
    final rec = await pumpApp(t, 'windows-free');
    await see(t, find.text(l.welcomeTitle));
    await tap(t, find.text(l.btnStart));
    await see(t, find.text(l.diskTitle));
    // 安装 U 盘列出来了、但禁用并写明原因（C 版是直接藏起来）
    await see(t, find.text(l.diskMedium));
    await tap(t, find.textContaining('/dev/nvme0n1'));
    await next(t);
    await see(t, find.text(l.modeTitle));
    await see(t, find.text(l.modeAlongOk));
    await tap(t, find.text(l.modeAlongTitle));
    await next(t);
    await see(t, find.text(l.optsTitle));
    await see(t, find.textContaining('/data'), findsWidgets);
    await next(t);
    await passRisk(t);
    await see(t, find.text(l.confirmTitle));
    await see(t, find.text(l.confirmAlongHead));
    await see(t, find.text(l.confirmWindowsKept));
    // 值落在对的位置（占位符顺序那个 bug：曾渲染成"创建 /dev/nvme0n1p1 的分区…EFI 分区 80 GiB"）
    await see(t, find.textContaining(tail(l.confirmAlongBody(hole, '/dev/nvme0n1p1'))));
    // 要安装的版本：安装介质上的系统文件（gk3_release_info 的 version）
    await see(t, find.text(l.confirmVersion('crDroidAndroid-16.0-20260916-gaokun3-v12.11')));
    await hold(t, l.confirmHoldIdle);
    await see(t, find.text(l.doneTitle));
    final a = rec.last('gk3_apply')!.join(' ');
    expect(a, contains('--mode alongside'));
    expect(a, contains('--rescue yes'));
    expect(a, contains('--region-start 790497280 --region-end 958269439'));
    expect(a, contains('--esp /dev/nvme0n1p1'));
    expect(a, contains('--release /media/gk3/gaokun3/payload'));
  });

  testWidgets('windows-setup（Windows 脚本只划了 GK3LIVE、没有空闲）：双系统禁用并报"至少要多少"，安装器里不缩分区', (t) async {
    await pumpApp(t, 'windows-setup');
    await tap(t, find.text(l.btnStart));
    await tap(t, find.textContaining('/dev/nvme0n1'));
    await next(t);
    await see(t, find.text(l.modeTitle));
    // 安装器就在这块盘上（GK3LIVE）⇒ 默认不另装救援（GUI-17），所以是不带救援的那个数（20644 MiB = 20.2 GiB）
    await see(t, find.text(l.modeWhyNoFree(fmtMib(20644))));
    expect(find.text(l.modeAlongOk), findsNothing);
    // 腾空间要回 Windows 压缩卷；这里只剩删除分区
    expect(find.text(l.editEntryTitle), findsOneWidget);
  });

  testWidgets('windows-setup-shrunk（在 Windows 里压缩出空间之后）→ 双系统装进 Data 与 GK3LIVE 之间', (t) async {
    final rec = await pumpApp(t, 'windows-setup-shrunk');
    await tap(t, find.text(l.btnStart));
    await tap(t, find.textContaining('/dev/nvme0n1'));
    await next(t);
    await see(t, find.text(l.modeAlongOk));
    await tap(t, find.text(l.modeAlongTitle));
    await next(t);
    await next(t); // 选项
    await passRisk(t);
    await see(t, find.text(l.confirmAlongHead));
    await hold(t, l.confirmHoldIdle);
    await see(t, find.text(l.doneTitle));
    final a = rec.last('gk3_apply')!.join(' ');
    expect(a, contains('--mode alongside'));
    expect(a, contains('--region-start 789448704 --region-end 957220863')); // Data 缩出来的那段，紧挨在 GK3LIVE 前面
  });

  testWidgets('windows-live（免 U 盘装双系统）：安装器跑在内置盘上 → 盘可选、整盘清空禁用、双系统一路装完', (t) async {
    final rec = await pumpApp(t, 'windows-live');
    await tap(t, find.text(l.btnStart));
    await see(t, find.text(l.diskTitle));
    // 没有 U 盘；内置盘就是介质 —— 不禁用，只提示"只能装在空闲空间里"
    expect(find.text(l.diskMedium), findsNothing);
    await see(t, find.text(l.diskMediumInternal));
    await tap(t, find.textContaining('/dev/nvme0n1'));
    await next(t);
    await see(t, find.text(l.modeTitle));
    await see(t, find.text(l.modeWhyMedium));
    // 点"清除整个磁盘"没反应：下一步仍是灰的
    await tap(t, find.text(l.modeWipeTitle));
    expect(find.ancestor(of: find.text(l.btnNext), matching: find.byWidgetPredicate((w) => w is ButtonStyleButton && w.onPressed != null)), findsNothing);
    await see(t, find.text(l.modeAlongOk));
    await tap(t, find.text(l.modeAlongTitle));
    await next(t);
    await next(t); // 选项
    await passRisk(t);
    await see(t, find.text(l.confirmAlongHead));
    await see(t, find.text(l.confirmWindowsKept));
    await hold(t, l.confirmHoldIdle);
    await see(t, find.text(l.doneTitle));
    final a = rec.last('gk3_apply')!.join(' ');
    expect(a, contains('--mode alongside'));
    expect(a, contains('--region-start 798885888 --region-end 958269439'));   // GK3LIVE 之后那段空闲
    expect(a, contains('--esp /dev/nvme0n1p1'));
  });

  testWidgets('退出到终端切不过去：SnackBar 说出原因与替代办法（M0 实测：镜像里没有 chvt，按钮点了没反应）', (t) async {
    await pumpApp(t, 'windows-free', shellError: 'busybox: No such file or directory');
    await tap(t, find.text(l.btnQuit));
    await see(t, find.textContaining('Ctrl+Alt+F2'));
    await see(t, find.textContaining('busybox: No such file or directory'));
  });

  testWidgets('factory：没有空闲区 → 双系统禁用并报"至少要多少"（后端报的数）', (t) async {
    await pumpApp(t, 'factory');
    await tap(t, find.text(l.btnStart));
    await tap(t, find.textContaining('/dev/nvme0n1'));
    await next(t);
    // 21668 MiB = 21.2 GiB —— 来自 gk3_plan 的 PLANERR need_mib，不是界面写死的
    await see(t, find.text(l.modeWhyNoFree(fmtMib(21668))));
    expect(find.text(l.modeAlongOk), findsNothing);
  });

  testWidgets('blank：整盘安装；确认页如实说"没有任何分区"', (t) async {
    final rec = await pumpApp(t, 'blank');
    await tap(t, find.text(l.btnStart));
    await tap(t, find.textContaining('/dev/nvme0n1'));
    await next(t);
    // 空盘没有 ESP：双系统禁用并写明原因
    await see(t, find.text(l.modeWhyNoEsp));
    await tap(t, find.text(l.modeWipeTitle));
    await next(t);
    await next(t); // 选项
    await passRisk(t);
    await see(t, find.text(l.confirmWipeHead));
    await see(t, find.text(l.confirmNoParts));
    await hold(t, l.confirmHoldIdle);
    await see(t, find.text(l.doneTitle));
    expect(rec.last('gk3_apply')!.join(' '), contains('--mode wipe'));
  });

  testWidgets('MBR 盘与读不出分区表的盘（S1 / S2）：读不出的盘禁用并写明原因；MBR 盘只给整盘清空，整盘清空照常装完', (t) async {
    final rec = await pumpApp(t, 'blank', overrides: {'gk3_probe': 'probe-mbr-unreadable.txt'});
    await tap(t, find.text(l.btnStart));
    // 读不出的盘：列出来（让人知道它在），但禁用、写明原因，不说"没有分区"（S1 原来把它报成一整块空闲）
    await see(t, find.text(l.diskUnreadable));
    final unreadable = find.ancestor(of: find.textContaining('/dev/sdb'), matching: find.byType(ChoiceCard));
    expect(t.widget<ChoiceCard>(unreadable).reason, l.diskUnreadable);
    expect(find.descendant(of: unreadable, matching: find.textContaining(l.diskNoParts)), findsNothing);
    await tap(t, find.textContaining('/dev/sdb'));
    expect(t.widget<ChoiceCard>(unreadable).selected, isFalse);
    // MBR 盘：双系统与手动调整都写明"MBR、只能整盘清空"，不去问 ESP、不去算双系统方案
    await tap(t, find.textContaining('/dev/nvme0n1'));
    await next(t);
    await see(t, find.text(l.errMbr), findsNWidgets(2));
    expect(rec.calls.where((c) => c.first == 'gk3_esp_info' || (c.first == 'gk3_plan' && c.contains('alongside'))), isEmpty);
    for (final title in [l.modeAlongTitle, l.editEntryTitle]) {
      final card = find.ancestor(of: find.text(title), matching: find.byType(ChoiceCard));
      expect(t.widget<ChoiceCard>(card).reason, l.errMbr, reason: title);
    }
    await tap(t, find.text(l.modeWipeTitle));
    await next(t);
    await next(t); // 选项
    await passRisk(t);
    await hold(t, l.confirmHoldIdle);
    await see(t, find.text(l.doneTitle));
    expect(rec.last('gk3_apply')!.join(' '), contains('--disk /dev/nvme0n1 --mode wipe'));
  });

  testWidgets('写盘入口拒绝 MBR 盘 / 读不出的盘（ERR mbr-disk / disk-unreadable）：失败的话按代码说', (t) async {
    final mbr = Gk3Record('ERR', {'code': 'mbr-disk', 'disk': '/dev/nvme0n1', 'touched': 'no'});
    final bad = Gk3Record('ERR', {'code': 'disk-unreadable', 'disk': '/dev/sdb', 'touched': 'no'});
    expect(errText(l, mbr), l.errMbr);
    expect(errText(l, bad), l.errDiskUnreadable('/dev/sdb'));
    expect(errText(en, bad), en.errDiskUnreadable('/dev/sdb'));
  });

  testWidgets('android（已经装过）：双系统禁用，原因里列出冲突的分区名', (t) async {
    await pumpApp(t, 'android');
    await tap(t, find.text(l.btnStart));
    await tap(t, find.textContaining('/dev/nvme0n1'));
    await next(t);
    await see(t, find.textContaining('super'), findsWidgets);
    expect(find.text(l.modeAlongOk), findsNothing);
  });

  testWidgets('android（已经装过）→ 重新安装：默认清除数据，确认页逐个列出，apply 走 reinstall', (t) async {
    final rec = await pumpApp(t, 'android');
    await tap(t, find.text(l.btnStart));
    await tap(t, find.textContaining('/dev/nvme0n1'));
    await next(t);
    await see(t, find.text(l.modeReinstallTitle));
    await tap(t, find.text(l.modeReinstallTitle));
    await next(t); // 选项
    await see(t, find.text(l.optsKeepTitle));
    expect(find.text(l.optsAdvanced), findsNothing);   // 不改分区表：没有分区大小可调
    await see(t, find.textContaining(lead(l.optsDataWiped(hole))));
    await next(t);
    await passRisk(t);
    await see(t, find.text(l.confirmReinstallHead));
    await see(t, find.text(l.actFormat), findsNWidgets(2));   // userdata、metadata
    await see(t, find.text(l.actWrite), findsNWidgets(5));    // misc、boot_a、boot_b、super、gk3rescue
    await hold(t, l.confirmHoldIdle);
    await see(t, find.text(l.doneTitle));
    final a = rec.last('gk3_apply')!.join(' ');
    expect(a, contains('--mode reinstall'));
    expect(a, contains('--keep-data no'));
    expect(a, contains('--esp /dev/nvme0n1p1'));
    expect(a, isNot(contains('--region-start')));
  });

  testWidgets('重新安装 + 保留数据：确认页写"保留"，apply 带 --keep-data yes', (t) async {
    final rec = await pumpApp(t, 'android');
    await tap(t, find.text(l.btnStart));
    await tap(t, find.textContaining('/dev/nvme0n1'));
    await next(t);
    await tap(t, find.text(l.modeReinstallTitle));
    await next(t);
    await tap(t, find.text(l.optsKeepTitle));
    await see(t, find.textContaining(lead(l.optsDataKept(hole))));
    await next(t);
    await passRisk(t);
    await see(t, find.text(l.actKeep), findsNWidgets(2));
    await hold(t, l.confirmHoldIdle);
    await see(t, find.text(l.doneTitle));
    expect(rec.last('gk3_apply')!.join(' '), contains('--keep-data yes'));
  });

  Future<Rec> openEdit(WidgetTester t, String scenario, {Map<String, String> overrides = const {}}) async {
    final rec = await pumpApp(t, scenario, overrides: overrides);
    await tap(t, find.text(l.btnStart));
    await tap(t, find.textContaining('/dev/nvme0n1'));
    await next(t);
    await tap(t, find.text(l.editEntryTitle));
    await next(t);
    await passRisk(t);
    await see(t, find.text(l.editTitle));
    return rec;
  }

  testWidgets('删除分区：删除华为一键恢复分区 —— 说明后果、按住确认，调用对，完成后回到安装方式页重新评估', (t) async {
    final rec = await openEdit(t, 'factory');
    await tap(t, find.textContaining('Onekey'));
    await tap(t, find.text(l.editDelete));
    await see(t, find.text(l.editDeleteWarn));
    await see(t, find.text(l.editDeleteOnekey));
    expect(find.text(l.editDeleteWinre), findsNothing);
    await hold(t, '${l.editDelete} · ${l.holdIdle}');
    expect(rec.last('gk3_part_delete'), ['gk3_part_delete', '/dev/nvme0n1p6']);
    await see(t, find.text(l.editOk('Onekey')));
    final probes = rec.calls.where((c) => c.first == 'gk3_probe').length;
    await tap(t, find.text(l.editDone)); // 右下角"完成" → 回到方式页
    await see(t, find.text(l.modeTitle));
    expect(rec.calls.where((c) => c.first == 'gk3_plan').isNotEmpty, isTrue);
    expect(rec.calls.where((c) => c.first == 'gk3_probe').length, greaterThanOrEqualTo(probes));
  });

  testWidgets('删除分区：Windows 恢复分区写明删除后恢复环境无法使用', (t) async {
    await openEdit(t, 'factory');
    await tap(t, find.textContaining('WinRE'));
    await tap(t, find.text(l.editDelete));
    await see(t, find.text(l.editDeleteWinre));
    expect(find.text(l.editDeleteOnekey), findsNothing);
  });

  testWidgets('删除分区：ESP 只说原因、不给按钮；未分配空间不能选', (t) async {
    await openEdit(t, 'windows-free');
    await tap(t, find.textContaining('SYSTEM'));
    await see(t, find.text(l.editWhyEsp));
    expect(find.text(l.editDelete), findsNothing);
    // 未分配空间点了不改变选择
    await tap(t, find.text(l.editFree));
    await see(t, find.text(l.editWhyEsp));
  });

  testWidgets('删除分区：后端拒绝时把原因显示出来', (t) async {
    await openEdit(t, 'factory', overrides: {'gk3_part_delete': 'part_delete-esp.txt'});
    await tap(t, find.textContaining('Onekey'));
    await tap(t, find.text(l.editDelete));
    await hold(t, '${l.editDelete} · ${l.holdIdle}');
    await see(t, find.text(l.editFailed(l.editWhyEsp)));
  });

  testWidgets('预检：型号不对 → 拦住，写明当前型号', (t) async {
    await pumpApp(t, 'blank', overrides: {'gk3_preflight': 'preflight-model.txt'});
    await see(t, find.text(l.checkBlocked));
    await see(t, find.text(l.checkModelBadTitle));
    await see(t, find.text(l.checkModelBad('GK-W57')));
    final btn = t.widget<ButtonStyleButton>(find.ancestor(of: find.text(l.btnStart), matching: find.byWidgetPredicate((w) => w is ButtonStyleButton)).first);
    expect(btn.onPressed, isNull);
  });

  testWidgets('安装介质里没有系统文件：三种安装方式都不可选并写明原因，删除分区照常可选', (t) async {
    await pumpApp(t, 'android', overrides: {'gk3_release_info': 'release_info-none.txt'});
    await tap(t, find.text(l.btnStart));
    await tap(t, find.textContaining('/dev/nvme0n1'));
    await next(t);
    await see(t, find.text(l.modeTitle));
    // 重新安装、清除整个磁盘、保留现有系统各写一次原因
    await see(t, find.text(l.modeWhyNoRelease), findsNWidgets(3));
    Finder nextOn() => find.ancestor(of: find.text(l.btnNext), matching: find.byWidgetPredicate((w) => w is ButtonStyleButton && w.onPressed != null));
    await tap(t, find.text(l.modeAlongTitle));
    expect(nextOn(), findsNothing);
    await tap(t, find.text(l.editEntryTitle));
    expect(nextOn(), findsOneWidget);
  });

  testWidgets('返回走的是真正来的那一页（C 版 screen-- 会掉进没走过的页）', (t) async {
    await pumpApp(t, 'windows-free');
    await tap(t, find.text(l.btnStart));
    await tap(t, find.textContaining('/dev/nvme0n1'));
    await next(t);
    await tap(t, find.text(l.modeAlongTitle));
    await next(t);
    await see(t, find.text(l.optsTitle));
    await tap(t, find.text(l.btnBack));
    await see(t, find.text(l.modeTitle));
  });

  testWidgets('安装失败：失败页给出错误、说明分区表有备份、不许返回', (t) async {
    await pumpApp(t, 'blank', failApply: true);
    await tap(t, find.text(l.btnStart));
    await tap(t, find.textContaining('/dev/nvme0n1'));
    await next(t);
    await tap(t, find.text(l.modeWipeTitle));
    await next(t);
    await next(t);
    await passRisk(t);
    await hold(t, l.confirmHoldIdle);
    await see(t, find.text(l.failTitle));
    await see(t, find.textContaining('分区 super 没解析出来'), findsWidgets);
    await see(t, find.text(l.failBackup));
    expect(find.text(l.btnBack), findsNothing);
    // 写盘阶段失败：不给重试、不说"盘没动"（v1.0 计划 GUI-3）
    expect(find.text(l.btnRetry), findsNothing);
  });

  ButtonStyleButton railBtn(WidgetTester t, String label) =>
      t.widget<ButtonStyleButton>(find.ancestor(of: find.text(label), matching: find.byWidgetPredicate((w) => w is ButtonStyleButton)).first);

  testWidgets('侧栏的关机 / 重新启动：先问一句；取消就什么都不做（v1.0 计划 GUI-3）', (t) async {
    final rec = await pumpApp(t, 'windows-free');
    await see(t, find.text(l.welcomeTitle));
    await tap(t, find.text(l.railPoweroff));
    await see(t, find.text(l.powerPoweroffTitle));
    await see(t, find.text(l.powerBody));
    await tap(t, find.text('取消'));
    expect(rec.power, isEmpty);
    await tap(t, find.text(l.railPoweroff));
    await tap(t, find.text(l.powerPoweroffTitle.replaceAll(RegExp(r'[？?]$'), '')));
    expect(rec.power, ['poweroff']);
    await tap(t, find.text(l.railReboot));
    await tap(t, find.text(l.powerRebootTitle.replaceAll(RegExp(r'[？?]$'), '')));
    expect(rec.power, ['poweroff', 'reboot']);
  });

  testWidgets('写盘期间侧栏的重启 / 关机禁用并写明原因；装完恢复', (t) async {
    final gate = Completer<void>();
    final rec = await pumpApp(t, 'blank', holdApply: gate);
    expect(railBtn(t, l.railReboot).onPressed, isNotNull);
    await tap(t, find.text(l.btnStart));
    await tap(t, find.textContaining('/dev/nvme0n1'));
    await next(t);
    await tap(t, find.text(l.modeWipeTitle));
    await next(t);
    await next(t);
    await passRisk(t);
    await hold(t, l.confirmHoldIdle);
    await see(t, find.text(l.runTitle));
    await see(t, find.text(l.railBusy));
    expect(railBtn(t, l.railReboot).onPressed, isNull);
    expect(railBtn(t, l.railPoweroff).onPressed, isNull);
    gate.complete();
    await see(t, find.text(l.doneTitle));
    expect(find.text(l.railBusy), findsNothing);
    expect(railBtn(t, l.railPoweroff).onPressed, isNotNull);
    expect(rec.power, isEmpty);
  });

  testWidgets('分区大小页：/data 拖到 32 GiB 以下时黄色提醒（v1.0 计划 GUI-20）', (t) async {
    await pumpApp(t, 'windows-free');
    await tap(t, find.text(l.btnStart));
    await tap(t, find.textContaining('/dev/nvme0n1'));
    await next(t);
    await tap(t, find.text(l.modeAlongTitle));
    await next(t);
    await see(t, find.text(l.optsTitle));
    expect(find.textContaining(tail(l.dataSmallWarn(hole))), findsNothing); // 默认 66.8 GiB
    await tap(t, find.text(l.optsAdvanced));
    await see(t, find.text(l.advTitle));
    expect(find.textContaining(tail(l.dataSmallWarn(hole))), findsNothing);
    await t.drag(find.byType(Slider), const Offset(-2000, 0));
    await settle(t);
    await see(t, find.textContaining(tail(l.dataSmallWarn(hole))));
  });

  // ── v1.0 批 3：安装器前端与协议（INST-10 / GUI-4/5/8/9/14/17/18/19）──────────────────────────────

  testWidgets('英文界面不出现 CJK 字符：U 盘双系统一路到完成，每一页都查（INST-10 / GUI-4 / GUI-14）', (t) async {
    await pumpApp(t, 'windows-free', language: 'en');
    void check(String page) => expect(cjkOnScreen(t), isEmpty, reason: '英文界面的「$page」页出现了中文');
    await see(t, find.text(en.welcomeTitle));
    check('welcome');
    await tap(t, find.text(en.btnStart));
    await see(t, find.text(en.diskTitle));
    await tap(t, find.textContaining('/dev/nvme0n1'));
    check('disk');
    await next(t, en);
    await see(t, find.text(en.modeAlongOk));
    check('mode');
    await tap(t, find.text(en.modeAlongTitle));
    await next(t, en);
    await see(t, find.text(en.optsTitle));
    check('opts');
    await next(t, en);
    await passRisk(t, loc: en, check: () => check('risk'));
    await see(t, find.text(en.confirmTitle));
    check('confirm');
    await hold(t, en.confirmHoldIdle);
    await see(t, find.text(en.doneTitle));
    check('done');
    // 侧栏原先写死"安装器"（GUI-4）
    expect(find.textContaining('Installer 0.'), findsOneWidget);
  });

  testWidgets('apply 在第一次写盘之前失败（ERR touched=no）：失败页说"盘没动过"、给返回与重试，不说"写了一半"', (t) async {
    final rec = await pumpApp(t, 'blank', failApplyUntouched: true);
    await tap(t, find.text(l.btnStart));
    await tap(t, find.textContaining('/dev/nvme0n1'));
    await next(t);
    await tap(t, find.text(l.modeWipeTitle));
    await next(t);
    await next(t);
    await passRisk(t);
    await hold(t, l.confirmHoldIdle);
    await see(t, find.text(l.failUntouchedTitle));
    await see(t, find.text(l.errReleaseNoSuper));
    expect(find.text(l.failTitle), findsNothing);
    expect(find.text(l.failBackup), findsNothing);
    await tap(t, find.text(l.failBackEdit));
    await see(t, find.text(l.optsTitle));
    expect(rec.calls.where((c) => c.first == 'gk3_apply').length, 1);
  });

  testWidgets('写盘阶段失败：标题下面是按 ERR 代码查的话（不是后端的中文原句）', (t) async {
    await pumpApp(t, 'blank', failApply: true, language: 'en');
    await tap(t, find.text(en.btnStart));
    await tap(t, find.textContaining('/dev/nvme0n1'));
    await next(t, en);
    await tap(t, find.text(en.modeWipeTitle));
    await next(t, en);
    await next(t, en);
    await passRisk(t, loc: en);
    await hold(t, en.confirmHoldIdle);
    await see(t, find.text(en.failTitle));
    await see(t, find.text(en.errPartMissing('super')));
    expect(cjkOnScreen(t), isEmpty);
  });

  testWidgets('预检：电量低且没接电源 → 拦住，写明原因（GUI-8）', (t) async {
    await pumpApp(t, 'blank', overrides: {'gk3_preflight': 'preflight-lowbatt.txt'});
    await see(t, find.text(l.checkBlocked));
    await see(t, find.text(l.checkPowerLow));
    await see(t, find.text(l.checkPowerBad('15')));
    final btn = t.widget<ButtonStyleButton>(find.ancestor(of: find.text(l.btnStart), matching: find.byWidgetPredicate((w) => w is ButtonStyleButton)).first);
    expect(btn.onPressed, isNull);
  });

  testWidgets('预检：电量够时照常显示百分比、不拦', (t) async {
    await pumpApp(t, 'blank');
    await see(t, find.text(l.checkPowerShown('76')));
    expect(find.text(l.checkBlocked), findsNothing);
  });

  testWidgets('免 U 盘（安装器在内置盘上）：默认不另装救援、写明为什么；完成页不叫人"移除安装介质"（GUI-17）', (t) async {
    final rec = await pumpApp(t, 'windows-live');
    await tap(t, find.text(l.btnStart));
    await tap(t, find.textContaining('/dev/nvme0n1'));
    await next(t);
    await tap(t, find.text(l.modeAlongTitle));
    await next(t);
    await see(t, find.text(l.optsTitle));
    await see(t, find.text(l.optsRescueSameDisk));
    await next(t);
    await passRisk(t);
    await see(t, find.text(l.confirmRescue(l.wordNoInstall)));
    await hold(t, l.confirmHoldIdle);
    await see(t, find.text(l.doneTitle));
    await see(t, find.text(l.doneBodyInternal));
    expect(find.text(l.doneBody), findsNothing);
    expect(rec.last('gk3_apply')!.join(' '), contains('--rescue no'));
  });

  testWidgets('从 U 盘装：完成页照旧提示移除介质、救援默认装', (t) async {
    final rec = await pumpApp(t, 'blank');
    await tap(t, find.text(l.btnStart));
    await tap(t, find.textContaining('/dev/nvme0n1'));
    await next(t);
    await tap(t, find.text(l.modeWipeTitle));
    await next(t);
    expect(find.text(l.optsRescueSameDisk), findsNothing);
    await next(t);
    await passRisk(t);
    await hold(t, l.confirmHoldIdle);
    await see(t, find.text(l.doneBody));
    expect(rec.last('gk3_apply')!.join(' '), contains('--rescue yes'));
  });

  testWidgets('整盘清空出厂盘：确认页标明 Onekey 是华为一键恢复分区、删掉的后果（GUI-18）', (t) async {
    await pumpApp(t, 'factory');
    await tap(t, find.text(l.btnStart));
    await tap(t, find.textContaining('/dev/nvme0n1'));
    await next(t);
    await tap(t, find.text(l.modeWipeTitle));
    await next(t);
    await next(t);
    await passRisk(t);
    await see(t, find.text(l.confirmWipeHead));
    await see(t, find.textContaining(l.confirmOnekey), findsWidgets);
    await see(t, find.text(l.confirmOnekeyWarn('Onekey')));
  });

  group('语言（GUI-19）', () {
    testWidgets('gk3.lang=en → 英文；切换后记住；记住的优先于 gk3.lang=', (t) async {
      final p = MemPrefs(boot: 'en_US');
      await pumpApp(t, 'blank', prefs: p);
      await see(t, find.text(en.welcomeTitle));
      await tap(t, find.text(en.btnLanguage));
      await see(t, find.text(l.welcomeTitle));
      expect(p.stored, 'zh');
      expect(Session(Rec(FixtureBackend('blank', bundle: DiskBundle())), prefs: p).language, 'zh');
    });
    test('不认识的值退回中文', () {
      expect(Session(Rec(FixtureBackend('blank', bundle: DiskBundle())), prefs: MemPrefs(boot: 'fr')).language, 'zh');
      expect(normalizeLang('zh-CN'), 'zh');
      expect(normalizeLang(' EN '), 'en');
    });
  });

  // ── 后端留给前端的三个接口 ──────────────

  testWidgets('界面重新起来时写盘任务还在跑：欢迎页直接接上去跟（gk3_job_follow <id>），侧栏禁用，跟完到完成页（GUI-11）', (t) async {
    final rec = await pumpApp(t, 'blank', overrides: {'gk3_job_status': 'job_status-running.txt', 'gk3_job_follow': 'job_follow-apply.txt'});
    await see(t, find.text(l.doneTitle));
    expect(rec.last('gk3_job_follow'), ['gk3_job_follow', '20261005-101500-4242-31337']);
    expect(rec.last('gk3_apply'), isNull, reason: '不该再起一次安装');
  });

  testWidgets('接着跟的过程中：标题说"接着刚才的写盘"，侧栏的重启 / 关机禁用', (t) async {
    final gate = Completer<void>();
    await pumpApp(t, 'blank', overrides: {'gk3_job_status': 'job_status-running.txt', 'gk3_job_follow': 'job_follow-apply.txt'}, holdFollow: gate);
    await see(t, find.text(l.runResumeTitle));
    await see(t, find.text(l.railBusy));
    gate.complete();
    await see(t, find.text(l.doneTitle));
  });

  testWidgets('没有在跑的写盘任务：照常停在欢迎页', (t) async {
    final rec = await pumpApp(t, 'blank');
    await see(t, find.text(l.welcomeTitle));
    expect(rec.last('gk3_job_status'), isNotNull);
    expect(rec.last('gk3_job_follow'), isNull);
  });

  Future<void> failWipe(WidgetTester t) async {
    await tap(t, find.text(l.btnStart));
    await tap(t, find.textContaining('/dev/nvme0n1'));
    await next(t);
    await tap(t, find.text(l.modeWipeTitle));
    await next(t);
    await next(t);
    await passRisk(t);
    await hold(t, l.confirmHoldIdle);
    await see(t, find.text(l.failTitle));
  }

  testWidgets('失败页"保存日志"：列出能存的地方（启动介质标出"电脑上看不到"），存完说存到哪（GUI-12）', (t) async {
    final rec = await pumpApp(t, 'blank', failApply: true);
    await failWipe(t);
    await tap(t, find.text(l.logsSave));
    await see(t, find.text(l.logsTitle));
    await see(t, find.text(l.logsEspNote));
    await see(t, find.textContaining('KINGSTON'));   // 另插的 U 盘排在前面（gk3_log_targets 的顺序）
    await tap(t, find.textContaining('GK3LIVE'));
    await see(t, find.textContaining(l.logsSavedEsp));
    expect(rec.last('gk3_save_logs'), ['gk3_save_logs', '/dev/sda1']);
  });

  testWidgets('保存日志：没有能存的地方 → 叫人插 U 盘、能重新查找', (t) async {
    final rec = await pumpApp(t, 'blank', failApply: true, overrides: {'gk3_log_targets': 'job_status-none.txt'});
    await failWipe(t);
    await tap(t, find.text(l.logsSave));
    await see(t, find.text(l.logsNoTarget));
    final n = rec.calls.where((c) => c.first == 'gk3_log_targets').length;
    await tap(t, find.text(l.logsRescan));
    expect(rec.calls.where((c) => c.first == 'gk3_log_targets').length, n + 1);
    expect(rec.last('gk3_save_logs'), isNull);
  });

  // ── 双系统专项（见 tools/gk3boot/README.md）──────────────────────────────────────────────
  /// U 盘双系统一路走到确认页（windows-free，或 overrides 换掉 gk3_esp_info）
  Future<Rec> toConfirm(WidgetTester t, {String scenario = 'windows-free', Map<String, String> overrides = const {}}) async {
    final rec = await pumpApp(t, scenario, overrides: overrides);
    await tap(t, find.text(l.btnStart));
    await tap(t, find.textContaining('/dev/nvme0n1'));
    await next(t);
    await tap(t, find.text(l.modeAlongTitle));
    await next(t);
    await next(t); // 选项
    await passRisk(t);
    await see(t, find.text(l.confirmTitle));
    return rec;
  }

  testWidgets('双系统确认页：默认系统预选 Android、大字写明冷开机进哪个、菜单 5 秒；不选就带 --default-os android（U12 / U13）', (t) async {
    final rec = await toConfirm(t);
    await see(t, find.text(l.confirmDefaultTitle));
    await see(t, find.text(l.confirmDefaultLead));
    await see(t, find.text(l.confirmMenu5));
    expect(find.text(l.confirmDefaultWindowsWhen), findsNothing);
    expect(find.text(l.confirmBitlockerHead), findsNothing); // 不是 BitLocker：不出勾选框
    await hold(t, l.confirmHoldIdle);
    await see(t, find.text(l.doneTitle));
    await see(t, find.text(l.doneDualAndroid));
    final a = rec.last('gk3_apply')!.join(' ');
    expect(a, contains('--default-os android'));
    expect(a, isNot(contains('--bitlocker-key')));
    // gk3_plan 不认识这两个参数：只发给 gk3_apply
    expect(rec.calls.where((c) => c.first == 'gk3_plan').any((c) => c.contains('--default-os')), isFalse);
  });

  testWidgets('双系统选 Windows 为默认：说清"前两次开机进 Android"，apply 带 --default-os windows，完成页照着说（U12）', (t) async {
    final rec = await toConfirm(t);
    await tap(t, find.text(l.confirmDefaultWindows));
    await see(t, find.text(l.confirmDefaultWindowsWhen));
    await hold(t, l.confirmHoldIdle);
    await see(t, find.text(l.doneTitle));
    await see(t, find.text(l.doneDualWindows));
    expect(rec.last('gk3_apply')!.join(' '), contains('--default-os windows'));
  });

  testWidgets('BitLocker + 要换 BOOTAA64（从 U 盘来）：不勾"已拿到恢复密钥"按住也开始不了；勾了才装、带 --bitlocker-key yes（U16）', (t) async {
    final rec = await toConfirm(t, overrides: {'gk3_esp_info': 'esp_info-bitlocker.txt'});
    await see(t, find.text(l.confirmBitlockerHead));
    await see(t, find.text(l.confirmBitlockerUsb));
    expect(find.text(l.confirmBitlockerScript), findsNothing);
    await hold(t, l.confirmHoldIdle);
    expect(find.text(l.doneTitle), findsNothing);
    expect(rec.last('gk3_apply'), isNull);
    // 勾选框在列表下方：先滚到它、等布局完再点（tap() 里的 ensureVisible 之后不等一帧，会点空）
    await t.ensureVisible(find.byType(Checkbox));
    await settle(t);
    await t.tap(find.byType(Checkbox));
    await settle(t);
    expect(t.widget<CheckboxListTile>(find.byType(CheckboxListTile)).value, isTrue);
    await hold(t, l.confirmHoldIdle);
    await see(t, find.text(l.doneTitle));
    expect(rec.last('gk3_apply')!.join(' '), contains('--bitlocker-key yes'));
  });

  testWidgets('BitLocker、走的是 Windows 工具那条路（安装器在内置盘上）：文案换成"工具已暂停 BitLocker"（§4.9.7 规则 6）', (t) async {
    await toConfirm(t, scenario: 'windows-live', overrides: {'gk3_esp_info': 'esp_info-bitlocker.txt'});
    await see(t, find.text(l.confirmBitlockerScript));
    expect(find.text(l.confirmBitlockerUsb), findsNothing);
  });

  testWidgets('Windows 在休眠：双系统禁用并写明"回 Windows 关快速启动、用关机退出"（U18）', (t) async {
    await pumpApp(t, 'windows-free', overrides: {'gk3_esp_info': 'esp_info-hibernated.txt'});
    await tap(t, find.text(l.btnStart));
    await tap(t, find.textContaining('/dev/nvme0n1'));
    await next(t);
    await see(t, find.text(l.modeWhyHibernated));
    expect(find.text(l.modeAlongOk), findsNothing);
    await tap(t, find.text(l.modeAlongTitle));
    expect(find.ancestor(of: find.text(l.btnNext), matching: find.byWidgetPredicate((w) => w is ButtonStyleButton && w.onPressed != null)), findsNothing);
  });

  testWidgets('100 MiB 的 EFI 分区：双系统明确禁用，说"这个版本不支持"而不是"请清理"（U17）', (t) async {
    await pumpApp(t, 'windows-free', overrides: {'gk3_esp_info': 'esp_info-small.txt'});
    await tap(t, find.text(l.btnStart));
    await tap(t, find.textContaining('/dev/nvme0n1'));
    await next(t);
    await see(t, find.text(l.modeWhyEspTooSmall('100')));
    expect(find.text(l.modeWhyEspSmall('70', '150')), findsNothing);
  });

  testWidgets('后端说 EFI 变量删不掉（NOTE loadervar-stuck）：完成页用红字说怎么在开机菜单里清除', (t) async {
    await toConfirm(t, overrides: {'gk3_apply': 'apply-note.txt'});
    await hold(t, l.confirmHoldIdle);
    await see(t, find.text(l.doneTitle));
    await see(t, find.text(l.doneNoteLoaderVar('auto-windows')));
  });

  testWidgets('纯 Android（整盘）：确认页与完成页都没有默认系统那一节', (t) async {
    await pumpApp(t, 'blank');
    await tap(t, find.text(l.btnStart));
    await tap(t, find.textContaining('/dev/nvme0n1'));
    await next(t);
    await tap(t, find.text(l.modeWipeTitle));
    await next(t);
    await next(t);
    await passRisk(t);
    await see(t, find.text(l.confirmTitle));
    expect(find.text(l.confirmDefaultTitle), findsNothing);
  });

  testWidgets('英文界面的双系统确认页（BitLocker）没有 CJK', (t) async {
    await pumpApp(t, 'windows-free', language: 'en', overrides: {'gk3_esp_info': 'esp_info-bitlocker.txt'});
    await tap(t, find.text(en.btnStart));
    await tap(t, find.textContaining('/dev/nvme0n1'));
    await next(t, en);
    await tap(t, find.text(en.modeAlongTitle));
    await next(t, en);
    await next(t, en);
    await passRisk(t, loc: en);
    await see(t, find.text(en.confirmTitle));
    // 英文比中文长，BitLocker 这一段落在列表下方：懒加载的列表要滚到才会建出来
    await t.scrollUntilVisible(find.text(en.confirmBitlockerHead), 200, scrollable: find.byType(Scrollable).last);
    expect(cjkOnScreen(t), isEmpty);
  });

  // ── 写盘前的风险确认（用户 2026-10-06：分区之前强制阅读风险提示）────────────────────────────────────
  group('风险确认页', () {
    /// 走到风险确认页。[install]：安装的三种方式（还要过选项一页）；否则是删除分区（方式页的下一页就是它）
    Future<Rec> toRisk(WidgetTester t, String scenario, String Function(L10n) pick, {L10n? loc, bool install = true, Map<String, String> overrides = const {}}) async {
      final x = loc ?? l;
      final rec = await pumpApp(t, scenario, language: x.localeName, overrides: overrides);
      await tap(t, find.text(x.btnStart));
      await tap(t, find.textContaining('/dev/nvme0n1'));
      await next(t, x);
      await tap(t, find.text(pick(x)));
      await next(t, x);
      if (install) await next(t, x); // 选项
      await see(t, find.text(x.riskTitle));
      return rec;
    }

    /// 按钮旁边那句"还差：…"正好是这几项
    Finder missing(List<String> items, [L10n? loc]) {
      final x = loc ?? l;
      return find.text(x.riskMissing(items.join(x.riskSep)));
    }

    /// "还差：再等 N 秒"（N 是几都行）
    Finder missingOnlyWait([L10n? loc]) {
      final x = loc ?? l;
      final re = RegExp('^${RegExp.escape(x.riskMissing(x.riskNeedWait('#'))).replaceFirst('#', r'\d+')}\$');
      return find.byWidgetPredicate((w) => w is Text && w.data != null && re.hasMatch(w.data!));
    }

    Future<void> tick(WidgetTester t) => tap(t, find.byKey(kRiskCheckKey));
    Future<void> type(WidgetTester t, String s) async {
      await t.enterText(find.byKey(kRiskWordKey), s);
      await settle(t);
    }

    Future<void> waitOut(WidgetTester t) async {
      await t.pump(const Duration(seconds: RiskPage.readSeconds));
      await settle(t);
    }

    /// 点了也不走：还在风险页，下一页没出来，后端什么都没收到
    Future<void> tapDoesNothing(WidgetTester t, Rec rec, String nextTitle) async {
      await t.tap(find.text(l.riskContinue), warnIfMissed: false);
      await settle(t);
      expect(find.text(l.riskTitle), findsOneWidget);
      expect(find.text(nextTitle), findsNothing);
      for (final fn in ['gk3_apply', 'gk3_part_delete']) {
        expect(rec.last(fn), isNull, reason: fn);
      }
    }

    testWidgets('整盘清空（出厂盘）：最醒目的样子、正文写明华为一键恢复分区；一开始四个门槛都没满足，按钮旁边逐项写明', (t) async {
      final rec = await toRisk(t, 'factory', (x) => x.modeWipeTitle);
      expect(find.byIcon(Icons.warning_amber_rounded), findsOneWidget);
      expect(find.text(l.confirmTitle), findsNothing);
      await see(t, find.text(l.riskWipeHead));
      await see(t, find.textContaining(l.confirmOnekeyWarn('Onekey')));
      await see(t, find.text(l.riskBackupHead));
      await see(t, find.text(l.riskScrollHint));
      expect(find.byKey(kRiskWordKey), findsOneWidget);
      expect(riskContinueEnabled(t), isFalse);
      // 刚进来：四项都差（秒数随测试推进的假时间变，只核对它在、且排在第二）
      final txt = t.widgetList<Text>(find.byType(Text)).map((w) => w.data ?? '').firstWhere((s) => s.startsWith(l.riskMissing('')));
      expect(txt, startsWith(l.riskMissing(l.riskNeedScroll + l.riskSep)));
      expect(txt, endsWith(l.riskSep + l.riskNeedCheck + l.riskSep + l.riskNeedWord(l.riskWord)));
      await tapDoesNothing(t, rec, l.confirmTitle);
    });

    testWidgets('没滚到底：等够了、勾了、确认词也对 —— 仍然不能继续；滚到底才行', (t) async {
      final rec = await toRisk(t, 'factory', (x) => x.modeWipeTitle);
      await waitOut(t);
      await tick(t);
      await type(t, 'ERASE');
      await see(t, missing([l.riskNeedScroll]));
      expect(riskContinueEnabled(t), isFalse);
      await tapDoesNothing(t, rec, l.confirmTitle);
      await scrollRiskToEnd(t);
      expect(find.text(l.riskScrollHint), findsNothing);
      expect(riskContinueEnabled(t), isTrue);
      await tap(t, find.text(l.riskContinue));
      await see(t, find.text(l.confirmTitle));
    });

    testWidgets('倒计时没到：滚到底、勾了、确认词对 —— 仍然不能继续；${RiskPage.readSeconds} 秒之后才行', (t) async {
      final rec = await toRisk(t, 'factory', (x) => x.modeWipeTitle);
      await scrollRiskToEnd(t);
      await tick(t);
      await type(t, 'ERASE');
      await see(t, missingOnlyWait());
      expect(riskContinueEnabled(t), isFalse);
      await tapDoesNothing(t, rec, l.confirmTitle);
      await waitOut(t);
      expect(missingOnlyWait(), findsNothing);
      expect(riskContinueEnabled(t), isTrue);
    });

    testWidgets('不勾选：读完、等够、确认词对 —— 仍然不能继续', (t) async {
      final rec = await toRisk(t, 'factory', (x) => x.modeWipeTitle);
      await scrollRiskToEnd(t);
      await waitOut(t);
      await type(t, 'ERASE');
      await see(t, missing([l.riskNeedCheck]));
      expect(riskContinueEnabled(t), isFalse);
      await tapDoesNothing(t, rec, l.confirmTitle);
      await tick(t);
      expect(riskContinueEnabled(t), isTrue);
      await tick(t); // 取消勾选：又不行了
      expect(riskContinueEnabled(t), isFalse);
    });

    testWidgets('确认词错误（打一半 / 中文 / 别的词）不能继续，打错了标红；大小写不论', (t) async {
      final rec = await toRisk(t, 'factory', (x) => x.modeWipeTitle);
      await scrollRiskToEnd(t);
      await waitOut(t);
      await tick(t);
      await see(t, missing([l.riskNeedWord(l.riskWord)]));
      for (final w in ['ERAS', '清除', 'CLEAR', 'ERASED', 'E RASE']) {
        await type(t, w);
        expect(riskContinueEnabled(t), isFalse, reason: w);
      }
      await tapDoesNothing(t, rec, l.confirmTitle);
      await see(t, find.text(l.riskWordWrong(l.riskWord)));   // 最后那个不是前缀：标红
      await type(t, 'ERA');
      expect(find.text(l.riskWordWrong(l.riskWord)), findsNothing);   // 打到一半不标红
      await type(t, ' erase ');
      expect(riskContinueEnabled(t), isTrue);
    });

    testWidgets('确认词能用软键盘打（live 里没有输入法，平板可能没接键盘盖）', (t) async {
      await toRisk(t, 'factory', (x) => x.modeWipeTitle);
      await scrollRiskToEnd(t);
      await waitOut(t);
      await tick(t);
      for (final k in 'erase'.split('')) {
        await tap(t, find.text(k));
      }
      expect(t.widget<TextField>(find.byKey(kRiskWordKey)).controller!.text, 'erase');
      expect(riskContinueEnabled(t), isTrue);
    });

    testWidgets('双系统：经过风险页（共用 EFI 分区、Windows 与 BitLocker 恢复密钥那一段），不要确认词', (t) async {
      final rec = await toRisk(t, 'windows-free', (x) => x.modeAlongTitle);
      expect(find.text(l.confirmTitle), findsNothing);
      await see(t, find.text(l.riskAlongHead));
      await see(t, find.text(l.riskWinHead));
      expect(find.textContaining(l.riskWinBitlocker), findsNothing); // 不是 BitLocker 盘
      expect(find.byKey(kRiskWordKey), findsNothing);
      await tapDoesNothing(t, rec, l.confirmTitle);
      await passRisk(t);
      await see(t, find.text(l.confirmTitle));
    });

    testWidgets('双系统 + BitLocker：风险页提一句"确认页会要你确认恢复密钥"，勾选框仍只在确认页（不重复确认）', (t) async {
      await toRisk(t, 'windows-free', (x) => x.modeAlongTitle, overrides: {'gk3_esp_info': 'esp_info-bitlocker.txt'});
      await see(t, find.textContaining(l.riskWinBitlocker));
      expect(find.text(l.confirmBitlockerCheck), findsNothing);
      expect(find.byType(CheckboxListTile), findsOneWidget); // 只有"我已备份…"那一个
      await passRisk(t);
      await see(t, find.text(l.confirmBitlockerCheck));
    });

    testWidgets('重新安装（默认清数据）：经过风险页，写明 /data 会被清空；不要确认词', (t) async {
      final rec = await toRisk(t, 'android', (x) => x.modeReinstallTitle);
      await see(t, find.text(l.riskReinstallHead));
      await see(t, find.text(l.riskReinstallWipe));
      expect(find.byKey(kRiskWordKey), findsNothing);
      await tapDoesNothing(t, rec, l.confirmTitle);
    });

    testWidgets('删除分区（盘上有 Windows）：经过风险页、要确认词；从删除分区页"完成"直接回到方式页（不停在风险页）', (t) async {
      final rec = await toRisk(t, 'factory', (x) => x.editEntryTitle, install: false);
      await see(t, find.text(l.riskEditHead));
      expect(find.byKey(kRiskWordKey), findsOneWidget);
      await tapDoesNothing(t, rec, l.editTitle);
      await passRisk(t);
      await see(t, find.text(l.editTitle));
      await tap(t, find.text(l.editDone));
      await see(t, find.text(l.modeTitle));
      expect(find.text(l.riskTitle), findsNothing);
    });

    testWidgets('删除分区（盘上没有 Windows）：经过风险页，不要确认词', (t) async {
      await toRisk(t, 'android', (x) => x.editEntryTitle, install: false);
      await see(t, find.text(l.riskEditHead));
      expect(find.byKey(kRiskWordKey), findsNothing);
    });

    testWidgets('从确认页返回：回到风险页、已满足的不用再来一遍；再返回到选项页', (t) async {
      await toRisk(t, 'windows-free', (x) => x.modeAlongTitle);
      await passRisk(t);
      await see(t, find.text(l.confirmTitle));
      await tap(t, find.text(l.btnBack));
      await see(t, find.text(l.riskTitle));
      expect(riskContinueEnabled(t), isTrue);
      await tap(t, find.text(l.btnBack));
      await see(t, find.text(l.optsTitle));
    });

    testWidgets('英文界面：整盘清空的风险页没有 CJK，确认词是 ERASE', (t) async {
      await toRisk(t, 'factory', (x) => x.modeWipeTitle, loc: en);
      expect(cjkOnScreen(t), isEmpty);
      await see(t, find.text(en.riskWordPrompt('ERASE')));
      await scrollRiskToEnd(t);
      await waitOut(t);
      await tap(t, find.byKey(kRiskCheckKey));
      expect(cjkOnScreen(t), isEmpty);
      await see(t, missing([en.riskNeedWord('ERASE')], en));
      await type(t, 'ERASE');
      expect(riskContinueEnabled(t, en), isTrue);
    });

    testWidgets('英文界面：删除分区的风险页没有 CJK', (t) async {
      await toRisk(t, 'factory', (x) => x.editEntryTitle, loc: en, install: false);
      expect(cjkOnScreen(t), isEmpty);
    });

    test('Session 守卫：没确认过风险的路，install / deletePartition 一个字节都不写；换了方式要重新确认', () async {
      final rec = Rec(FixtureBackend('factory', bundle: DiskBundle(), speed: 0));
      final s = Session(rec);
      await s.start();
      await s.probe();
      await s.assess(s.disks!.firstWhere((d) => d.path == '/dev/nvme0n1'));
      s.setMode(Mode.wipe);
      await s.computePlan();
      final ev = await s.install().toList();
      expect(ev.whereType<Gk3Record>().where((r) => r.type == 'ERR').single['code'], kRiskUnackedCode);
      expect(ev.whereType<Gk3Record>().single['touched'], 'no');
      expect((ev.last as Gk3Exit).code, isNot(0));
      final ed = await s.deletePartition('/dev/nvme0n1p6', (_) {});
      expect(ed.err?['code'], kRiskUnackedCode);
      for (final fn in ['gk3_apply', 'gk3_net_release', 'gk3_part_delete']) {
        expect(rec.last(fn), isNull, reason: fn);
      }
      // 认过的那一条才放行；换了方式（或换了盘、改了保不保留数据）就又不算了
      s.ackRisk(RiskKind.install);
      expect(s.riskAcked(RiskKind.install), isTrue);
      expect(s.riskAcked(RiskKind.edit), isFalse);
      await s.install().toList();
      expect(rec.last('gk3_apply'), isNotNull);
      s.setMode(Mode.alongside);
      expect(s.riskAcked(RiskKind.install), isFalse);
      s.ackRisk(RiskKind.edit);
      await s.deletePartition('/dev/nvme0n1p6', (_) {});
      expect(rec.last('gk3_part_delete'), isNotNull);
    });

    test('界面认识守卫的错误代码（两种语言）', () {
      final e = const Gk3Record('ERR', {'code': kRiskUnackedCode});
      expect(errText(l, e), l.errRiskUnacked);
      expect(errText(en, e), en.errRiskUnacked);
    });
  });
}
