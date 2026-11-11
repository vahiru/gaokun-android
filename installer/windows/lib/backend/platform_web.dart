import 'package:gk3_common/backend/backend.dart';

/// Web 上没有真后端
Gk3Backend? locateRealBackend() => null;

/// http://localhost:xxxx/?scenario=installed
String? requestedScenario() => Uri.base.queryParameters['scenario'];

/// ?lang=en：预览英文界面
String? requestedLanguage() => Uri.base.queryParameters['lang'];

/// Web 上没有测试模式
int? requestedTestDisk() => null;
