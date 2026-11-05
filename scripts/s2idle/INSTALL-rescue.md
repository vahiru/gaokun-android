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

## 为什么必须是 `system-sleep` 钩子，而不是开机设一次

`a600000.usb` 的 role switch 由 typec/UCSI 层管理
（dmesg 中的 `Fixed dependency cycle(s) with .../embedded-controller@38/connector@0`）。实测：

* 开机单元虽然可以将角色置为 host（`role: device -> host（子 xhci = 1）`），
* 但开机约 76 秒后会被 typec 层改回 `device`；
* 且开机过早写入时可能失败。

因此单靠开机设置一次不够，需要通过 `/usr/lib/systemd/system-sleep/` 钩子在每次挂起前将角色置为 `host`。

## 验证注意事项

**`echo mem > /sys/power/state` 不会执行 `system-sleep` 钩子** ——
只有 `systemctl suspend`（以及合盖、闲置等经过 logind 的路径）才会触发该钩子。验证时应使用 `systemctl suspend`。

## 不要照搬到 Android

USB adb 的 UDC 就在这个控制器上（`sys.usb.controller=a600000.usb`），
置成 host 就没有 USB device-mode adb 了。Android 侧的角色切换逻辑见 `device/huawei/gaokun3/bin/gaokun3-usbrole.sh`。
