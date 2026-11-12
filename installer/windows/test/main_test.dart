// main() 自己建好绑定：不能依赖测试框架先建好的那个（testWidgets 会建，所以别的测试发现不了这一类问题）。
// 绑定没建好就注册原生通道的处理器会抛异常，main 中止，窗口一直不出来
import 'package:flutter_test/flutter_test.dart';
import 'package:gk3_windows/main.dart' as app;

void main() {
  test('main() 在没有测试绑定时也能走完', () {
    expect(app.main, returnsNormally);
  });
}
