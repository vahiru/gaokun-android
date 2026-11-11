import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:gk3_common/ui/theme.dart';

import '../app.dart';
import '../session.dart';
import 'run_pages.dart';

/// 和 Windows 原生窗口打交道（windows/runner/flutter_window.cpp 里的 gk3/window 通道）：窗口没有系统标题栏，
/// 拖动、最小化、关闭、选文件夹都由界面发起。浏览器预览与测试里没有这个通道，调用什么都不做。
class WindowControl {
  static const _ch = MethodChannel('gk3/window');
  static bool? _busy;

  static Future<T?> _call<T>(String m, [Object? arg]) async {
    try {
      return await _ch.invokeMethod<T>(m, arg);
    } on MissingPluginException {
      return null;
    }
  }

  static Future<void> minimize() => _call<void>('minimize');
  static Future<void> close() => _call<void>('close');

  /// 按住顶上一行时调用：把这次按下交给 Windows 当作拖动标题栏
  static Future<void> startDrag() => _call<void>('startDrag');

  /// 安装或卸载期间：原生窗口拒绝关闭，阻止关机（Windows 显示 [shutdown] 作为原因），并创建电源请求阻止系统
  /// 进入睡眠状态、保持屏幕开启（[power] 是 powercfg /requests 中显示的原因）。结束后撤销
  static void setBusy(bool busy, {String power = '', String shutdown = ''}) {
    if (_busy == busy) return;
    _busy = busy;
    _call<void>('setBusy', {'busy': busy, 'power': power, 'shutdown': shutdown});
  }

  /// 系统的"选择文件夹"对话框（IFileOpenDialog）；取消或没有原生窗口时为 null
  static Future<String?> pickFolder({String? title}) => _call<String>('pickFolder', title);
}

/// 画布最上面一行：按住拖动窗口。每一页这一行都是空的（侧栏与正文的上边距都不小于 32）
class DragStrip extends StatelessWidget {
  const DragStrip({super.key});
  static const height = 32.0;

  @override
  Widget build(BuildContext context) => GestureDetector(
        behavior: HitTestBehavior.translucent,
        onPanStart: (_) => WindowControl.startDrag(),
      );
}

/// 右上角的最小化与关闭（Windows 11 的尺寸；关闭键悬停是 #C42B1C 底、白字）。正在写盘时关闭键禁用
class WindowButtons extends StatelessWidget {
  const WindowButtons({super.key});
  static const width = 2 * _CaptionButton.width;

  @override
  Widget build(BuildContext context) {
    final s = context.session, l = context.l;
    WindowControl.setBusy(
      s.writing,
      power: runTitle(l, s.task),
      shutdown: switch (s.task) {
        WriteTask.install => l.shutdownReasonInstall,
        WriteTask.uninstall => l.shutdownReasonUninstall,
        WriteTask.media => l.shutdownReasonMedia,
      },
    );
    return Row(mainAxisSize: MainAxisSize.min, children: [
      _CaptionButton(icon: Icons.remove, label: context.l.winMinimize, onPressed: WindowControl.minimize),
      _CaptionButton(icon: Icons.close, label: context.l.winClose, danger: true, onPressed: s.writing ? null : WindowControl.close),
    ]);
  }
}

class _CaptionButton extends StatefulWidget {
  const _CaptionButton({required this.icon, required this.label, this.onPressed, this.danger = false});
  static const width = 46.0;
  final IconData icon;
  final String label;
  final VoidCallback? onPressed;
  final bool danger;

  @override
  State<_CaptionButton> createState() => _CaptionButtonState();
}

class _CaptionButtonState extends State<_CaptionButton> {
  var _hover = false;

  @override
  Widget build(BuildContext context) {
    final cs = context.cs;
    final on = widget.onPressed != null;
    final bg = !on || !_hover ? Colors.transparent : (widget.danger ? const Color(0xFFC42B1C) : cs.onSurface.withValues(alpha: 0.08));
    final fg = !on ? cs.onSurface.withValues(alpha: 0.38) : (_hover && widget.danger ? Colors.white : cs.onSurfaceVariant);
    return Semantics(
      button: true,
      label: widget.label,
      child: MouseRegion(
        onEnter: (_) => setState(() => _hover = true),
        onExit: (_) => setState(() => _hover = false),
        child: GestureDetector(
          onTap: widget.onPressed,
          child: Container(width: _CaptionButton.width, height: DragStrip.height, color: bg, child: Icon(widget.icon, size: 18, color: fg)),
        ),
      ),
    );
  }
}
