# Known limitations and unsupported features

[**中文 → known-limitations.zh-CN.md**](known-limitations.zh-CN.md) · [FAQ](FAQ.md) · [Installing](INSTALL.md)

This system runs on a machine that has no vendor Android support at all. Below is what you should know before installing: some items are deliberate trade-offs, some are features not finished yet, and some are limits of the machine itself.

Each entry follows the same format: **Symptom** (what you will run into), **Cause**, and **What to do** (for now). Problems specific to one release are in that release's [release notes](relnotes/).

## 1. Security and privacy (please read this part first)

### v0.7.1 and earlier opened network adb without authorization
* **Symptom**: nothing visible in use.
* **Cause**: v0.7.1 and earlier opened adb on TCP port 5555 on every network interface (including the Wi-Fi hotspot), without asking for authorization; once connected, root was available, and the image also carried the maintainer's adb public key. Anyone on the same Wi-Fi, or connected to your hotspot, could take full control of the machine. Since 1.0.0-rc.1, adb is off by default, every computer needs your authorization, port 5555 is no longer opened, and the maintainer's key is gone. Updating through System update is enough for this to take effect.
* **What to do**:
  * If you are still on v0.7.1 or earlier, back up first, then update in Settings → System → System update. Upgrading from Android 16 to 17 rewrites your user data and cannot be undone; see the [rc.3 release notes](relnotes/v1.0.0-rc.3.md). Until you have updated, do not join Wi-Fi networks you do not trust, and do not let strangers join your hotspot.
  * If you ever set `persist.adb.tcp.port` by hand, that setting survives the update; how to clear it has not been documented yet.
  * To use adb over the network, use [wireless debugging](FAQ.md#wireless-debugging).

### The system is signed with AOSP's public test keys
* **Symptom**: nothing visible. The `release-keys` shown in the build information is only a label.
* **Cause**: the system, system apps and OTA packages are signed with the test keys published in the AOSP source, whose private keys anyone can download. So anyone can build an OTA package this machine will accept, or an app that runs with system privileges. The machine's security therefore depends on the download channel being trustworthy, and on you not installing "system components" from unknown sources.
* **What to do**:
  * Update only through System update in Settings, or from this project's GitHub release pages.
  * Do not install APKs or zip files that someone sends you as a "system patch" or "system component".
  * On a fresh install, the installer checks `install-artifacts.sha256` before writing to the disk; please do not skip this.

### User data is not encrypted
* **Symptom**: the screen lock only stops others from using the machine directly; it does not stop them from reading its data.
* **Cause**: the user data partition is an unencrypted ext4 file system. Secure Boot must be off, the boot menu offers the installer and the rescue system, and logging in as root in front of the machine needs no password; plugging in a Linux USB stick reads the disk just as well. Whoever has the machine can read photos, chat histories, browser logins, Wi-Fi passwords and everything else.
* **What to do**:
  * Treat this machine as one whose data anyone holding it can read, and do not keep sensitive material on it.
  * Before selling it, sending it for repair or lending it out, erase your data with "Reinstall Android" in the graphical installer; see the [FAQ](FAQ.md#erasing-your-data).
  * If file-based encryption is added later, getting it will require wiping data and reinstalling; it cannot arrive through a system update.

### Root is built into the kernel (KernelSU / ReSukiSU)
* **Symptom**: the kernel has KernelSU (the ReSukiSU fork) built in. Some banking and payment apps and games with anti-cheat may detect it, warn that the device is at risk, or refuse to run.
* **Cause**: developing and troubleshooting need root, so release builds keep it. Which apps get root is decided by the ReSukiSU manager: only apps you have approved there get it; without the manager installed, no app can get it.
* **What to do**:
  * If you do not need root, do not grant it to any app in the manager.
  * Once adb root is turned on in the manager, any computer you have authorized for USB or wireless debugging gets root directly through `adb shell`. Leave adb root off unless you need it, only authorize computers you trust, and turn USB and wireless debugging off when you are not using them.
  * There is no build without root yet, and the kernel's root support cannot be switched off.
  * In smoke tests, *Delta Force* and *Strinova*, which use ACE anti-cheat, run normally; banking and payment apps have not been tested systematically yet, and feedback is welcome.

### SELinux is enforcing; hand-partitioned disks may need their EFI partition renamed
* **Symptom**: nothing visible on most machines. But if you partitioned the disk yourself and the EFI partition's GPT partition name is neither `esp` nor `EFI system partition`, system updates and slot switching fail.
* **Cause**: since 1.0.0-rc.1, SELinux has changed from permissive (log only) to enforcing (actually block). SELinux is the second layer of Android's app sandbox: even an app that finds a vulnerability can do far less. The policy only lets the update and boot control components open the EFI partition by these two names: `esp` is the name the installer gives it when erasing the whole disk, and `EFI system partition` is the name Windows gives it, which is what dual boot uses. Earlier versions scanned every storage device when the partition was not found; enforcing mode does not allow that.
* **What to do**:
  * If your EFI partition has a different name, rename it once from any Linux (the installer's terminal or the rescue system will do): `sgdisk -c <N>:esp /dev/nvme0n1`, where N is the EFI partition's number. UEFI only looks at the partition type, so renaming has no side effects. The installer also warns you when it sees such a name.
  * If something worked on v0.7.1 and no longer does, please report it and attach the output of `adb shell dmesg | grep avc`.

### The boot chain is unlocked, and system partitions are not verified
* **Symptom**: nothing checks at boot whether the system has been modified, and Play Integrity cannot pass (see "Not Google-certified" below).
* **Cause**: this machine boots through UEFI and systemd-boot, Secure Boot must be off, the kernel is not signed, and dm-verity is not enabled. This open boot chain is exactly what makes it possible to install other systems on the machine.
* **What to do**: there is no way around it; it is the precondition for running Android on this machine.

### Apps are told a kernel version that is not the real one
* **Symptom**: apps read the kernel version `6.6.30-android15-8-g0a4d3c12b3`, which matches no real Android kernel; the kernel actually running is mainline Linux 7.2.9 with this project's patches. On 1.0.0-rc.2 and rc.3 the whole system reports this version; from the next release, only apps see it, while `uname -r` in `adb shell` and the kernel version shown in Settings are the real one.
* **Cause**: some apps' environment checks only accept the version numbers of Android's common kernels. Only the number apps read is changed; the kernel itself, its build-time version and module checks are untouched. This is a deliberate disguise, which is why it is listed here.
* **What to do**: from the next release, you can turn it off or change it in Settings → System → Kernel version spoofing; it takes effect after a restart. 1.0.0-rc.2 and rc.3 do not have this setting; to see the real version, run `dmesg | head -1` as root. Please do not use "Install" or LKM patching in the KernelSU manager: KernelSU is already built into this kernel.

## 2. Unsupported or incomplete features

### "Erase all data" in Settings has not been tested
* **Symptom**: "Erase all data (factory reset)" in Settings is carried out by fastboot mode after a restart, but this has not been tested on a real machine, so there is no guarantee that your data is fully erased.
* **Cause**: this feature normally relies on Android recovery, which cannot start on this machine, so fastboot mode in the boot menu carries it out instead.
* **What to do**: erase your data with "Reinstall Android" in the graphical installer, which wipes user data by default; see the [FAQ](FAQ.md#erasing-your-data).

### File transfer over USB: no prompt, and USB 2.0 speed only
* **Symptom**: when connected to a computer, the tablet does not show the "USB usage" notification. To transfer files, choose "File transfer" or "PTP" in Settings → Connected devices → USB; a portable device called "MateBook E Go" then appears on the computer. The same page offers "USB tethering" and "MIDI". Transfers only reach USB 2.0 speed, about 30 MB/s.
* **Cause**: the keyboard cover's connector appears to the system as a permanently attached USB keyboard, and Android hides the USB connection notification when it sees such a device. The USB-C port next to the power button only runs at USB 2.0 speed when connected to a computer.
* **What to do**: choose "File transfer" in Settings as above. The tablet does not enter standby while connected to a computer, so transfers continue with the screen off; please unplug the cable when you are done.

### Fingerprint is not available yet
* **Symptom**: Settings has no fingerprint option, and the sensor in the power button cannot be used.
* **Cause**: the fingerprint driver and system service are still in development.
* **What to do**: unlock with a PIN or password. Even once fingerprints are supported, fingerprint payment in payment apps will most likely not work.

### The stylus does not support the first-generation M-Pencil
* **Symptom**: on 1.0.0-rc.1 and earlier, the stylus does nothing at all. Since rc.2, the second- and third-generation M-Pencil support pressure, hover, tilt and double-tap on the side; everyday use of the third-generation pen has not been tested yet. The first-generation M-Pencil does not work.
* **Cause**: the stylus signal comes from the touchscreen's second controller chip, whose driver was reverse-engineered by a community contributor; the first-generation pen uses a different protocol that the driver does not support.
* **What to do**: if a second- or third-generation pen does not work properly on rc.2 or later, please open an issue and attach the output of `adb shell getevent -pl`.

### Location mostly does not work
* **Symptom**: maps, weather, delivery and ride-hailing apps cannot get a location, or stay on "locating".
* **Cause**: this machine has no GPS. Network location is provided by Google Play services and is unavailable in mainland China, where Google cannot be reached.
* **What to do**:
  * Choose your city manually in weather and similar apps.
  * Chinese apps with their own location services (for example map apps using Wi-Fi positioning) may work; this has not been tested yet.
  * Use a phone for navigation.

### DRM-protected video cannot be played
* **Symptom**: Netflix, Disney+, Prime Video and similar services cannot play their content and report a DRM error or that the device is not supported. Some members-only or licensed content in Chinese video apps may be affected too; this has not been tested yet.
* **Cause**: the system has no DRM module. Widevine is a closed-source Google component that requires licensing and certification, which this project cannot ship.
* **What to do**: please watch such content on another device.

### The Bluetooth headset microphone does not work in calls
* **Symptom**: in WeChat voice calls, Tencent Meeting or in-game voice chat over a Bluetooth headset, the headset microphone does not work, and the call audio may not play through the headset either. The microphone on wired headsets has not been tested yet.
* **Cause**: Bluetooth calls need a dedicated audio path; this machine lacks the one Qualcomm provides for phones, and no replacement has been implemented yet.
* **What to do**: use the tablet's own microphone and speakers for calls. Listening to music over a Bluetooth headset uses a different path and is not affected.

### Messages from some apps are delayed in standby
* **Symptom**: once the screen is off and the machine is in standby, new messages from apps that do not use Google's push service (FCM), such as WeChat and QQ, only arrive at the system's next scheduled wake-up or when you turn the screen on, usually one or two minutes late, occasionally around three. Apps that use FCM are delayed the same way on 1.0.0-rc.3 and earlier; from the next release they can wake the system and arrive on time. Wi-Fi stays connected during standby, so there is no need to reconnect after waking.
* **Cause**: Chinese apps have no vendor push channel on this system, so they can only fetch messages from their servers when the system is awake.
* **What to do**: when you need messages on time, keep the screen on, or receive them on a phone.

### Not Google-certified
* **Symptom**: the Play Store says "This device isn't Play Protect certified", and some apps cannot be installed. Apps that depend on Play Integrity (for example Google Wallet and some banking apps outside China) cannot be used.
* **Cause**: this system is not on Google's list of certified devices. Play Integrity also requires a locked boot chain and a system signed by Google, which this machine cannot provide.
* **What to do**: register the device once as described in the [install guide](INSTALL.md#this-device-isnt-play-protect-certified), and the Play Store works normally. There is no way around Play Integrity.

### Google apps are built in, and there is no Chinese app store
* **Symptom**: where Google cannot be reached, the Play Store and Google services do not work, and Google services keep trying to connect in the background; the power cost of this has not been measured. The system has no Chinese app store.
* **Cause**: this project only releases builds with Google apps.
* **What to do**: download APKs with a browser from the apps' official websites or the web versions of Chinese app stores.

### Hardware this machine does not have
* **Mobile network and SIM card**: this machine has no modem.
* **Compass**: there is no magnetometer. Auto-rotation uses the accelerometer and gyroscope and is not affected.

## 3. Unresolved problems

The following are bugs, not trade-offs; they will be removed from this list once fixed.

### After a long uptime, sound and Bluetooth may stop working together
* **Symptom**: after the machine has been running for a long time, sound and Bluetooth stop working together, and only a restart brings them back. This was reported by users and has not been reproduced yet.
* **Cause**: not known yet. Audio and Bluetooth share a path to the DSP, and that path is suspected of getting stuck.
* **What to do**: a restart brings them back. A watchdog in the system saves records to `/data/vendor/gaokun3/hangdump-*` when it detects the hang, and they survive the restart. Please attach them when reporting (root is needed; see the [FAQ](FAQ.md#when-android-starts) for the commands); if the directory does not exist, please say so in the report too.

### Plugged into a computer, the tablet may charge the computer instead
* **Symptom**: after connecting the tablet (the USB-C port next to the power button) to a computer, neither the computer nor USB debugging sees the tablet, and the tablet's battery goes down instead.
* **Cause**: the USB-C ports of the tablet and of many computers can both supply and receive power. When the computer does not support USB PD, which side supplies power is decided at the moment of plugging in, and occasionally it ends up the wrong way round. The tablet then acts as the host, while the computer will not act as a USB device, so neither sees the other.
* **What to do**: unplug the cable and plug it in again (once is usually enough), or use another port on the computer. Only the USB-C port next to the power button can be used for USB debugging; the other port only connects peripherals.

### Occasionally the USB-C port does not work after power-on until a restart
* **Symptom**: occasionally right after power-on, the USB-C port does not work: USB debugging cannot connect and USB devices are not recognized, until a restart.
* **Cause**: the Type-C controller (UCSI) occasionally fails to initialize at boot; the cause is still being investigated.
* **What to do**: a restart brings it back. Until then, use adb and transfer files over Wi-Fi.

### Video processed with the screen off may come out incomplete
* **Symptom**: if recording, exporting a video or compressing one before sending keeps going in the background after the screen turns off, the result may be shorter than expected or miss frames. Once this has happened, recording may keep failing until a restart.
* **Cause**: with the screen off and the system still awake, the processor enters a deeper power-saving state, and the video encoding hardware gets stuck during it; why has not been found yet. Use with the screen on is not affected.
* **What to do**: keep the screen on while recording, exporting or compressing video. If recording keeps failing, restart the tablet.

### With two recordings at once, the second one is slower or the first one stops
* **Symptom**: starting a second recording while one is already running may give the second one choppy video. If the system starts the second one (for example screen recording from Quick Settings while recording video), the recording in progress may stop; this has not been tested yet.
* **Cause**: the video encoding hardware handles one stream at a time. The second recording usually falls back to slower software encoding, but when its requester has a higher priority, the system first takes the hardware encoder away from the app that is recording.
* **What to do**: record one thing at a time where possible.

### *League of Legends: Wild Rift* starts; whether matches work has not been tested
* **Symptom**: on 1.0.0-rc.1 and earlier, the game quits as soon as it opens. Since rc.2 it starts and reaches the user agreement screen; whether you can get into a match has not been tested yet.
* **Cause**: the game engine assumes that a 64-bit process's memory addresses stay below 2^39, while this kernel gave processes a 2^48 address space by default. The kernel now hands out addresses below 2^39 by default.
* **What to do**: if the game still quits or gets stuck elsewhere on rc.2 or later, collect the crash log right after it quits (see the [FAQ](FAQ.md#when-android-starts)) and attach it to [#15](https://github.com/vahiru/gaokun-android/issues/15).

### The Chinese input method has to be turned on once
* **Symptom**: the default keyboard does not support Chinese.
* **Cause**: since 1.0.0-rc.1 the Chinese input method fcitx5 (with Pinyin, Shuangpin and Wubi) is preinstalled, but it is not set as the default.
* **What to do**: turn on Fcitx5 in Settings → System → Keyboard → On-screen keyboard, then switch to it. Update it later from fcitx5-android's GitHub release page; the F-Droid build has a different signature and cannot be installed over it. How to switch between Chinese and English on the physical keyboard has not been documented yet.
