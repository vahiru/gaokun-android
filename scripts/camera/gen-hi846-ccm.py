#!/usr/bin/env python3
"""Generate device/huawei/gaokun3/camera/ipa-data/softisp/hi846.yaml.

The colour correction matrices come from the tuning Huawei's Windows camera
driver ships for the front camera: com.qti.tuned.hlt_hi846.bin in
qccamfrontsensor_extension8280.cab (driver package 200.0.10.0). That file is
byte-identical to Qualcomm's default tuning (com.qti.tuned.default.bin in the
same cab), so the matrices are generic, not a calibration of this module; they
are what Windows renders the front camera with. Its white points are generic
too, so the AWB curve below is measured instead.

The file's tuning-mode tree gives the default node an IPE cc13 set (BPS cc13
and IFE cc12 identity there); the 4:3 sensor modes libcamera uses
(3264x2448, 1632x1224) inherit it. Sensor mode 1 (3264x1836, going by the
order of the modes in the sensor module file) has its own set instead (BPS
cc13 and IFE cc12, equal); in a blind test through the HAL the two looked the
same.
This takes the default node's bright region, lux index 1-180, because
libcamera's Ccm interpolates by colour temperature only. Region entries are
laid out as for the rear camera (gen-ov13b10-ccm.py).

AWB white points, measured on one unit (2026-11-04): a white sheet covering
the middle of the view, one RAW frame from each camera under the same light,
and the gains that make the middle third neutral (G/R, G/B after black level
64, no clipped pixels; the rear's 4208x3120 frame without its phase-detection
pixels). Two lights: room LED and a phone LED. Projecting the rear camera's
gains onto its module curve (ov13b10.yaml, from the Windows tuning) gives each
light's colour temperature, about 3860 K and 4840 K, and the front/rear gain
ratio per channel at that temperature. The front curve is the rear curve
scaled by these ratios, interpolated between the two lights and held outside
them. It keeps the rear module's shape instead of passing through the
measured front points, which lie 0.015 and 0.019 off it in (R/G, B/G), inside
the AWB's 0.02 band. The R ratio differs by 4.5% between the two lights (B by
0.8%), so outside 3860-4840 K the curve is an extrapolation.

Usage: gen-hi846-ccm.py <com.qti.tuned.hlt_hi846.bin> > hi846.yaml
"""
import math
import struct
import sys

# Symbol ids of the default node's IPE cc13 lux-index region table and of its
# bright region's CCT table, in this file.
AEC_ID = 0x368
BRIGHT_ID = 0x369

# Rear module white points (ct, R gain, B gain), as in ov13b10.yaml.
REAR_CURVE = ((2300, 0.9409, 2.4767), (2856, 1.1876, 2.0625), (4000, 1.6824, 1.8002),
              (5000, 1.9442, 1.3920), (6500, 2.3550, 1.2480), (7500, 2.4383, 1.2411))

# White-sheet gains (R, B) per light: (light, rear, front). The rear's blue leaves
# out its phase-detection pixels (16 blue sites per 32x32 block in 4208x3120,
# about 25% brighter than the others), as libcamera's statistics do.
MEASURED = (('room LED', (1.5864, 1.7763), (1.9909, 1.9455)),
            ('phone LED', (1.9828, 1.4698), (2.3785, 1.6227)))


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


def rear_ct(gr, gb):
    """Colour temperature of rear gains: nearest point of the rear curve in
    (R/G, B/G) = (1/gain), linear between its points as libcamera's search."""
    pr, pb = 1 / gr, 1 / gb
    best = None
    for (t0, r0, b0), (t1, r1, b1) in zip(REAR_CURVE, REAR_CURVE[1:]):
        ax, ay, dx, dy = 1 / r0, 1 / b0, 1 / r1 - 1 / r0, 1 / b1 - 1 / b0
        u = max(0.0, min(1.0, ((pr - ax) * dx + (pb - ay) * dy) / (dx * dx + dy * dy)))
        d = math.hypot(pr - (ax + u * dx), pb - (ay + u * dy))
        if best is None or d < best[1]:
            best = (t0 + u * (t1 - t0), d)
    ct, off = best
    assert off < 0.04, (gr, gb, off)
    return ct


def awb_curve():
    # (ct, front/rear R ratio, front/rear B ratio) per light, by temperature.
    pts = sorted((rear_ct(*rear), front[0] / rear[0], front[1] / rear[1])
                 for _, rear, front in MEASURED)
    out = []
    for ct, r, b in REAR_CURVE:
        if ct <= pts[0][0]:
            kr, kb = pts[0][1:]
        elif ct >= pts[-1][0]:
            kr, kb = pts[-1][1:]
        else:
            (t0, r0, b0), (t1, r1, b1) = pts
            u = (ct - t0) / (t1 - t0)
            kr, kb = r0 + u * (r1 - r0), b0 + u * (b1 - b0)
        out.append((ct, r * kr, b * kb))
    rs, bs = [p[1] for p in out], [p[2] for p in out]
    assert rs == sorted(rs) and bs == sorted(bs, reverse=True), out
    return out


def main():
    recs = load(sys.argv[1])
    name, aec = recs[AEC_ID]
    assert name == 'mod_cc13_aec_data', name
    lux = struct.unpack_from('<f4xf4x', aec, 0)
    count, table = struct.unpack_from('<I', aec, 32)[0], struct.unpack_from('<I', aec, 40)[0]
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
        for ct in (start, end):
            ct = int(round(ct))
            if ct < 2000:
                continue
            out.append((ct, c, f'{int(start)}-{int(end)} K'))

    print(HEADER, end='')
    for ct, r, b in awb_curve():
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
# Software ISP tuning for the front camera (hi846).
#
# Ccm: colour correction matrices from the hi846 tuning Huawei's Windows camera
# driver ships (default node, IPE cc13, lux index 1-180). That tuning is
# Qualcomm's default, so these are generic matrices, not a calibration of this
# module. Generated by scripts/camera/gen-hi846-ccm.py, which says where the
# data comes from; do not edit by hand. Each matrix is flat over its colour
# temperature range, so each appears at both ends of its range here. With a
# Ccm the CPU debayer takes its slower matrix path.
#
# Awb: Bayesian AWB along the rear module's white-point curve, scaled by the
# front/rear gain ratios measured on one unit under two lights (about 3860 K
# and 4840 K); the generator says how. Grey world alone takes a scene filled
# with one colour for the illuminant. As for the rear camera, the prior is flat
# and the search may leave the curve by 0.02.
%YAML 1.1
---
version: 1
algorithms:
  - BlackLevel:
  - Awb:
      algorithm: bayes
      colourGains:
"""

AWB_TAIL = """\
      priors:
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
...
"""

if __name__ == '__main__':
    main()
