#!/usr/bin/env python3
"""Extract ff_cmd_name switch tables: cmd number -> name string."""
import struct, sys

MBN = '/Users/vahiru/Projects/gaokun-android/tools/fingerprint-bringup/rev/cabs/qctree/fingerpr.mbn'
data = open(MBN, 'rb').read()

def va2off(va):
    # file = VA + 0x3000 for RX segment (VA < 0x226000)
    return va + 0x3000

def u32(va):
    return struct.unpack_from('<I', data, va2off(va))[0]

def i32(va):
    return struct.unpack_from('<i', data, va2off(va))[0]

def cstr(va):
    off = va2off(va)
    end = data.index(b'\x00', off)
    return data[off:end].decode('ascii', 'replace')

# Jump tables: (table_va, base_cmd, count)
tables = [
    (0x1e1b00, 0x1000, 0x1c),  # w8-0x1000 <= 0x1b
    (0x1e1b70, 0x2000, 0x09),  # w8-0x2000 <= 8
    (0x1e1b94, 0x3000, 0x25),  # w8-0x3000 <= 0x24
    (0x1e1c28, 0x3100, 0x07),  # w8-0x3100 <= 6
    (0x1e1c44, 0x4000, 0x06),  # w8-0x4000 <= 5
]

# Pattern at each case body: adrp x19, #page ; add x19, x19, #imm ; b 0x2dec
# adrp encoding: immlo bits 30:29, immhi bits 23:5, Rd bits 4:0
def decode_adrp(va, rd_expect=19):
    w = u32(va)
    if (w & 0x9F000000) != 0x90000000:
        return None
    rd = w & 0x1F
    if rd != rd_expect:
        return None
    immlo = (w >> 29) & 3
    immhi = (w >> 5) & 0x7FFFF
    imm = (immhi << 2) | immlo
    if imm & (1 << 20):
        imm -= (1 << 21)
    page = (va & ~0xFFF) + (imm << 12)
    return page

def decode_add_imm(va, rd_expect=19, rn_expect=19):
    w = u32(va)
    # ADD (immediate) 64-bit: 1001 0001 sh imm12 Rn Rd
    if (w >> 23) != 0x122:  # 10010001 0 << 1 ... check shift=0
        # more precise: bits 31:23 == 0b100100010 for shift=0
        return None
    rd = w & 0x1F
    rn = (w >> 5) & 0x1F
    if rd != rd_expect or rn != rn_expect:
        return None
    imm12 = (w >> 10) & 0xFFF
    sh = (w >> 22) & 1
    if sh:
        imm12 <<= 12
    return imm12

results = {}
for tva, base, count in tables:
    for i in range(count):
        cmd = base + i
        off = i32(tva + i * 4)
        tgt = tva + off
        # case body should start with adrp x19
        page = decode_adrp(tgt)
        if page is None:
            # some bodies may have a different layout; try next word
            page = decode_adrp(tgt + 4)
            addva = tgt + 8
        else:
            addva = tgt + 4
        imm = decode_add_imm(addva)
        if page is None or imm is None:
            results[cmd] = '??? @0x%x' % tgt
            continue
        sva = page + imm
        try:
            results[cmd] = cstr(sva)
        except Exception:
            results[cmd] = '??str@0x%x' % sva

for cmd in sorted(results):
    print('0x%04x  %s' % (cmd, results[cmd]))
