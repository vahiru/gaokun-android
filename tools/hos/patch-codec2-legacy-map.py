#!/usr/bin/env python3
"""Pinned piano OS4 compatibility patch for BLOB-backed linear Codec2 blocks.

Reinstate allocation->map() dispatch in mapReadView/mapWriteView, as in A16,
instead of routing every non-ION handle through the DMABUF custom mapper.
The virtual allocation method still validates/maps the allocation. CFI, access
checks and allocator choice remain intact. Custom mapper callbacks are unused
on this port and are not supported by this compatibility mode.

Only the two inspected donor binaries are accepted. No device writes.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

INPUTS = {
    'system': ('4035e9e29dff9dea1c7192d29cb7e7f50feaa0a30659efef69dc6e6325b53bb6',
               (0x63468, 0x68fd0)),
    'apex': ('c07b11013996b65acf6ff402bd05f31ac8e8eee27b75ad1b907fefe5e19e5eec',
             (0x64a90, 0x6a9bc)),
}

def patch(blob, variant):
    checksum, sites = INPUTS[variant]
    if hashlib.sha256(blob).hexdigest() != checksum:
        raise ValueError('unreviewed Codec2 binary; refusing patch')
    result = bytearray(blob)
    for offset in sites:
        # tbz w0, #0, +0x60 after C2HandleIon::IsUsed(): fall through to
        # allocation->map() instead, retaining the original validated vcall.
        if struct.unpack_from('<I', blob, offset)[0] != 0x36000300:
            raise ValueError('unexpected mapper branch')
        struct.pack_into('<I', result, offset, 0xd503201f)  # AArch64 NOP
    return bytes(result)

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--variant', choices=INPUTS, required=True)
    p.add_argument('--input', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    a = p.parse_args()
    blob = a.input.read_bytes()
    result = patch(blob, a.variant)
    with a.out.open('xb') as f:
        f.write(result)
    a.out.with_suffix('.json').write_text(json.dumps({
        'input_sha256': hashlib.sha256(blob).hexdigest(),
        'output_sha256': hashlib.sha256(result).hexdigest(),
        'variant': a.variant, 'branch_offsets': INPUTS[a.variant][1],
        'custom_mapper_callbacks_supported': False,
    }, indent=2) + '\n')

if __name__ == '__main__':
    main()
