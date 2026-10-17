<!--
  Maintenance (not rendered):
  * This list is "what you should know before installing this ROM": long-standing limitations, unsupported features,
    and the security and licensing trade-offs. Bugs specific to one release go into that release's Known issues, not here.
  * The comment after each entry names its id in docs/v1.0-plan.md. Check every entry before a release; delete an entry
    once it is fixed, rewrite it when the trade-off changes (as the SELinux and Chinese-input entries were rewritten for
    1.0.0-rc.1, when SELinux went enforcing and the IME was really preinstalled).
  * Keep this file and known-limitations.zh-CN.md in sync. Evidence (file:line / on-device checks) is in the
    comments of the Chinese file; it is the same for both.
-->
# Known limitations and unsupported features

[**中文 → known-limitations.zh-CN.md**](known-limitations.zh-CN.md) · [FAQ](FAQ.md) · [Installing](INSTALL.md)

This ROM was built from scratch for a machine that has **no vendor Android support at all**. Below is what you should
know before installing it. Some of these are deliberate trade-offs, some are not done yet, and some cannot be done on
this hardware.

Every entry has the same three parts: **what you see**, **why** (in one sentence or two), and **what to do instead**.

Problems specific to one release are in that release's [release notes](relnotes/).

---

## 1. Security and privacy (read this part first)

### Up to v0.7.1, network adb lets anyone in without asking
<!-- B1 / D1 (SEC-1, OTA-1 and others in docs/v1.0-plan.md): ⬜ not built, not tested on hardware. Check before the
     release. Evidence in the Chinese file. -->
* **What you see**: on v0.7.1 and earlier, nothing. That is the problem.
* **Why**: v0.7.1 and earlier images open adb on TCP port 5555 on every network interface, the Wi-Fi hotspot included,
  and accept connections **without** the "Allow debugging?" prompt. The images are debuggable (`ro.debuggable=1`), so
  `adb root` then gives a root shell. They also carry the maintainer's adb public key. Anyone on the same Wi-Fi, or
  connected to your hotspot, can take over the machine.
  From the next release (planned as 1.0) adb is **off** by default, asks you to authorize each computer, does not
  listen on port 5555, cannot `adb root`, and the image carries no maintainer key. Existing users get this with the
  OTA update, nothing to do: the old port-5555 setting came from the image's built-in defaults, and Android never
  saves those to `/data`. The exception is anyone who set `persist.adb.tcp.port` themselves.
* **What to do**:
  * Update to the next release as soon as it is out. Until then, stay off Wi-Fi networks you don't trust, and don't
    let strangers join your hotspot.
  * If you set `persist.adb.tcp.port` yourself: how to clear it after updating will be added here (⬜ not yet verified
    on hardware).
  * For adb over the network, use **Wireless debugging** ([FAQ](FAQ.md#wireless-debugging)).

### The system is signed with Android's public test keys
<!-- B2 / SEC-2 (user decision D2, 2026-10-04: keep test-keys, disclose). -->
* **What you see**: nothing. The build information says `release-keys`, but that is only a label.
* **Why**: the system, the system apps and the OTA update packages are signed with the test keys that ship **publicly**
  in the AOSP source; anyone can download the private keys. So **anyone** can build an OTA package, or an app that runs
  with system privileges, that this ROM will accept as genuine. The safety of this machine therefore rests on the
  download channel not being taken over, and on you not installing "system components" from unknown sources.
* **What to do**:
  * Update only through the built-in system updater in Settings, or from this project's GitHub Releases page.
  * Do not install APKs or zips that someone sends you as a "system patch" or "system component".
  * On a fresh install the installer checks `install-artifacts.sha256` before writing to the disk. Don't skip it.

### `/data` (all your personal data) is not encrypted
<!-- B3 / SEC-3 (user decision D3: no encryption in 1.0, disclose honestly). -->
* **What you see**: the lock screen protects the machine while it is running, and nothing more. It does not protect
  your data from someone who has the machine in their hands.
* **Why**: the data partition is plain ext4. Secure Boot has to be off; the boot menu offers the installer and a rescue
  system, both of which give a root login at the local console without a password; and any Linux USB stick works too.
  Whoever holds the machine can read your photos, chats, browser logins, Wi-Fi passwords and everything else.
* **What to do**:
  * Treat this machine as one whose data anyone holding it can read. Don't keep sensitive material on it.
  * Before you sell it, send it for repair or lend it out, wipe it with the graphical installer's
    **Reinstall Android** (wipes data by default), see the [FAQ](FAQ.md#factory-reset--wiping-before-you-sell).
  * If encryption arrives later, getting it will mean wiping and reinstalling. An OTA update cannot encrypt an
    existing installation.

### Root is built in (KernelSU / ReSukiSU)
<!-- SEC-12 / REL-10 (user decision D6: keep root, disclose fully; no root-less variant before 1.0).
     ⚠️ v0.7.1 and earlier do NOT preinstall the manager APK (TODO B11). If 1.0 preinstalls it, say so in the first
     sentence; the "without the manager" sentence stays true either way. -->
* **What you see**: the kernel contains a root implementation, KernelSU (the ReSukiSU fork). Some banking and payment
  apps, and games with anti-cheat, may detect it and warn about a "risky device" or refuse to run.
* **Why**: root is essential for development and troubleshooting, and we decided to keep it in release builds.
  The **ReSukiSU manager** app decides who gets root: only apps you approve in the manager do. Without the manager
  installed, no app can get root.
* **What to do**:
  * If you don't need root, don't grant it to any app in the manager.
  * **adb is root too**: any computer you have authorized for USB or wireless debugging gets a root shell
    (`adb shell` runs as root through KernelSU, even though `ro.debuggable=0`). Only authorize computers you trust,
    and turn USB / wireless debugging off when you don't need them.
    <!-- 2026-10-05 measured on the 1.0.0-dev.1/dev.2 release builds: adbd in u:r:ksu:s0, `adb shell id` = uid 0. -->
  * There is no build without root yet, and the kernel's root capability cannot be switched off.
  * In our smoke tests, *Delta Force* and *Strinova* (both protected by ACE anti-cheat) ran normally. Banking and payment
    apps have not been tested systematically. Reports are welcome.

### SELinux is enforcing from 1.0.0-rc.1 — but hand-partitioned disks may need their EFI partition renamed
<!-- SEC-4 / D5 (user decision 2026-10-06: switch). Enforcing by default from 1.0.0-dev.10; the first published build with it
     is 1.0.0-rc.1. Evidence in the Chinese file. Rewrite this entry if the policy is ever relaxed again, and drop the
     OTA-5 half once the components can find a differently named ESP under enforcing. -->
* **What you see**: on most machines, nothing. v0.7.1 and earlier ran SELinux in `permissive` mode (it only logged what its
  rules did not allow); from 1.0.0-rc.1 it is `enforcing` and blocks those things. **On a disk you partitioned by hand**,
  system updates and switching slots fail if the EFI partition's GPT partition *name* is neither `esp` nor
  `EFI system partition`.
* **Why**: Android's app sandbox has two layers, ordinary user permissions and SELinux; with SELinux enforcing, an app
  that finds a vulnerability can do much less. The rules let the update and boot-control components open the EFI
  partition only under those two names: `esp` is what the installer's *Erase the whole disk* creates, and
  `EFI system partition` is what Windows creates (dual boot uses that one). Scanning every block device for it, as
  earlier releases did as a fallback, is not allowed under enforcing.
* **What to do**:
  * If your EFI partition has another name, rename it once from any Linux (the installer's terminal or the rescue system):
    `sgdisk -c <N>:esp /dev/nvme0n1`, where N is the EFI partition's number. UEFI only looks at the partition type,
    so renaming is harmless. See [Updating](INSTALL.md#updating). The installer warns when it sees such a name.
  * The rules are new. If something worked on v0.7.1 and is broken now, report it with the output of
    `adb shell dmesg | grep avc`.

### The boot chain is unlocked, and system partitions are not verified
* **What you see**: nothing at boot checks whether the system has been modified. Play Integrity never passes (see
  *Not Google-certified* below).
* **Why**: the machine boots through UEFI and systemd-boot with Secure Boot off; the kernel is unsigned, and dm-verity is
  off. That open boot chain is exactly what makes installing another OS on this machine possible.
* **What to do**: nothing; this is the price of running Android on this machine at all.

---

## 2. Proprietary components shipped in the images

<!-- SEC-10 / REL-15 (D20: disclose, and prepare a build switch without Histen). NOTICE now covers the binary
     releases too (its section "Third-party proprietary components in the binary releases", 2026-10-05). -->
The source code of this project is released under GPL and other open-source licenses (see [NOTICE](../NOTICE)). The
**system images, OTA packages and installer images** we publish also contain the components below, which are **not
part of this project and not open source**. Without them there is no GPU, Wi-Fi, Bluetooth, sound or sensors:

| Component | Where on the system | Comes from | Notes |
|---|---|---|---|
| Huawei firmware: GPU zap shader, ADSP / CDSP / SLPI firmware, audio topology, pd_mapper service tables | `/vendor/firmware/qcom/sc8280xp/HUAWEI/gaokun3/` | Huawei's Windows driver packages | Not licensed by Huawei for redistribution. The installer image carries the GPU one as well |
| Sensor DSP configuration (SLPI JSON files and registry) | `/vendor/etc/hexagonrpcd-root/` | Same (Qualcomm reference configuration) | Same |
| Histen audio engine `libhw_histen_processing.so` | `/vendor/lib64/soundfx/` | Huawei Windows driver | Proprietary to Huawei, not licensed for redistribution, and binary-patched. It processes audio only when *Speaker enhancement (experimental)* is turned on |
| Google apps and services (MindTheGapps: Play Store, Play services, …) | `/system_ext`, `/product` | Google | Closed source, under Google's terms |
| GPU microcode, Wi-Fi and Bluetooth firmware | `/vendor/firmware/` | linux-firmware | Under Qualcomm's redistributable license; not affected by the above |

So if a rights holder asks for it, the downloads could be taken down. If you mirror or redistribute the images
yourself, you should know that they contain these components.

---

## 3. Unsupported or incomplete features

### *Erase all data* (factory reset) in Settings does nothing
<!-- ⬜ 2026-10-05：1.0 构建起镜像默认 persist.vendor.gaokun3.gk3boot=action、条目带 gk3.dispatch=1（device.mk / Gk3Boot.cpp），Settings 写的 --wipe_data 会由统一启动入口分派给 fastboot 执行端去擦（QEMU exec-wipe 过、E7 分派真机过）。但 E10 真机恢复出厂还没做（要用户同意 + 备份）⇒ 这一节先不改；E10 过了再改成"1.0 起可用"，并写明首次开机后才生效（0.7.x 升上来的机器第一次开机走直连）。 -->
<!-- B6 / A5 (user decision D4: to be handled by a future fastboot; design in progress). Rewrite once fastboot ships. -->
* **What you see**: the machine reboots and **all your data is still there**, with no message.
* **Why**: that feature relies on recovery to carry it out, and recovery cannot boot on this machine, so after the reboot
  nothing acts on the request. From 1.0 the request is handed to the boot entry's fastboot environment instead
  (see [INSTALL](INSTALL.md#the-boot-entry-and-fastboot-from-10)), but that path has not been verified on the machine —
  treat it as not working.
* **What to do**: use the graphical installer's **Reinstall Android**, which wipes data by default. Steps in the
  [FAQ](FAQ.md#factory-reset--wiping-before-you-sell).

### File transfer over USB: no notification, choose it in Settings; USB 2.0 speed only
<!-- PWR-6 / BKUP-7 / STOR-7. See the comment in known-limitations.zh-CN.md. -->
* **What you see**: when you connect the tablet to a computer, the tablet shows no "USB preferences" notification. To
  transfer files, go to **Settings > Connected devices > USB** and choose "File transfer" or "PTP"; "MateBook E Go" then
  appears on the computer as a portable device. Transfers run at USB 2.0 speed (about 30 MB/s). The same page also
  offers "USB tethering" and "MIDI".
* **Why**: the keyboard connector shows up as a USB keyboard that is always plugged in, and Android hides the USB
  connection notification while such a device is present. The USB-C port next to the power button only runs at USB 2.0
  in device mode.
* **What to do**:
  * Choose "File transfer" in Settings as above.
  * While it is plugged into a computer the tablet does not go to standby (deliberately: standby in that state resets
    the board), so transfers keep going with the screen off. Unplug when you are done.

### USB sticks are not recognised
<!-- STOR-1 / BKUP-6: the voldmanaged lines are in fstab (8da5974), ⬜ not built, not tested on hardware.
     Once tested, rewrite this entry as "port1 works; port0 only once usbrole has switched it to host", and say which
     physical port is port1 (STOR-3: user confirmed 2026-10-05 that port0 is the one next to the power button, so port1 is the other). Body text unchanged until then. -->
* **What you see**: a USB stick or external drive does not appear in the file manager.
* **Why**: the kernel does see it, but the system's partition table doesn't declare any removable storage, so Android
  never mounts it.
* **What to do**: nothing yet. Use the network to copy files.

### Fingerprint does not work (in progress)
<!-- HW-5 / T6 (D12: not a 1.0 gate). -->
* **What you see**: the fingerprint reader in the power button does not exist as far as Android is concerned; Settings
  has no fingerprint option.
* **Why**: matching runs inside Huawei-signed secure firmware, and its command protocol has to be reverse-engineered from
  the Windows driver. Huawei's fingerprint program can already be loaded into the secure environment; the kernel driver
  and the Android fingerprint service are still missing.
* **What to do**: unlock with a PIN or password. Even once fingerprint works, fingerprint payments in payment apps will
  most likely still be unavailable.

### Stylus (Huawei M-Pencil) is not supported
<!-- DISP-13. Postponed past 1.0 (needs reverse-engineering of raw frames). -->
* **What you see**: the pen does nothing at all: no pressure, no hover.
* **Why**: the touch points are computed by the kernel driver itself from raw capacitance data, and that algorithm only
  knows fingers. Nobody has reverse-engineered the pen's signal format yet.
* **What to do**: nothing.

### No automatic brightness
<!-- DISP-8 / HW-4 / A3 (#121). -->
* **What you see**: there is no *Adaptive brightness* in Settings; brightness is manual only.
* **Why**: the light sensor answers on its bus, but turning it on crashes the DSP that runs the sensors, so it stays off.
* **What to do**: adjust brightness by hand.

### Location mostly does not work (no GPS)
<!-- APP-13 / NET-5. Whether domestic apps' own Wi-Fi location SDKs work: not tested yet (batch 4). -->
* **What you see**: maps, weather, delivery and ride-hailing apps cannot get a position, or keep "locating".
* **Why**: the system has no GPS. Network location is provided by Google Play services, which cannot reach Google from
  mainland China.
* **What to do**:
  * Choose your city manually in weather apps and similar.
  * Apps with their own location SDK (for example map apps that locate by Wi-Fi) may work; we have not tested them.
  * Use a phone for navigation.

### No DRM-protected video (no Widevine)
<!-- AV-9 / APP-6 (D11: don't ship it, disclose). Impact on Chinese video apps' VIP content: not tested. -->
* **What you see**: Netflix, Disney+, Prime Video and the like will not play their shows; they report a DRM or "device
  not supported" error. Some paid or licensed content in Chinese video apps may be affected too (not tested yet).
* **Why**: the system has no DRM module at all. Widevine is a closed Google component that needs licensing and
  certification, which we cannot provide.
* **What to do**: watch such content on another device.

### Bluetooth headset microphone does not work in calls
<!-- AV-2 (1.0: disclose only). AV-3: A2DP playback never fully verified on hardware. AV-10: the wired headset mic is
     broken too (planned for batch 2; drop that sentence once fixed). -->
* **What you see**: in WeChat voice calls, Tencent Meeting, in-game voice chat and the like, the microphone on a
  Bluetooth headset does not work, and the call audio may not go to the headset either. The microphone on a wired headset
  does not work at the moment either.
* **Why**: Bluetooth calls need a dedicated audio path. Phones get it from Qualcomm's Android software, which this
  machine doesn't have, and the system has no replacement for it yet.
* **What to do**: use the tablet's built-in microphone and speakers for calls. Listening to music over Bluetooth (A2DP)
  takes a different path and is not affected, though it has not been fully tested on hardware yet.

### No push notifications while in standby
<!-- APP-5 / NET-9 / PWR-12 (1.0: measure, then disclose; WoW after 1.0). Fill in measured delays after batch 4.
     The standby switch is the Parts one from 1.0 on (PWR-16); its place and wording on the Battery page still have to be checked on hardware. -->
* **What you see**: once the screen is off and the machine is in standby, new WeChat, QQ and other messages do not
  arrive in real time. They arrive together when you turn the screen on (or when the system wakes up on a timer).
  After each wake, Wi-Fi takes a few seconds to reconnect.
* **Why**: in standby the Wi-Fi chip is powered off completely, so nothing arriving over the network can wake the
  machine. Chinese apps also have no vendor push channel available on this system.
* **What to do**:
  * When you need messages promptly, keep the screen on, or receive them on your phone.
  * Or turn standby off completely, which costs noticeably more battery with the screen off: **Settings → Battery →
    Standby (sleep)**, turn off "Allow standby". Turning it off applies right away and survives restarts; turning it
    back on restores standby from the next time the screen turns off.

### Not Google-certified
<!-- T2 / INST-14 / APP-4. -->
* **What you see**: the Play Store says "This device isn't Play Protect certified" and some apps will not install. Apps
  that depend on Play Integrity (Google Wallet, some banks outside China) do not work.
* **Why**: this ROM is not on Google's list of certified devices. Play Integrity also requires a locked boot chain and a
  Google-signed system, which this machine can never meet.
* **What to do**: register the device once, as described in
  [INSTALL.md, "This device isn't Play Protect certified"](INSTALL.md#this-device-isnt-play-protect-certified), and the
  Play Store works normally. There is nothing to be done about Play Integrity.

### Google apps are built in, unusable in mainland China, and there is no Chinese app store
<!-- APP-12 (user decision D7: ship only the GApps build, no vanilla build). Battery cost of GMS retrying: not measured. -->
* **What you see**: without a proxy, neither the Play Store nor Google services can connect, and Google services keep
  retrying in the background (battery impact not measured). There is no Chinese app store on the system.
* **Why**: we only ship the build with Google apps.
* **What to do**: download APKs with the browser, from each app's official website or from the web version of a Chinese
  app store.

### Other hardware that is not there
* **No cellular network, no SIM**: the machine has no modem.
* **No compass**: there is no magnetometer. Auto-rotate works; it uses the accelerometer and gyroscope.

---

## 4. Other common problems (planned to improve)

These are bugs, not trade-offs, and they will be removed from this list once fixed.

### Audio and Bluetooth can deadlock after a long uptime
<!-- A1 / AV-6 (#38: reported by a user, never reproduced by us). -->
* **What you see**: after the machine has been running for a long time, sound and Bluetooth stop working, and only a
  reboot brings them back. Reported by users; we have never reproduced it.
* **Why**: not known yet. Audio and Bluetooth share one path to the DSPs, and the suspicion is that this path gets
  stuck.
* **What to do**: reboot. A watchdog collects evidence automatically when it detects the hang, in
  `/data/vendor/gaokun3/hangdump-*`, and keeps it across reboots. Please attach it to your report (it needs root; the
  command is in the [FAQ](FAQ.md#with-a-computer-when-the-machine-boots)). If there is no such folder, say so: that is
  a clue too.

### Plugged into a computer, the tablet may charge the computer instead
<!-- USB-1 (2026-10-05 on the dev machine with a Mac): port0 settled as power source, partner without USB PD,
     kernel picked host ⇒ nothing enumerates; usbfollow only corrects the case where we are the sink. -->
* **What you see**: you plug the tablet into a computer (port next to the power button) and USB debugging / the
  computer does not see it. The tablet's battery goes *down* instead of charging.
* **Why**: both the tablet's port and many computers' USB-C ports can be either the power source or the power sink.
  Without USB Power Delivery on the computer's side, which side becomes which is decided at plug-in and can come out
  the wrong way round. Then the tablet acts as the host, the computer does not act as a USB device, and neither sees
  the other.
* **What to do**: unplug and plug the cable in again (once is usually enough), or use another port on the computer.
  A fix that asks to renegotiate automatically is planned.
* Only the USB-C port **next to the power button** can do USB debugging at all; the other port is host-only.

### The USB-C port can stop working after replugging, until a reboot
<!-- A6 / PWR-4 / HW-14. The "no standby" half is inferred from the code (role switch keeps failing and holds the
     machine awake), not observed; README.md:64 says the same. -->
* **What you see**: after you unplug and plug something back into the USB-C port (seen mostly after standby), the port
  stops working: no USB adb, USB devices are not detected. It stays like that until you reboot. Going by the code, the
  machine also no longer goes into standby until that reboot, so the battery drains with the screen off.
* **Why**: the root cause in the USB controller is not known yet. While the port is broken, the service that switches
  the port's role keeps failing and keeps the machine awake.
* **What to do**: reboot. Until then, use Wi-Fi for adb or file transfer.

### A palm on the screen turns into several touches
<!-- DISP-7 / T1 (after 1.0: needs shape tracking across frames in the driver). -->
* **What you see**: resting your palm or the side of your hand on the screen produces several separate touches, which
  can tap, zoom or scroll by accident.
* **Why**: the touch points are computed by the kernel driver itself from raw capacitance data, and it cannot tell a
  palm from fingers yet, so a large contact area is split into several touches.
* **What to do**: keep your palm off the screen while you use it.

### *League of Legends: Wild Rift* closes right after launch
<!-- #15 (docs/TODO.md:150（10-06 之前的行号，那段现在在 archive/TODO-history-2026-10.md，按节内标注的原行号找）). No log yet. -->
* **What you see**: the game closes right after you open it.
* **Why**: not known. It was reported in
  [#15](https://github.com/vahiru/gaokun-android/issues/15), and we have no log yet.
* **What to do**: if it happens to you, collect the crash log right after it closes (see the
  [FAQ](FAQ.md#with-a-computer-when-the-machine-boots)) and attach it to that issue.

### On a low-power charger the battery says *charging* while it drains
<!-- BATT-1 (BATT-2 merged into it). -->
* **What you see**: on a weak power source (a computer's USB port, a small phone charger) the battery shows
  *charging*, but the percentage keeps going down. Android then never does its low-battery shutdown, and at 0% the
  machine switches off hard; unsaved work is lost.
* **Why**: the battery status comes from Huawei's embedded controller. Either the driver decodes some of its status
  values wrongly, or the controller itself reports *charging*; which one is not settled yet.
* **What to do**: charge with a charger that can actually charge the machine, such as the one that came with it. On a
  weak source, watch the percentage rather than the charging icon, and save your work before it runs low.

### Turning on the Wi-Fi hotspot disconnects the tablet from Wi-Fi
<!-- NET-2 (batch 2). The v0.7.1 notes blamed the chip; iw shows the driver supports STA+AP, the software config does not.
     2026-10-05 confirmed in source: ath11k advertises STA and AP in one interface combination for WCN6855 hw2.1
     (ath11k mac.c:10327-10356, core.c:525-529/:572 @7.2.9). Fix written (BoardConfig WIFI_HAL_INTERFACE_COMBINATIONS +
     wlan1 pre-created by gaokun3-wlan-ap.sh), not built or tested yet. Delete this entry once a release passes the
     "hotspot + Wi-Fi at the same time" check. -->
* **What you see**: when you turn on the hotspot, the tablet drops its own Wi-Fi connection. With no modem, the hotspot
  then has no connection to share.
* **Why**: the current software configuration cannot run Wi-Fi and the hotspot at the same time. This is not a limit
  of the chip.
* **What to do**: nothing yet; connect your other devices to the router directly.

### The Chinese input method has to be turned on once
<!-- DISP-3 (D10). fcitx5-android 0.1.3 (GitHub release build), preinstalled from 1.0.0-rc.1 — the first build that really
     carries it (in 1.0.0-dev.10 Android refused to install it: the build system had damaged its v2 signature; fixed by
     installing it unmodified). Evidence in the Chinese file. ⬜ Not yet checked on the machine: the menu names below, and
     how a hardware keyboard switches between Chinese and English. Rewrite this entry once that is checked. -->
* **What you see**: the default keyboard has no Chinese.
* **Why**: from 1.0.0-rc.1 a Chinese input method (fcitx5, pinyin / shuangpin / wubi built in) is preinstalled, but it is
  not made the default. v0.7.1 and earlier had none.
* **What to do**: Settings → System → Keyboard → On-screen keyboard → turn on *Fcitx5*, then switch to it (menu names not yet checked on the machine). Updates come
  from the project's GitHub releases (the F-Droid build is signed differently and will not install over it).
  How the hardware keyboard switches between Chinese and English has not been checked yet.
