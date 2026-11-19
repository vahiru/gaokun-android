#!/usr/bin/env python3
"""Generate the Awb and Ccm sections of the s5k4h7 qcom-ife tuning.

Some Huawei MateBook E Go units carry an S5K4H7 front camera module (Xinli)
instead of the HI846. The tuning Huawei's Windows camera driver ships for it,
com.qti.tuned.xinli_s5k4h7.bin in qccamfrontsensor_extension8280.cab, is a
tuning of that module (not Qualcomm's default one, unlike the HI846's), so
its white points and matrices are used directly, by the method of
gen-ov13b10-ccm.py, which describes the file layout:

- Ccm: the IPE cc13 matrices of the bright lux region (lux index 1-180),
  which has two colour temperature entries here (1-5000 K and 5500-10000 K).
  Each matrix is kept flat over its range: it appears at both ends of it, and
  libcamera interpolates between the points and clamps outside them.
- Awb: the MWBGainV1 gains (R, G, B) of H, A, TL84, D50, D65 and D75, at the
  same positions in the table as in the ov13b10 file. Several tuning nodes
  carry the table; they must agree.

Record ids differ between files, so the tables are looked up by name: the
IPE cc13 table is the first mod_cc13_aec_data with more than one region.

This prints the two sections as they appear in
device/huawei/gaokun3/camera/ipa-data/qcom-ife/s5k4h7.yaml (the same file as
src/ipa/qcom-ife/data/s5k4h7.yaml of patches/libcamera).

Usage: gen-s5k4h7-ccm.py <com.qti.tuned.xinli_s5k4h7.bin>
"""
import struct
import sys

# MWBGainV1 entries used for the AWB curve: (index, colour temperature).
MWB_POINTS = ((8, 2300), (7, 2856), (5, 4000), (3, 5000), (2, 6500), (1, 7500))


def load(path):
    b = open(path, 'rb').read()
    tab, n = struct.unpack_from('<II', b, 0x98)
    secs = [struct.unpack_from('<QQQ', b, tab + 24 * i) for i in range(n)]
    _, s0off, s0size = secs[0]
    base = secs[1][1]
    recs, order = {}, []
    for o in range(s0off, s0off + s0size - 71, 72):
        rid, = struct.unpack_from('<Q', b, o)
        name = b[o + 8:o + 48].split(b'\0')[0].decode('latin1')
        doff, dsize = struct.unpack_from('<QQ', b, o + 56)
        recs[rid] = (name, b[base + doff:base + doff + dsize])
        order.append(rid)
    return recs, order


def ccm_points(recs, order):
    aec_id = next(r for r in order
                  if recs[r][0] == 'mod_cc13_aec_data' and len(recs[r][1]) >= 96)
    aec = recs[aec_id][1]
    lux = struct.unpack_from('<f4xf4x', aec, 0)
    count, = struct.unpack_from('<I', aec, 32)
    table, = struct.unpack_from('<I', aec, 40)
    assert lux == (1.0, 180.0), lux
    name, data = recs[table]
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
    return out


def awb_points(recs, order):
    tables = {recs[r][1] for r in order if recs[r][0] == 'MWBGainV1'}
    (data,) = tables
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
    recs, order = load(sys.argv[1])
    print('  - Awb:')
    print('      algorithm: bayes')
    print('      colourGains:')
    for ct, r, b in awb_points(recs, order):
        print(f'        - ct: {ct}')
        print(f'          gains: [ {r:.4f}, {b:.4f} ]')
    print(AWB_TAIL, end='')
    for ct, c, src in ccm_points(recs, order):
        rows = [', '.join(f'{v:8.5f}' for v in c[3 * r:3 * r + 3]) for r in range(3)]
        print(f'        - ct: {ct}  # region {src}')
        print(f'          ccm: [ {rows[0]},')
        print(f'                 {rows[1]},')
        print(f'                 {rows[2]} ]')
        print('          offsets: [ 0, 0, 0 ]')


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

if __name__ == '__main__':
    main()
