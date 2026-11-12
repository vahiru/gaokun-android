import 'package:flutter/services.dart';

import 'backend.dart';
import 'protocol.dart';

/// 回放 [root]/<场景>/ 里录好的后端输出。录像是应用自己的资源：放在应用的目录里、写进应用 pubspec 的
/// assets（LiveCD：live/testdata/，scripts/live/gen-fixtures.sh 用真后端录的）。本类不认识任何场景名，
/// 有哪些场景、默认用哪个，由应用自己定。
///
/// 目录约定：每个场景一个目录，里面的 index.txt 是"调用串 → 文件名"的索引；[root]/common/ 放各场景共用的录像。
/// 查找顺序：[overrides] → 场景里的精确调用 → 场景里的 `<函数> *` → common/ 里同样两步。
/// 索引里的 `@next <函数> <场景>`：这个函数成功之后换到另一个场景接着回放
/// （LiveCD 的 factory 上缩完分区 → windows-free，那份就是同一块盘缩完之后录的）。
/// 找不到就当作一次失败的调用（退出码 127），而不是编一个结果 —— 界面对着
/// "fixture 里没有"的状态也得能画出来。
class FixtureBackend extends Gk3Backend {
  FixtureBackend(String scenario, {AssetBundle? bundle, this.speed = 1.0, this.overrides = const {}, this.root = 'testdata'})
      : _scenario = scenario,
        _bundle = bundle ?? rootBundle;

  /// 录像的根目录（资源键的前缀）
  final String root;

  String _scenario;
  String get scenario => _scenario;
  final _next = <String, Map<String, String>>{}; // 场景 → {函数: 下一个场景}

  /// 回放速度：1 = 录制时的节奏，0 = 不停顿（测试用）
  final double speed;

  /// 调用串 → common/ 里的文件名，例如 {'gk3_preflight': 'preflight-bios217.txt'}
  final Map<String, String> overrides;
  final AssetBundle _bundle;
  final _index = <String, Future<Map<String, String>>>{};

  @override
  String? get demoLabel => scenario;

  Future<Map<String, String>> _load(String dir) => _index.putIfAbsent(dir, () async {
        final map = <String, String>{};
        final String text;
        try {
          text = await _bundle.loadString('$root/$dir/index.txt');
        } catch (_) {
          return map;
        }
        for (var line in text.split('\n')) {
          if (line.startsWith('@next ')) {
            final p = line.split(RegExp(r'\s+'));
            if (p.length >= 3) (_next[dir] ??= {})[p[1]] = p[2];
            continue;
          }
          final hash = line.indexOf('  #');
          if (hash >= 0) line = line.substring(0, hash);
          line = line.trim();
          if (line.isEmpty || line.startsWith('#')) continue;
          final sp = line.lastIndexOf(' ');
          if (sp < 0) continue;
          map[line.substring(0, sp).trim()] = '$root/$dir/${line.substring(sp + 1)}';
        }
        return map;
      });

  Future<String?> _lookup(String fn, List<String> args) async {
    final key = [fn, ...args].join(' ');
    final o = overrides[key] ?? overrides[fn];
    if (o != null) return '$root/common/$o';
    for (final dir in [scenario, 'common']) {
      final idx = await _load(dir);
      final hit = idx[key] ?? idx['$fn *'];
      if (hit != null) return hit;
    }
    return null;
  }

  @override
  Stream<Gk3Event> call(String fn, [List<String> args = const []]) async* {
    final file = await _lookup(fn, args);
    if (file == null) {
      yield Gk3Log('!! no recorded output for this call in the demo data: ${[fn, ...args].join(' ')}');
      yield const Gk3Exit(127);
      return;
    }
    final text = await _bundle.loadString(file);
    var exited = false;
    for (final line in text.split('\n')) {
      if (line.length < 2 && line != 'O' && line != 'E') continue;
      final tag = line[0];
      final body = line.length > 2 ? line.substring(2) : '';
      switch (tag) {
        case 'O':
          yield parseStdoutLine(body);
        case 'E':
          yield parseStderrLine(body);
        case 'D':
          if (speed > 0) {
            await Future<void>.delayed(Duration(milliseconds: ((int.tryParse(body) ?? 0) * speed).round()));
          }
        case 'X':
          exited = true;
          final code = int.tryParse(body) ?? 1;
          final to = _next[_scenario]?[fn];
          if (code == 0 && to != null) _scenario = to;
          yield Gk3Exit(code);
      }
    }
    if (!exited) yield const Gk3Exit(0);
  }

  @override
  Future<void> reboot() async {}

  @override
  Future<void> poweroff() async {}

  @override
  Future<String?> openShell() async => null;
}
