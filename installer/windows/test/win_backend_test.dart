// WinBackend（lib/backend/win_backend.dart）：参数怎么传给 gk3w、测试模式改了什么。gk3w 换成一个记录参数的脚本
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:gk3_common/backend/backend.dart';
import 'package:gk3_windows/backend/win_backend.dart';

void main() {
  late Directory d;
  late File argsLog;
  late String fake;

  setUp(() {
    d = Directory.systemTemp.createTempSync('gk3w');
    argsLog = File('${d.path}/args');
    fake = '${d.path}/gk3w';
    File(fake).writeAsStringSync('''#!/bin/sh
printf '%s\\n' "\$*" >> "${argsLog.path}"
case "\$1" in
  probe)
    echo "DISK path=disk1 num=1 size_mib=976762 model=UMIS bus=nvme removable=0 system=yes table=gpt"
    echo "DISK path=disk2 num=2 size_mib=488387 model=Msft%20Virtual%20Disk bus=vhd removable=0 system=no table=gpt"
    echo "PROGRESS 50 scan" >&2 ;;
  hash) echo "HASH file=\$2 size=3 sha256=\$(printf %064d 7)" ;;
  apply) echo "ERR code=plan touched=no" >&2; echo "!! too small" >&2; exit 1 ;;
  bitlocker-key)
    read -r k
    printf '%s\\n' "\$k" > "${d.path}/stdin"
    echo "BITLOCKER volume=C: key=ok" ;;
esac
''');
    Process.runSync('chmod', ['+x', fake]);
  });
  tearDown(() => d.deleteSync(recursive: true));

  test('apply 补上 --sdboot、--mke2fs、--backup-dir，uninstall 补上 --backup-dir，其余参数原样', () async {
    final b = WinBackend(gk3w: fake, sdboot: r'C:\gk3\backend\systemd-bootaa64.efi', mke2fs: r'C:\gk3\backend\mke2fs.exe', backupDir: r'C:\ProgramData\gaokun3\backup');
    final r = await b.run('apply', ['--disk', '1', '--real-disk', '--bitlocker-key', '-']);
    expect(r.ok, isFalse);
    expect(r.err!['code'], 'plan');
    expect(r.error, 'too small');
    await b.run('uninstall', ['--disk', '1', '--real-disk']);
    await b.run('plan', ['--sector', '512']);
    expect(argsLog.readAsLinesSync(), [
      r'apply --disk 1 --real-disk --bitlocker-key - --sdboot C:\gk3\backend\systemd-bootaa64.efi --mke2fs C:\gk3\backend\mke2fs.exe --backup-dir C:\ProgramData\gaokun3\backup',
      r'uninstall --disk 1 --real-disk --backup-dir C:\ProgramData\gaokun3\backup',
      'plan --sector 512',
    ]);
  });

  test('日志：连续几次调用都记下（参数、记录、退出码），不因上一次的 flush 出错', () async {
    final f = File('${d.path}/installer.log');
    final b = WinBackend(gk3w: fake, log: f.openSync(mode: FileMode.append));
    for (var i = 0; i < 3; i++) {
      expect((await b.run('probe')).ok, isTrue);
    }
    await b.run('hash', ['/x/y']);
    final lines = f.readAsLinesSync();
    expect(lines.where((l) => l.contains('>> probe')), hasLength(3));
    expect(lines.where((l) => l.contains('<< probe exit 0')), hasLength(3));
    expect(lines, contains(contains('| DISK path=disk2')));
    expect(lines, contains(contains('PROGRESS 50 scan')));
    expect(lines, contains(contains('<< hash exit 0')));
    // 记的是真正交给 gk3w 的参数
    final t = WinBackend(gk3w: fake, testDisk: 2, backupDir: r'C:\bk', log: f.openSync(mode: FileMode.append));
    await t.run('uninstall', ['--disk', '2', '--real-disk']);
    expect(f.readAsLinesSync(), contains(endsWith(r'>> uninstall --disk 2 --backup-dir C:\bk')));
  });

  test('测试模式：probe 里只有那块 VHD 是系统盘；写盘不带 --real-disk / --bitlocker-key', () async {
    final b = WinBackend(gk3w: fake, testDisk: 2);
    final p = await b.run('probe');
    expect(p.ok, isTrue);
    expect({for (final r in p.ofType('DISK')) r['num']: r['system']}, {'1': 'no', '2': 'yes'});
    expect(p.ofType('DISK').last['model'], 'Msft Virtual Disk');
    await b.run('uninstall', ['--disk', '2', '--real-disk', '--bitlocker-key', '-']);
    expect(argsLog.readAsLinesSync().last, 'uninstall --disk 2');
    // 不动这台 PC 自己的 BitLocker：校验照常，暂停去掉
    await Gk3Backend.collect(b.callWithInput('bitlocker-key', ['--suspend', '2'], 'x\n'));
    expect(argsLog.readAsLinesSync().last, 'bitlocker-key');
  });

  test('恢复密钥经标准输入交给 gk3w，日志里只记"有标准输入"', () async {
    final f = File('${d.path}/installer.log');
    final b = WinBackend(gk3w: fake, log: f.openSync(mode: FileMode.append));
    final r = await Gk3Backend.collect(b.callWithInput('bitlocker-key', [], '135795-000000-720885-111111-222222-333333-444444-555555\n'));
    expect(r.ok, isTrue);
    expect(r.first('BITLOCKER')!['key'], 'ok');
    expect(File('${d.path}/stdin').readAsStringSync(), '135795-000000-720885-111111-222222-333333-444444-555555\n');
    expect(argsLog.readAsLinesSync().last, 'bitlocker-key');
    final log = f.readAsStringSync();
    expect(log, contains('>> bitlocker-key (+ stdin, not logged)'));
    expect(log, isNot(contains('135795')));
  });

  test('测试模式只认 VHD：磁盘号对上但不是 VHD 的盘不标成系统盘', () async {
    final b = WinBackend(gk3w: fake, testDisk: 1);
    final p = await b.run('probe');
    expect({for (final r in p.ofType('DISK')) r['num']: r['system']}, {'1': 'no', '2': 'no'});
  });

  test('hash 取 gk3w hash 的结果；不认识的函数退出码 127；gk3w 起不来报 backend-missing', () async {
    final b = WinBackend(gk3w: fake);
    expect(await b.hash('/x/boot.img'), '0' * 63 + '7');
    expect((await b.run('nope')).exitCode, 127);
    final missing = await WinBackend(gk3w: '${d.path}/does-not-exist').run('probe');
    expect(missing.exitCode, 127);
    expect(missing.err!['code'], 'backend-missing');
  });
}
