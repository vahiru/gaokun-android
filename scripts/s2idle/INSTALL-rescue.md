# 在救援 Ubuntu 上启用 s2idle（挂起）

**实测：装完之后 `systemctl suspend` 3/3 成功**（`success=3 fail=0`，
每次墙钟 ~115 秒、内核时间只走 ~2.1 秒 = 真睡）。

## 装什么

```sh
sudo install -m755 gaokun3-usb-role-host.sh   /usr/local/bin/
sudo install -m644 gaokun3-usb-role.service   /etc/systemd/system/
sudo install -m755 gaokun3-usb-role-sleephook /usr/lib/systemd/system-sleep/gaokun3-usb-role
sudo systemctl enable gaokun3-usb-role.service
```

## ★★ 为什么必须是 `system-sleep` 钩子，而不是开机设一次

`a600000.usb` 的 role switch **不是我们说了算的** —— **typec/UCSI 层才是它的主人**
（dmesg 里那几行 `Fixed dependency cycle(s) with .../embedded-controller@38/connector@0`
就是这个关系）。实测：

* 开机单元确实成功置成了 host（journal: `role: device -> host（子 xhci = 1）`），
* **但到 up=76 秒时它又变回 `device` 了** —— 被 typec 层改回去的；
* 而且开机太早时写入还会直接失败（journal 里有一次 `⚠️ 置 host 失败，仍是 device`）。

⇒ **开机设一次是不够的。管用的是"每次挂起之前再设一次"，也就是
`/usr/lib/systemd/system-sleep/` 钩子。**（那个 service 留着无害，
它让系统正常运行时也处在好状态。）

## ⚠️ 验证时最容易踩的坑

**`echo mem > /sys/power/state` 不会跑 `system-sleep` 钩子** ——
只有 `systemctl suspend`（以及合盖/闲置这些走 logind 的路径）才会。
我第一版验收脚本用 `echo mem`，于是钩子从没被调用，看起来"修复无效"。
★ **用错的触发方式去验证一个挂在正确触发方式上的修复，得到的必然是假阴性。**

## ⚠️ 不要照搬到 Android

USB adb 的 UDC 就在这个控制器上（`sys.usb.controller=a600000.usb`），
置成 host 就没有 USB device-mode adb 了。
而且 Android 的 `device` 角色**有真实 gadget**，未必受影响 —— 需要单独实测。
