import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:gk3_common/ui/theme.dart';
import 'package:gk3_common/ui/widgets.dart';

import '../app.dart';
import '../model.dart';
import 'bits.dart';
import 'messages.dart';

/// 输入框的占位文字：恢复密钥的样子（8 组、每组 6 位）
const kRecoveryKeyHint = '000000-000000-000000-000000-000000-000000-000000-000000';

/// 恢复密钥输入框的格式：只留数字，每 6 位自动加连字符，最多 48 位。粘贴来的密钥（带连字符或空格）照样整理成这个样子；
/// 光标按它前面的数字个数放回原处
class RecoveryKeyFormatter extends TextInputFormatter {
  static String _digits(String s) => s.replaceAll(RegExp(r'[^0-9]'), '');

  @override
  TextEditingValue formatEditUpdate(TextEditingValue oldValue, TextEditingValue newValue) {
    final text = newValue.text;
    final end = newValue.selection.isValid ? newValue.selection.extentOffset.clamp(0, text.length) : text.length;
    var digits = _digits(text);
    var before = _digits(text.substring(0, end)).length;
    // 只删掉了一个自动加的连字符：退格改为删它前面的那位数字，Delete 改为删它后面的那位
    // （不然连字符马上又补回来，按键看起来没有反应）
    if (digits == _digits(oldValue.text) &&
        text.length == oldValue.text.length - 1 &&
        oldValue.selection.isCollapsed &&
        newValue.selection.isCollapsed) {
      if (newValue.selection.extentOffset < oldValue.selection.extentOffset) {
        if (before > 0) {
          digits = digits.substring(0, before - 1) + digits.substring(before);
          before--;
        }
      } else if (before < digits.length) {
        digits = digits.substring(0, before) + digits.substring(before + 1);
      }
    }
    if (digits.length > 48) digits = digits.substring(0, 48);
    if (before > digits.length) before = digits.length;
    final b = StringBuffer();
    var cursor = 0;
    for (var i = 0; i < digits.length; i++) {
      if (i > 0 && i % 6 == 0) b.write('-');
      b.write(digits[i]);
      if (i + 1 == before) cursor = b.length;
    }
    return TextEditingValue(text: b.toString(), selection: TextSelection.collapsed(offset: cursor));
  }
}

/// 输入并校验 BitLocker 恢复密钥（确认页；检查页上关闭 Secure Boot 之前暂停保护也用它）。
/// 密钥交给 gk3w 与系统盘的恢复密钥比对（Session.verifyBitlockerKey），界面上不显示系统保存的密钥。
/// [suspendReboots] 大于 0：按钮是"校验并暂停保护"，通过之后暂停保护这么多次重启
class BitlockerKeyBox extends StatefulWidget {
  const BitlockerKeyBox({super.key, required this.text, this.suspendReboots = 0});
  final String text;
  final int suspendReboots;

  @override
  State<BitlockerKeyBox> createState() => _BitlockerKeyBoxState();
}

class _BitlockerKeyBoxState extends State<BitlockerKeyBox> {
  final _key = TextEditingController();

  /// 校验结果那一行：校验完滚动到看得见（方框在页面下方时，结果可能被底栏挡住）
  final _statusKey = GlobalKey();

  @override
  void dispose() {
    _key.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final s = context.session, l = context.l, cs = context.cs, gk = context.gk;
    final ids = s.preflight?.bitlockerKeyIds;
    // 移动 MFT 期间不校验：校验并暂停保护之后要重新检查，而那时检查会被拒绝
    final canVerify = !s.bitlockerChecking && !s.mftMoving && normalizeRecoveryKey(_key.text) != null;
    Future<void> submit() async {
      if (!canVerify) return;
      await s.verifyBitlockerKey(_key.text, suspendReboots: widget.suspendReboots);
      if (!mounted) return;
      WidgetsBinding.instance.addPostFrameCallback((_) {
        final c = _statusKey.currentContext;
        if (c != null && c.mounted) {
          Scrollable.ensureVisible(c, duration: const Duration(milliseconds: 200), alignmentPolicy: ScrollPositionAlignmentPolicy.keepVisibleAtEnd);
        }
      });
    }

    final fg = gk.onWarningContainer;
    final status = s.bitlockerChecking
        ? Row(children: [
            const SizedBox(width: 20, height: 20, child: CircularProgressIndicator(strokeWidth: 2)),
            const SizedBox(width: 12),
            Text(l.blChecking, style: context.tt.bodyLarge!.copyWith(color: fg)),
          ])
        : s.bitlockerVerified
            ? Row(children: [
                Icon(Icons.check_circle, color: gk.success),
                const SizedBox(width: 12),
                Text(l.blOk, style: context.tt.bodyLarge!.copyWith(color: fg)),
              ])
            : s.bitlockerErr != null
                ? Row(crossAxisAlignment: CrossAxisAlignment.start, children: [
                    Icon(Icons.cancel, color: cs.error),
                    const SizedBox(width: 12),
                    Expanded(child: Text(errText(l, s.bitlockerErr!), style: context.tt.bodyLarge!.copyWith(color: fg))),
                  ])
                : null;
    return NoteBox(
      warn: true,
      text: widget.text,
      child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
        if (ids != null && ids.isNotEmpty) ...[
          Text(l.blKeyIds(ids.join(' / ')), style: context.tt.bodyLarge!.copyWith(color: fg)),
          const SizedBox(height: 12),
        ] else
          const SizedBox(height: 4),
        Row(children: [
          Expanded(
            child: TextField(
              controller: _key,
              enabled: !s.bitlockerChecking,
              keyboardType: TextInputType.number,
              autocorrect: false,
              enableSuggestions: false,
              inputFormatters: [RecoveryKeyFormatter()],
              style: context.tt.bodyLarge!.copyWith(fontFeatures: const [FontFeature.tabularFigures()]),
              decoration: InputDecoration(
                labelText: l.blKeyLabel,
                hintText: kRecoveryKeyHint,
                filled: true,
                fillColor: cs.surface,
                border: const OutlineInputBorder(),
                prefixIcon: const Icon(Icons.key_outlined),
              ),
              onChanged: (_) {
                s.clearBitlockerKey();
                setState(() {});
              },
              onSubmitted: (_) => submit(),
            ),
          ),
          const SizedBox(width: 12),
          Btn(widget.suspendReboots > 0 ? l.blVerifySuspend : l.blVerify, kind: BtnKind.secondary, onPressed: canVerify ? submit : null),
        ]),
        if (status != null) ...[const SizedBox(height: 12), KeyedSubtree(key: _statusKey, child: status)],
      ]),
    );
  }
}
