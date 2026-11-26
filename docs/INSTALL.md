<!--
  Maintenance notes (not rendered):
  * Mirrors docs/INSTALL.zh-CN.md; change both together.
  * Live installer labels follow installer/live/lib/l10n/app_en.arb, Windows installer labels follow
    installer/windows/lib/l10n/app_en.arb; boot menu entries are quoted as they appear.
-->
# Installing

[**中文 → INSTALL.zh-CN.md**](INSTALL.zh-CN.md) · [FAQ](FAQ.md) · [Known limitations](known-limitations.md)

This guide explains how to install the system on a Huawei MateBook E Go, and covers system updates, the boot menu and erasing your data afterwards.

> ### ⚠️ Before you install
> Except when you keep the existing system (dual boot with Windows), installing erases the internal disk, including Windows and all your files, and cannot be undone. This project provides no image of the factory system; to get Windows back later, reinstall it from Huawei's recovery media or a Windows installation USB drive.
>
> Please back up your important data first, and make sure you are able to deal with a machine that does not boot.

## Two installers

| | Windows installer | Live installer |
|---|---|---|
| How it runs | Inside the Windows installed on the machine | Started from a USB drive or from the installer partition on the internal disk; does not depend on any system on the disk |
| What it does | Installs into unallocated space on the system disk (dual boot with Windows); uninstalls Android; creates standalone installation media (Live installer) | Keep existing system (dual boot); erase entire disk; reinstall Android; delete partitions; delete installer partition |
| System installation files | The latest release is downloaded, or files you have already downloaded are used | Carried on the installation media; no network is used during installation |
| Best for | Keeping Windows on a machine that already has it | Not keeping Windows, reinstalling Android, or repairing a system that does not start |

Both installers install only to the machine's internal disk.

## Before you begin

| Item | Requirement |
|---|---|
| Model | Huawei MateBook E Go, 2022 or 2023 model. The installer checks the model; both report GK-W7X in their system information |
| BIOS version | Any |
| Secure Boot | Must be disabled. The kernel is not signed and does not start while Secure Boot is enabled |
| Disk space | To keep Windows, the system disk needs at least 20.2 GiB of contiguous unallocated space; at least 22.2 GiB if you first write the Live installer to the internal disk and install from it |
| Installation media | For the Live installer, a USB drive of 4 GB or more; alternatively, the Windows installer can write the Live installer to the internal disk, so no USB drive is needed |
| Power | Charger connected, or battery above 15% |
| Keyboard cover | Both installers work by touch, and the Live installer has an on-screen keyboard. The boot menu works with the volume and power buttons |

You do not need a second computer, adb or a copy of this repository. The firmware the system needs is already included in the released images.

Note also that the user data partition of this system is not encrypted: anyone who gets hold of the machine can read all of it. See [Known limitations](known-limitations.md).

### If you want to keep Windows

- **Using the Windows installer**: you can [download](#downloads) and launch the Windows installer first — its "Environment check" page guides you through each requirement below (including showing the unmovable files that prevent a partition from being shrunk, and verifying your recovery key to suspend BitLocker protection before you disable Secure Boot), after which you enter the firmware settings to disable Secure Boot.
- **Using the Live installer directly from a USB drive**: complete the following 4 steps in Windows beforehand, in order:

1. **Disable Fast Startup**: uncheck "Turn on fast startup" under Control Panel → Power Options → Choose what the power buttons do. From then on, leave Windows with "Shut down" rather than "Hibernate". Writing to the shared EFI system partition while Windows is hibernated can corrupt its data, and the installers refuse to do it.
2. **Free up space**: in Disk Management, shrink a partition on the system disk to free up at least 20.2 GiB of contiguous unallocated space (at least 22.2 GiB if you first write the Live installer to the internal disk and install from it). The installers do not resize existing partitions. If a partition cannot be shrunk enough, the Windows installer can show which file is restricting the shrink and explain how to resolve it.
3. **Deal with BitLocker**: if BitLocker or device encryption is enabled, first make sure you can retrieve the 48-digit recovery key at <https://aka.ms/myrecoverykey> and save a copy on another device such as your phone. If you disable Secure Boot or modify boot files without first suspending protection in the Windows installer, Windows will prompt for this recovery key on its next boot (suspending and resuming BitLocker on the system drive has not been verified on this machine yet).
4. **Disable Secure Boot**: Settings → System → Recovery → Advanced startup → Restart now, then Troubleshoot → Advanced options → UEFI Firmware Settings, and disable Secure Boot there.

## Downloads

The installers and the system installation files are on the [Releases page](https://github.com/vahiru/gaokun-android/releases):

| File | Purpose | Release |
|---|---|---|
| `gaokun-android-windows-installer-<version>-arm64.zip` | Windows installer | `windows-installer-v<version>` |
| `gaokun3-live-<version>-android-<system version>.iso` | Live installer carrying the installation files of that system version (about 1.5 GB); write it to a USB drive and install | `live-installer-v<version>-for-<system version>` |
| `gaokun3-live-<version>-for-<system version>.iso` | Live installer without system installation files (about 180 MB); used by the Windows installer to create installation media, and usually not needed otherwise | Same as above |
| `boot.img`, `super.img.zst`, `install-artifacts.sha256` | System installation files | The release of each system version |

Each time a system version is released, a Live installer carrying that version's system installation files is released along with it; download the latest one. The Windows installer downloads the system installation files and the Live installer itself, so you do not need to download them by hand when you use it. Checksums are in the `.sha256` file, `SHA256SUMS` and `install-artifacts.sha256` of the same release.

### Downloading the system installation files from the mirror

If GitHub is slow or unreachable (for example in mainland China), the system installation files can be downloaded from a mirror. The mirror currently provides only the system installation files, not the installers. In the Windows installer, switch the download source to "R2 mirror". To download by hand, use:

```
https://ota.072172.xyz/install/<build>/<file name>
```

`<build>` is the file name of that version's OTA package without `.zip`; the current one is in the `filename` field of <https://ota.072172.xyz/ota/gaokun3.json>.

## Using the Windows installer

### Installing

1. Extract all of `gaokun-android-windows-installer-<version>-arm64.zip`, run `gk3_windows.exe` inside it and allow it to run as administrator when prompted. Do not run it directly from an archive viewer.
2. Resolve any unmet requirements as the environment check describes, then select "Next".
3. Select the installation location (an unallocated area on the system disk), the download source, and the system to start by default. The installer downloads the latest system release; to install another version, select "Local files" and use the installation files of that version that you have downloaded.
4. Check the summary, then hold the confirm button for 2 seconds to start. Downloading and writing take a few minutes; do not close the keyboard cover or press the power button meanwhile.
5. Restart when the installation is complete. The boot menu now offers Android and Windows.

The installer creates the Android partitions in the selected unallocated space, installs the boot manager (systemd-boot) to the EFI system partition, and backs up the original default boot file (BOOTAA64.EFI). Windows and your files on C: are not affected.

Some security software (for example Huorong) may block the installer's backend process `gk3w.exe` while it writes to the disk. If you see such a prompt, allow it; blocking it midway may leave disk partitions in an incomplete state. The installer's logs are in `%ProgramData%\gaokun3\logs`.

### Uninstalling

When Android is installed, select "Manage" on the environment check page, then "Uninstall Android". This deletes all Android partitions and the installer partition on the internal disk, and restores the default EFI boot file to its state before the installation (usually the Windows Boot Manager). The freed space stays unallocated; to merge it back, right-click the partition just before it in Disk Management and select "Extend Volume".

### Creating installation media

"Create installation media" at the bottom of the left menu writes the Live installer and the system installation files to:

- **A USB drive**: the whole drive is erased first. The finished drive can be used on any MateBook E Go.
- **The internal disk**: an installer partition of about 2 GiB (GK3LIVE) is created at the end of an unallocated area on the system disk, and `gaokun3 installer` is added to the boot menu. If Android is not installed yet, the boot manager (systemd-boot) is installed to the EFI system partition and the original default boot file (BOOTAA64.EFI) is backed up, with Windows remaining the default. Whenever BitLocker is enabled on the system disk (whether or not Android is already installed), verifying the recovery key is required before writing or deleting the installer partition. Other partitions and their data are not affected.

The Live installer can only be downloaded from GitHub or taken from local system installation files placed in the same directory as a Live installer ISO; with "R2 mirror" selected, installation media cannot be created.

If the internal disk already has an installer partition, it can be deleted in the "Select target location" step (if Android is not installed yet, deleting it also restores the default EFI boot file; deleting the installer partition separately has not been verified on real hardware yet). When you install Android, the Live installer also deletes the installer partition by default.

## Using the Live installer

### Preparing the installation media

Use any one of the following:

- **Create it with the Windows installer**: see [Creating installation media](#creating-installation-media).
- **Write the ISO that carries the system installation files**: write `gaokun3-live-<version>-android-<system version>.iso` to a USB drive. Rufus (choose "Write in DD Image mode") and balenaEtcher can write it directly; on Linux, run:

  ```sh
  sudo dd if=gaokun3-live-<version>-android-<system version>.iso of=/dev/sdX bs=4M conv=fsync status=progress
  ```

- **Copy the files to a USB drive**: format the USB drive as FAT32 (Windows built-in formatting supports drives up to 32 GB; use one of the two methods above for larger drives), open the ISO that carries the system installation files in Windows by double-clicking it, and copy all of its files to the drive.

### Starting the Live installer

- **From a USB drive**: plug it in, press `F12` at power-on to open the firmware boot menu, and select it (see the [FAQ](FAQ.md#how-do-i-get-into-the-firmware-settings-or-boot-from-a-usb-stick)).
- **From the internal disk**: select `gaokun3 installer` in the boot menu.

The Live installer normally reads all of its files into memory when it starts, so the USB drive can be removed afterwards.

### Choosing how to install

| Installation mode | What it does |
|---|---|
| Keep existing system (dual boot) | Creates new partitions in the largest unallocated area without modifying existing ones, installs the boot manager (systemd-boot) to the EFI system partition (backing up original boot files), and shows a boot menu at startup. Free up space in Windows first; see [If you want to keep Windows](#if-you-want-to-keep-windows) |
| Erase entire disk | Deletes everything on the disk and partitions it as in [Partition layout](#partition-layout) (not yet verified on real hardware). When started from the installer partition on the internal disk, that partition is kept while the disk is erased and deleted after the installation by default (see below) |
| Reinstall Android | Available when Android is already on the disk. Keeps the existing partitions and writes the system again, to repair it or change versions. User data is erased by default; you can select "Keep user data" in the next step. Keeping data when moving to an older version may leave the system unable to start |
| Delete partitions | Deletes partitions on the disk; each deletion is confirmed separately and takes effect immediately |
| Delete installer partition | Available when started from the installer partition on the internal disk (not yet verified on real hardware). Deletes that partition; if Android is not installed yet, the EFI system partition is also restored to its state before the installer partition was written |

Before anything is written to the disk, the installer shows a risk page that you have to scroll to the end of, wait on for a while, and tick to confirm. Erasing the entire disk, or deleting partitions on a disk that has Windows, also requires typing `ERASE`. If the Windows partition uses BitLocker and this installation replaces the default boot file on the EFI partition, you also have to confirm that you have the recovery key.

When started from the installer partition on the internal disk, the options page has "Keep installer partition", which is disabled by default: the partition is deleted after the installation, and if it lies next to Android's data partition, its space is added to the data partition.

On a dual-boot machine, both "Keep existing system" and "Reinstall Android" let you choose on the confirmation page which operating system starts by default (Android is preselected each time, so re-select Windows when reinstalling if you want Windows to remain the default). See [Dual boot](#dual-boot).

### If an installation fails

Use the installer's "Save logs" to save the logs to a USB drive (a FAT32 or exFAT partition that Windows can assign a drive letter to). They are saved in a folder named `gaokun3-logs-<time>`; please attach the whole folder to your report. A USB drive with the ISO written to it has no partition the logs can be saved to, so plug in a second USB drive. The Live installer does not write logs to the installation media by itself, and they are lost when the machine is shut down.

### Partition layout

After erasing the entire disk, the disk holds these partitions:

| Partition | Size | Purpose |
|---|---|---|
| `esp` | 300 MiB | Boot menu and the kernel of each slot |
| `misc` | 4 MiB | A/B slot state |
| `metadata` | 32 MiB | Android metadata |
| `boot_a`, `boot_b` | 64 MiB each | Kernel and boot image, A and B copies |
| `super` | 12 GiB | System partitions, A and B copies |
| `userdata` | All remaining space | User data |

When keeping the existing system, all partitions except `esp` are created in unallocated space, and the existing EFI system partition is shared with Windows.

## First boot

The first boot takes about two to three minutes. Then follow the setup wizard; no extra scripts need to be run.

### "This device isn't Play Protect certified"

This system is not on Google's list of certified devices, so the Play Store shows "This device isn't Play Protect certified", and some apps refuse to install until the device is registered. Registering is free and only needs to be done once per device:

1. Sign in to your Google account on the tablet.
2. Read the device's Android ID. First run `adb devices` on your computer to find the device serial number, then run the script from this repository:

   ```sh
   bash scripts/google/gsf-android-id.sh <adb-serial>
   ```

   (Since 1.0.0-rc.4 the device reports its real serial number, so running the script without arguments fails because it defaults to `gaokun3`.) The ID is stored in Google Play services' private directory, so reading it needs root. Install the ReSukiSU manager first and grant root to Shell; the script uses it automatically. For setting up adb, see the [FAQ](FAQ.md).
3. With the same Google account, open <https://www.google.com/android/uncertified/>, enter the ID and register it.
4. Wait a few minutes, then clear the Google Play Store's storage (or run `adb shell pm clear com.android.vending`).

If, after registering, the Play Store or the Google app still reports that the device is not certified when opened, the app is still using a certification result it cached earlier. Delete the cached result and open the app again; as in step 2, this needs root. Unlock the tablet first, then run on the computer:

```sh
adb shell "su -c 'rm -f /data/data/com.google.android.gms/files/gmscompliance/shared/cache.pb /data/data/com.android.vending/cache/gmscompliance.pb; am force-stop com.google.android.gms; am force-stop com.android.vending; monkey -p com.android.vending -c android.intent.category.LAUNCHER 1'"
```

The Play Store then opens on the tablet and obtains a new certification result within a few seconds. If it still reports that the device is not certified, or the message appears again later, run the command again. If the Google app shows the same message, replace every `com.android.vending` in the command with `com.google.android.googlequicksearchbox`.

After a factory reset, or after clearing Google Play services' data, the ID changes and has to be registered again.

Play Integrity still fails after registering. It is a stricter check used by some banking and payment apps, and it requires a locked bootloader and a system signed by Google, while this system can only run with Secure Boot disabled. Apps that depend on Play Integrity cannot be used on this system.

## System updates

Check for and install updates in Settings → System → System update; the kernel is updated together with the system. Updates are written to the other slot, so you can keep using the tablet meanwhile, and take effect after a restart. If the network is unreliable, you can also download the OTA package another way, put it in the tablet's internal storage, and use "Install local update" on the same screen.

If an updated system repeatedly fails to finish booting, the machine automatically goes back to the previous version and tells you once Android is up. If the system hangs without restarting by itself, hold the power button to turn it off, then start it again.

> **You cannot go back to the previous version after an update.** Once the new version has booted successfully, the previous system is merged away. The boot menu still lists the other slot, but choosing it only wastes a boot. A major update (for example Android 16 to 17) also upgrades your user data on its first boot, so even if an older version starts, it may not be able to use that data. To go back, the only way is to reinstall the older version with the Live installer, most likely wiping data. Please back up important data before updating.

## The boot menu

The boot menu appears at every start for 5 seconds by default; if you do not choose, the default system starts. You can select an entry with the volume up/down buttons and confirm with the power button, or use the arrow keys and Enter on the keyboard cover. The main entries are:

| Entry | Purpose |
|---|---|
| `Android` | Android, started by default |
| `Android fastboot / boot menu` | Fastboot mode; see [fastboot](#fastboot) |
| `gaokun3 installer` | Shown when the internal disk has an installer partition; starts the Live installer |
| `Windows Boot Manager` | Shown with dual boot; starts Windows |

The menu also lists Android entries for each slot and an entry for the previous boot loader. They are for troubleshooting and normally not needed.

### fastboot

Choose `Android fastboot / boot menu` in the boot menu, or run `adb reboot bootloader` on a computer, to enter fastboot mode. The screen shows `FASTBOOT MODE`, the current slot and the USB connection state.

- Plug the cable into the USB-C port **next to the power button**; the other port does not support fastboot.
- On a computer with the Android SDK Platform Tools, you can use `fastboot devices`, `fastboot getvar all`, `fastboot oem device-info` (state of the disk, partitions, A/B and boot entries), `fastboot reboot` and `fastboot reboot bootloader`.
- On the tablet, the volume keys move between options and the power button confirms.

Flashing (`fastboot flash`) has not been tested on a real machine yet, and neither has confirming with the power button. To rewrite the system, please use the Live installer.

### Dual boot

With dual boot, the system that starts by default is chosen during installation and can be changed later in Android under Settings → System → Startup options. If you choose Windows, the first two starts after installing still go to Android, so that you can finish the initial setup. "Startup options" in Android can also restart straight into Windows. These features have not been tested systematically.

The 100 MiB EFI partition created when Windows is reinstalled has insufficient space, and the installers do not support that layout.

To uninstall Android, use the Windows installer; see [Uninstalling](#uninstalling).

## Erasing your data (factory reset)

"Erase all data (factory reset)" in Settings has not been verified on real hardware yet; to erase your data, use "Reinstall Android" in the Live installer as described in the [FAQ](FAQ.md#erasing-your-data).

## If it does not boot

| Symptom | What to do |
|---|---|
| Black screen after power-on, no boot menu | Make sure Secure Boot is disabled |
| The machine starts Windows directly, without the boot menu | See the [FAQ](FAQ.md#the-machine-starts-windows-directly-without-the-boot-menu) |
| After an update, you chose the other slot's entry and the machine restarted | Expected: the old version has been merged away; see [System updates](#system-updates) |
| Android does not start | See the [FAQ](FAQ.md#the-machine-keeps-restarting-or-hangs-what-can-i-do) |

## Reporting problems

Please use the [issue forms](https://github.com/vahiru/gaokun-android/issues/new/choose), and read [issues.md](../issues.md) before submitting. Logs help more than descriptions: if Android starts, please attach the archive produced by `adb bugreport`. It contains personal information such as Wi-Fi network names, account names and installed apps, so please check it before posting it publicly. How to collect each kind of log is described in the [FAQ](FAQ.md).
