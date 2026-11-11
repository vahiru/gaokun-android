import 'dart:async';
import 'dart:ui';

import 'package:flutter/material.dart';
import 'package:flutter/scheduler.dart';

import 'theme.dart';

/// 浸泡页：持续出帧，屏幕上显示帧率与光栅化耗时，每 5 秒往 stdout 打一行。
///
///   GK3_SOAK=1 gk3_installer
///
/// 为什么要它：M0 要量"帧率 + RSS 随时间的曲线（至少 10 分钟）"—— flutter/flutter#192603
/// 报的是 GTK embedder 在 ARM GLES + Wayland 上【逐帧】泄漏原生内存。而安装器大部分时候
/// 是静止的，Flutter 空闲时不出新帧，泄漏根本不会被触发（容器里第一次量：60 秒 RSS 一动不动，
/// 那条曲线没有意义）。这一页让每一帧都真的画。
class SoakPage extends StatefulWidget {
  const SoakPage({super.key});
  @override
  State<SoakPage> createState() => _SoakPageState();
}

class _SoakPageState extends State<SoakPage> with SingleTickerProviderStateMixin {
  late final AnimationController _a = AnimationController(vsync: this, duration: const Duration(seconds: 3))..repeat();
  final _sw = Stopwatch()..start();
  int _frames = 0;
  final _raster = <int>[];
  String _line = '…';
  Timer? _t;

  @override
  void initState() {
    super.initState();
    SchedulerBinding.instance.addTimingsCallback(_onTimings);
    _t = Timer.periodic(const Duration(seconds: 5), (_) => _report());
  }

  void _onTimings(List<FrameTiming> ts) {
    _frames += ts.length;
    for (final t in ts) {
      _raster.add(t.rasterDuration.inMicroseconds);
    }
  }

  void _report() {
    final secs = _sw.elapsedMilliseconds / 1000;
    final fps = _frames / 5;
    _raster.sort();
    final p50 = _raster.isEmpty ? 0 : _raster[_raster.length ~/ 2] / 1000;
    final p95 = _raster.isEmpty ? 0 : _raster[(_raster.length * 95) ~/ 100] / 1000;
    final l = 'SOAK t=${secs.toStringAsFixed(0)} fps=${fps.toStringAsFixed(1)} raster_p50_ms=${p50.toStringAsFixed(1)} raster_p95_ms=${p95.toStringAsFixed(1)}';
    // ignore: avoid_print
    print(l);
    setState(() => _line = l);
    _frames = 0;
    _raster.clear();
  }

  @override
  void dispose() {
    SchedulerBinding.instance.removeTimingsCallback(_onTimings);
    _t?.cancel();
    _a.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final tt = Theme.of(context).textTheme;
    return Scaffold(
      body: Padding(
        padding: const EdgeInsets.all(64),
        child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
          Text('浸泡测试 · soak', style: tt.headlineMedium),
          const SizedBox(height: 12),
          Text(_line, style: tt.titleMedium!.copyWith(fontFamily: 'monospace')),
          const SizedBox(height: 40),
          AnimatedBuilder(
            animation: _a,
            builder: (_, _) => LinearProgressIndicator(value: _a.value, minHeight: 8),
          ),
          const SizedBox(height: 40),
          Row(children: [
            const SizedBox.square(dimension: 96, child: CircularProgressIndicator(strokeWidth: 10)),
            const SizedBox(width: 40),
            // 一段会变的中文：字形缓存也跟着每帧被用到
            Expanded(child: Text('让我们在这台电脑上安装 Android —— 这一页每一帧都在重画。', style: context.tt.headlineSmall)),
          ]),
          const Spacer(),
          Text('${PlatformDispatcher.instance.views.first.physicalSize}', style: tt.bodySmall),
        ]),
      ),
    );
  }
}
