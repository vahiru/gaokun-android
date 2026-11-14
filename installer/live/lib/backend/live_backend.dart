import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:gk3_common/backend/backend.dart';
import 'package:gk3_common/backend/protocol.dart';

/// 真后端：gk3l（installer/backend 的 Linux 版，与 Windows 安装器的 gk3w 同源）。调用名就是 gk3l 的命令：
/// probe、preflight、release-info、esp-info、plan、apply、delete、remove-installer、job、log-targets、save-logs。
///
/// ★ 参数按 argv 传（Process.start 不经过 shell），不拼命令行。
///
/// ★ 每次调用的参数、stderr、stdout 记录与退出码都照抄一份到 [log]（默认是本进程的 stderr ——
///   gk3-installer-session 把它接到内存里的 /run/gk3-installer/installer.log，另存日志时一并带走）：
///   装坏的那一次要能看到后端的每一行输出。
///
/// ★ 写盘的命令（[writesDisk]）经 `gk3l job run` 放进独立的 systemd 临时单元：界面崩了、cage 退出、
///   gk3-installer.service 的 cgroup 被整个杀掉时，写 super 的那个进程不跟着死。stdout / stderr / 退出码与直接调用
///   逐行相同，只多一条 stderr 上的 JOB 记录（id 记进日志，界面重新起来时按它接着跟）。
///   单元里还套着 systemd-inhibit：写盘的那几分钟里电源键、合盖、睡眠都不许把机器关掉或挂起
///   （另外两层在镜像里：overlay-common 的 logind.conf.d 关了按键与合盖、build-rootfs.sh mask 了几个睡眠 target）。
class LiveBackend extends Gk3Backend {
  LiveBackend(this.gk3l, {IOSink? log}) : _log = log ?? stderr;
  final IOSink _log;

  /// gk3l 的路径
  final String gk3l;

  /// 会动盘的命令：安装、删除分区、删除安装器分区
  static bool writesDisk(String fn) => fn == 'apply' || fn == 'delete' || fn == 'remove-installer';

  /// 找 gk3l：GK3L 环境变量 → live 镜像里的位置 → 仓库里编出来的（在 Linux 开发机上直接跑）
  static LiveBackend? locate() {
    final candidates = [
      Platform.environment['GK3L'],
      '/usr/lib/gaokun3/gk3l',
      '${Directory.current.path}/../backend/build/linux/gk3l',
    ];
    for (final c in candidates) {
      if (c != null && File(c).existsSync()) return LiveBackend(File(c).absolute.path);
    }
    return null;
  }

  @override
  Stream<Gk3Event> call(String fn, [List<String> args = const []]) {
    final ctl = StreamController<Gk3Event>();
    final t0 = DateTime.now();
    String ts() => DateTime.now().toIso8601String().substring(11, 19);
    _log.writeln('[${ts()}] >> $fn ${args.join(' ')}'.trimRight());
    () async {
      final Process p;
      final argv = [if (writesDisk(fn)) ...['job', 'run'], fn, ...args];
      if (writesDisk(fn)) _log.writeln('   （gk3l job run：写盘进程在独立单元里，界面退出后照样写完）');
      try {
        p = await Process.start(gk3l, argv);
      } on ProcessException catch (e) {
        _log.writeln('[${ts()}] << $fn cannot start $gk3l: ${e.message}');
        ctl.add(Gk3Log('!! cannot start $gk3l: ${e.message}'));
        ctl.add(const Gk3Exit(127));
        await ctl.close();
        return;
      }
      const dec = Utf8Decoder(allowMalformed: true);
      final out = p.stdout.transform(dec).transform(const LineSplitter()).listen((l) {
        _log.writeln('   | $l');
        ctl.add(parseStdoutLine(l));
      }).asFuture<void>();
      final err = p.stderr.transform(dec).transform(const LineSplitter()).listen((l) {
        _log.writeln('   $l');
        ctl.add(parseStderrLine(l));
      }).asFuture<void>();
      await Future.wait([out, err]);
      final code = await p.exitCode;
      _log.writeln('[${ts()}] << $fn exit $code（${DateTime.now().difference(t0).inMilliseconds / 1000} 秒）');
      ctl.add(Gk3Exit(code));
      await ctl.close();
    }();
    return ctl.stream;
  }

  @override
  Future<void> reboot() => _power('reboot', '重启');

  @override
  Future<void> poweroff() => _power('poweroff', '关机');

  Future<void> _power(String cmd, String what) async {
    _log.writeln('[${DateTime.now().toIso8601String().substring(11, 19)}] $what');
    await _log.flush();
    await Process.run('sync', const []);
    await Process.run(cmd, const []);
  }

  /// 命令行逃生口：切到 tty2（那里有 getty）。镜像里 chvt 链到 busybox（overlay-common），这里再退一步直接调 busybox；
  /// 都不行就把原因交给界面说出来（不能点了没反应）
  @override
  Future<String?> openShell() async {
    String? why;
    for (final (cmd, args) in const [('chvt', ['2']), ('busybox', ['chvt', '2'])]) {
      try {
        final r = await Process.run(cmd, args);
        if (r.exitCode == 0) return null;
        why = '$cmd: ${'${r.stderr}'.trim().isEmpty ? 'exit ${r.exitCode}' : '${r.stderr}'.trim()}';
      } on ProcessException catch (e) {
        why = '$cmd: ${e.message}';
      }
    }
    return why;
  }
}
