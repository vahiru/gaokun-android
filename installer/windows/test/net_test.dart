// 版本列表与下载（lib/backend/net.dart）：对着本机的 HTTP 服务测续传、停滞、重试、换版本、校验不符。
// 散列用 sha256sum（Windows 上是 gk3w hash）。test/data/ 是 2026-11-11 取的真实返回：GitHub API 的 releases
// （只留用到的字段）与 R2 上的 OTA 清单。
import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:gk3_common/backend/backend.dart';
import 'package:gk3_common/backend/protocol.dart';
import 'package:gk3_windows/backend/net.dart';

/// 一个记录请求、行为可以按路径改的本地 HTTP 服务
class Srv {
  late HttpServer _s;
  final files = <String, List<int>>{};
  final requests = <String>[];

  /// 路径 → 处理办法；返回 true 表示已经处理（不走默认的发文件）
  final hooks = <String, Future<bool> Function(HttpRequest)>{};
  bool ignoreRange = false;

  String get base => 'http://127.0.0.1:${_s.port}/';

  Future<void> start() async {
    _s = await HttpServer.bind(InternetAddress.loopbackIPv4, 0);
    _s.listen((req) async {
      final path = req.uri.path.substring(1);
      final range = req.headers.value(HttpHeaders.rangeHeader);
      requests.add('${req.method} $path${range == null ? '' : ' $range'}');
      final h = hooks[path];
      if (h != null && await h(req)) return;
      final body = files[path];
      final resp = req.response;
      if (body == null) {
        resp.statusCode = 404;
        await resp.close();
        return;
      }
      final m = RegExp(r'^bytes=(\d+)-$').firstMatch(range ?? '');
      if (m != null && !ignoreRange) {
        final from = int.parse(m.group(1)!);
        if (from >= body.length) {
          resp.statusCode = 416;
          await resp.close();
          return;
        }
        resp.statusCode = 206;
        resp.headers.set(HttpHeaders.contentRangeHeader, 'bytes $from-${body.length - 1}/${body.length}');
        resp.contentLength = body.length - from;
        if (req.method != 'HEAD') resp.add(body.sublist(from));
      } else {
        resp.contentLength = body.length;
        if (req.method != 'HEAD') resp.add(body);
      }
      await resp.close();
    });
  }

  Future<void> stop() => _s.close(force: true);
}

Future<String> sha256sum(String path) async {
  final r = await Process.run('sha256sum', [path]);
  if (r.exitCode != 0) throw Exception('sha256sum failed');
  return '${r.stdout}'.split(' ').first;
}

Future<String> sumOfBytes(List<int> b) async {
  final d = await Directory.systemTemp.createTemp('gk3sum');
  final f = File('${d.path}/x')..writeAsBytesSync(b);
  final h = await sha256sum(f.path);
  await d.delete(recursive: true);
  return h;
}

Future<void> waitUntil(bool Function() ok, {Duration timeout = const Duration(seconds: 5)}) async {
  final end = DateTime.now().add(timeout);
  while (!ok()) {
    if (DateTime.now().isAfter(end)) throw TimeoutException('waitUntil');
    await Future<void>.delayed(const Duration(milliseconds: 20));
  }
}

List<int> blob(int n, int seed) => List<int>.generate(n, (i) => (i * 31 + seed * 7 + (i >> 9)) & 0xff);

const fast = NetConfig(
  connectTimeout: Duration(seconds: 2),
  stallWindow: Duration(milliseconds: 600),
  stallMinBytesPerSecond: 1,
  tries: 3,
  retryDelay: Duration(milliseconds: 50),
  progressInterval: Duration.zero,
);

Future<CallResult> collect(Stream<Gk3Event> s, [List<Gk3Event>? all]) async {
  final records = <Gk3Record>[], log = <Gk3Log>[];
  var code = -1;
  await for (final e in s) {
    all?.add(e);
    switch (e) {
      case Gk3Record():
        records.add(e);
      case Gk3Log():
        log.add(e);
      case Gk3Exit():
        code = e.code;
      case Gk3Progress():
        break;
    }
  }
  return CallResult(records, log, code);
}

void main() {
  late Srv srv;
  late Directory dir;
  late List<int> boot, sup;

  Future<void> publish(List<int> b, List<int> s) async {
    srv.files['boot.img'] = b;
    srv.files['super.img.zst'] = s;
    srv.files['install-artifacts.sha256'] = utf8.encode(
        '${await sumOfBytes(b)}  boot.img\n${await sumOfBytes(s)} *super.img.zst\n0000000000000000000000000000000000000000000000000000000000000000  crDroidAndroid-x.zip\n');
  }

  setUp(() async {
    srv = Srv();
    await srv.start();
    dir = await Directory.systemTemp.createTemp('gk3dl');
    boot = blob(300 * 1024, 1);
    sup = blob(3 * 1024 * 1024 + 123, 2);
    await publish(boot, sup);
  });
  tearDown(() async {
    await srv.stop();
    await dir.delete(recursive: true);
  });

  Stream<Gk3Event> dl([NetConfig cfg = fast]) => Net(cfg: cfg).download([srv.base, dir.path], sha256sum);

  test('下载：两个文件都核对过才改成正式的名字，最后报 RELEASE', () async {
    final all = <Gk3Event>[];
    final r = await collect(dl(), all);
    expect(r.ok, isTrue, reason: '${r.log.map((l) => l.line)}');
    expect(File('${dir.path}/boot.img').readAsBytesSync(), boot);
    expect(File('${dir.path}/super.img.zst').readAsBytesSync(), sup);
    expect(File('${dir.path}/super.img.zst.part').existsSync(), isFalse);
    expect(r.first('RELEASE')!.fields, {'dir': dir.path, 'source': 'net'});
    final codes = [for (final e in all) if (e is Gk3Progress) e.code];
    expect(codes.first, 'dl-sums');
    expect(codes, containsAllInOrder(['dl-start', 'dl-verify', 'dl-done', 'dl-start', 'dl', 'dl-verify', 'dl-done', 'dl-ready']));
    final pcts = [for (final e in all) if (e is Gk3Progress) e.percent];
    expect(pcts, orderedEquals([...pcts]..sort()));
    expect(pcts.last, 100);
  });

  test('已经下完、核对过的文件不再下载', () async {
    expect((await collect(dl())).ok, isTrue);
    srv.requests.clear();
    final r = await collect(dl());
    expect(r.ok, isTrue);
    expect(srv.requests, ['GET install-artifacts.sha256']);
  });

  test('续传：.part 接着下（Range），不从头来', () async {
    File('${dir.path}/super.img.zst.part').writeAsBytesSync(sup.sublist(0, 1000000));
    final r = await collect(dl());
    expect(r.ok, isTrue);
    expect(srv.requests, contains('GET super.img.zst bytes=1000000-'));
    expect(File('${dir.path}/super.img.zst').readAsBytesSync(), sup);
  });

  test('服务器不认 Range（回 200）：从头下，不把整个文件接在半截后面', () async {
    srv.ignoreRange = true;
    File('${dir.path}/super.img.zst.part').writeAsBytesSync(sup.sublist(0, 1000000));
    final r = await collect(dl());
    expect(r.ok, isTrue, reason: '${r.log.map((l) => l.line)}');
    expect(File('${dir.path}/super.img.zst').readAsBytesSync(), sup);
  });

  test('续传一个已经下完的 .part（416）：交给 sha256 裁决', () async {
    File('${dir.path}/super.img.zst.part').writeAsBytesSync(sup);
    final r = await collect(dl());
    expect(r.ok, isTrue);
    expect(File('${dir.path}/super.img.zst').readAsBytesSync(), sup);
  });

  test('暂时的错误（503）重试后成功；404 直接失败', () async {
    var n = 0;
    srv.hooks['super.img.zst'] = (req) async {
      if (n++ > 0) return false;
      req.response.statusCode = 503;
      await req.response.close();
      return true;
    };
    expect((await collect(dl())).ok, isTrue);
    expect(n, 2);

    await dir.delete(recursive: true);
    dir = await Directory.systemTemp.createTemp('gk3dl');
    srv.files.remove('boot.img');
    final r = await collect(dl());
    expect(r.ok, isFalse);
    expect(r.err!['code'], 'dl-http');
    expect(r.err!['http'], '404');
  });

  test('停滞（只发了一半就不再发数据）：判这一次失败、续传；次数用完报 dl-failed，半截文件留着', () async {
    srv.hooks['super.img.zst'] = (req) async {
      final resp = req.response;
      final from = int.tryParse(RegExp(r'bytes=(\d+)-').firstMatch(req.headers.value('range') ?? '')?.group(1) ?? '') ?? 0;
      resp.statusCode = from == 0 ? 200 : 206;
      if (from > 0) resp.headers.set(HttpHeaders.contentRangeHeader, 'bytes $from-${sup.length - 1}/${sup.length}');
      resp.contentLength = sup.length - from;
      resp.add(sup.sublist(from, from + 100000));
      await resp.flush();
      await Future<void>.delayed(const Duration(seconds: 5));
      await resp.close().catchError((Object _) {});
      return true;
    };
    final r = await collect(dl());
    expect(r.ok, isFalse);
    expect(r.err!['code'], 'dl-failed');
    expect(srv.requests.where((q) => q.startsWith('GET super.img.zst')), hasLength(3));
    expect(srv.requests, contains('GET super.img.zst bytes=200000-'));
    expect(File('${dir.path}/super.img.zst.part').lengthSync(), 300000);
  }, timeout: const Timeout(Duration(seconds: 30)));

  test('校验不符：删掉，报 sha-mismatch', () async {
    srv.files['super.img.zst'] = blob(sup.length, 9);
    final r = await collect(dl());
    expect(r.ok, isFalse);
    expect(r.err!['code'], 'sha-mismatch');
    expect(r.err!['name'], 'super.img.zst');
    expect(File('${dir.path}/super.img.zst.part').existsSync(), isFalse);
    expect(File('${dir.path}/super.img.zst').existsSync(), isFalse);
  });

  test('换了版本：上一个版本留下的文件与半截文件丢掉', () async {
    expect((await collect(dl())).ok, isTrue);
    final sup2 = blob(sup.length + 5000, 3);
    File('${dir.path}/super.img.zst.part').writeAsBytesSync(sup.sublist(0, 500000));
    await publish(boot, sup2);
    srv.requests.clear();
    final r = await collect(dl());
    expect(r.ok, isTrue);
    expect(File('${dir.path}/super.img.zst').readAsBytesSync(), sup2);
    // 没拿旧版本的半截去续传
    expect(srv.requests, contains('GET super.img.zst'));
    expect(srv.requests.where((q) => q.contains('bytes=')), isEmpty);
    expect(r.log.map((l) => l.line), contains(contains('another release')));
  });

  test('取不到校验清单：dl-sums', () async {
    srv.files.remove('install-artifacts.sha256');
    final r = await collect(dl());
    expect(r.err!['code'], 'dl-sums');
  });

  test('续传时服务器从错的位置开始回 206：丢掉、删 .part，从头重下（不写错位的数据）', () async {
    File('${dir.path}/super.img.zst.part').writeAsBytesSync(sup.sublist(0, 1000));
    var n = 0;
    srv.hooks['super.img.zst'] = (req) async {
      if (n++ > 0 || req.headers.value('range') == null) return false;
      final resp = req.response;
      resp.statusCode = 206;
      resp.headers.set(HttpHeaders.contentRangeHeader, 'bytes 1500-${sup.length - 1}/${sup.length}');
      resp.contentLength = sup.length - 1500;
      resp.add(sup.sublist(1500));
      await resp.close();
      return true;
    };
    final r = await collect(dl());
    expect(r.ok, isTrue, reason: '${r.log.map((l) => l.line)}');
    expect(File('${dir.path}/super.img.zst').readAsBytesSync(), sup);
    expect(srv.requests.where((q) => q.startsWith('GET super.img.zst')), ['GET super.img.zst bytes=1000-', 'GET super.img.zst']);
  });

  test('放不下：开始下载前报 dl-space，不把盘写满', () async {
    final r = await collect(Net(cfg: fast).download([srv.base, dir.path], sha256sum, space: (_) async => 1 << 20));
    expect(r.ok, isFalse);
    expect(r.err!['code'], 'dl-space');
    expect(r.err!['name'], 'boot.img');
    expect(File('${dir.path}/boot.img.part').existsSync(), isFalse);
  });

  test('放得下：照常下载', () async {
    final r = await collect(Net(cfg: fast).download([srv.base, dir.path], sha256sum, space: (_) async => 10 << 30));
    expect(r.ok, isTrue);
  });

  /// 慢慢发、一直发的服务器（每 50 ms 发 64 KiB）：不该把它读完才报错
  Future<bool> slow(HttpRequest req, List<int> body, {int from = 0, String? range}) async {
    final resp = req.response;
    resp.statusCode = range == null ? 200 : 206;
    if (range != null) resp.headers.set(HttpHeaders.contentRangeHeader, range);
    resp.contentLength = body.length - from;
    try {
      for (var i = from; i < body.length; i += 65536) {
        resp.add(body.sublist(i, i + 65536 > body.length ? body.length : i + 65536));
        await resp.flush();
        await Future<void>.delayed(const Duration(milliseconds: 50));
      }
      await resp.close();
    } catch (_) {
      // 客户端断开
    }
    return true;
  }

  test('放不下时马上报错，不先把整个响应读完（服务器还在慢慢发）', () async {
    // 4 MiB 按每 50 ms 64 KiB 要 3 秒多才发完（boot.img 本身只有 300 KiB，太小了抓不住"先读完再报错"）
    final big = blob(4 << 20, 5);
    srv.hooks['boot.img'] = (req) => slow(req, big);
    final sw = Stopwatch()..start();
    final r = await collect(Net(cfg: fast).download([srv.base, dir.path], sha256sum, space: (_) async => 1 << 20));
    expect(r.err!['code'], 'dl-space');
    expect(sw.elapsedMilliseconds, lessThan(1500));
  });

  test('206 起点不对时马上丢掉重试，不先读完这份错位的正文', () async {
    File('${dir.path}/super.img.zst.part').writeAsBytesSync(sup.sublist(0, 1000));
    var n = 0;
    srv.hooks['super.img.zst'] = (req) async {
      if (n++ > 0 || req.headers.value('range') == null) return false;
      return slow(req, sup, from: 1500, range: 'bytes 1500-${sup.length - 1}/${sup.length}');
    };
    final sw = Stopwatch()..start();
    final r = await collect(dl());
    expect(r.ok, isTrue, reason: '${r.log.map((l) => l.line)}');
    // 一份 3 MiB 的错位正文按每 50 ms 64 KiB 要 2.5 秒才发完
    expect(sw.elapsedMilliseconds, lessThan(2000));
  });

  test('续传时服务器回 206 却从 0 开始：当作整个文件从头写，不必重连', () async {
    File('${dir.path}/super.img.zst.part').writeAsBytesSync(sup.sublist(0, 1000));
    srv.hooks['super.img.zst'] = (req) async {
      final resp = req.response;
      resp.statusCode = 206;
      resp.headers.set(HttpHeaders.contentRangeHeader, 'bytes 0-${sup.length - 1}/${sup.length}');
      resp.contentLength = sup.length;
      resp.add(sup);
      await resp.close();
      return true;
    };
    final r = await collect(dl());
    expect(r.ok, isTrue, reason: '${r.log.map((l) => l.line)}');
    expect(File('${dir.path}/super.img.zst').readAsBytesSync(), sup);
    expect(srv.requests.where((q) => q.startsWith('GET super.img.zst')), hasLength(1));
  });


  test('中途停止（abort）：断开连接、不再重试，很快结束', () async {
    final big = blob(8 << 20, 6);
    srv.hooks['super.img.zst'] = (req) => slow(req, big);
    final net = Net(cfg: fast);
    final events = <Gk3Event>[];
    final done = Completer<void>();
    final sub = net.download([srv.base, dir.path], sha256sum).listen(events.add, onDone: done.complete);
    await waitUntil(() => File('${dir.path}/super.img.zst.part').existsSync());
    await Future<void>.delayed(const Duration(milliseconds: 200));
    final sw = Stopwatch()..start();
    net.abort();
    await sub.cancel();
    expect(sw.elapsedMilliseconds, lessThan(1000));
    final n = srv.requests.where((q) => q.startsWith('GET super.img.zst')).length;
    await Future<void>.delayed(const Duration(milliseconds: 500));
    expect(srv.requests.where((q) => q.startsWith('GET super.img.zst')).length, n, reason: 'no retry after abort');
    // .part 已经关掉，可以删
    File('${dir.path}/super.img.zst.part').deleteSync();
  });


  test('版本列表格式不对（response 是空的）：报 manifest-unavailable，不让流出错', () async {
    srv.files['ota/gaokun3.json'] = utf8.encode('{"response": []}');
    final r = await collect(Net(cfg: NetConfig(otaJson: '${srv.base}ota/gaokun3.json')).releases(['--source', 'r2']));
    expect(r.ok, isFalse);
    expect(r.err!['code'], 'manifest-unavailable');
  });

  test('代理：每次请求前问一次，请求经由代理发出', () async {
    var asked = 0;
    final net = Net(cfg: fast, proxy: () async {
      asked++;
      return parseProxyServer('http://127.0.0.1:${Uri.parse(srv.base).port}/');
    });
    // 不存在的主机名：只有经由代理（本地服务）才拿得到
    final r = await collect(net.download(['http://gk3.invalid/', dir.path], sha256sum));
    expect(r.ok, isTrue, reason: '${r.log.map((l) => l.line)}');
    expect(asked, greaterThanOrEqualTo(3));
  });

  test('ProxyServer 的几种写法', () {
    expect(parseProxyServer('127.0.0.1:7890'), '127.0.0.1:7890');
    expect(parseProxyServer('http://127.0.0.1:7890'), '127.0.0.1:7890');
    expect(parseProxyServer('http://127.0.0.1:7890/'), '127.0.0.1:7890');
    expect(parseProxyServer('http=127.0.0.1:7890;https=127.0.0.1:7891;ftp=x:1'), '127.0.0.1:7891');
    expect(parseProxyServer('http=http://127.0.0.1:7890'), '127.0.0.1:7890');
    expect(parseProxyServer('socks=127.0.0.1:7891'), isNull);
    expect(parseProxyServer(''), isNull);
    // 没写端口补 :80（Dart 对不带端口的项直接抛 Invalid proxy configuration，连 DIRECT 都不试）
    expect(parseProxyServer('127.0.0.1'), '127.0.0.1:80');
    expect(parseProxyServer('http=proxy.corp'), 'proxy.corp:80');
    expect(parseProxyServer('127.0.0.1:abc'), isNull);
    expect(parseProxyServer('a b:1'), isNull);
  });


  test('GitHub 的版本列表与回放数据（testdata/common/releases-github.txt）相同', () {
    final list = (jsonDecode(File('test/data/github-releases.json').readAsStringSync()) as List).cast<Map<String, dynamic>>();
    final got = [for (final r in githubVariants(list)) '${r.type} ${r.fields}'];
    final want = [
      for (final l in File('testdata/common/releases-github.txt').readAsLinesSync())
        if (l.startsWith('O ')) parseStdoutLine(l.substring(2)) as Gk3Record
    ].map((r) => '${r.type} ${r.fields}').toList();
    expect(got, want);
  });

  test('R2：OTA 清单 + HEAD 安装文件 → 一个 VARIANT（与回放数据相同）', () async {
    // 真实的 OTA 清单，把下载地址指到本地服务
    final ota = jsonDecode(File('test/data/r2-ota.json').readAsStringSync()) as Map<String, dynamic>;
    final r0 = (ota['response'] as List).first as Map<String, dynamic>;
    final name = (r0['filename'] as String).replaceFirst('.zip', '');
    r0['download'] = '${srv.base}builds/${r0['filename']}';
    srv.files['ota/gaokun3.json'] = utf8.encode(jsonEncode(ota));
    // 大小不比（本地服务发不了 1.3 GB），只要 HEAD 回 200
    for (final f in ['install-artifacts.sha256', 'boot.img', 'super.img.zst']) {
      srv.files['install/$name/$f'] = List.filled(1024, 0);
    }
    final net = Net(cfg: NetConfig(otaJson: '${srv.base}ota/gaokun3.json'));
    final r = await collect(net.releases(['--source', 'r2']));
    expect(r.ok, isTrue, reason: '${r.log.map((l) => l.line)}');
    final v = r.first('VARIANT')!;
    final want = parseStdoutLine(File('testdata/common/releases-r2.txt').readAsLinesSync().firstWhere((l) => l.startsWith('O VARIANT')).substring(2)) as Gk3Record;
    expect({...v.fields}..remove('base')..remove('size_mib'), {...want.fields}..remove('base')..remove('size_mib'));
    expect(v['base'], '${srv.base}install/$name/');
  });

  test('版本列表取不到：manifest-unavailable', () async {
    final r = await collect(Net(cfg: NetConfig(githubApi: '${srv.base}nope', connectTimeout: const Duration(seconds: 2))).releases(['--source', 'github']));
    expect(r.ok, isFalse);
    expect(r.err!['code'], 'manifest-unavailable');
  });
}
