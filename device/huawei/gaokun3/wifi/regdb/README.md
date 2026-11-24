# 无线监管数据库（`regulatory.db`）

内核cfg80211使用的无线监管数据库，取自[wireless-regdb](https://git.kernel.org/pub/scm/linux/kernel/git/wens/wireless-regdb.git) 2026.09.03发布包（`wireless-regdb-2026.09.03.tar.xz`，SHA-256 `b22e0901227b820cd1c280abe681a15b773a5103a5e10dc442e94ebb34cbf58d`），许可证见`LICENSE`（ISC）。

| 文件 | SHA-256 |
|---|---|
| `regulatory.db` | `7e236caecd939c8ec98be4870bf30422f28ffef2565a38aaaa2d9ddabd0c2641` |
| `regulatory.db.p7s` | `50332f0db09b8bcc719235ec4985c27377f2cc2f42abb7b5dcf951866c5a888a` |

## 用途

框架下发Wi-Fi国家码时，cfg80211除了把它交给ath11k，还要在数据库中查询该国家的规则来更新全局监管域。缺少数据库时cfg80211会改为等待用户态的CRDA回应，而Android没有CRDA：每次等待约3秒超时后重新设置全局监管域，如此反复约10次，在此期间后续的国家码请求都要排队，约30秒后才会处理。ath11k本身使用固件提供的信道表，不依赖这份数据库。

## 安装位置

`device.mk`把这两个文件安装到ramdisk的`/lib/firmware/`。cfg80211在启动早期（`late_initcall`）加载数据库，此时`/vendor`尚未挂载，`firmware_class.path`指向的`/vendor/firmware/`还不存在，内核会回退到ramdisk中的`/lib/firmware/`。首次加载失败后内核不会自动重试，所以不能只放在`/vendor/firmware/`。

内核开启了`CONFIG_CFG80211_REQUIRE_SIGNED_REGDB`，只接受由内核内置证书（`net/wireless/certs/`中的sforshee与wens）签名的数据库，`regulatory.db.p7s`必须与`regulatory.db`来自同一发布包。加载成功时内核不输出信息；启动后`dmesg`中不应出现`failed to load regulatory.db`或`loaded regulatory.db is malformed or signature is missing/invalid`。

## 更新

从发布包中取出`regulatory.db`、`regulatory.db.p7s`和`LICENSE`替换这里的文件，更新上表，然后用内核内置的证书校验签名：

```bash
python3 -I -c "import re,sys; sys.stdout.buffer.write(bytes(int(x,16) for x in re.findall(r'0x([0-9a-fA-F]{2})', open('<内核树>/net/wireless/certs/wens.hex').read())))" > wens.der
openssl cms -verify -binary -inform DER -in regulatory.db.p7s -content regulatory.db -certfile <(openssl x509 -inform DER -in wens.der) -nointern -noverify -purpose any -out /dev/null
```
