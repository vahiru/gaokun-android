// 协议解析与 fixture 回放的单元测试。
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:gk3_common/backend/fixture_backend.dart';
import 'package:gk3_common/backend/protocol.dart';
import 'package:gk3_installer/model/model.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  group('百分号解码', () {
    test('空格、百分号、制表符', () {
      expect(pctDecode('Basic%20data%20partition'), 'Basic data partition');
      expect(pctDecode('100%25%09sure'), '100%\tsure');
    });
    test('中文原样透过；编码与未编码的字节混在一起也对', () {
      expect(pctDecode('宿舍网'), '宿舍网');
      expect(pctDecode('隔壁%20的%20网'), '隔壁 的 网');
    });
    test('不成对的 % 原样保留，不抛异常', () {
      expect(pctDecode('50%'), '50%');
      expect(pctDecode('%zz'), '%zz');
    });
  });

  group('行记录', () {
    test('PART：两个自由文本字段夹在中间（D7 —— C 版会切成 "Basic"）', () {
      final e = parseStdoutLine('PART path=/dev/nvme0n1p3 num=3 name=Basic%20data%20partition fs=ntfs fslabel=My%20Data os=windows');
      expect(e, isA<Gk3Record>());
      final r = e as Gk3Record;
      expect(r.type, 'PART');
      expect(r['name'], 'Basic data partition');
      expect(r['fslabel'], 'My Data');
      expect(r['os'], 'windows');
      expect(r.intOf('num'), 3);
    });
    test('值为空、值里有等号', () {
      final r = parseStdoutLine('SHRINK part=/dev/x why= url=https://a/b?c=d') as Gk3Record;
      expect(r['why'], '');
      expect(r['url'], 'https://a/b?c=d');
    });
    test('不像记录的 stdout 行当日志（命令回显、说明文字）', () {
      expect(parseStdoutLine('+ sgdisk --zap-all /dev/nvme0n1'), isA<Gk3Log>());
      expect(parseStdoutLine('DRY: sgdisk -n 0:1:2'), isA<Gk3Log>());
      expect(parseStdoutLine('super：LP geometry 魔数正确'), isA<Gk3Log>());
      expect(parseStdoutLine('WARNING something happened'), isA<Gk3Log>());
    });
    test('stderr：PROGRESS 与 gk3_die', () {
      final p = parseStderrLine('PROGRESS 31 写入 super（337 / 12288 MiB）') as Gk3Progress;
      expect(p.percent, 31);
      expect(p.text, '写入 super（337 / 12288 MiB）');
      final d = parseStderrLine('!! ESP 空间不够') as Gk3Log;
      expect(d.isError, isTrue);
      expect(d.message, 'ESP 空间不够');
    });
    // v1.0 计划 INST-10：后端给界面的只有代码
    test('stderr：PROGRESS <代码> k=v（值百分号解码）；旧格式的一句话照旧当说明', () {
      final p = parseStderrLine('PROGRESS 23 dl name=super.img.zst pct=31 speed=1843k left=0:07:12') as Gk3Progress;
      expect(p.percent, 23);
      expect(p.code, 'dl');
      expect(p['name'], 'super.img.zst');
      expect(p['left'], '0:07:12');
      expect((parseStderrLine('PROGRESS 20 wifi-assoc ssid=My%20Net') as Gk3Progress)['ssid'], 'My Net');
      expect((parseStderrLine('PROGRESS 100 done') as Gk3Progress).code, 'done');
      final old = parseStderrLine('PROGRESS 31 写入 super（337 / 12288 MiB）') as Gk3Progress;
      expect(old.code, isNull);
      expect(old.text, '写入 super（337 / 12288 MiB）');
      expect(p.at(9).percent, 9);
      expect(p.at(9).code, 'dl');
    });
    test('stderr：ERR code=… 解析成记录（与 stdout 的记录同一套规则）', () {
      final e = parseStderrLine('ERR code=esp-full need_mib=46 free_mib=12 touched=no') as Gk3Record;
      expect(e.type, 'ERR');
      expect(e['code'], 'esp-full');
      expect(e.intOf('free_mib'), 12);
      expect(e['touched'], 'no');
      expect(parseStderrLine('ERRATA 不是记录'), isA<Gk3Log>());
    });
  });

  group('fixture 回放（真后端录的）', () {
    FixtureBackend b(String sc, [Map<String, String> o = const {}]) =>
        FixtureBackend(sc, bundle: rootBundle, speed: 0, overrides: o);

    test('factory：7 个分区、没有空闲区、安装 U 盘被认出来', () async {
      final disks = Disk.fromProbe(await b('factory').run('probe'));
      final nvme = disks.firstWhere((d) => d.path == '/dev/nvme0n1');
      final usb = disks.firstWhere((d) => d.path == '/dev/sda');
      expect(nvme.parts, hasLength(7));
      expect(nvme.free, isEmpty);
      expect(nvme.esp?.path, '/dev/nvme0n1p1');
      expect(nvme.parts[3].label, 'Data');
      expect(usb.medium, isTrue);
      expect(usb.external, isTrue);
    });
    test('windows-free：80 GiB 空闲区；ESP 够用、有 Windows', () async {
      final bk = b('windows-free');
      final d = Disk.fromProbe(await bk.run('probe')).firstWhere((d) => d.path == '/dev/nvme0n1');
      expect(d.largestFree?.sizeMib, 81920);
      final esp = EspInfo((await bk.run('esp-info', ['--disk', '/dev/nvme0n1'])).first('ESP')!);
      expect(esp.roomy, isTrue);
      expect(esp.windows, isTrue);
    });
    test('android：盘上已有 Android 的分区（双系统会重名，界面据此拦住）；重新安装的方案成立', () async {
      final bk = b('android');
      final d = Disk.fromProbe(await bk.run('probe')).firstWhere((d) => d.path == '/dev/nvme0n1');
      expect(d.parts.map((p) => p.name).where(kAndroidNames.contains), containsAll(['super', 'userdata', 'misc']));
      final re = Plan(await bk.run('plan', ['--mode', 'reinstall', '--disk', '/dev/nvme0n1', '--keep-data', 'no']));
      expect(re.ok, isTrue);
      expect(re.reuse.map((r) => r.name), containsAll(['super', 'userdata', 'misc', 'metadata', 'boot_a', 'boot_b']));
    });
    test('apply 的进度单调、走到 100、stdout 里没有记录', () async {
      final events = <Gk3Event>[];
      final r = await b('blank').run('apply', ['--anything'], events.add);
      expect(r.ok, isTrue);
      final pcts = events.whereType<Gk3Progress>().map((p) => p.percent).toList();
      expect(pcts.last, 100);
      for (var i = 1; i < pcts.length; i++) {
        expect(pcts[i], greaterThanOrEqualTo(pcts[i - 1]));
      }
      // stdout 上只有方案与分区表备份的记录
      expect(r.records.map((x) => x.type).toSet(), {'PLAN', 'PLANSUM', 'BACKUP'});
    });
    test('windows-live：介质在内置盘上、读进了内存 —— 盘与介质分区都标 medium，介质分区是安装器分区（ram）', () async {
      final disks = Disk.fromProbe(await b('windows-live').run('probe'));
      expect(disks.single.medium, isTrue);
      expect(disks.single.external, isFalse);
      expect(disks.single.parts.where((p) => p.medium).map((p) => p.path), ['/dev/nvme0n1p8']);
      expect(disks.single.parts.where((p) => p.ram).map((p) => p.path), ['/dev/nvme0n1p8']);
    });
    test('windows-setup：介质挂着（没读进内存）—— medium 但不是安装器分区', () async {
      final d = Disk.fromProbe(await b('windows-setup').run('probe')).firstWhere((d) => d.path == '/dev/nvme0n1');
      expect(d.parts.where((p) => p.medium).length, 1);
      expect(d.parts.where((p) => p.ram), isEmpty);
    });
    test('方案预览：整盘清空保留安装器分区（op=keep）、删掉它之后 /data 多大（op=rmlive）', () async {
      final p = Plan(await b('windows-live').run('plan', ['--mode', 'wipe', '--disk', '/dev/nvme0n1', '--remove-installer']));
      expect(p.ok, isTrue);
      expect(p.keep.single.name, 'GK3LIVE');
      expect(p.parts.map((x) => x.name), isNot(contains('GK3LIVE')));
      expect(p.rmlive!.merge, isTrue);
      expect(p.rmlive!.userdataMib, greaterThan(p.userdataMib));
      expect(p.rmlive!.part, '/dev/nvme0n1p8');
    });
    test('overrides：换一份预检（型号不对拦住，电源照常）', () async {
      final checks = Check.fromPreflight((await b('blank', {'preflight': 'preflight-model.txt'}).run('preflight')).first('PREFLIGHT')!);
      expect(checks.map((c) => c.id), ['model', 'power']);
      expect(checks.firstWhere((c) => c.id == 'model').state, CheckState.fail);
      expect(checks.firstWhere((c) => c.id == 'model').value, 'GK-W57');
      expect(checks.firstWhere((c) => c.id == 'power').state, CheckState.ok);
    });
    test('双系统至少要多少：后端自己报（C 版写死了 20.2 GiB）', () async {
      final p = Plan(await b('factory').run('plan', ['--sector', '512', '--region-start', '0', '--region-end', '0', '--disk', '/dev/nvme0n1']));
      expect(p.error?['msg'], 'not-enough-space');
      expect(p.error?.intOf('need_mib'), greaterThan(20000));
    });
    test('fixture 里没有的调用：失败（127），而不是编一个结果', () async {
      final r = await b('blank').run('no-such-command');
      expect(r.exitCode, 127);
      expect(r.error, contains('no recorded output'));
    });
    test('发布目录（安装介质上的系统文件）', () async {
      final rel = Release((await b('blank').run('release-info')).first('RELEASE')!);
      expect(rel.installable, isTrue);
      expect(rel.version, startsWith('crDroidAndroid-16.0'));
      final none = Release((await b('blank', {'release-info': 'release_info-none.txt'}).run('release-info')).first('RELEASE')!);
      expect(none.installable, isFalse);
    });
  });
}
