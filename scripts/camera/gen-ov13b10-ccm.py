#!/usr/bin/env python3
"""Generate device/huawei/gaokun3/camera/ipa-data/softisp/ov13b10.yaml.

The colour correction matrices come from the tuning Huawei's Windows camera
driver ships for this camera module: com.qti.tuned.ofilm_ov13b10.bin in
qccamrearsensor_extension8280.cab (driver package 200.0.10.0). This is tuning
for the module type, not a per-unit calibration (that lives in the module
EEPROM). Only the IPE cc13 block is tuned there (BPS cc13 and IFE cc12 are
identity), per colour temperature region. With the flash off it has three
lux-index regions; this uses the bright one, lux index 1-180, because
libcamera's Ccm interpolates by colour temperature only. A separate single
matrix for flash shots (LED branch, table 0x1b7) is not used.

Each region entry is 72 bytes: CCT start, pad, CCT end, pad (f32), the 3x3
matrix c[9] (row-major RGB), offsets k[3] (all zero here), 2 pad.

The AWB white points come from the same file: MWBGainV1 holds the module's
manual white balance gains (R, G, B) per illuminant, the inverse of the
reference points in refPtV1. The file names no illuminants; the order is taken
to be Qualcomm's (one unnamed entry, D75, D65, D50, CW, TL84, U30, A, H and one
more), which the data fits: R/G rises steadily along it. libcamera's Bayesian
AWB searches along the curve these points make, linear in colour temperature
between them in R/G and B/G, and up to transversePos/Neg off it.

This takes H, A, TL84, D50, D65 and D75. CW and U30 lie near TL84 and within
that band of the curve, but between A and D50 they would put a sharp kink into
it, which made the colour temperature estimate jump in a simulation of the
search. TL84 bends the curve towards the fluorescent green around 4000 K: a
warm non-fluorescent light there may sit about twice the band off it and keep
a slight magenta tint (estimated from the geometry, not measured); without
TL84 the fluorescent lights would turn green instead. The H point's R gain
(0.94) is below the 1.0 the AWB clamps gains to, so below about 2460 K red
stays slightly too strong.

Usage: gen-ov13b10-ccm.py <com.qti.tuned.ofilm_ov13b10.bin> > ov13b10.yaml
"""
import struct
import sys

# Symbol id of the IPE cc13 lux-index region table, and the bright region's
# CCT table, in this file.
AEC_ID = 0x1b2
BRIGHT_ID = 0x1b3


def load(path):
    b = open(path, 'rb').read()
    tab, n = struct.unpack_from('<II', b, 0x98)
    secs = [struct.unpack_from('<QQQ', b, tab + 24 * i) for i in range(n)]
    _, s0off, s0size = secs[0]
    base = secs[1][1]
    recs = {}
    for o in range(s0off, s0off + s0size - 71, 72):
        rid, = struct.unpack_from('<Q', b, o)
        name = b[o + 8:o + 48].split(b'\0')[0].decode('latin1')
        doff, dsize = struct.unpack_from('<QQ', b, o + 56)
        recs[rid] = (name, b[base + doff:base + doff + dsize])
    return recs


# MWBGainV1 entries used for the AWB curve: (index, colour temperature).
MWB_POINTS = ((8, 2300), (7, 2856), (5, 4000), (3, 5000), (2, 6500), (1, 7500))


def awb_points(recs):
    (name, data), = [r for r in recs.values() if r[0] == 'MWBGainV1']
    f = [struct.unpack_from('<f', data, 8 * i)[0] for i in range(len(data) // 8)]
    triples = [f[6 + 3 * i: 9 + 3 * i] for i in range(10)]
    out = []
    for idx, ct in MWB_POINTS:
        r, g, b = triples[idx]
        assert g == 1.0, (idx, triples[idx])
        out.append((ct, r, b))
    rs, bs = [p[1] for p in out], [p[2] for p in out]
    assert rs == sorted(rs) and bs == sorted(bs, reverse=True), out
    return out


def main():
    recs = load(sys.argv[1])
    name, aec = recs[AEC_ID]
    assert name == 'mod_cc13_aec_data', name
    # First region: lux index 1..180, 4 CCT entries, table id BRIGHT_ID.
    lux = struct.unpack_from('<f4xf4x', aec, 0)
    count, table = struct.unpack_from('<II', aec, 32)[0], struct.unpack_from('<I', aec, 40)[0]
    assert lux == (1.0, 180.0) and table == BRIGHT_ID, (lux, table)
    name, data = recs[BRIGHT_ID]
    assert name == 'mod_cc13_cct_data' and len(data) == 72 * count, (name, len(data))

    out = []
    for i in range(count):
        start, end = struct.unpack_from('<f4xf4x', data, 72 * i)
        c = struct.unpack_from('<9f', data, 72 * i + 16)
        k = struct.unpack_from('<3f', data, 72 * i + 52)
        assert k == (0.0, 0.0, 0.0), k
        for r in range(3):
            s = sum(c[3 * r:3 * r + 3])
            assert abs(s - 1.0) < 1e-3, (start, end, r, s)
        # libcamera interpolates linearly between points and clamps outside
        # them; two points per region keep each region flat like the
        # Windows tuning, which interpolates only in the gaps.
        for ct in (start, end):
            ct = int(round(ct))
            if ct < 2000:
                continue
            out.append((ct, c, f'{int(start)}-{int(end)} K'))

    print(HEADER, end='')
    for ct, r, b in awb_points(recs):
        print(f'        - ct: {ct}')
        print(f'          gains: [ {r:.4f}, {b:.4f} ]')
    print(AWB_TAIL, end='')
    for ct, c, src in out:
        print(f'        - ct: {ct}  # region {src}')
        rows = [', '.join(f'{v:8.5f}' for v in c[3 * r:3 * r + 3]) for r in range(3)]
        print(f'          ccm: [ {rows[0]},')
        print(f'                 {rows[1]},')
        print(f'                 {rows[2]} ]')
        print('          offsets: [ 0, 0, 0 ]')
    print(FOOTER, end='')


HEADER = """\
# SPDX-License-Identifier: CC0-1.0
#
# Software ISP tuning for the rear camera (ov13b10).
#
# Ccm: colour correction matrices from the tuning Huawei's Windows camera driver
# ships for this module (IPE cc13, lux index 1-180). Generated by
# scripts/camera/gen-ov13b10-ccm.py, which says where the file comes from; do
# not edit by hand. The Windows tuning keeps each matrix flat over its colour
# temperature range, so each appears at both ends of its range here; libcamera
# interpolates between the points and clamps outside them. The colour
# temperature that picks the matrix is the AWB's, on this module's white-point
# scale (Awb below). With a Ccm the CPU debayer takes its slower matrix path.
#
# Awb: Bayesian AWB along this module's white points from the same Windows
# tuning (MWBGainV1: H, A, TL84, D50, D65, D75). Grey world alone takes a scene
# filled with one colour for the illuminant: green close-ups turned magenta.
# With no lux estimate in the soft ISP the prior is flat; the search may leave
# the curve by 0.02 in R/G and B/G. The curve and its trade-offs are explained
# in the generator; below about 2460 K red stays slightly too strong, as the H
# point asks for an R gain under the AWB's floor of 1.0.
%YAML 1.1
---
version: 1
algorithms:
  - BlackLevel:
  - Awb:
      algorithm: bayes
      colourGains:
"""

AWB_TAIL = """      priors:
        - lux: 0
          ct: [ 2000, 8000 ]
          probability: [ 1.0, 1.0 ]
      transversePos: 0.02
      transverseNeg: 0.02
      AwbMode:
        AwbAuto:
          lo: 2300
          hi: 7500
  - Ccm:
      ccms:
"""

FOOTER = """\
  - Adjust:
  - Agc:
      # Keep 30 fps (33 ms) until the gain reaches 8x before lengthening the
      # exposure to 66 ms; with patches/libcamera/0006-0008 the AGC otherwise
      # drops a 15-30 fps preview to 15 fps in ordinary room light.
      AeExposureMode:
        ExposureNormal:
          exposureTime: [ 33000, 66000 ]
          gain: [ 8.0, 15.5 ]
...
"""

if __name__ == '__main__':
    main()
