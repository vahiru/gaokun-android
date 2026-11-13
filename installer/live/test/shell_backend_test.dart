// ShellBackend：参数走位置参数、事件分流、以及照抄到日志（2026-09-26 M4b：装完在介质上找不到 gk3_apply 的输出）。
// 要 bash —— Mac 与 Linux 都有。
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:gk3_common/backend/protocol.dart';
import 'package:gk3_installer/backend/shell_backend.dart';

void main() {
  late Directory tmp;
  late File lib, logf;
  late IOSink log;

  setUp(() {
    tmp = Directory.systemTemp.createTempSync('gk3-shell-');
    lib = File('${tmp.path}/lib.sh')
      ..writeAsStringSync(r'''
gk3_demo() { echo "REC n=${#1}"; echo "PROGRESS 50 一半" >&2; echo "arg:$1" >&2; return 3; }
gk3_part_delete() { echo "PARTOP op=delete part=$1"; }
''');
    logf = File('${tmp.path}/installer.log');
    log = logf.openWrite();
  });
  tearDown(() => tmp.deleteSync(recursive: true));

  test(r'事件分流 + 退出码；参数里的 $(…) 与引号不被 shell 解释', () async {
    final b = ShellBackend(lib.path, log: log);
    final ev = await b.call('gk3_demo', [r"a'$(touch pwned)"]).toList();
    expect(ev.whereType<Gk3Record>().single['n'], '${r"a'$(touch pwned)".length}');
    expect(ev.whereType<Gk3Progress>().single.percent, 50);
    expect(ev.whereType<Gk3Log>().single.line, r"arg:a'$(touch pwned)");
    expect((ev.last as Gk3Exit).code, 3);
    expect(File('${tmp.path}/pwned').existsSync() || File('pwned').existsSync(), isFalse);
  });

  test('照抄到日志：调用、stdout 记录、stderr、退出码都在', () async {
    final b = ShellBackend(lib.path, log: log);
    await b.call('gk3_demo', ['x']).toList();
    await log.flush();
    await log.close();
    final t = logf.readAsStringSync();
    expect(t, contains('>> gk3_demo x'));
    expect(t, contains('   | REC n=1'));
    expect(t, contains('   PROGRESS 50 一半'));
    expect(t, contains('   arg:x'));
    expect(t, contains('<< gk3_demo exit 3'));
  });

  // v1.0 计划 B5 / GUI-2：写盘期间电源键、合盖、睡眠都不许把机器关掉或挂起
  test('写盘的调用套 systemd-inhibit（--what 含 shutdown、sleep、电源键与合盖）；别的调用不套', () async {
    final rec = File('${tmp.path}/inhibit.log');
    final fake = File('${tmp.path}/fake-inhibit')
      ..writeAsStringSync('#!/bin/bash\necho "\$*" >> "${rec.path}"\nwhile [ "\${1#--}" != "\$1" ]; do shift; done\nexec "\$@"\n');
    await Process.run('chmod', ['+x', fake.path]);
    final b = ShellBackend(lib.path, log: log, inhibitor: [fake.path]);
    final ev = await b.call('gk3_part_delete', ['/dev/nvme0n1p6']).toList();
    expect(ev.whereType<Gk3Record>().single['part'], '/dev/nvme0n1p6'); // 套了一层照样跑、输出照样分流
    expect((ev.last as Gk3Exit).code, 0);
    await b.call('gk3_demo', ['x']).toList();
    final lines = rec.readAsLinesSync();
    // 第一行是试拿（立刻放掉），第二行才是真的 gk3_part_delete；gk3_demo 不动盘，不套
    expect(lines.length, 2);
    for (final w in ['shutdown', 'sleep', 'handle-power-key', 'handle-lid-switch']) {
      expect(lines.last, contains(w));
    }
    expect(lines.last, contains('--mode=block'));
    expect(lines.last, contains('gk3_part_delete /dev/nvme0n1p6'));
    expect(ShellBackend.writesDisk('gk3_apply') && ShellBackend.writesDisk('gk3_part_delete'), isTrue);
    expect(ShellBackend.writesDisk('gk3_probe') || ShellBackend.writesDisk('gk3_release_info'), isFalse);
  });

  // GUI-11：库里有 gk3_job_run 时，写盘的调用经它跑（独立单元，界面死了不连带）；别的调用照旧直接调
  test('写盘调用经 gk3_job_run；JOB 记录（stderr）解析成记录；不写盘的不经它', () async {
    File(lib.path).writeAsStringSync(r'''
gk3_job_run() { echo "JOB id=j1 state=running mode=setsid unit=-" >&2; echo "via-job:$1" >&2; "$@"; }
gk3_part_delete() { echo "PARTOP op=delete part=$1"; }
gk3_demo() { echo "REC n=1"; }
''');
    final b = ShellBackend(lib.path, log: log, inhibitor: ['${tmp.path}/no-such-inhibit']);
    expect(b.hasJobs, isTrue);
    final ev = await b.call('gk3_part_delete', ['/dev/nvme0n1p6']).toList();
    expect(ev.whereType<Gk3Log>().map((e) => e.line), contains('via-job:gk3_part_delete'));
    expect(ev.whereType<Gk3Record>().firstWhere((r) => r.type == 'JOB')['id'], 'j1');
    expect(ev.whereType<Gk3Record>().firstWhere((r) => r.type == 'PARTOP')['part'], '/dev/nvme0n1p6');
    final ev2 = await b.call('gk3_demo').toList();
    expect(ev2.whereType<Gk3Log>().where((e) => e.line.startsWith('via-job')), isEmpty);
    // 旧库（没有 gk3_job_run）：照旧直接调
    File(lib.path).writeAsStringSync('gk3_part_delete() { echo "PARTOP op=delete part=\$1"; }\n');
    final old = ShellBackend(lib.path, log: log, inhibitor: ['${tmp.path}/no-such-inhibit']);
    expect(old.hasJobs, isFalse);
    expect((await old.call('gk3_part_delete', ['/dev/x']).toList()).last, isA<Gk3Exit>());
  });

  test('拿不到 inhibitor（logind 没在跑 / 没有 systemd-inhibit）：照样执行，日志里记一笔', () async {
    final b = ShellBackend(lib.path, log: log, inhibitor: ['${tmp.path}/no-such-inhibit']);
    final ev = await b.call('gk3_part_delete', ['/dev/nvme0n1p6']).toList();
    expect((ev.last as Gk3Exit).code, 0);
    expect(ev.whereType<Gk3Record>().single['part'], '/dev/nvme0n1p6');
    await log.flush();
    await log.close();
    expect(logf.readAsStringSync(), contains('拿不到 systemd-inhibit'));
  });
}
