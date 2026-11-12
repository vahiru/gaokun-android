import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:gk3_common/backend/backend.dart';
import 'package:gk3_common/backend/protocol.dart';

import 'net.dart';

/// 随程序分发、缺一不可的后端文件（CMake 打包进程序目录的 backend\）
const kBackendFiles = ['gk3w.exe', 'mke2fs.exe', 'mke2fs.conf', 'systemd-bootaa64.efi'];

/// 真后端：gk3w.exe（installer/backend）做检查、分区方案、写盘与卸载；版本列表与下载在 Dart 里做（net.dart）。
/// 参数按 argv 传（Process.start 不经过 shell），不拼命令行。
///
/// ★ 每次调用的参数、输出与退出码照抄进日志文件（[logPath]，%ProgramData%\gaokun3\logs\）：装坏了的时候，
///   能拿来查的不能只有屏幕上的日志区。
///
/// ★ 测试模式（[testDisk]，环境变量 GK3_TEST_DISK）：在 PC 上对着一块 VHD 走完整个流程。
///   probe 的结果里把那块 VHD 标成系统盘（别的盘标成不是），apply / uninstall 去掉 --real-disk 与
///   --bitlocker-key —— gk3w 不带 --real-disk 时本来就只肯写 VHD（disk.c 的 VHD 检查），真盘碰不到；
///   "立即重启"什么都不做。界面在角上标出测试模式。
class WinBackend extends Gk3Backend {
  WinBackend({required this.gk3w, this.sdboot, this.backupDir, this.mke2fs, this.testDisk, Net? net, this.log, this.logPath})
      : _net = net ?? Net();

  final String gk3w;

  /// apply 装进 EFI 分区的 systemd-bootaa64.efi（随程序分发，installer/backend/sdboot/fetch.sh 取的 Debian 那份）
  final String? sdboot;

  /// apply / uninstall 改分区表之前把 GPT 备份到这里（%ProgramData%\gaokun3\backup）
  final String? backupDir;

  /// mke2fs.exe（同目录要有 mke2fs.conf）：apply 用它格式化 metadata / userdata；没有就由 gk3w 退回清零
  final String? mke2fs;

  /// 测试模式下当作系统盘的 VHD 的磁盘号
  final int? testDisk;
  final String? logPath;
  final Net _net;

  /// 日志文件（[logPath]）；null = 不记。
  /// ⚠️ 同步写（RandomAccessFile），不用 IOSink：IOSink.flush() 没完成时再 writeln 会抛 "StreamSink is bound to a stream"，
  ///   而每次调用结束时都要 flush
  final RandomAccessFile? log;

  static const _gk3wCommands = {'preflight', 'probe', 'plan', 'apply', 'uninstall', 'hash', 'space'};

  /// 写盘的调用：测试模式下去掉 --real-disk / --bitlocker-key
  static const _writes = {'apply', 'uninstall'};

  String _ts() => DateTime.now().toIso8601String().substring(11, 19);
  void _w(String line) {
    try {
      log?.writeStringSync('$line\n');
    } on FileSystemException {
      // 日志写不了（盘满之类）不影响安装本身
    }
  }

  void _flush() {
    try {
      log?.flushSync();
    } on FileSystemException {
      // 同上
    }
  }

  /// 找后端：程序目录下的 backend\（CMake 打包进去的），或环境变量 GK3W 指的 gk3w.exe 所在的目录。
  /// [kBackendFiles] 缺任何一个都返回 [MissingBackend]（检查页报"安装器文件不完整"）：缺 mke2fs 时不能悄悄不传
  /// --mke2fs、改走不格式化的路径，缺 systemd-boot 也不能等下载完才报错
  static Gk3Backend locate({int? testDisk}) {
    final exeDir = File(Platform.resolvedExecutable).parent.path;
    final env = Platform.environment['GK3W'];
    final dir = env != null && env.isNotEmpty ? File(env).parent.path : '$exeDir\\backend';
    final missing = [for (final f in kBackendFiles) if (!File('$dir\\$f').existsSync()) f];
    if (missing.isNotEmpty) return MissingBackend('$dir: ${missing.join(', ')}');
    final gk3w = '$dir\\gk3w.exe', mk = '$dir\\mke2fs.exe';
    final base = Platform.environment['ProgramData'] ?? r'C:\ProgramData';
    RandomAccessFile? log;
    String? logPath;
    try {
      final d = Directory('$base\\gaokun3\\logs')..createSync(recursive: true);
      final now = DateTime.now();
      String two(int v) => v.toString().padLeft(2, '0');
      logPath = '${d.path}\\installer-${now.year}${two(now.month)}${two(now.day)}-${two(now.hour)}${two(now.minute)}${two(now.second)}.log';
      log = File(logPath).openSync(mode: FileMode.append);
    } on FileSystemException {
      log = null;
    }
    log?.writeStringSync('gaokun3 Windows installer: backend=$dir test_disk=${testDisk ?? '(none)'}\n');
    return WinBackend(
      gk3w: gk3w,
      sdboot: '$dir\\systemd-bootaa64.efi',
      backupDir: '$base\\gaokun3\\backup',
      mke2fs: mk,
      testDisk: testDisk,
      net: Net(proxy: windowsSystemProxy),
      log: log,
      logPath: logPath,
    );
  }

  @override
  Stream<Gk3Event> call(String fn, [List<String> args = const []]) {
    final t0 = DateTime.now();
    // 记的是真正交给 gk3w 的参数（补上的文件路径、测试模式去掉的 --real-disk 都看得见），不是界面传进来的
    final argv = _gk3wCommands.contains(fn) ? _argsFor(fn, args) : args;
    _w('[${_ts()}] >> $fn ${argv.join(' ')}'.trimRight());
    final Stream<Gk3Event> src;
    if (fn == 'releases') {
      src = _net.releases(args);
    } else if (fn == 'download') {
      src = _abortable(_net.download(args, hash, space: space), () {
        _net.abort();
        _w('[${_ts()}] << $fn cancelled (${DateTime.now().difference(t0).inMilliseconds / 1000} s)');
        _flush();
      });
    } else if (_gk3wCommands.contains(fn)) {
      src = _run(fn, argv);
    } else {
      src = Stream.fromIterable([Gk3Log('!! unknown backend function: $fn'), const Gk3Exit(127)]);
    }
    // 日志：下载的进度每个文件只记每 10 %（换了文件重新算），别的照抄
    var lastDl = -10;
    var dlName = '';
    return src.map((e) {
      switch (e) {
        case Gk3Progress(code: 'dl'):
          final pct = int.tryParse(e['pct']) ?? 0;
          if (e['name'] != dlName) {
            dlName = e['name'];
            lastDl = -10;
          }
          if (pct >= lastDl + 10) {
            lastDl = pct;
            _w('[${_ts()}] PROGRESS ${e.percent} ${e.text}');
          }
        case Gk3Progress():
          _w('   PROGRESS ${e.percent} ${e.text}');
        case Gk3Record():
          _w('   | ${e.type} ${[for (final f in e.fields.entries) '${f.key}=${f.value}'].join(' ')}');
        case Gk3Log():
          _w('   ${e.line}');
        case Gk3Exit():
          _w('[${_ts()}] << $fn exit ${e.code} (${DateTime.now().difference(t0).inMilliseconds / 1000} s)');
          _flush();
      }
      return e;
    });
  }

  /// 订阅被取消时（下载中途退出）先 [abort]：断开连接、不再重试，生成器才能很快走到结束、关掉 .part；
  /// 取消返回的 Future 等它收拾完（之后才能删文件 —— Windows 上删不掉打开着的文件）。
  /// 正常结束后控制器同样会回调 onCancel：那不是取消，不调 [abort]
  static Stream<Gk3Event> _abortable(Stream<Gk3Event> s, void Function() abort) {
    StreamSubscription<Gk3Event>? sub;
    var done = false;
    final c = StreamController<Gk3Event>();
    c.onListen = () => sub = s.listen(c.add, onError: c.addError, onDone: () {
          done = true;
          c.close();
        });
    c.onCancel = () async {
      if (done) return;
      abort();
      await sub?.cancel();
    };
    return c.stream;
  }

  /// 写一行到日志文件（界面这一侧的事：程序出错、用户撤回下载）
  void note(String line) {
    _w('[${_ts()}] $line');
    _flush();
  }

  /// 界面只传它关心的参数；随程序分发的文件与目录在这里补上（界面不必知道它们在哪）
  List<String> _argsFor(String fn, List<String> args) {
    var a = [...args];
    if (fn == 'apply') {
      if (sdboot != null && !a.contains('--sdboot')) a = [...a, '--sdboot', sdboot!];
      if (mke2fs != null && !a.contains('--mke2fs')) a = [...a, '--mke2fs', mke2fs!];
    }
    if (_writes.contains(fn) && backupDir != null && !a.contains('--backup-dir')) a = [...a, '--backup-dir', backupDir!];
    if (testDisk != null && _writes.contains(fn)) {
      final out = <String>[];
      for (var i = 0; i < a.length; i++) {
        if (a[i] == '--real-disk') continue;
        if (a[i] == '--bitlocker-key') {
          i++;
          continue;
        }
        out.add(a[i]);
      }
      a = out;
    }
    return a;
  }

  /// 测试模式：probe 的 DISK 记录里只有那块 VHD 是系统盘
  Gk3Event _rewrite(String fn, Gk3Event e) {
    if (testDisk == null || fn != 'probe' || e is! Gk3Record || e.type != 'DISK') return e;
    final mine = e.intOf('num', -1) == testDisk && e['bus'] == 'vhd';
    return Gk3Record('DISK', {...e.fields, 'system': mine ? 'yes' : 'no'});
  }

  Stream<Gk3Event> _run(String fn, List<String> args) {
    final ctl = StreamController<Gk3Event>();
    () async {
      final Process p;
      try {
        p = await Process.start(gk3w, [fn, ...args]);
      } on ProcessException catch (e) {
        ctl
          ..add(Gk3Record('ERR', {'code': 'backend-missing'}))
          ..add(Gk3Log('!! cannot start $gk3w: ${e.message}'))
          ..add(const Gk3Exit(127));
        await ctl.close();
        return;
      }
      const dec = Utf8Decoder(allowMalformed: true);
      final out = p.stdout.transform(dec).transform(const LineSplitter()).listen((l) => ctl.add(_rewrite(fn, parseStdoutLine(l)))).asFuture<void>();
      final err = p.stderr.transform(dec).transform(const LineSplitter()).listen((l) => ctl.add(parseStderrLine(l))).asFuture<void>();
      await Future.wait([out, err]);
      ctl.add(Gk3Exit(await p.exitCode));
      await ctl.close();
    }();
    return ctl.stream;
  }

  /// 目录所在卷的可用字节（gk3w space；下载前用）。查不了 → null（不挡，写满时照样报 dl-write）
  Future<int?> space(String dir) async {
    final r = await run('space', [dir]);
    final v = r.first('SPACE')?['avail'];
    return r.ok && v != null ? int.tryParse(v) : null;
  }

  /// 一个文件的 sha256（gk3w hash；下载核对用）
  Future<String> hash(String path) async {
    final r = await run('hash', [path]);
    final h = r.first('HASH')?['sha256'];
    if (!r.ok || h == null || h.length != 64) throw NetError('hash-failed', 'cannot compute the sha256 of $path', {'file': path});
    return h;
  }

  @override
  String? get demoLabel => null;

  @override
  Future<void> reboot() async {
    if (testDisk != null) {
      _w('[${_ts()}] reboot requested; test mode, not rebooting');
      return;
    }
    _w('[${_ts()}] reboot');
    _flush();
    await Process.run('shutdown', ['/r', '/t', '0']);
  }

  @override
  Future<void> poweroff() async {
    if (testDisk != null) return;
    _flush();
    await Process.run('shutdown', ['/s', '/t', '0']);
  }

  @override
  Future<String?> openShell() async => 'not available on Windows';
}

/// 找不到 gk3w.exe 时（安装器的文件不全）：每次调用都失败，检查页上说清楚，而不是悄悄回放演示数据
class MissingBackend extends Gk3Backend {
  MissingBackend(this.where);
  final String where;

  @override
  Stream<Gk3Event> call(String fn, [List<String> args = const []]) => Stream.fromIterable([
        const Gk3Record('ERR', {'code': 'backend-missing'}),
        Gk3Log('!! installer files missing ($where)'),
        const Gk3Exit(127),
      ]);

  @override
  Future<void> reboot() async {}
  @override
  Future<void> poweroff() async {}
  @override
  Future<String?> openShell() async => null;
}
