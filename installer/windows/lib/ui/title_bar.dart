import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:gk3_common/ui/theme.dart';

import '../app.dart';

/// 和 Windows 原生窗口打交道（runner/flutter_window.cpp 里的 gk3/window 通道）：窗口没有系统标题栏，
/// 拖动、最小化、关闭都由界面发起。浏览器预览里没有这个通道，调用什么都不做。
class WindowControl {
  static const _ch = MethodChannel('gk3/window');
  static bool? _busy;

  static Future<void> _call(String m, [Object? arg]) async {
    try {
      await _ch.invokeMethod<void>(m, arg);
    } on MissingPluginException {
      // 浏览器预览 / 测试
    }
  }

  static Future<void> minimize() => _call('minimize');
  static Future<void> close() => _call('close');

  /// 按下标题栏空白处时调用：把这次按下交给 Windows 当作拖动标题栏
  static Future<void> startDrag() => _call('startDrag');

  /// 正在写盘时原生窗口拒绝关闭（Alt+F4、任务栏右键关闭都不行），写完再放开
  static void setBusy(bool busy) {
    if (_busy == busy) return;
    _busy = busy;
    _call('setBusy', busy);
  }
}

/// 自绘的标题栏：左边应用名（按住拖动窗口），右边最小化与关闭（按 Windows 11 的尺寸与关闭键的红色）
class WinTitleBar extends StatelessWidget {
  const WinTitleBar({super.key});
  static const height = 40.0;

  @override
  Widget build(BuildContext context) {
    final s = context.session, l = context.l, cs = context.cs;
    WindowControl.setBusy(s.writing);
    return Material(
      color: cs.surfaceContainerLow,
      child: SizedBox(
        height: height,
        child: Row(children: [
          Expanded(
            child: GestureDetector(
              behavior: HitTestBehavior.opaque,
              onPanStart: (_) => WindowControl.startDrag(),
              child: Padding(
                padding: const EdgeInsets.only(left: 16),
                child: Row(children: [
                  Icon(Icons.android, size: 18, color: cs.primary),
                  const SizedBox(width: 10),
                  Text(l.appTitle, style: context.tt.labelLarge!.copyWith(color: cs.onSurfaceVariant)),
                ]),
              ),
            ),
          ),
          _CaptionButton(icon: Icons.remove, tooltip: l.winMinimize, onPressed: WindowControl.minimize),
          _CaptionButton(
            icon: Icons.close,
            tooltip: s.writing ? l.winCloseBusy : l.winClose,
            danger: true,
            onPressed: s.writing ? null : WindowControl.close,
          ),
        ]),
      ),
    );
  }
}

class _CaptionButton extends StatefulWidget {
  const _CaptionButton({required this.icon, required this.tooltip, this.onPressed, this.danger = false});
  final IconData icon;
  final String tooltip;
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
    // Windows 11 的关闭键悬停是 #C42B1C 底、白字；其余按钮是淡淡的一层
    final bg = !on || !_hover ? Colors.transparent : (widget.danger ? const Color(0xFFC42B1C) : cs.onSurface.withValues(alpha: 0.08));
    final fg = !on ? cs.onSurface.withValues(alpha: 0.38) : (_hover && widget.danger ? Colors.white : cs.onSurfaceVariant);
    // 不用 Tooltip：标题栏在 Navigator 之外，没有 Overlay
    return Semantics(
      button: true,
      label: widget.tooltip,
      child: MouseRegion(
        onEnter: (_) => setState(() => _hover = true),
        onExit: (_) => setState(() => _hover = false),
        child: GestureDetector(
          onTap: widget.onPressed,
          child: Container(width: 46, height: WinTitleBar.height, color: bg, child: Icon(widget.icon, size: 18, color: fg)),
        ),
      ),
    );
  }
}
