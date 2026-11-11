// 按平台选实现：Windows 上有 dart:io（以后接真的 gk3w）；Web（浏览器预览）只能回放演示数据。
export 'platform_web.dart' if (dart.library.io) 'platform_io.dart';
