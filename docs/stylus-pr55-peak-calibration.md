# Stylus: strong-peak extraction on top of PR #55

This change builds on PR #55's One Euro filter and 1/16-pixel reporting.
Those remain upstream work; this contribution changes the TX1 region selection,
tunes the existing filter, and adds an optional measured horizontal calibration.

## Coordinate path

The on-sensor maximum seeds a four-connected region strictly above both the
noise floor and half peak. The existing bounding-box projections, triangle
interpolation, pitch compensation, panel pitch map, tilt correction and edge
smoothing follow it. A region of ten or more cells, a weak peak, or invalid
anchor falls back to the legacy solver. This avoids weak connected tails
expanding the coordinate projection around an otherwise stable strong peak.
This is discrete strong-peak selection; there is no fractional weighting,
Gaussian fitting or learned residual correction in this implementation.

The strong-peak policy is enabled by default. `-lr` restores legacy region
selection and `-pr` explicitly enables strong-peak selection.

## Filter and calibration

PR #55's filter is retained with defaults `-pf 1000,125,5000` (previously
`1000,500,5000`). The bounded offline scan tested minimum cutoff 500/1000/2000
and beta 125/250/500 with derivative cutoff fixed at 5000. It did not establish
a universal optimum: the scan used a Gaussian-plus-residual input chain, so its
numerical gains cannot be attributed to this hard-peak chain. Lower beta trades
more smoothing for more turn error. The hard-peak plus beta-125 combination was
subsequently tried on the device; the user reported it better than the preceding
version. This is subjective validation, not a measured end-to-end latency claim.

`-sx SCALE` expands raw Y (horizontal in the tested landscape orientation)
around 1279.875 px after edge smoothing and before SUBPX rounding, then clips
to display bounds. The default is identity, 1.0. Finite values strictly between
0.5 and 1.5 are accepted; invalid values retain the current setting.

The tested unit's profile is:

```
gk3pend -pr -pf 1000,125,5000 -sx 1.0040851371
```

The scale comes from a 5x3 target session with repeated taps, excluding an
identified accidental tap and holding out two columns for validation. The user
reported substantially improved inward left/right offset. This coefficient is
unit-specific and must not be assumed correct for every panel or orientation.
It is intentionally not the global default and is not a full 2D calibration.

## Device validation

MateBook E Go, Android 17, OneNote and StarNote:

* Strong-peak plus PR55 filtering substantially reduced fine jagged handwriting;
  the user reported normal pressure, edge placement and writing responsiveness.
* The calibrated combination improved the inward horizontal offset.
* On 2026-10-31 the same hard-peak/scanned-filter/calibration profile was installed
  as module v0.4, replacing the previous profile. The user reported better writing.
* The installed service was verified as UID system, domain `u:r:gk3pend:s0`, with
  one M-Pencil device: X max 25599, Y max 40959, resolution 153 on both axes.
* The installed module executable SHA256 was
  `cfa162cc63744991c6d3575feda0e493970b925432d0b6a4a5e8f5179098d917`.

The upstream source here omits the module's inactive experimental branches and
wrapper. It retains the same accepted active coordinate path. Long slow-diagonal
waves are not claimed completely eliminated, and reboot persistence of this
latest module was not tested in this session.

## Reproducible checks

```
python3 scripts/tests/test_stylus_peak_region.py --ubsan
```

The test compiles the real daemon and solver. Cases cover default profile and
calibration direction, weak tails, diagonal-only secondary peaks, translated
peaks, broad-region fallback, nine/ten-cell boundary, strict half-peak threshold,
all physical anchors, 10,000 random signed grids, legacy selection and empty
signals. These checks verify coordinate behavior and bounds; they do not replace
handwriting or independent tip-position measurements.
