import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'backend.dart';
import 'protocol.dart';

/// 真后端：`bash -c '. "$0" && "$@"' <installer-lib.sh> <函数> <参数…>`
///
/// ⚠️★ 参数【不拼进 shell 字符串】，走位置参数。否则一个含单引号或 `$(…)` 的
///    WiFi 密码 / SSID 就是一次命令注入 —— 而这个进程是 root。
///
/// ★ 每次调用的参数、stderr、stdout 记录与退出码都照抄一份到 [log]（默认是本进程的 stderr ——
///   gk3-installer-session 把它接到介质上的 diag/installer.log）。2026-09-26 M4b 第一次真装：
///   装成了，但事后在介质上找不到 gk3_apply 的任何一行输出（只在屏幕上的日志区里出现过）——
///   装坏的那一次要是也这样，就只剩"屏幕上好像报了个错"。
///
/// ★ 写盘的调用（[writesDisk]）套一层 `systemd-inhibit`（v1.0 计划 B5 / GUI-2）：缩 NTFS、写 super 的
///   那几分钟里，电源键、合盖、睡眠都不许把机器关掉或挂起。这是第三层 —— 前两层在镜像里
///   （overlay-common 的 logind.conf.d 关了按键与合盖、build-rootfs.sh mask 了几个睡眠 target）。
///   拿不到 inhibitor（logind 没在跑）时照样执行、只在日志里记一笔：前两层不靠 logind 活着。
class ShellBackend extends Gk3Backend {
  ShellBackend(this.libPath, {IOSink? log, this.inhibitor = const ['systemd-inhibit']}) : _log = log ?? stderr;
  final IOSink _log;

  /// 拿 inhibitor 的命令（测试换成一个记录参数的假脚本）
  final List<String> inhibitor;

  /// 可以中途取消的函数（取消 = 取消对 call() 返回的流的订阅）：只有下载 —— 写盘的那些半路停下就是半个盘
  static bool cancellable(String fn) => fn == 'gk3_net_release';

  /// setsid(1)（util-linux，live 镜像里有；macOS 没有）。有它就让可取消的调用自成一个进程组，
  /// 取消时连 curl 一起杀掉 —— 只杀 bash 的话 curl 成了孤儿，照样往 /run 里那个文件写，下一次重试就是两个 curl 写同一个文件
  static final String? _setsid = ['/usr/bin/setsid', '/bin/setsid'].where((f) => File(f).existsSync()).firstOrNull;

  /// 会动盘的函数：安装、缩分区、手动调整磁盘
  static bool writesDisk(String fn) => fn == 'gk3_apply' || fn == 'gk3_shrink' || fn.startsWith('gk3_part_');

  /// 挡住的东西：关机 / 重启、睡眠，以及 logind 对电源键、睡眠键、休眠键、合盖的处理（systemd-inhibit(1) 的 --what）
  static const inhibitWhat = 'shutdown:sleep:handle-power-key:handle-suspend-key:handle-hibernate-key:handle-lid-switch';

  bool _inhibitOk = false;

  /// 试拿一次（立刻放掉）。只记住"能拿"：logind 晚起来的话，下一次写盘再试
  Future<bool> _canInhibit() async {
    if (_inhibitOk) return true;
    try {
      final r = await Process.run(inhibitor.first, [...inhibitor.skip(1), '--what=$inhibitWhat', '--who=gaokun3 installer', '--why=probe', 'true']);
      _inhibitOk = r.exitCode == 0;
    } on ProcessException {
      _inhibitOk = false;
    }
    return _inhibitOk;
  }

  /// 写进日志时要遮住的参数：WiFi 密码（gk3_wifi_connect 的第二个）
  static List<String> redact(String fn, List<String> args) => [
        for (var i = 0; i < args.length; i++) fn == 'gk3_wifi_connect' && i == 1 && args[i].isNotEmpty ? '<密码 ${args[i].length} 个字符>' : args[i],
      ];

  /// 找库：GK3_LIB 环境变量 → live 镜像里的位置 → 仓库里（在 Linux 开发机上直接跑）
  static ShellBackend? locate() {
    final candidates = [
      Platform.environment['GK3_LIB'],
      '/usr/share/gaokun3/installer-lib.sh',
      '${Directory.current.path}/../../scripts/live/installer-lib.sh',
    ];
    for (final c in candidates) {
      if (c != null && File(c).existsSync()) return ShellBackend(File(c).absolute.path);
    }
    return null;
  }

  final String libPath;

  /// 库里有没有 gk3_job_run（GUI-11 的后端，2026-10-05 起才有）。没有就照旧直接调 —— 旧库、测试里的假库
  late final bool hasJobs = () {
    try {
      return File(libPath).readAsStringSync().contains('gk3_job_run()');
    } on FileSystemException {
      return false;
    }
  }();

  @override
  Stream<Gk3Event> call(String fn, [List<String> args = const []]) {
    Process? proc;
    var group = false, cancelled = false;
    final ctl = StreamController<Gk3Event>(onCancel: () {
      if (!cancellable(fn) || cancelled) return;
      cancelled = true;
      final p = proc;
      if (p == null) return;
      _log.writeln('[${DateTime.now().toIso8601String().substring(11, 19)}] 取消 $fn（${group ? '进程组' : '进程'} ${p.pid}）');
      if (group) {
        Process.run('kill', ['-TERM', '--', '-${p.pid}']).catchError((Object _) => ProcessResult(0, 1, '', ''));
      } else {
        p.kill();
      }
    });
    final t0 = DateTime.now();
    String ts() => DateTime.now().toIso8601String().substring(11, 19);
    _log.writeln('[${ts()}] >> $fn ${redact(fn, args).join(' ')}'.trimRight());
    () async {
      final Process p;
      // ★ 写盘的调用放进独立的 systemd 临时单元（GUI-11，installer-lib.sh 的 gk3_job_run）：界面崩了、cage 退出、
      //   gk3-installer.service 的 cgroup 被整个杀掉时，写 super / 缩 NTFS 的那个进程不跟着死。stdout / stderr / 退出码
      //   与直接调用逐行相同，只多一条 stderr 上的 JOB 记录（id 记进日志，界面重新起来时按它接着跟）
      final viaJob = writesDisk(fn) && hasJobs;
      var argv = ['bash', '-c', r'. "$0" && "$@"', libPath, if (viaJob) 'gk3_job_run', fn, ...args];
      if (viaJob) _log.writeln('   （gk3_job_run：写盘进程在独立单元里，界面死了它照样写完）');
      if (writesDisk(fn)) {
        if (await _canInhibit()) {
          argv = [...inhibitor, '--what=$inhibitWhat', '--who=gaokun3 installer', '--why=writing the disk ($fn)', '--mode=block', ...argv];
          _log.writeln('   （systemd-inhibit：$inhibitWhat）');
        } else {
          _log.writeln('   ⚠️ 拿不到 systemd-inhibit（logind 没在跑？）—— 照样执行；电源键与合盖仍由 logind.conf.d 屏蔽、睡眠 target 已 mask');
        }
      }
      if (cancellable(fn) && _setsid != null) {
        // setsid 不是进程组长时不 fork，直接 exec：pid 不变、自己就是新进程组的组长（kill -- -pid 杀整组）
        argv = [_setsid!, ...argv];
        group = true;
      }
      try {
        p = await Process.start(argv.first, argv.sublist(1));
        proc = p;
      } on ProcessException catch (e) {
        _log.writeln('[${ts()}] << $fn 起不来 ${argv.first}：${e.message}');
        ctl.add(Gk3Log('!! 起不来 ${argv.first}：${e.message}'));
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

  /// 命令行逃生口：切到 tty2（那里有 getty）。C 版是 system("chvt 2")（gk3-installer.c:1288；C 版已删，git show 445e978:live/installer/gk3-installer.c）。
  /// ⚠️★ 2026-09-25 M0 实测：Debian 镜像里【没有 chvt】（它在 kbd 包里，没装）—— 按钮点了什么都不发生，
  ///   日志里只有一条没人接的 ProcessException。现在：镜像里 chvt 链到 busybox（overlay-common），
  ///   这里再退一步直接调 busybox；都不行就把原因交给界面说出来。
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
