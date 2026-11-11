// 文案的两条约定。
import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

Map<String, dynamic> _arb(String lang) => jsonDecode(File('lib/l10n/app_$lang.arb').readAsStringSync()) as Map<String, dynamic>;
List<String> _ph(String s) => RegExp(r'\{(\w+)\}').allMatches(s).map((m) => m.group(1)!).toSet().toList();

void main() {
  final zh = _arb('zh'), en = _arb('en');
  final keys = zh.keys.where((k) => !k.startsWith('@')).toList();

  // ⚠️★ gen-l10n 生成的方法参数顺序 = 模板里 @元数据 的顺序，而调用处是照着文案读的顺序写的。
  //   第一版元数据按字母序生成 ⇒ 8 条多占位符文案里 6 条的参数被填反，测试却全绿 ——
  //   因为测试和代码用了同样错的顺序。是看离线出图才发现的（"不足 0 MiB（最大的一块是 21.2 GiB）"）。
  test('占位符的元数据顺序 == 它在中文文案里出现的顺序', () {
    for (final k in keys) {
      final meta = (zh['@$k'] as Map?)?['placeholders'] as Map?;
      final inText = _ph(zh[k] as String);
      expect(meta?.keys.toList() ?? <String>[], inText, reason: k);
    }
  });

  // 风险确认页的确认词（screens_risk.dart）：live 里没有输入法，软键盘与实体键盘都只出 ASCII ——
  // 中文界面要人输"清除"，就是一道永远过不去的门
  test('确认词两种语言都是软键盘打得出来的 ASCII 字母', () {
    for (final arb in [zh, en]) {
      expect(arb['riskWord'], matches(RegExp(r'^[A-Za-z]{3,}$')));
    }
  });

  test('中英文逐条都有，占位符集合一致', () {
    for (final k in keys) {
      expect(en.containsKey(k), isTrue, reason: '英文缺 $k');
      expect(_ph(en[k] as String).toSet(), _ph(zh[k] as String).toSet(), reason: k);
    }
  });
}
