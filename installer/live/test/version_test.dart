// 版本号只有一个真相源：pubspec.yaml。界面上显示的常量与它必须一致（镜像构建也从 pubspec 读）。
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:gk3_installer/version.dart';

void main() {
  test('kInstallerVersion == pubspec 的 version（去掉 +构建号）', () {
    final m = RegExp(r'^version:\s*(\S+)', multiLine: true).firstMatch(File('pubspec.yaml').readAsStringSync());
    expect(m, isNotNull);
    expect(m!.group(1)!.split('+').first, kInstallerVersion);
  });
}
