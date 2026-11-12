// Session 里不经过界面也能测的几件事：按来源取版本列表、BitLocker 恢复密钥的校验、本地目录的引号
import 'dart:async';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:gk3_common/backend/fixture_backend.dart';
import 'package:gk3_common/backend/protocol.dart';
import 'package:gk3_windows/model.dart';
import 'package:gk3_windows/session.dart';
import 'package:gk3_windows/ui/bitlocker_box.dart';

import 'helpers.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  test('本地目录：去掉"复制文件地址"带的一对双引号', () {
    final s = Session(Rec(FixtureBackend('free', bundle: DiskBundle(), speed: 0)));
    s.setLocalDir('  "D:\\Downloads\\gaokun3 rc3"  ');
    expect(s.localDir, r'D:\Downloads\gaokun3 rc3');
    s.setLocalDir('"');
    expect(s.localDir, '"');
  });

  test('恢复密钥：经标准输入交给 gk3w bitlocker-key（参数里没有）；重新检查之后要重新输入', () async {
    final rec = Rec(FixtureBackend('free', bundle: DiskBundle(), speed: 0));
    rec.map['preflight'] = bitlockerOn();
    rec.respond['bitlocker-key'] = fakeBitlockerKey;
    final s = Session(rec);
    await s.check();
    expect(s.needBitlockerKey, isTrue);
    expect(s.bitlockerReady, isFalse);
    expect(s.preflight!.bitlockerKeyIds, ['3F2A9C41']);
    // 空格分组也行：规整成连字符分组再交出去
    expect(await s.verifyBitlockerKey(kGoodKey.replaceAll('-', ' ')), isTrue);
    expect(s.bitlockerReady, isTrue);
    expect(rec.calls.last, ['bitlocker-key']);
    expect(rec.inputs.last, '$kGoodKey\n');
    await s.check();
    expect(s.bitlockerVerified, isFalse);
    expect(s.bitlockerReady, isFalse);
  });

  test('恢复密钥：对不上、位数不对（不调 gk3w）都不通过，原因留给界面', () async {
    final rec = Rec(FixtureBackend('free', bundle: DiskBundle(), speed: 0));
    rec.map['preflight'] = bitlockerOn();
    rec.respond['bitlocker-key'] = fakeBitlockerKey;
    final s = Session(rec);
    await s.check();
    expect(await s.verifyBitlockerKey(kOtherKey), isFalse);
    expect(s.bitlockerErr!['code'], 'bitlocker-key-wrong');
    final n = rec.calls.length;
    expect(await s.verifyBitlockerKey('123456-123456'), isFalse);
    expect(s.bitlockerErr!['code'], 'bitlocker-key-format');
    expect(rec.calls.length, n);
    s.clearBitlockerKey();
    expect(s.bitlockerErr, isNull);
  });

  test('保护已暂停、没有加密、测试模式：不要恢复密钥；系统盘没有恢复密钥：挡住', () async {
    Future<Session> withPreflight(Map<String, String> f, {int? testDisk}) async {
      final rec = Rec(FixtureBackend('free', bundle: DiskBundle(), speed: 0));
      rec.map['preflight'] = (e) => e is Gk3Record && e.type == 'PREFLIGHT' ? Gk3Record('PREFLIGHT', {...e.fields, ...f}) : e;
      final s = Session(rec, testDisk: testDisk);
      await s.check();
      return s;
    }

    expect((await withPreflight({'bitlocker': 'suspended'})).needBitlockerKey, isFalse);
    expect((await withPreflight({'bitlocker': 'off'})).needBitlockerKey, isFalse);
    expect((await withPreflight({'bitlocker': 'on'}, testDisk: 2)).needBitlockerKey, isFalse);
    // 状态不明、ID 查不到：仍要密钥（由 gk3w 决定能不能校验）
    final unknown = await withPreflight({'bitlocker': 'unknown'});
    expect(unknown.needBitlockerKey, isTrue);
    expect(unknown.bitlockerNoKey, isFalse);
    final none = await withPreflight({'bitlocker': 'on', 'bitlocker_key_ids': ''});
    expect(none.bitlockerNoKey, isTrue);
  });

  test('normalizeRecoveryKey：去掉空格与连字符后正好 48 位数字才算', () {
    expect(normalizeRecoveryKey(' ${kGoodKey.replaceAll('-', '')} '), kGoodKey);
    expect(normalizeRecoveryKey(kGoodKey.substring(1)), isNull);
    expect(normalizeRecoveryKey('${kGoodKey}1'), isNull);
    expect(normalizeRecoveryKey(kGoodKey.replaceFirst('1', 'a')), isNull);
  });

  test('一个来源还在取版本列表时切到另一个：另一个也会去取', () async {
    // 按录制的节奏回放（GitHub 640 ms、R2 410 ms），两次取在时间上重叠
    final rec = Rec(FixtureBackend('free', bundle: DiskBundle(), speed: 1));
    final s = Session(rec);
    final gh = s.loadVariants();
    expect(s.loadingVariants, isTrue);
    s.setSource(ReleaseSource.r2);
    await Future<void>.delayed(const Duration(milliseconds: 1500));
    await gh;
    expect(rec.calls.where((c) => c.first == 'releases').map((c) => c.last), ['github', 'r2']);
    expect(s.variants.single.name, 'crDroid 13.0');
    s.setSource(ReleaseSource.github);
    expect(s.variants, isNotEmpty);
    expect(s.loadingVariants, isFalse);
  });

  test('版本列表的调用出错（流里抛异常）：不一直转圈，报"无法获取版本列表"', () async {
    final rec = Rec(FixtureBackend('free', bundle: DiskBundle(), speed: 0));
    rec.replace['releases'] = (_) => Stream.error(StateError('No element'));
    final s = Session(rec);
    await s.loadVariants();
    expect(s.loadingVariants, isFalse);
    expect(s.variants, isEmpty);
    expect(s.variantsErr!['code'], 'manifest-unavailable');
  });

  test('正在加密或解密：不要密钥、单独挡住；测试模式不挡', () async {
    Future<Session> make({int? testDisk}) async {
      final rec = Rec(FixtureBackend('free', bundle: DiskBundle(), speed: 0));
      rec.map['preflight'] = (e) => e is Gk3Record && e.type == 'PREFLIGHT' ? Gk3Record('PREFLIGHT', {...e.fields, 'bitlocker': 'converting'}) : e;
      final s = Session(rec, testDisk: testDisk);
      await s.check();
      return s;
    }

    final s = await make();
    expect(s.bitlockerConverting, isTrue);
    expect(s.needBitlockerKey, isFalse);
    expect(s.bitlockerSuspendFirst, isFalse);
    expect((await make(testDisk: 2)).bitlockerConverting, isFalse);
  });

  test('重新检查时上一次还没返回：晚到的旧结果不盖住新的', () async {
    final rec = Rec(FixtureBackend('free', bundle: DiskBundle(), speed: 0));
    final slow = StreamController<Gk3Event>();
    Gk3Record pf(String bl) => Gk3Record('PREFLIGHT', {
          'manufacturer': 'HUAWEI', 'model': 'GK-W7X', 'model_ok': 'yes', 'secure_boot': 'on', 'bitlocker': bl,
          'fast_startup': 'off', 'hibernate': 'on', 'hiberfil': 'yes', 'ac': 'yes', 'battery': '95',
        });
    var n = 0;
    rec.respond['preflight'] = (args, input) => n++ == 0 ? slow.stream : Stream.fromIterable([pf('suspended'), const Gk3Exit(0)]);
    final s = Session(rec);
    final first = s.check();
    await s.check();
    expect(s.preflight!.bitlocker, 'suspended');
    slow
      ..add(pf('on'))
      ..add(const Gk3Exit(0));
    await slow.close();
    await first;
    expect(s.preflight!.bitlocker, 'suspended');
    expect(s.checking, isFalse);
  });

  group('恢复密钥输入框的格式', () {
    final f = RecoveryKeyFormatter();
    TextEditingValue v(String t, [int? c]) => TextEditingValue(text: t, selection: TextSelection.collapsed(offset: c ?? t.length));
    TextEditingValue type(TextEditingValue old, String ch) {
      final c = old.selection.extentOffset;
      return f.formatEditUpdate(old, v(old.text.substring(0, c) + ch + old.text.substring(c), c + ch.length));
    }

    test('逐位输入：满 6 位之后的下一位前面加连字符，光标跟在刚输入的数字后面', () {
      var x = v('');
      for (final ch in '1234567'.split('')) {
        x = type(x, ch);
      }
      expect(x.text, '123456-7');
      expect(x.selection.extentOffset, 8);
    });

    test('粘贴：连字符、空格、别的字符都整理掉，超过 48 位的截掉', () {
      expect(f.formatEditUpdate(v(''), v(kGoodKey.replaceAll('-', ' '))).text, kGoodKey);
      expect(f.formatEditUpdate(v(''), v('密钥：$kGoodKey')).text, kGoodKey);
      expect(f.formatEditUpdate(v(''), v('${kGoodKey}999')).text, kGoodKey);
    });

    test('在连字符后面退格：删掉它前面的那位数字；在它前面按 Delete：删掉它后面的那位', () {
      // "123456-7|" 退格两次：先删 7，再删 6（不是只删连字符、马上又补回来）
      var x = f.formatEditUpdate(v('123456-7'), v('123456-', 7));
      expect(x.text, '123456');
      x = f.formatEditUpdate(v('123456-789', 7), v('123456789', 6));
      expect(x.text, '123457-89');
      expect(x.selection.extentOffset, 5);
      // "123456|-789" 按 Delete
      x = f.formatEditUpdate(v('123456-789', 6), v('123456789', 6));
      expect(x.text, '123456-89');
      expect(x.selection.extentOffset, 6);
    });

    test('在中间插入一位：后面的重新分组，光标跟在插入的数字后面', () {
      final x = type(v('123456-789', 2), '0');
      expect(x.text, '120345-6789');
      expect(x.selection.extentOffset, 3);
    });
  });
}
