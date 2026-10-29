# HyperOS Recents component overlay

Single-resource static RRO for `/vendor/overlay/HosRecentsOverlay.apk`. The donor
framework defaults to Launcher3, while this port installs MIUI Home. SystemUI's
LauncherProxyService resolves QUICKSTEP by that component's package; system_server
RecentTasks caches the component and UID at boot. Both require the MIUI component.

Before deployment verify the exact activity with `cmd package resolve-activity`
and the QUICKSTEP service with `cmd package query-services`; package resolution
alone does not prove the activity class exists. This manifest intentionally uses
SDK28 for the legacy preinstalled overlay contract, has no code and declares one
string. It does not change navigation mode, touch thresholds or permissions.

Build with SDK37 aapt2 compile/link against android-35/android.jar, zipalign, then
sign with the existing local test keystore `out/hyperos-r11/probe/debug.jks`
(alias test). Do not commit the key or replace its signer. Use
`prepare-vendor.py --recents-overlay APK` with **all** current R14 arguments,
including `--media-profiles out/hyperos-r9/media_profiles_candidate.xml` and
`--composer-apex out/hyperos-r9/composer-repacked/composer.apex`.
Compare every old replacement's bytes/metadata/xattrs, unchanged CIL/file contexts,
and all five non-vendor partitions before deployment. The new APK inherits
`vendor_overlay_file`, 0644 root:root from Gaokun3WifiOverlay.apk.

Preflight on R14 accepted idmap2 create with `--policy vendor`, without
`--ignore-overlayable`. The signer certificate SHA256 is
`4839029b0a0d8e0990ba02ce22e13a07271add5a12de97a731aba2e2d70d21cd`.

Runtime `recents-compat.sh apply` is diagnostic only: this donor discarded the
shell fabricated overlay on full reboot. Do not describe it as persistent.
Static-RRO acceptance requires a full isolated reboot, effective resource lookup,
LauncherProxyService Connected/Enabled/Bound, valid RecentTasks UID/component,
and actual app-to-home/recent-tasks gestures. Preserve launcher data throughout.
