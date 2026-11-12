// 版本列表与下载。gk3w 不碰网络：JSON 与 HTTP 在 Dart 里写省事得多，也能在 Linux 上对着本地 HTTP 服务测
// （test/net_test.dart）。输出的事件与 LiveCD 的 gk3_net_manifest / gk3_net_release 相同（scripts/live/installer-lib.sh），
// 界面用同一套代码处理，回放数据（testdata/common/releases-*.txt、download.txt）也是这个格式。
//
// 下载的规矩照搬 LiveCD 的 gk3_net_fetch（那边的注释记着每一条是怎么来的）：
//   * 断点续传：半截文件留着（<名字>.part），重试与界面上的再装一次都接着它下；服务器不认 Range（回 200）就从头。
//   * 停滞判死：60 秒里平均不到 10 KiB/s（或一个字节都没来）算这一次失败 —— 服务器活着但不再发数据时，TCP 不会超时。
//   * 只重试暂时的失败（连不上、断线、超时、HTTP 408 / 429 / 5xx），最多 5 次；404 之类直接失败。
//   * 换了版本：上一次留下的文件属于另一个版本（两份校验清单里它的 sha256 不同）就丢掉，不让新版本的后半截接在旧版本后面。
//   * 下完先核对 sha256（gk3w hash）再改成正式的名字：目录里不带 .part 的文件一定是核对过的。
// 校验清单与镜像来自同一台服务器：防的是下载不完整，不是防恶意服务器（那一层靠 HTTPS）。
import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:gk3_common/backend/protocol.dart';

class NetConfig {
  const NetConfig({
    this.githubApi = 'https://api.github.com/repos/vahiru/gaokun-android/releases?per_page=50',
    this.otaJson = 'https://ota.072172.xyz/ota/gaokun3.json',
    this.userAgent = 'gaokun-android-installer',
    this.connectTimeout = const Duration(seconds: 20),
    this.stallWindow = const Duration(seconds: 60),
    this.stallMinBytesPerSecond = 10 * 1024,
    this.tries = 5,
    this.retryDelay = const Duration(seconds: 3),
    this.progressInterval = const Duration(milliseconds: 300),
  });

  final String githubApi, otaJson, userAgent;
  final Duration connectTimeout, stallWindow, retryDelay, progressInterval;
  final int stallMinBytesPerSecond, tries;
}

/// 一次调用里出的错：顶层把它变成 `ERR code=…` + `!! …` + 退出码 1
class NetError implements Exception {
  NetError(this.code, this.human, [this.fields = const {}]);
  final String code, human;
  final Map<String, String> fields;
  @override
  String toString() => '$code: $human';
}

/// 这一次失败是暂时的（重试有意义）
class _Transient implements Exception {
  _Transient(this.why);
  final String why;
  @override
  String toString() => why;
}

/// 系统代理（Windows 的"设置 → 网络 → 代理 → 手动设置代理"，Clash 之类"设为系统代理"写的就是它）。
/// 没有设置、或读不到 → null，按环境变量（http_proxy 等）或直连。自动配置脚本（PAC）不支持。
/// 每次联网前重读（[Net.proxy]）：用户可能在打开安装器之后才开代理。
Future<String?> windowsSystemProxy() async {
  if (!Platform.isWindows) return null;
  try {
    final r = await Process.run('reg', ['query', r'HKCU\Software\Microsoft\Windows\CurrentVersion\Internet Settings']);
    if (r.exitCode != 0) return null;
    final out = '${r.stdout}';
    final enable = RegExp(r'ProxyEnable\s+REG_DWORD\s+0x(\d+)').firstMatch(out)?.group(1);
    // [ \t]+ 而不是 \s+：\s 会跨行，ProxyServer 为空时取到下一行的 ProxyOverride
    final server = RegExp(r'ProxyServer[ \t]+REG_SZ[ \t]+(\S+)').firstMatch(out)?.group(1);
    if (enable != '1' || server == null) return null;
    return parseProxyServer(server);
  } on ProcessException {
    return null;
  }
}

/// Internet Settings 的 ProxyServer → "host:port"（给 HttpClient 的 "PROXY host:port"）；用不了 → null。
/// 没写端口时补 :80（WinINet 也按 80 算）：Dart 遇到不带端口的项直接抛 Invalid proxy configuration，连 DIRECT 都不试。
///   "127.0.0.1:7890"、"http://127.0.0.1:7890"、"http=127.0.0.1:7890;https=127.0.0.1:7890;socks=…"（取 https 的，没有取 http 的）。
/// ⚠️ 前缀 http:// 要去掉：Dart 会把 "http://127.0.0.1" 整个当成主机名，连不上就按 "PROXY …; DIRECT" 悄悄直连。
/// 只有 socks= 时返回 null（HttpClient 不支持 SOCKS），按环境变量或直连。
String? parseProxyServer(String server) {
  String? clean(String? v) {
    if (v == null) return null;
    final t = v.trim().replaceFirst(RegExp(r'^[a-zA-Z][a-zA-Z0-9+.-]*://'), '').replaceFirst(RegExp(r'/+$'), '');
    // host:port（port 是数字）；没写端口补 :80；别的样子（空的、端口不是数字、含空白）不用
    final m = RegExp(r'^([^\s:/]+|\[[0-9a-fA-F:]+\])(?::(\d{1,5}))?$').firstMatch(t);
    if (m == null) return null;
    return '${m.group(1)}:${m.group(2) ?? '80'}';
  }

  if (!server.contains('=')) return clean(server);
  final m = <String, String>{};
  for (final p in server.split(';')) {
    final eq = p.indexOf('=');
    if (eq > 0) m[p.substring(0, eq).trim().toLowerCase()] = p.substring(eq + 1);
  }
  return clean(m['https']) ?? clean(m['http']);
}

class Net {
  Net({this.cfg = const NetConfig(), this.proxy});
  final NetConfig cfg;

  /// 每次联网前问一次代理（"host:port"；null = 按环境变量或直连）。Windows 上是 [windowsSystemProxy]
  final Future<String?> Function()? proxy;

  Future<HttpClient> _client() async {
    final c = HttpClient()
      ..connectionTimeout = cfg.connectTimeout
      ..userAgent = cfg.userAgent;
    final p = await proxy?.call();
    c.findProxy = p == null ? HttpClient.findProxyFromEnvironment : (_) => 'PROXY $p; DIRECT';
    // 查代理的时候 [abort] 了：这个连接还不在 _clients 里、断不到，不能再交出去
    if (_aborted) {
      c.close(force: true);
      throw NetError('cancelled', 'download cancelled');
    }
    _clients.add(c);
    return c;
  }

  /// 正在用的连接（[abort] 把它们都断掉）
  final _clients = <HttpClient>{};
  bool _aborted = false;

  /// 停止正在进行的下载：断开连接，不再重试（下载中途退出时，WinBackend 在下载的订阅被取消时调用）。
  /// 断开之后读数据的那一步出错、走到重试那里，看到 [_aborted] 就结束，finally 里关掉 .part
  void abort() {
    _aborted = true;
    for (final c in _clients.toList()) {
      c.close(force: true);
    }
  }

  Future<(int, String)> _getText(String url, {Map<String, String> headers = const {}}) async {
    final c = await _client();
    try {
      final req = await c.getUrl(Uri.parse(url));
      headers.forEach(req.headers.set);
      final resp = await req.close().timeout(cfg.stallWindow);
      final body = await resp.transform(utf8.decoder).join().timeout(cfg.stallWindow);
      return (resp.statusCode, body);
    } finally {
      _clients.remove(c);
      c.close(force: true);
    }
  }

  /// HEAD：(状态码, Content-Length 或 -1)
  Future<(int, int)> _head(String url) async {
    final c = await _client();
    try {
      final req = await c.headUrl(Uri.parse(url));
      final resp = await req.close().timeout(cfg.stallWindow);
      await resp.drain<void>();
      return (resp.statusCode, resp.contentLength);
    } finally {
      _clients.remove(c);
      c.close(force: true);
    }
  }

  // ── 版本列表 ──

  /// `releases --source github|r2`：VARIANT 记录（最新的在前，第一个 latest=yes），GitHub 另有最新安装器的 LIVECD
  Stream<Gk3Event> releases(List<String> args) => _guard(() async* {
        final i = args.indexOf('--source');
        final source = i >= 0 && i + 1 < args.length ? args[i + 1] : '';
        switch (source) {
          case 'github':
            await for (final e in _github()) {
              yield e;
            }
          case 'r2':
            await for (final e in _r2()) {
              yield e;
            }
          default:
            throw NetError('usage', 'usage: releases --source github|r2');
        }
      }, failCode: 'manifest-unavailable');

  Stream<Gk3Event> _github() async* {
    final (status, body) = await _getText(cfg.githubApi, headers: {'Accept': 'application/vnd.github+json'});
    if (status != 200) throw NetError('manifest-unavailable', 'GitHub API returned HTTP $status', {'source': 'github', 'http': '$status'});
    final list = (jsonDecode(body) as List).cast<Map<String, dynamic>>();
    for (final r in githubVariants(list)) {
      yield r;
    }
  }

  Stream<Gk3Event> _r2() async* {
    final (status, body) = await _getText(cfg.otaJson);
    if (status != 200) throw NetError('manifest-unavailable', 'OTA manifest returned HTTP $status', {'source': 'r2', 'http': '$status'});
    final r = ((jsonDecode(body) as Map<String, dynamic>)['response'] as List).first as Map<String, dynamic>;
    final name = (r['filename'] as String).replaceFirst(RegExp(r'\.zip$'), '');
    final dl = Uri.parse(r['download'] as String);
    final base = '${dl.scheme}://${dl.authority}/install/$name/';
    final (sumStatus, _) = await _head('${base}install-artifacts.sha256');
    if (sumStatus != 200) {
      throw NetError('manifest-unavailable', 'the latest release $name has no installation files under $base', {'source': 'r2'});
    }
    var size = 0;
    for (final f in const ['boot.img', 'super.img.zst']) {
      final (st, len) = await _head('$base$f');
      if (st != 200) throw NetError('manifest-unavailable', '$base$f returned HTTP $st', {'source': 'r2'});
      size += len < 0 ? 0 : len;
    }
    final date = DateTime.fromMillisecondsSinceEpoch((r['timestamp'] as num).toInt() * 1000, isUtc: true);
    yield Gk3Record('VARIANT', {
      'id': name,
      'name': 'crDroid ${r['version'] ?? ''}'.trim(),
      'date': date.toIso8601String().substring(0, 10),
      'base': base,
      'size_mib': '${size >> 20}',
      'pre': 'no',
      'latest': 'yes',
      'source': 'r2',
    });
  }

  // ── 下载 ──

  /// `download <base-url> <目录>`：校验清单、boot.img、super.img.zst，核对后报 RELEASE dir=… source=net。
  /// [hash] 算一个文件的 sha256（小写 hex；Windows 上是 gk3w hash），算不了就抛异常。
  /// [space] 目录所在卷的可用字节（Windows 上是 gk3w space；null = 不查）：每个文件开始下载前确认放得下，
  /// 不把盘写满了才失败
  Stream<Gk3Event> download(List<String> args, Future<String> Function(String path) hash, {Future<int?> Function(String dir)? space}) =>
      _guard(() async* {
        if (args.length != 2) throw NetError('usage', 'usage: download <base-url> <dir>');
        _aborted = false;
        final base = args[0].endsWith('/') ? args[0] : '${args[0]}/';
        final dir = args[1];
        await Directory(dir).create(recursive: true);
        final sumsFile = File('$dir${Platform.pathSeparator}install-artifacts.sha256');
        final old = await sumsFile.exists() ? await sumsFile.readAsString() : null;
        yield _prog(0, 'dl-sums');
        final sums = await _fetchSums('${base}install-artifacts.sha256');
        await sumsFile.writeAsString(sums, flush: true);
        // 先小后大：boot.img 失败的话，不必先等 1.2 GiB 下完才知道。进度：boot.img 占 0–5 %，super 占 5–100 %
        for (final (name, lo, span) in const [('boot.img', 0, 5), ('super.img.zst', 5, 95)]) {
          if (_aborted) throw NetError('cancelled', 'download cancelled', {'name': name});
          final want = sumOf(sums, name);
          if (want == null) throw NetError('dl-sums-missing', 'install-artifacts.sha256 has no line for $name', {'name': name});
          final ow = old == null ? null : sumOf(old, name);
          if (ow != null && ow != want) {
            yield Gk3Log('$name: the file left from the previous download belongs to another release, starting over');
            for (final f in [name, '$name.part']) {
              final x = File('$dir${Platform.pathSeparator}$f');
              if (await x.exists()) await x.delete();
            }
          }
          await for (final e in _fetch('$base$name', dir, name, want, lo, span, hash, space)) {
            yield e;
          }
        }
        yield _prog(100, 'dl-ready');
        yield Gk3Record('RELEASE', {'dir': dir, 'source': 'net'});
      }, failCode: 'dl-failed');

  Future<String> _fetchSums(String url) async {
    Object? last;
    for (var t = 1; t <= 3; t++) {
      if (_aborted) throw NetError('cancelled', 'download cancelled');
      try {
        final (status, body) = await _getText(url);
        if (status == 200) return body;
        if (!_transientHttp(status)) throw NetError('dl-sums', 'cannot fetch $url (HTTP $status)', {'http': '$status'});
        last = 'HTTP $status';
      } on NetError {
        rethrow;
      } on Exception catch (e) {
        last = e;
      }
      if (_aborted) throw NetError('cancelled', 'download cancelled');
      if (t < 3) await Future<void>.delayed(cfg.retryDelay);
    }
    throw NetError('dl-sums', 'cannot fetch $url ($last)');
  }

  static bool _transientHttp(int s) => s == 408 || s == 429 || (s >= 500 && s < 600);

  Stream<Gk3Event> _fetch(String url, String dir, String name, String want, int lo, int span, Future<String> Function(String) hash,
      Future<int?> Function(String)? space) async* {
    final sep = Platform.pathSeparator;
    final dst = File('$dir$sep$name'), part = File('$dir$sep$name.part');
    yield _prog(lo, 'dl-start', {'name': name});
    // 上一次已经下完、核对过的
    if (await dst.exists()) {
      yield _prog(lo + span * 9 ~/ 10, 'dl-verify', {'name': name});
      if (await hash(dst.path) == want) {
        yield _prog(lo + span, 'dl-done', {'name': name});
        return;
      }
      yield Gk3Log('$name: the existing file does not match install-artifacts.sha256, downloading it again');
      await dst.delete();
    }
    for (var t = 1;; t++) {
      try {
        await for (final e in _get(url, dir, part, name, lo, span, space)) {
          yield e;
        }
        break;
      } on _Transient catch (e) {
        if (_aborted) throw NetError('cancelled', 'download of $name cancelled', {'name': name});
        if (t >= cfg.tries) {
          final kept = await part.exists() ? (await part.length()) >> 20 : 0;
          throw NetError('dl-failed', 'downloading $name failed after $t attempts ($e); $kept MiB kept, the next attempt resumes',
              {'name': name, 'why': '$e'});
        }
        yield Gk3Log('downloading $name was interrupted ($e), resuming in ${cfg.retryDelay.inSeconds} s (attempt ${t + 1}/${cfg.tries})');
        await Future<void>.delayed(cfg.retryDelay);
      }
    }
    yield _prog(lo + span * 9 ~/ 10, 'dl-verify', {'name': name});
    final got = await hash(part.path);
    if (got != want) {
      await part.delete();
      throw NetError('sha-mismatch', '$name does not match install-artifacts.sha256 (got $got), deleted', {'name': name});
    }
    await part.rename(dst.path);
    yield _prog(lo + span, 'dl-done', {'name': name});
  }

  /// 一次 GET（按 .part 现有的长度续传）。暂时的失败抛 [_Transient]，别的抛 [NetError]
  Stream<Gk3Event> _get(String url, String dir, File part, String name, int lo, int span, Future<int?> Function(String)? space) async* {
    if (_aborted) throw NetError('cancelled', 'download of $name cancelled', {'name': name});
    final c = await _client();
    RandomAccessFile? out;
    try {
      var offset = await part.exists() ? await part.length() : 0;
      final HttpClientResponse resp;
      try {
        final req = await c.getUrl(Uri.parse(url));
        if (offset > 0) req.headers.set(HttpHeaders.rangeHeader, 'bytes=$offset-');
        resp = await req.close().timeout(cfg.stallWindow);
      } on TimeoutException {
        throw _Transient('no response in ${cfg.stallWindow.inSeconds} s');
      } on SocketException catch (e) {
        throw _Transient('${e.message}${e.osError == null ? '' : ' (${e.osError!.message})'}');
      } on HttpException catch (e) {
        throw _Transient(e.message);
      }
      final st = resp.statusCode;
      // ⚠️ 下面不要的响应一律不读完（drain）就丢：drain 没有超时、会把剩下的正文整个读完 —— 空间不足时要先下完整个
      //   文件才报错，服务器停住时卡死。finally 里的 close(force: true) 断开连接
      // 续传一个已经下完的文件：服务器回 416（LiveCD 那边 curl 当成功）—— 交给 sha256 裁决
      if (st == 416 && offset > 0) return;
      if (st != 200 && st != 206) {
        if (_transientHttp(st)) throw _Transient('HTTP $st');
        throw NetError('dl-http', 'cannot download $url (HTTP $st)', {'name': name, 'http': '$st'});
      }
      final range = resp.headers.value(HttpHeaders.contentRangeHeader) ?? '';
      // 服务器不认 Range（回 200，或回 206 却从 0 开始）：这份正文是整个文件，从头写
      final old = offset;
      if (offset > 0 && (st == 200 || range.startsWith('bytes 0-'))) {
        yield Gk3Log('$name: the server did not resume at byte $offset, starting over');
        offset = 0;
      } else if (st == 206 && !range.startsWith('bytes $offset-')) {
        // 206 但起点既不是 offset 也不是 0：写进哪里都是错位的数据，必然校验不符。
        // 删掉 .part，按暂时的失败重试 —— 下一次不带 Range，从头下
        if (await part.exists()) await part.delete();
        throw _Transient('the server resumed at the wrong offset (asked for byte $offset, got "$range")');
      }
      final total = resp.contentLength < 0 ? -1 : offset + resp.contentLength;
      // 放得下吗：还要下的字节 + 64 MiB 余量。.part 已经占着的不算；要从头写时它将被截断，那部分也算可用
      if (space != null && total > 0) {
        final avail = await space(dir);
        final need = total - offset + (64 << 20);
        if (avail != null && avail + (offset == 0 ? old : 0) < need) {
          throw NetError('dl-space', 'not enough free space for $name in $dir: ${need >> 20} MiB needed, ${avail >> 20} MiB available',
              {'name': name, 'need_mib': '${need >> 20}', 'avail_mib': '${avail >> 20}'});
        }
      }
      out = await part.open(mode: offset == 0 ? FileMode.write : FileMode.append);
      var done = offset;
      var winStart = DateTime.now(), winBytes = 0, lastProg = DateTime.fromMillisecondsSinceEpoch(0), lastPct = -1;
      try {
        await for (final chunk in resp.timeout(cfg.stallWindow)) {
          try {
            await out.writeFrom(chunk);
          } on FileSystemException catch (e) {
            throw NetError('dl-write', 'cannot write ${part.path}: ${e.osError?.message ?? e.message}', {'name': name});
          }
          done += chunk.length;
          winBytes += chunk.length;
          final now = DateTime.now();
          if (now.difference(winStart) >= cfg.stallWindow) {
            if (winBytes < cfg.stallMinBytesPerSecond * cfg.stallWindow.inSeconds) {
              throw _Transient('less than ${cfg.stallMinBytesPerSecond >> 10} KiB/s for ${cfg.stallWindow.inSeconds} s');
            }
            winStart = now;
            winBytes = 0;
          }
          if (total > 0 && now.difference(lastProg) >= cfg.progressInterval) {
            final pct = done * 100 ~/ total;
            if (pct != lastPct) {
              lastProg = now;
              lastPct = pct;
              yield _prog(lo + pct * span * 90 ~/ 10000, 'dl', {'name': name, 'pct': '$pct'});
            }
          }
        }
      } on TimeoutException {
        throw _Transient('no data for ${cfg.stallWindow.inSeconds} s');
      } on SocketException catch (e) {
        throw _Transient(e.message);
      } on HttpException catch (e) {
        throw _Transient(e.message);
      }
      if (total > 0 && done < total) throw _Transient('the connection closed at $done of $total bytes');
    } finally {
      await out?.close();
      _clients.remove(c);
      c.close(force: true);
    }
  }

  static Gk3Progress _prog(int pct, String code, [Map<String, String> fields = const {}]) =>
      Gk3Progress(pct, [code, for (final f in fields.entries) '${f.key}=${f.value}'].join(' '), code: code, fields: fields);

  /// 跑一个生成器；它抛的 [NetError]（或别的异常，按 [failCode]）变成 ERR + `!! …` + 退出码 1。
  /// ⚠️ 套生成器一律用 await for：yield* 把内层的错误当作流的错误事件转发出去，外层的 try 接不住
  static Stream<Gk3Event> _guard(Stream<Gk3Event> Function() body, {required String failCode}) async* {
    try {
      await for (final e in body()) {
        yield e;
      }
      yield const Gk3Exit(0);
    } on NetError catch (e) {
      yield Gk3Record('ERR', {'code': e.code, ...e.fields});
      yield Gk3Log('!! ${e.human}');
      yield const Gk3Exit(1);
    } catch (e) {
      // 兜底：解析出错（TypeError、空列表上取 .first 的 StateError）也必须报成一次失败的调用，不能让流出错 ——
      // 否则调用方的 run() 抛出，界面一直转圈
      yield Gk3Record('ERR', {'code': failCode});
      yield Gk3Log('!! $e');
      yield const Gk3Exit(1);
    }
  }
}

/// install-artifacts.sha256（sha256sum 的格式，名字前可能带 *）里 [name] 那一行的 sha256（小写）
String? sumOf(String sums, String name) {
  for (final line in const LineSplitter().convert(sums)) {
    final m = RegExp(r'^([0-9a-fA-F]{64}) [ *]?(.+?)\s*$').firstMatch(line);
    if (m != null && m.group(2) == name) return m.group(1)!.toLowerCase();
  }
  return null;
}

/// GitHub API 的 releases 列表 → VARIANT（只列挂着整套安装文件的版本，最新的在前、第一个 latest=yes）
/// + 最新安装器的 LIVECD（挂着 gaokun3-installer-<版本>-windows.zip 的最新一个 release）。
/// 与回放数据生成器（experiments/…/gen-testdata.py 的 releases_github）同一套规则
List<Gk3Record> githubVariants(List<Map<String, dynamic>> releases) {
  final out = <Gk3Record>[];
  var first = true;
  Gk3Record? live;
  for (final r in releases) {
    if (r['draft'] == true) continue;
    final assets = {for (final a in (r['assets'] as List).cast<Map<String, dynamic>>()) a['name'] as String: a};
    final tag = r['tag_name'] as String;
    if (live == null) {
      for (final a in assets.values) {
        final m = RegExp(r'^gaokun3-installer-(.+)-windows\.zip$').firstMatch(a['name'] as String);
        if (m != null) {
          live = Gk3Record('LIVECD', {
            'id': m.group(1)!,
            'rom': tag,
            'date': '${a['updated_at'] ?? ''}'.split('T').first,
            'url': a['browser_download_url'] as String,
            'size_mib': '${(a['size'] as num).toInt() >> 20}',
            'source': 'github',
          });
          break;
        }
      }
    }
    if (!const ['boot.img', 'super.img.zst', 'install-artifacts.sha256'].every(assets.containsKey)) continue;
    final bootUrl = assets['boot.img']!['browser_download_url'] as String;
    out.add(Gk3Record('VARIANT', {
      'id': tag,
      'name': ((r['name'] as String?) ?? '').trim().isEmpty ? tag : (r['name'] as String).trim(),
      'date': '${r['published_at'] ?? ''}'.split('T').first,
      'base': bootUrl.substring(0, bootUrl.lastIndexOf('/') + 1),
      'size_mib': '${((assets['boot.img']!['size'] as num) + (assets['super.img.zst']!['size'] as num)).toInt() >> 20}',
      'pre': r['prerelease'] == true ? 'yes' : 'no',
      'latest': first ? 'yes' : 'no',
      'source': 'github',
    }));
    first = false;
  }
  return [...out, ?live];
}
