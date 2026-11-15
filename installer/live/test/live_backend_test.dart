// LiveBackend：参数按 argv 传、事件分流、照抄到日志、写盘的命令经 gk3l job run。
// 用一个假的 gk3l（bash 脚本）—— Mac 与 Linux 都有 bash。
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:gk3_common/backend/protocol.dart';
import 'package:gk3_installer/backend/live_backend.dart';

void main() {
  late Directory tmp;
  late File fake, logf;
  late IOSink log;

  setUp(() {
    tmp = Directory.systemTemp.createTempSync('gk3-live-');
    // 假 gk3l：demo 命令回显参数长度；job run 打一条 JOB 再接着跑；delete 回显分区
    fake = File('${tmp.path}/gk3l')
      ..writeAsStringSync(r'''#!/bin/bash
if [ "$1" = job ] && [ "$2" = run ]; then
    shift 2; echo "JOB id=j1 state=running mode=setsid unit=-" >&2; echo "via-job:$1" >&2
fi
case "$1" in
    demo) echo "REC n=${#2}"; echo "PROGRESS 50 half" >&2; echo "arg:$2" >&2; exit 3 ;;
    delete) echo "RESULT op=delete part=$3" ;;
esac
''');
    Process.runSync('chmod', ['+x', fake.path]);
    logf = File('${tmp.path}/installer.log');
    log = logf.openWrite();
  });
  tearDown(() => tmp.deleteSync(recursive: true));

  test(r'事件分流 + 退出码；参数里的 $(…) 与引号不被 shell 解释', () async {
    final b = LiveBackend(fake.path, log: log);
    final ev = await b.call('demo', [r"a'$(touch pwned)"]).toList();
    expect(ev.whereType<Gk3Record>().single['n'], '${r"a'$(touch pwned)".length}');
    expect(ev.whereType<Gk3Progress>().single.percent, 50);
    expect(ev.whereType<Gk3Log>().single.line, r"arg:a'$(touch pwned)");
    expect((ev.last as Gk3Exit).code, 3);
    expect(File('${tmp.path}/pwned').existsSync() || File('pwned').existsSync(), isFalse);
  });

  test('照抄到日志：调用、stdout 记录、stderr、退出码都在', () async {
    final b = LiveBackend(fake.path, log: log);
    await b.call('demo', ['x']).toList();
    await log.flush();
    await log.close();
    final t = logf.readAsStringSync();
    expect(t, contains('>> demo x'));
    expect(t, contains('   | REC n=1'));
    expect(t, contains('   PROGRESS 50 half'));
    expect(t, contains('   arg:x'));
    expect(t, contains('<< demo exit 3'));
  });

  test('写盘的命令经 gk3l job run（JOB 记录在 stderr 上，解析成记录）；不写盘的直接调', () async {
    final b = LiveBackend(fake.path, log: log);
    final ev = await b.call('delete', ['--part', '/dev/nvme0n1p6']).toList();
    expect(ev.whereType<Gk3Log>().map((e) => e.line), contains('via-job:delete'));
    expect(ev.whereType<Gk3Record>().firstWhere((r) => r.type == 'JOB')['id'], 'j1');
    expect(ev.whereType<Gk3Record>().firstWhere((r) => r.type == 'RESULT')['part'], '/dev/nvme0n1p6');
    expect((ev.last as Gk3Exit).code, 0);
    final ev2 = await b.call('demo', ['x']).toList();
    expect(ev2.whereType<Gk3Log>().where((e) => e.line.startsWith('via-job')), isEmpty);
    expect(LiveBackend.writesDisk('apply') && LiveBackend.writesDisk('delete'), isTrue);
    expect(LiveBackend.writesDisk('probe') || LiveBackend.writesDisk('plan') || LiveBackend.writesDisk('job'), isFalse);
  });

  test('gk3l 起不来：一条 !! 说明，退出码 127', () async {
    final b = LiveBackend('${tmp.path}/no-such-gk3l', log: log);
    final ev = await b.call('probe').toList();
    expect(ev.whereType<Gk3Log>().single.isError, isTrue);
    expect((ev.last as Gk3Exit).code, 127);
  });
}
