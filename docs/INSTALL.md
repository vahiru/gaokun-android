<!--
  Maintenance notes (not rendered):
  * Mirrors docs/INSTALL.zh-CN.md; change both together.
  * Installer labels follow live/installer-flutter/lib/l10n/app_en.arb; boot menu entries are quoted as they appear.
  * The installer's risk page (riskExpBody) refers to the section "What has been tested on real hardware";
    if you rename it, update app_en.arb and app_zh.arb too.
-->
# Installing

[**中文 → INSTALL.zh-CN.md**](INSTALL.zh-CN.md) · [FAQ](FAQ.md) · [Known limitations](known-limitations.md)

This guide explains how to install the system on a Huawei MateBook E Go, and covers system updates, the boot menu, the rescue system and erasing your data afterwards.

> ### ⚠️ Before you install
> Except in dual-boot mode, installing erases the internal disk, including Windows and all your files, and cannot be undone. This project provides no image of the factory system; to get Windows back later, reinstall it from Huawei's recovery media or a Windows installation USB stick.
>
> Please back up your important data first, and make sure you are able to deal with a machine that does not boot.

## What has been tested on real hardware

Every installer feature has been checked on test disks. This is how far each path has been used on real machines:

| Path | Status |
|---|---|
| Reinstall Android, keeping user data | Done on a real machine, with the system image taken from the installation medium and downloaded over the network |
| Reinstall Android, wiping data | Test disks only |
| Erase the whole disk | Test disks only |
| Keep the current system (dual boot) | Users have installed this way on real machines |
| Adjust the disk manually | Test disks only |
| Starting the installer from a USB stick | Users have installed this way on real machines |
| Starting the installer from Windows | Users have installed this way on real machines |
| Command-line installer | Test disks only |

A "test disk" is a disk simulated on a computer, where the result of every write the installer makes is checked. It catches many problems, but it cannot fully stand in for a real disk and real firmware. If you take a path that has not been tested yet, we would welcome a [test report](https://github.com/vahiru/gaokun-android/issues/new/choose), whatever the outcome.

## Before you begin

| Item | Requirement |
|---|---|
| Model | Huawei MateBook E Go, 2022 or 2023 model. The installer checks the model; both report GK-W7X in their system information |
| BIOS version | Any |
| Secure Boot | Must be off. The kernel is not signed, and the installer stops if Secure Boot is on |
| Installation medium | A USB stick of 4 GB or more. If Windows is still on the machine, you can instead start the installer from Windows without a USB stick |
| Network | If the installation medium does not carry the system image, the installer downloads it over Wi-Fi (about 1.3 GB) |
| Keyboard cover | The graphical installer works by touch and has an on-screen keyboard; the command-line installer and choosing entries in the boot menu need the keyboard cover |

You do not need a second computer, adb or a copy of this repository. The firmware the system needs is already included in the released images.

Please also note that the user data partition is not encrypted: anyone who gets hold of the machine can read all of it. See [known limitations](known-limitations.md).

### If Windows is on the machine

Before installing, please do the following in Windows, in this order:

1. **Turn off Fast Startup**: Control Panel → Power Options → Choose what the power buttons do, and untick "Turn on fast startup". From then on, leave Windows with "Shut down" rather than "Hibernate". A hibernated Windows partition cannot be resized safely, and the installer refuses to touch it.
2. **Take care of BitLocker**: if BitLocker or Device Encryption is on, first make sure you can get the recovery key (<https://aka.ms/myrecoverykey>), then suspend BitLocker. Turning off Secure Boot makes Windows ask for the recovery key, so this step has to come before it.
3. **Turn off Secure Boot**: Settings → System → Recovery → Advanced startup → Restart now, then Troubleshoot → Advanced options → UEFI Firmware Settings, and turn off Secure Boot in the firmware settings.

When you start the installer from Windows, the script walks you through the first two steps in this order. Whether BitLocker resumes cleanly after being suspended on this machine has not been tested yet.

## Downloads

| File | Purpose | Where |
|---|---|---|
| `gaokun3-installer-<version>-usb.img.xz` | USB image of the graphical installer; carries the system image, so no network is needed | [v1.0.0-rc.2 release page](https://github.com/vahiru/gaokun-android/releases/tag/v1.0.0-rc.2) |
| `gaokun3-installer-<version>-windows.zip` | Starts the graphical installer from Windows; downloads the latest system during installation | Same as above |
| `boot.img`, `super.img.zst`, `install-artifacts.sha256` | System image, needed only for command-line and offline installs | Each release page; currently the latest is [v1.0.0-rc.3](https://github.com/vahiru/gaokun-android/releases/tag/v1.0.0-rc.3) |
| `rescue.squashfs`, `initramfs.img` | Rescue system, optional for command-line installs | [v1.0.0-rc.2 release page](https://github.com/vahiru/gaokun-android/releases/tag/v1.0.0-rc.2) |

The current graphical installer is version 0.2.0-rc.2. v1.0.0-rc.3 did not come with a new installer: the USB image carries the rc.2 system, so after installing, please update to rc.3 in Settings. Checksums are in `gaokun3-installer-<version>-SHA256SUMS` and `install-artifacts.sha256` on the same release pages.

### Downloading from the mirror

If GitHub downloads are slow or unreachable (for example in mainland China), use the mirror:

- Graphical installer: `https://ota.072172.xyz/installer/<installer version>/<file name>`
- System image: `https://ota.072172.xyz/install/<build>/<file name>`

`<build>` is the file name of that version's OTA package without `.zip`; the current one is in the `filename` field of <https://ota.072172.xyz/ota/gaokun3.json>. For v1.0.0-rc.3:

```
https://ota.072172.xyz/installer/0.2.0-rc.2/gaokun3-installer-0.2.0-rc.2-usb.img.xz
https://ota.072172.xyz/install/crDroidAndroid-17.0-20261030-gaokun3-v13.0/boot.img
https://ota.072172.xyz/install/crDroidAndroid-17.0-20261030-gaokun3-v13.0/super.img.zst
https://ota.072172.xyz/install/crDroidAndroid-17.0-20261030-gaokun3-v13.0/install-artifacts.sha256
```

## Using the graphical installer

### Step 1: start the installer

#### Option 1: from Windows, without a USB stick

1. Unzip `gaokun3-installer-<version>-windows.zip` and double-click `gaokun3-setup.cmd`; it asks for administrator rights. The script checks the model, the boot mode and Secure Boot, and asks you to type `YES` before it changes the disk.
2. The script shrinks D: by the little space the installer itself needs (about 0.5–2 GB), puts the installer there, and sets the next boot, and only the next one, to start it. The space for Android is chosen later, in the installer. If Fast Startup is still on, the script offers to turn it off for you. If D: is encrypted with BitLocker or Device Encryption, which the installer cannot shrink, the script asks whether to make room for Android right away.
3. Reboot into the installer. Choose "Shrink an existing partition to make room" and pick D:, then "Keep the current system". If you already made room for Android in step 2, choose "Keep the current system" directly. After the installer has shrunk D:, Windows checks the disk once on its next start; this is expected.

For an offline install, before step 1, put `boot.img`, `super.img.zst` and `install-artifacts.sha256` into a folder named `payload` next to `gaokun3-setup.cmd`. The installer then uses these files instead of downloading.

If you change your mind before installing, run `gaokun3-setup.cmd -Uninstall` to remove the installer's partition and boot entry and restore D:. If the machine went straight back into Windows instead of the installer, run the script again with `-UseFallbackPath`.

#### Option 2: from a USB stick

Write `gaokun3-installer-<version>-usb.img.xz` to a USB stick. balenaEtcher and Rufus read `.xz` files directly; on Linux you can run:

```sh
xz -dc gaokun3-installer-<version>-usb.img.xz | sudo dd of=/dev/sdX bs=4M conv=fsync status=progress
```

Plug in the stick and choose it in the firmware's boot menu.

### Step 2: choose how to install

| Option | What it does |
|---|---|
| Keep the current system (dual boot) | Creates partitions only in free space and leaves existing partitions alone; adds a boot loader to the EFI partition, so a menu to choose the system appears at every start. If there is not enough free space, use "Shrink an existing partition to make room" first to free some from an NTFS or ext4 partition |
| Erase the whole disk | Deletes everything on the disk and creates the [partition layout](#partition-layout) below |
| Reinstall Android | Available when Android is already on the disk. Keeps the existing partitions and rewrites the system, for repairing it or changing versions. Wipes user data by default; you can choose "Keep user data" in the next step. When moving to an older version, kept data may stop it from booting |
| Adjust the disk manually | Delete, create, format, shrink or grow partitions; each step is confirmed separately and carried out immediately |

Before writing to the disk, the installer shows a page of warnings: you have to scroll to the end, wait a moment and tick the confirmation before you can continue. Erasing the whole disk, or adjusting partitions on a disk with Windows, also asks you to type `ERASE`. If the Windows partition uses BitLocker and this installation has to replace the default boot program on the EFI partition, you also have to confirm that you have the recovery key.

For dual boot, the confirmation page lets you choose which system starts by default (Android unless you change it); see [Dual boot](#dual-boot).

If the installation medium carries the rescue system, you can tick "Also install the rescue system"; see [Rescue system](#rescue-system).

### If an installation fails

Use the installer's "Save logs" to save the logs to a second USB stick formatted as FAT or exFAT. You can also save them to the installation medium itself, but its EFI partition is hidden by default on Windows and macOS. When reporting the problem, please attach the whole log folder.

## Using the command-line installer (advanced)

The command-line installer can only erase the whole disk; it runs the same installation steps as "Erase the whole disk" in the graphical installer. Run it from the graphical installer's terminal:

1. Start the graphical installer from a USB stick. If you need to download the system image, connect to Wi-Fi in the installer first.
2. Tap "Quit to terminal" on the installer's first page, or press Ctrl+Alt+F2 (Ctrl+Alt+F1 switches back), and log in as `root` with no password.
3. Download the system image and run the installer:

   ```sh
   mkdir -p /tmp/rel && cd /tmp/rel
   B=crDroidAndroid-17.0-20261030-gaokun3-v13.0      # the version to install, see "Downloads"
   for f in boot.img super.img.zst install-artifacts.sha256; do
       curl -fLO "https://ota.072172.xyz/install/$B/$f" || echo "FAILED: $f"
   done
   /usr/share/gaokun3/install-gaokun3.sh /tmp/rel
   ```

Do not unpack the downloaded files. The installer first checks the machine, lists the partitions it will delete and the layout it will create, and writes nothing until you type `ERASE`. The target disk is `/dev/nvme0n1` by default; set `DISK=` to use another one. To install the rescue system as well, put `rescue.squashfs` and `initramfs.img` in the same directory.

> This procedure has not been run on a real machine yet.

You can also run `scripts/install-gaokun3.sh` from this repository on another arm64 Linux, but no general-purpose Linux distribution has been shown to boot on this machine yet.

### Partition layout

After erasing the whole disk, the disk contains:

| Partition | Size | Purpose |
|---|---|---|
| `esp` | 300 MiB | Boot menu and the kernel of each slot |
| `misc` | 4 MiB | A/B slot state |
| `metadata` | 32 MiB | Android metadata |
| `super` | 12 GiB | System partitions, A and B |
| `boot_a`, `boot_b` | 64 MiB each | Kernel and boot image, A and B |
| `gk3rescue` | 1 GiB | Rescue system (optional) |
| `userdata` | All remaining space | User data |

## First boot

The first boot takes about two to three minutes. Then follow the setup wizard; no extra scripts need to be run.

### "This device isn't Play Protect certified"

This system is not on Google's list of certified devices, so the Play Store shows "This device isn't Play Protect certified", and some apps refuse to install until the device is registered. Registering is free and only needs to be done once per device:

1. Sign in to your Google account on the tablet.
2. Read the device's Android ID. For now this means running a script from this repository on a computer, over adb:

   ```sh
   bash scripts/google/gsf-android-id.sh
   ```

   The ID is stored in Google Play services' private directory, so reading it needs root. Install the ReSukiSU manager first and grant root to Shell; the script uses it automatically. For setting up adb, see the [FAQ](FAQ.md).
3. With the same Google account, open <https://www.google.com/android/uncertified/>, enter the ID and register it.
4. Wait a few minutes, then clear the Google Play Store's storage (or run `adb shell pm clear com.android.vending`).

After a factory reset, or after clearing Google Play services' data, the ID changes and has to be registered again.

Play Integrity still fails after registering. It is a stricter check used by some banking and payment apps, and it requires a locked bootloader and a system signed by Google, while this system can only run with Secure Boot turned off. Apps that depend on Play Integrity cannot be used on this system.

## System updates

Check for and install updates in Settings → System → System update; the kernel is updated together with the system. Updates are written to the other slot, so you can keep using the tablet meanwhile, and take effect after a restart. If the network is unreliable, you can also download the OTA package another way, put it in the tablet's internal storage, and use "Install local update" on the same screen.

If an updated system repeatedly fails to finish booting, the machine automatically goes back to the previous version and tells you once Android is up. If the system hangs without restarting by itself, hold the power button to turn it off, then start it again.

> **You cannot go back to the previous version after an update.** Once the new version has booted successfully, the previous system is merged away. The boot menu still lists the other slot, but choosing it only wastes a boot. A major update (for example Android 16 to 17) also upgrades your user data on its first boot, so even if an older version starts, it may not be able to use that data. To go back, the only way is to reinstall the older version with the graphical installer, most likely wiping data. Please back up important data before updating.

## The boot menu

The boot menu appears at every start for 5 seconds by default; if you do not choose, the default system starts. You can select an entry with the volume up/down buttons and confirm with the power button, or use the arrow keys and Enter on the keyboard cover. The main entries are:

| Entry | Purpose |
|---|---|
| `Android` | Android, started by default |
| `Android fastboot / boot menu` | Fastboot mode; see [fastboot](#fastboot) |
| `gaokun3 rescue` | The rescue system; see [Rescue system](#rescue-system) |
| `Windows Boot Manager` | Windows, with dual boot |

The menu also lists Android entries for each slot and one for the previous boot loader. They are there for troubleshooting and are not needed in everyday use.

### Rescue system

This machine has no working Android recovery; the rescue system takes its place. It is a small Linux system, the same program as the graphical installer, so it can reinstall Android, and it also offers a terminal and SSH. It lives in its own 1 GiB partition, and changes made while it runs are gone after a restart.

To install it, tick "Also install the rescue system" during installation. When you install from a copy of the installer on the internal disk, this option is off by default, because that installer is itself in the boot menu and can reinstall just as well.

When Android hangs, the machine does not switch to the rescue system by itself. Hold the power button to turn it off, start it again and choose `gaokun3 rescue` in the boot menu. Restarting from the rescue system takes you back to Android.

To log in to the rescue system over SSH, save your SSH public key as `gaokun3/authorized_keys` on the installer USB stick before installing. The installer copies it to the rescue partition together with the Wi-Fi network you connected to. The released images contain no one's key; without one, the rescue system can only be used in front of the machine.

### fastboot

Choose `Android fastboot / boot menu` in the boot menu, or run `adb reboot bootloader` on a computer, to enter fastboot mode. The screen shows `FASTBOOT MODE`, the current slot and the USB connection state.

- Plug the cable into the USB-C port **next to the power button**; the other port does not support fastboot.
- On a computer with the Android SDK Platform Tools, you can use `fastboot devices`, `fastboot getvar all`, `fastboot oem device-info` (state of the disk, partitions, A/B and boot entries), `fastboot reboot` and `fastboot reboot bootloader`.
- On the tablet, the volume keys move between options and the power button confirms.

Flashing (`fastboot flash`) has not been tested on a real machine yet, and neither has confirming with the power button. To rewrite the system, please use the graphical installer.

### Dual boot

With dual boot, the system that starts by default is chosen on the installer's confirmation page and can be changed later in Android under Settings → System → Startup options. If you choose Windows, the first two starts after installing still go to Android, so that you can finish the initial setup. "Startup options" in Android can also restart straight into Windows. These features have not been tested systematically yet.

The 100 MiB EFI partition created when Windows is reinstalled is too small, and the installer does not support that layout.

When you start the installer from Windows, the script finally also installs a set of Windows companion tools, found in the gaokun3 folder of the Start menu. They can restart into Android, set the default system, repair the boot entry after a Windows update has replaced it, or remove Android. If you installed from a USB stick, run `gaokun3-windows\gaokun3-setup.cmd` from the stick in Windows to install them. The companion tools are a preview, and their functions have not been tested systematically yet.

## Erasing your data (factory reset)

"Erase all data (factory reset)" in Settings is carried out by fastboot mode after a restart, but this has not been tested on a real machine yet, so please do not rely on it for now.

To erase your data (including before selling or sending the machine for repair), start the graphical installer (`gaokun3 rescue` in the boot menu, or the installer USB stick), choose "Reinstall Android", and leave "Keep user data" unticked.

## If it does not boot

| Symptom | What to do |
|---|---|
| Black screen after power-on, no boot menu | Make sure Secure Boot is off |
| After an update, you chose the other slot's entry and the machine restarted | Expected: the old version has been merged away; see [System updates](#system-updates) |
| USB sticks do not appear in the firmware's boot menu, or Huawei's official recovery does not start | Earlier versions wrote kernel crash records into the firmware storage, and this happens once it is full; the mainboard is not broken. Since 1.0.0-rc.3 nothing is written there any more; existing records have to be cleaned up once as described in [issue #56](https://github.com/vahiru/gaokun-android/issues/56) |
| Android does not start | Start the rescue system and reinstall Android with the graphical installer. You can try keeping user data first |

## Reporting problems

Please use the [issue forms](https://github.com/vahiru/gaokun-android/issues/new/choose), and read [issues.md](../issues.md) before submitting. Logs help more than descriptions: if Android starts, please attach the archive produced by `adb bugreport`. It contains personal information such as Wi-Fi network names, account names and installed apps, so please check it before posting it publicly. How to collect each kind of log is described in the [FAQ](FAQ.md).
