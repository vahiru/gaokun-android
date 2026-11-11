import 'protocol.dart';

/// 安装器后端。唯一的真实现是 ShellBackend（调 installer-lib.sh）；
/// FixtureBackend 回放录好的输出，给 Mac 上开发和出图用。
///
/// ★ 界面里【不许】有任何分区逻辑 —— 连"userdata 能给多大"都问 gk3_plan。
///   一旦在 Dart 里再算一遍，就是本仓反复吃过的"两份拷贝各自漂移"。
abstract class Gk3Backend {
  /// 调 installer-lib.sh 里的一个函数。事件按到达顺序，最后一个一定是 [Gk3Exit]。
  Stream<Gk3Event> call(String fn, [List<String> args = const []]);

  /// 给界面角落的小字：现在连的是真后端还是演示数据
  String? get demoLabel => null;

  Future<void> reboot();

  /// 关机（侧栏的"关机"：不装了、或者要回 Windows 关快速启动 —— v1.0 计划 GUI-3）
  Future<void> poweroff();
  /// 切到 tty2 的命令行。成功返回 null，失败返回原因（界面拿去告诉用户 —— 不能点了没反应）
  Future<String?> openShell();

  /// 跑完并收集结果
  Future<CallResult> run(String fn, [List<String> args = const [], void Function(Gk3Event)? onEvent]) async {
    final records = <Gk3Record>[];
    final log = <Gk3Log>[];
    var code = -1;
    await for (final e in call(fn, args)) {
      onEvent?.call(e);
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
}

class CallResult {
  const CallResult(this.records, this.log, this.exitCode);
  final List<Gk3Record> records;
  final List<Gk3Log> log;
  final int exitCode;

  bool get ok => exitCode == 0;
  Iterable<Gk3Record> ofType(String type) => records.where((r) => r.type == type);
  Gk3Record? first(String type) {
    for (final r in records) {
      if (r.type == type) return r;
    }
    return null;
  }

  /// 后端报的失败原因（最后一条 ERR code=…）。界面按 code 查 l10n（ui/messages.dart 的 errText）
  Gk3Record? get err {
    for (final r in records.reversed) {
      if (r.type == 'ERR') return r;
    }
    return null;
  }

  /// 最后一条 `!! …`（后端给人看的中文说明，只该进日志 —— 界面上的话用 [err]）
  String? get error {
    for (final l in log.reversed) {
      if (l.isError) return l.message;
    }
    return null;
  }
}
