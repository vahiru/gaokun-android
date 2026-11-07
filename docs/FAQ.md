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
* [The rescue system and SSH](#the-rescue-system-and-ssh)
* [Erasing your data](#erasing-your-data)
* [Removing Android, getting Windows back](#removing-android-getting-windows-back)
* [Developer options and adb](#developer-options-and-adb)

## Starting up and the boot menu

### How do I get into the firmware settings, or boot from a USB stick?

If Windows is still on the machine, you can open the firmware settings from Windows; see the [install guide](INSTALL.md#if-windows-is-on-the-machine). The keys for entering the firmware settings and the boot menu at power-on, and how to do it without the keyboard cover, have not been documented yet. If you know them, please tell us in the community groups or in an issue.

To boot from a USB stick, turn off Secure Boot, plug in the installer stick and choose it in the firmware's boot menu.

### USB sticks do not appear in the firmware's boot menu, and Huawei's official recovery does not start

Every version before 1.0.0-rc.3 writes the log into the mainboard firmware's variable storage on each kernel crash, and never cleans it up. After enough crashes the storage is full and the firmware can no longer create boot entries: USB sticks, card readers and Huawei's official recovery image no longer show up in the boot menu, and firmware updates or Windows changing its boot entries may fail too. The keyboard and mouse keep working, so this is easy to miss.

**The mainboard is not broken and does not need replacing.** Since 1.0.0-rc.3 the system no longer writes these records, but the ones already written do not go away by themselves and have to be cleaned up once as follows. Deleting them makes the firmware reorganize its storage, which can take a while; please do not force the machine off meanwhile.

**From Android** (first [set up adb](#setting-up-adb) and grant root to Shell in the ReSukiSU manager):

1. Check for leftover records:

   ```sh
   adb shell "su -c 'mkdir -p /data/local/tmp/efivars; mount -t efivarfs none /data/local/tmp/efivars 2>/dev/null; ls /data/local/tmp/efivars | grep -c ^dump-type'"
   ```

   `0` means there are none and nothing needs cleaning.

2. If you are willing to share these logs with us, save a copy first:

   ```sh
   adb exec-out "su -c 'cd /data/local/tmp/efivars && tar -cf - dump-type*'" > efi-dumps.tar
   ```

3. Delete the records:

   ```sh
   adb shell "su -c 'cd /data/local/tmp/efivars && chattr -i dump-type* && rm -f dump-type*; ls | grep -c ^dump-type'"
   ```

   A final `0` means they are gone.

**From the rescue system or another Linux**: run `sudo bash scripts/pstore-ctl.sh clear`, or run `chattr -i` on `/sys/firmware/efi/efivars/dump-type*` and then delete them. Deleting only the files in `/sys/fs/pstore/` does not actually remove the records on this machine.

**From Windows**: a method provided by the reporter, which we have not verified on this machine: with administrator rights, call `SetFirmwareEnvironmentVariableEx` with GUID `{CFC8FC79-BE2E-4DDC-97F0-9F98BFE298A0}`, the name of each `dump-type0-*`, a data length of 0 and attributes 7 (attributes 0 fails with error 19).

See [issue #56](https://github.com/vahiru/gaokun-android/issues/56) for the discussion.

### What are the entries in the boot menu?

The boot menu stays for 5 seconds by default. If you do not choose, the default entry starts.

| Entry | Purpose |
|---|---|
| `Android` | Android, started by default. It picks the slot by itself, and goes back to the previous version if an updated system repeatedly fails to start |
| `Android (previous loader)` | The previous version of the boot program, for when the `Android` entry has a problem |
| `Android fastboot / boot menu` | Fastboot mode; see the [install guide](INSTALL.md#fastboot) |
| `crDroid … (gaokun3) — slot _a`<br>`crDroid … (gaokun3) — slot _b` | Start Android from one particular slot, for troubleshooting. After an update, the old slot's entry no longer starts; see [below](#something-broke-after-an-update-can-i-go-back-to-the-previous-version) |
| `gaokun3 rescue (slot a kernel)`<br>`gaokun3 rescue (slot b kernel)` | The rescue system, present only if "Also install the rescue system" was ticked during installation. The two differ only in which slot's kernel they borrow |
| `gaokun3 installer` | Left behind when the installer was started from Windows |
| `Windows Boot Manager` | Windows, with dual boot |

### Can I use the boot menu without the keyboard cover?

Yes. Without the keyboard cover, use the volume up/down buttons to move the cursor and the power button to confirm; with the keyboard cover attached, you can also use the arrow keys and Enter.

## Updates and going back

### Something broke after an update. Can I go back to the previous version?

It depends on whether the new version starts:

- **The new version does not finish starting** (for example it keeps restarting): the machine goes back to the previous version by itself, and you get a notification once Android is up.
- **The new version starts, but something does not work**: you cannot go back. Once the new version has booted successfully, the previous system is merged away, and from then on the old slot's entry in the boot menu no longer starts; choosing it only wastes a restart. Please wait for a fixed version, or reinstall the older version with the graphical installer (see the next question).

On a freshly installed machine that has not been updated yet, the other slot holds no system.

### Can I install an older version?

You can install any version with "Reinstall Android" in the graphical installer. But the newer version may already have upgraded your user data to a format the older one does not understand, so installing an older version while keeping data may leave it unable to start. When installing an older version, please wipe data (leave "Keep user data" unticked, which is also the default) and back up first.

### The machine keeps restarting or hangs. What can I do?

1. If it started after an update, wait for a few restarts first; the machine usually goes back to the previous version by itself. If it is stuck on one screen, hold the power button to turn it off, then start it again.
2. If it still does not start, choose `gaokun3 rescue` in the boot menu, or boot from the installer USB stick, and choose "Reinstall Android". Ticking "Keep user data" keeps your data; reinstalling the same version this way has been verified on a real machine.
3. You can also collect logs from the rescue system; see [Collecting logs when Android does not start](#collecting-logs-when-android-does-not-start).

## Collecting logs

When reporting a problem, please describe what you did and what happened, and include the system version and the logs below. Please use the [issue forms](https://github.com/vahiru/gaokun-android/issues/new/choose); you can also give feedback on [Telegram](https://t.me/gaokunAndroid) or in QQ group **920133252**.

### Setting up adb

1. Install Google's SDK Platform Tools on your computer; they include `adb`.
2. Turn on Developer options and USB debugging on the tablet; see [below](#turning-on-developer-options).
3. Connect the tablet to the computer with a USB cable and run `adb devices` on the computer. Use the USB-C port **next to the power button**; the other port cannot be used for USB debugging.
4. If the computer does not see the device, unplug the cable and plug it in again. With some computers the two sides occasionally negotiate the wrong way round, so that the tablet powers the computer and neither sees the other; see [known limitations](known-limitations.md#plugged-into-a-computer-the-tablet-may-charge-the-computer-instead).
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

### Collecting logs when Android does not start

Choose `gaokun3 rescue` in the boot menu, or boot from the installer USB stick.

- 45 seconds after starting, the rescue system writes diagnostics to `/media/gk3/gaokun3/diag/`. This directory is on the disk and survives a restart.
- `journalctl -k` shows the kernel log.
- If SSH is set up, fetch the diagnostics on your computer with `scp -r root@<IP>:/media/gk3/gaokun3/diag .`; see the next section for setting it up.
- When booted from the installer USB stick, `/media/gk3` is the stick itself. Plug it into any computer and the logs are in `gaokun3/diag/`.

## The rescue system and SSH

### How do I log in to the rescue system over SSH?

The rescue system only accepts SSH public-key logins. The released images contain no one's key, so you need to add your own first; otherwise it can only be used in front of the machine.

**Before installing** (recommended):

1. Generate a key pair on your computer (skip if you already have one): `ssh-keygen -t ed25519`.
2. Save the public key (the contents of `~/.ssh/id_ed25519.pub`) as `gaokun3/authorized_keys` on the installer USB stick. That partition of the stick is FAT, so Windows and macOS can write to it directly.
3. If you want the rescue system to join Wi-Fi when it starts, connect to Wi-Fi once in the installer and it copies that configuration along; or put a `gaokun3/wpa_supplicant.conf` on the stick.
4. Tick "Also install the rescue system" during installation.

**After installing**:

1. Choose `gaokun3 rescue` in the boot menu; the installer screen appears.
2. Tap "Quit to terminal" (or press Ctrl+Alt+F2) and log in as `root`. No password is needed in front of the machine.
3. Add your public key and restart the SSH service:

   ```sh
   cat >> /media/gk3/gaokun3/authorized_keys     # paste the key, press Enter, then Ctrl+D
   systemctl restart ssh
   ```

   The file is stored on the rescue partition, so it stays in effect every time you enter the rescue system.

**Connecting**: in the rescue system's terminal, find the IP address with `ip addr`, then run `ssh root@<IP>` on your computer.

## Erasing your data

### How do I factory reset? How do I erase my data before selling or sending the machine for repair?

"Erase all data (factory reset)" in Settings is carried out by fastboot mode after a restart, but this has not been tested on a real machine yet, so please do not rely on it for now.

Erase your data as follows:

1. Choose `gaokun3 rescue` in the boot menu; if it is not there, boot from the installer USB stick.
2. Choose "Reinstall Android" and leave "Keep user data" unticked (it is unticked by default).
3. The system image is read from the USB stick; if the stick does not carry it, it is downloaded over Wi-Fi (about 1.3 GB).

The user data partition on this machine is not encrypted (see [known limitations](known-limitations.md)), so please make sure to erase it before selling the machine or sending it for repair.

## Removing Android, getting Windows back

### Using Windows now and then with dual boot

Choose `Windows Boot Manager` in the boot menu. With dual boot you can also make Windows the default system; see the [install guide](INSTALL.md#dual-boot).

### Removing Android completely from a dual-boot machine

If you started the installer from Windows, the gaokun3 folder in the Start menu has a "Remove Android" entry. It restores the boot settings, deletes Android's partitions and gives the space back to D:. This tool is a preview and has not been run on a real Windows installation yet, and the full manual removal has not been carried out on a real machine either. Before you start, please back up and ask in the community groups.

For reference, the installer changed the following:

- **Boot program**: `EFI/BOOT/BOOTAA64.EFI` on the EFI partition was replaced with this system's boot menu, and the original was backed up as `EFI/BOOT/BOOTAA64.EFI.before-gaokun3`. Once that file is restored, the machine starts straight into Windows.
- **Files on the EFI partition**: a directory named after the machine-id containing `android/`, and the configuration and boot entries under `loader/`.
- **Partitions**: `misc`, `metadata`, `super`, `boot_a`, `boot_b`, `userdata`, and possibly `gk3rescue`. After deleting them, you can merge the space back in Windows.

Note that once Android is installed, `gaokun3-setup.cmd -Uninstall` only removes the installer's own boot entry and files; it does not remove Android.

### Installed with "Erase the whole disk" and want Windows back

In this case Windows has already been deleted, and the only way back is to reinstall it from Huawei's recovery media or a Windows installation USB stick. This has not been tested on this machine yet. If you want to keep Windows, choose "Keep the current system" (dual boot) when installing.

## Developer options and adb

### Turning on Developer options

1. In Settings → About tablet, tap "Build number" seven times and enter your screen lock when asked.
2. From then on you can find it under Settings → System → Developer options.

### USB debugging

Turn on "USB debugging" in Developer options and connect the computer with a cable. The tablet asks "Allow USB debugging?"; check the computer's fingerprint, tick "Always allow from this computer" and tap Allow. Computers that have not been authorized cannot connect.

### Wireless debugging

1. Connect the tablet and the computer to the same Wi-Fi network.
2. In Developer options, turn on "Wireless debugging", tap "Pair device with pairing code", and note the IP address, port and pairing code.
3. On the computer, run `adb pair <IP>:<pairing port>` and enter the pairing code. Each computer only needs to pair once.
4. Then run `adb connect <IP>:<port>`. This port is the one shown on the "Wireless debugging" page, not the pairing port.

Please turn wireless debugging off when you are not using it.

### Why doesn't `adb root` work?

Release builds do not support `adb root`. When you need a root shell, first grant root to Shell in the ReSukiSU manager, then use `adb shell su`.

> v0.7.1 and earlier opened network adb on port 5555 without authorization by default, so anyone on the same network could connect and get root. This has been turned off since 1.0.0-rc.1. If you are still on an older version, please update soon, and until then do not join Wi-Fi networks you do not trust.
