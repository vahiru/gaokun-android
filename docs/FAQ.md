<!--
  Maintenance notes (not rendered):
  * Organized as "what happened → what to do". Where there is no reliable answer, say "not documented yet" or
    "not tested yet" rather than writing from memory.
  * Mirrors docs/FAQ.zh-CN.md; change both together.
  * Settings menu names: what the device shows is authoritative.
-->
# FAQ

[**中文 → FAQ.zh-CN.md**](FAQ.zh-CN.md) · [Installing](INSTALL.md) · [Known limitations](known-limitations.md)

* [Starting up and the boot menu](#starting-up-and-the-boot-menu)
* [Updates and going back](#updates-and-going-back)
* [Collecting logs](#collecting-logs)
* [Erasing your data](#erasing-your-data)
* [Removing Android, getting Windows back](#removing-android-getting-windows-back)
* [App display](#app-display)
* [Developer options and adb](#developer-options-and-adb)

## Starting up and the boot menu

### How do I get into the firmware settings, or boot from a USB stick?

With the keyboard cover (or a USB keyboard) attached, press `Esc` at power-on to enter the firmware settings, or press `F12` to open the firmware boot menu; how to do this without a keyboard has not been documented yet. If Windows is still on the machine, you can also restart into the firmware settings from Windows; see the [install guide](INSTALL.md#if-you-want-to-keep-windows).

To boot from a USB drive, disable Secure Boot, plug in the Live installer's USB drive, press `F12` at power-on, and choose it in the firmware boot menu.

### How do I use the boot menu, and what are its entries?

The boot menu stays for 5 seconds by default; without the keyboard cover, use the volume up/down buttons to move the cursor and the power button to confirm (with the keyboard cover attached, you can also use the arrow keys and Enter). The main entries are listed in the [install guide](INSTALL.md#the-boot-menu); the extra `Android (previous loader)` and per-slot `crDroid … — slot _a / _b` entries are for troubleshooting only (after an update, the old slot's entry no longer starts; see [below](#something-broke-after-an-update-can-i-go-back-to-the-previous-version)).

### The machine starts Windows directly, without the boot menu

There are two possible causes:

- **Secure Boot has been enabled again**, for example after a firmware update or after restoring the default settings. Disable Secure Boot again in the firmware settings; see the [install guide](INSTALL.md#if-you-want-to-keep-windows).
- **A Windows update has replaced the default boot file on the EFI partition.** The boot menu no longer appears, so `gaokun3 installer` on the internal disk cannot be reached either. In Windows, use the Windows installer to create a USB drive with the latest version (see the [install guide](INSTALL.md#creating-installation-media)), start the Live installer from it, choose "Reinstall Android" and select "Keep user data"; this writes the boot menu again. The version you install must not be older than the one on the tablet, or it may not start; the system to start by default has to be chosen again on the confirmation page. This has not been tested yet.

## Updates and going back

### Something broke after an update. Can I go back to the previous version?

It depends on whether the new version starts:

- **The new version does not finish starting** (for example it keeps restarting): the machine goes back to the previous version by itself, and you get a notification once Android is up.
- **The new version starts, but something does not work**: you cannot go back. Once the new version has booted successfully, the previous system is merged away, and from then on the old slot's entry in the boot menu no longer starts; choosing it only wastes a restart. Please wait for a fixed version, or reinstall the older version with the Live installer (see the next question).

On a freshly installed machine that has not been updated yet, the other slot holds no system.

### Can I install an older version?

Yes. The Windows installer only downloads the latest release; to install another version, download `boot.img`, `super.img.zst` and `install-artifacts.sha256` from that version's release and select "Local files" in the Windows installer. You can also install any version with "Reinstall Android" in the Live installer. But the newer version may already have upgraded your user data to a format the older one does not understand, so installing an older version while keeping data may leave it unable to start. When installing an older version, please wipe data (leave "Keep user data" unticked, which is also the default) and back up first.

### The machine keeps restarting or hangs. What can I do?

1. If it started after an update, wait for a few restarts first: when a new version repeatedly fails to start, the machine goes back to the previous version by itself, and you get a notification once Android is up. If it is stuck on one screen, hold the power button to turn it off, then start it again.
2. After going back to the previous version, collect logs as described in [When Android starts](#when-android-starts) and attach them to your report.
3. If it still does not start, start the Live installer and choose "Reinstall Android". Selecting "Keep user data" keeps your data; reinstalling the same version this way has been verified on a real machine. Do not install a version older than the one on the tablet.

## Collecting logs

When reporting a problem, please describe what you did and what happened, and include the system version and the logs below. Please use the [issue forms](https://github.com/vahiru/gaokun-android/issues/new/choose); you can also give feedback on [Telegram](https://t.me/gaokunAndroid) or in QQ group **920133252**.

### Setting up adb

1. Install Google's SDK Platform Tools on your computer; they include `adb`.
2. Enable Developer options and USB debugging on the tablet; see [below](#enabling-developer-options).
3. Connect the tablet to the computer with a USB cable and run `adb devices` on the computer. Use the USB-C port **next to the power button**; the other port cannot be used for USB debugging.
4. If the computer does not see the device, unplug the cable and plug it in again.
5. If a cable is inconvenient, use [wireless debugging](#wireless-debugging).

### When Android starts

The simplest and most complete way is:

```sh
adb bugreport gaokun3-bugreport.zip
```

It contains the system log, the kernel log and crash records. It also contains personal information such as Wi-Fi network names, account names and installed apps, so please check it before posting it publicly.

If you only need some of the logs:

```sh
adb logcat -b all -d > logcat.txt          # the whole system log
adb logcat -b crash -d > crash.txt         # crash records only
adb shell dmesg > dmesg.txt                # the kernel log
```

If `dmesg` reports a permission error, or you need the records saved automatically when audio and Bluetooth hang, first grant root to Shell in the ReSukiSU manager, then run:

```sh
adb shell su -c dmesg > dmesg.txt
adb exec-out "su -c 'cd /data/vendor/gaokun3 && tar -cf - hangdump-*'" > hangdump.tar
```

System version:

```sh
adb shell getprop ro.build.version.incremental
adb shell getprop ro.build.date.utc
```

### Without a computer

Whether "Bug report" in Developer options works on this machine has not been tested yet. For now, please collect logs with a computer as described above.

## Erasing your data

### How do I factory reset? How do I erase my data before selling or sending the machine for repair?

"Erase all data (factory reset)" in Settings is carried out by fastboot mode after a restart, but this has not been tested on a real machine yet, so please do not rely on it for now.

Erase your data as follows:

1. Start the Live installer: choose `gaokun3 installer` in the boot menu if it is there, otherwise boot from the Live installer's USB drive.
2. Choose "Reinstall Android" and do not select "Keep user data" (disabled by default).

The user data partition on this machine is not encrypted (see [known limitations](known-limitations.md)), so please make sure to erase it before selling the machine or sending it for repair.

## Removing Android, getting Windows back

### Using Windows now and then with dual boot

Choose `Windows Boot Manager` in the boot menu. With dual boot you can also make Windows the default system; see the [install guide](INSTALL.md#dual-boot).

### Removing Android completely from a dual-boot machine

Run the Windows installer in Windows, select "Manage" on the environment check page, then "Uninstall Android". This deletes all Android partitions and the installer partition on the internal disk, and restores the default EFI boot file to its state before the installation (usually the Windows Boot Manager). The freed space stays unallocated; to merge it back, right-click the partition just before it in Disk Management and select "Extend Volume".

### Installed with "Erase entire disk" and want Windows back

In this case Windows has already been deleted, and the only way back is to reinstall it from Huawei's recovery media or a Windows installation USB stick. This has not been tested on this machine yet. If you want to keep Windows, choose "Keep existing system" (dual boot) when installing.

## App display

### Some apps only use the middle of the screen in landscape

Apps that only support portrait, such as many phone apps, are shown in the middle of the screen in landscape, with black bars on both sides; the screen does not rotate for them. To make an app fill the screen:

1. Open Settings → Apps → Aspect ratio.
2. Select the app, then select "Full screen".

In full screen the app lays itself out for the landscape screen, and some screens may be cut off or stretched. If that happens, change it back to "App default".

Apps that only support landscape, such as most games, are not affected: the screen rotates to landscape when they open, unless the app is set to "Full screen" under Aspect ratio.

### Showing app pages side by side

Some apps, such as Zhihu, Taobao and WeChat, include split layouts for tablets. To show their pages side by side in landscape:

1. Open Settings → Apps → Force activity embedding (experimental).
2. Turn on the switch for the app.

The app is closed when the switch is changed; the change applies the next time the app opens. Only apps that include split layouts are listed. Known issues are listed in [Known limitations](known-limitations.md#showing-app-pages-side-by-side-is-experimental).

### The interface and icons got larger after an update — how do I change them back?

Since 1.0.0-rc.5, the default display density was raised from 240 to 280 (about 17% larger on devices that had not customized display size; devices that already customized it keep their existing setting) to suit typical tablet viewing distances. To make UI elements smaller:

- Lower the level under Settings → Display → Display size.
- The earlier 240 density is no longer a slider stop (the nearest stops are 224 and 252); to match the exact earlier scale, enable [Developer options](#enabling-developer-options) and set "Smallest width" to `1067` dp.

## Developer options and adb

### Enabling Developer options

1. In Settings → About tablet, tap "Build number" seven times and enter your screen lock when asked.
2. From then on you can find it under Settings → System → Developer options.

### USB debugging

Enable "USB debugging" in Developer options and connect the computer with a cable. The tablet asks "Allow USB debugging?"; check the computer's fingerprint, tick "Always allow from this computer" and tap Allow. Computers that have not been authorized cannot connect.

### Wireless debugging

1. Connect the tablet and the computer to the same Wi-Fi network.
2. In Developer options, enable "Wireless debugging", tap "Pair device with pairing code", and note the IP address, port and pairing code.
3. On the computer, run `adb pair <IP>:<pairing port>` and enter the pairing code. Each computer only needs to pair once.
4. Then run `adb connect <IP>:<port>`. This port is the one shown on the "Wireless debugging" page, not the pairing port.

Please disable wireless debugging when not in use.

### Why doesn't `adb root` work?

Release builds do not support `adb root`. When you need a root shell, first grant root to Shell in the ReSukiSU manager, then use `adb shell su`.

> v0.7.1 and earlier opened network adb on port 5555 without authorization by default, so anyone on the same network could connect and get root. This has been disabled since 1.0.0-rc.1. If you are still on an older version, please update soon, and until then do not join Wi-Fi networks you do not trust.
