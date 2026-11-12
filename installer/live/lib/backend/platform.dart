// 按平台选实现：Linux（真机 / Linux 开发机）有 dart:io，能调 installer-lib.sh；
// Web（Mac 上用 Chrome 预览）没有，只能回放演示数据。
export 'platform_web.dart' if (dart.library.io) 'platform_io.dart';
