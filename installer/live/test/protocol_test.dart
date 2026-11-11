// 协议解析与 fixture 回放的单元测试。
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:gk3_installer/backend/fixture_backend.dart';
import 'package:gk3_installer/backend/protocol.dart';
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
      final disks = Disk.fromProbe(await b('factory').run('gk3_probe'));
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
      final d = Disk.fromProbe(await bk.run('gk3_probe')).firstWhere((d) => d.path == '/dev/nvme0n1');
      expect(d.largestFree?.sizeMib, 81920);
      final esp = EspInfo((await bk.run('gk3_esp_info', ['/dev/nvme0n1p1'])).first('ESP')!);
      expect(esp.roomy, isTrue);
      expect(esp.windows, isTrue);
    });
    test('android：双系统被 partlabel-conflict 拒绝', () async {
      final r = await b('android').run('gk3_plan', [
        '--disk', '/dev/nvme0n1', '--mode', 'alongside', '--rescue', 'no',
        '--region-start', '0', '--region-end', '0', '--esp', '/dev/nvme0n1p1'
      ]);
      final plan = Plan(r);
      expect(plan.ok, isFalse);
      expect(plan.error?['msg'], 'partlabel-conflict');
      expect(plan.error?['names'], contains('super'));
    });
    test('apply 的进度单调、走到 100、stdout 里没有记录', () async {
      final events = <Gk3Event>[];
      final r = await b('blank').run('gk3_apply', ['--anything'], events.add);
      expect(r.ok, isTrue);
      final pcts = events.whereType<Gk3Progress>().map((p) => p.percent).toList();
      expect(pcts.last, 100);
      for (var i = 1; i < pcts.length; i++) {
        expect(pcts[i], greaterThanOrEqualTo(pcts[i - 1]));
      }
      expect(r.records, isEmpty);
    });
    test('WiFi：中文 SSID 与原始字节', () async {
      final aps = (await b('blank').run('gk3_wifi_scan')).ofType('WIFI').map(Ap.new).toList();
      expect(aps.first.ssid, '宿舍网-5G');
      expect(aps.any((a) => a.ssid == '隔壁 的 网'), isTrue);
      expect(aps.firstWhere((a) => a.ssid == 'eduroam').supported, isFalse);
    });
    test('windows-live：介质在内置盘上 —— 盘与分区都标 medium，介质分区不可缩（why=mounted）', () async {
      final disks = Disk.fromProbe(await b('windows-live').run('gk3_probe'));
      expect(disks.single.medium, isTrue);
      expect(disks.single.external, isFalse);
      final sh = (await b('windows-live').run('gk3_shrink_scan', ['/dev/nvme0n1'])).ofType('SHRINK').map(Shrinkable.new).toList();
      final live = sh.firstWhere((x) => x.part == '/dev/nvme0n1p8');
      expect(live.can, isFalse);
      expect(live.why, 'mounted');
    });
    test('版本列表：变体清单 404 时退回 OTA 清单的"最新发布"（真实输出）', () async {
      final vs = (await b('blank', {'gk3_net_manifest': 'net_manifest-latest.txt'}).run('gk3_net_manifest')).ofType('VARIANT').map(Variant.new).toList();
      expect(vs.single.latest, isTrue);
      expect(vs.single.name, 'crDroid 12.11');
      expect(vs.single.base, 'https://ota.072172.xyz/install/crDroidAndroid-16.0-20260916-gaokun3-v12.11/');
      expect(vs.single.sizeMib, 1247);
    });
    test('overrides：换一份预检（BIOS 2.17 放行，安全启动拦住）', () async {
      final checks = (await b('blank', {'gk3_preflight': 'preflight-secureboot.txt'}).run('gk3_preflight'))
          .ofType('CHECK').map(Check.new).toList();
      expect(checks.firstWhere((c) => c.id == 'bios').state, CheckState.ok);
      expect(checks.firstWhere((c) => c.id == 'secureboot').state, CheckState.fail);
    });
    test('@next：factory 上缩完分区，接着按 windows-free 回放（有 80 GiB 空闲了）', () async {
      final bk = b('factory');
      expect(Disk.fromProbe(await bk.run('gk3_probe')).firstWhere((d) => d.path == '/dev/nvme0n1').free, isEmpty);
      expect((await bk.run('gk3_shrink', ['/dev/nvme0n1p4', '262788'])).ok, isTrue);
      expect(bk.scenario, 'windows-free');
      expect(Disk.fromProbe(await bk.run('gk3_probe')).firstWhere((d) => d.path == '/dev/nvme0n1').largestFree?.sizeMib, 81920);
    });
    test('双系统至少要多少：后端自己报（C 版写死了 20.2 GiB）', () async {
      final p = Plan(await b('factory').run('gk3_plan', [
        '--disk', '/dev/nvme0n1', '--mode', 'alongside', '--rescue', 'yes',
        '--region-start', '0', '--region-end', '0', '--esp', '/dev/nvme0n1p1'
      ]));
      expect(p.error?['msg'], 'not-enough-space');
      expect(p.error?.intOf('need_mib'), greaterThan(20000));
    });
    test('fixture 里没有的调用：失败（127），而不是编一个结果', () async {
      final r = await b('blank').run('gk3_no_such_function');
      expect(r.exitCode, 127);
      expect(r.error, contains('演示数据里没有'));
    });
    test('发布目录与网络下载', () async {
      final rel = Release((await b('blank').run('gk3_release_info')).first('RELEASE')!);
      expect(rel.installable, isTrue);
      expect(rel.rescue, isTrue);
      expect(rel.version, startsWith('crDroidAndroid-16.0'));
      final none = Release((await b('blank', {'gk3_release_info': 'release_info-none.txt'}).run('gk3_release_info')).first('RELEASE')!);
      expect(none.installable, isFalse);
    });
  });
}
