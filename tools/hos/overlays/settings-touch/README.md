# Settings card touch tint candidate

Code-free static RRO targeting `com.android.settings`, package
`org.gaokun3.hos.settingscard`, output `HosSettingsCardOverlay.apk`. Current R18
candidate sets exactly two light-mode colors to `#ff007aff`:

- `color/miuix_recyclerview_card_tint_color_light`
- `color/miuix_preference_item_tint_color_light`

Donor CardStateDrawable, alpha animation and rounded geometry remain intact.
Dark-mode resources are unchanged. The candidate changes touch tint, not an
outline, and does not establish a graphics-driver fault.

## Evidence and scope

The earliest preference-only `HosSettingsTouchOverlay.apk` changed its resource
lookup but did not affect the tested homepage row. It was rejected. The later
recyclerview-only candidate affected the homepage HeaderAdapter row, which uses
`cardGroupItemForegroundEffect` and `miuix_recyclerview_card_item_press_fg_light`,
but did not establish coverage of preference cards on the right-hand page.
Neither single-resource candidate is the current artifact.

The parent agent's new R18 runtime experiment used two temporary fabricated
overlays covering both colors. The right-side network-acceleration preference
also turned blue while pressed. On the left “More connections” row, measured RGB
was (225,239,255) while held, (246,250,255) during MOVE, and (255,255,255) after
release, against page (243,243,243). Rounded corners remained visible. These are
runtime measurements supplied by the parent, not results from installing this
static APK. Earlier gray feedback was (244,244,244) against (243,243,243).

The dual-resource behavior has runtime evidence, but this signed static RRO has
**not been persistently deployed or accepted by the user**. Do not infer coverage
of every Settings subpage. Other views referencing these colors may also change.

Fabricated overlays are lost on reboot on this donor. Before image integration,
verify `idmap2 create --policy vendor` accepts the actual target APK without
`--ignore-overlayable`. Persistence acceptance requires a full reboot, both
effective-resource lookups, light/dark mode, touch/drag/release and navigation
highlighting checks. Building this candidate does not install it.

## R18 build

SDK37 aapt2 compile/link against android-35/android.jar, zipalign, existing
`out/hyperos-r11/probe/debug.jks` alias `test`. Min/target SDK28 follows the
recents-overlay contract. No new signer was created.

Artifact: `~/.cache/hos-r18-settings/overlay/HosSettingsCardOverlay.apk`

APK SHA256: `bb94d7e507a28059ecc0f6ae5a0fa76a38c30e4605d6d787d432a49085e3e490`

Signer SHA256: `4839029b0a0d8e0990ba02ce22e13a07271add5a12de97a731aba2e2d70d21cd`

`aapt2 dump resources` confirms exactly the two color entries above, each with
only default `#ff007aff`. Compiled manifest has `hasCode=false`, static overlay,
and expected target/package. ZIP contains only manifest/resources and signing
metadata, no DEX or native code. apksigner verification passed. Resource/manifest
dumps, signature output and `verification.json` are beside the APK. No device,
VM or image changes were made for this build.
