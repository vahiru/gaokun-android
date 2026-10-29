# R16 physical display density correction

R15 SurfaceFlinger reports both native 1600×2560 modes at
152.78195×152.78195 DPI. The local panel source has landscape physical
sizes (266×166 mm) for portrait modes. This is the physical-density
problem already recorded in `docs/v1.0-plan.md`.

The source chain is:

- `refs/gaokun-buildbot/drivers/panel-hx83121a/panel-himax-hx83121a.c`: both BOE and CSOT descriptors use width 266, height 166; DSC 60/120 Hz and non-DSC 60 Hz modes all use 1600×2560. `himax_get_modes` copies descriptor dimensions into the connector.
- `refs/a17/external/drm_hwcomposer/hwc/HwcDisplayConfigs.cpp`: reads connector millimeters into `mm_width`/`mm_height`.
- `refs/a17/external/drm_hwcomposer/hwc/HwcDisplay.cpp`, `GetDisplayBoundsMm`: when EDID has no valid bounds, returns `{configs_.mm_width, -1}`.
- `refs/a17/external/drm_hwcomposer/hwc3/ComposerClient.cpp`, `HwcDisplayConfigToAidlConfiguration`: computes horizontal DPI from mode width and physical width; copies it to vertical DPI when physical height is unavailable.

Reproducing float32 arithmetic gives 152.78195190429688 DPI, exactly
matching the reported value. Swapping the descriptor dimensions gives
244.81927490234375 DPI on both axes in that fallback path. Independently,
the vertical axis is 244.4511 DPI; integer-millimeter rounding accounts
for the small difference. Physical diagonal is 12.3444 inches.

`tools/hos/patches/panel-himax-portrait-physical-size.patch` changes only
four initializer values, covering both suppliers. It leaves timings,
orientation, touch mapping and logical density policy unchanged. The
fix belongs in the panel metadata; changing HWC to special-case this
panel would hide the incorrect connector geometry.

Offline validation: generated the four-line patch against local panel
source, successfully ran `git apply --check` and applied it to an isolated
temporary copy, then verified exact equality with the intended result.
No reference tree or existing output was modified. Building against the
isolated R15 tree must confirm its actual source matches this diagnosis.
Runtime acceptance requires new connector dimensions 166×266 mm and
SurfaceFlinger physical DPI about 244.82; MIUI AutoDensity and control
center layout still need runtime validation, since physical DPI alone
does not prove the final application density policy.

## Isolated R16 build audit

The actual VM R15 source accepted the patch with a one-line offset in
`/home/vahiru/hos-r16-kernel-display`, a separate copy of R15. Incremental
build completed with the same metadata-preserving make wrapper as R15.
Across all 3,799 objects, only the panel object, `vmlinux.o`, and
`arch/arm64/boot/vmlinuz.o` differ. The panel object differs in exactly
eight bytes, representing the four swapped 32-bit initializer values.
`System.map` and both kallsyms objects are identical: the change does not
alter symbol layout. Configuration, version metadata and embedded cpio
also remain identical. Existing Himax and UCSI objects are unchanged.

R16 EFI SHA256:
`cf1685272eb0dfa6b09a679c0d7a448e4012595f4d4ed4b9365448be6a8092b0`.
R15 baseline EFI remains:
`83bed9ab407c68fff7fb54fc99bbce5d902fb76b916de653e1f51d3385312d96`.
Build/audit files are staged under local cache
`/Users/vahiru/.cache/hos-r16-display/kernel/`; no historical `out/`
artifacts were modified. Device deployment and runtime acceptance are
separate from this build audit.
