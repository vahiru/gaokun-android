#!/usr/bin/env python3
"""Produce an offline, hash-pinned HyperOS Fusion guard candidate. Never deploys."""
import argparse
import hashlib
import importlib.util
import json
import lzma
from pathlib import Path
import struct
import subprocess
import tempfile

spec = importlib.util.spec_from_file_location('fusion_study',
    Path(__file__).with_name('fusion-guard-study.py'))
study = importlib.util.module_from_spec(spec)
spec.loader.exec_module(study)
PIN = study.PIN
ENTRY, GUARD, GUARD_SIZE = study.ENTRY, study.GUARD, 72
PHOFF, PHENT, RX_INDEX = 64, 56, 2
RX_HEADER = PHOFF + PHENT * RX_INDEX
EXPECTED_RX = (1, 5, 0x3c000, 0x3c000, 0x3c000, 0x449c0, 0x449c0, 0x4000)
ALLOWED = ((RX_HEADER + 32, RX_HEADER + 48), (ENTRY, ENTRY + 4),
           (GUARD, GUARD + GUARD_SIZE))


def symbols(data):
    off = struct.unpack_from('<Q', data, 40)[0]
    size, count = struct.unpack_from('<HH', data, 58)
    sections = [struct.unpack_from('<IIQQQQIIQQ', data, off+i*size) for i in range(count)]
    for sec in sections:
        if sec[1] not in (2, 11):
            continue
        for i in range(sec[4], sec[4]+sec[5], sec[9]):
            yield struct.unpack_from('<IBBHQQ', data, i)


def validate_layout(data):
    if data[:6] != b'\x7fELF\x02\x01':
        raise ValueError('expected ELF64 little endian')
    if struct.unpack_from('<H', data, 18)[0] != 183:
        raise ValueError('expected AArch64')
    if struct.unpack_from('<Q', data, 32)[0] != PHOFF or struct.unpack_from('<HH', data, 54) != (56, 12):
        raise ValueError('unexpected program header layout')
    if struct.unpack_from('<IIQQQQQQ', data, RX_HEADER) != EXPECTED_RX:
        raise ValueError('RX segment differs from reviewed baseline')
    if data[ENTRY-4:ENTRY+4] != bytes.fromhex('5f2403d50af441b9'):
        raise ValueError('BTI/entry instruction mismatch')
    if any(data[GUARD:GUARD+GUARD_SIZE]):
        raise ValueError('guard area must be entirely zero')
    sections = study.sections(data)
    for name, sec in sections.items():
        if sec[2] & 2 and sec[5] and sec[3] < GUARD+GUARD_SIZE and sec[3]+sec[5] > GUARD:
            raise ValueError('guard overlaps allocated section '+name)
    # Next PT_LOAD is RW at0x84000. Existing final RX mapping page already
    # ends there; declared segment extension does not create a new page.
    next_load = struct.unpack_from('<IIQQQQQQ', data, PHOFF + PHENT * 3)
    if next_load[:4] != (1, 6, 0x84000, 0x84000) or GUARD+GUARD_SIZE >= 0x84000:
        raise ValueError('unexpected next load segment')
    for label, blob in [('dynamic', data)]:
        for _, info, _, idx, value, size in symbols(blob):
            if idx and size and value < GUARD+GUARD_SIZE and value+size > GUARD:
                raise ValueError(label+' symbol overlaps guard')
    debug = sections['.gnu_debugdata']
    mini = lzma.decompress(data[debug[4]:debug[4]+debug[5]])
    for _, info, _, idx, value, size in symbols(mini):
        if idx and size and value < GUARD+GUARD_SIZE and value+size > GUARD:
            raise ValueError('debug symbol overlaps guard')


def assemble_guard():
    with tempfile.TemporaryDirectory(prefix='fusion-guard-asm-') as tmp:
        p = Path(tmp)
        (p/'guard.S').write_text(study.ASM)
        subprocess.run(['clang','-target','aarch64-linux-gnu','-c',str(p/'guard.S'),
                        '-o',str(p/'guard.o')],check=True)
        obj = (p/'guard.o').read_bytes()
        text = study.sections(obj)['.text']
        code = bytearray(obj[text[4]:text[4]+text[5]])
    if len(code) != 72 or code[56:60] != bytes.fromhex('0af441b9') or code[60:64] != study.branch(0,0):
        raise ValueError('unexpected assembled guard layout')
    code[60:64] = study.branch(GUARD+60, ENTRY+4)
    return bytes(code)


def patch(data, guard):
    if hashlib.sha256(data).hexdigest() != PIN:
        raise ValueError('original library SHA256 mismatch')
    validate_layout(data)
    if len(guard) != GUARD_SIZE:
        raise ValueError('unexpected guard size')
    result = bytearray(data)
    result[ENTRY:ENTRY+4] = study.branch(ENTRY,GUARD)
    result[GUARD:GUARD+GUARD_SIZE] = guard
    struct.pack_into('<QQ', result, RX_HEADER+32, EXPECTED_RX[5]+GUARD_SIZE,
                     EXPECTED_RX[6]+GUARD_SIZE)
    verify_scope(data,result,guard)
    return bytes(result)


def verify_scope(original, candidate, guard):
    if len(original) != len(candidate):
        raise ValueError('file length changed')
    for i, (a,b) in enumerate(zip(original,candidate)):
        if a != b and not any(lo <= i < hi for lo,hi in ALLOWED):
            raise ValueError(f'unexpected byte change at {i:#x}')
    if candidate[GUARD:GUARD+GUARD_SIZE] != guard or candidate[ENTRY:ENTRY+4] != study.branch(ENTRY,GUARD):
        raise ValueError('guard/entry content mismatch')
    rx = list(EXPECTED_RX);rx[5]+=GUARD_SIZE;rx[6]+=GUARD_SIZE
    if struct.unpack_from('<IIQQQQQQ',candidate,RX_HEADER) != tuple(rx):
        raise ValueError('segment extension mismatch')
    # All section headers, dynamic symbols, dependencies, relocation bytes,
    # build-id note and original BTI/PAC instructions are outside ALLOWED.


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('library',type=Path)
    p.add_argument('--out',type=Path,required=True,help='new output directory; must not exist')
    a = p.parse_args()
    original=a.library.read_bytes();guard=assemble_guard();candidate=patch(original,guard)
    sec=study.sections(original)['.note.gnu.build-id']
    note=original[sec[4]:sec[4]+sec[5]]
    manifest={'schema':1,'original_sha256':PIN,
              'candidate_sha256':hashlib.sha256(candidate).hexdigest(),
              'guard_sha256':hashlib.sha256(guard).hexdigest(),
              'allowed_file_ranges':[[hex(x),hex(y)] for x,y in ALLOWED],
              'guard_va':hex(GUARD),'entry_patch_va':hex(ENTRY),'guard_bytes':72,
              'rx_before':list(EXPECTED_RX),
              'rx_after':list(struct.unpack_from('<IIQQQQQQ',candidate,RX_HEADER)),
              'build_id_retained':note[16:].hex(),
              'unwind_limitation':'No FDE covers the72-byte leaf guard; asynchronous unwinding may stop there.',
              'status':'offline candidate only; no device validation or causal-root claim'}
    a.out.mkdir(parents=True,exist_ok=False)
    (a.out/'libsensorservice.so').write_bytes(candidate)
    (a.out/'guard.bin').write_bytes(guard)
    (a.out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print(json.dumps(manifest,indent=2))


if __name__ == '__main__':
    main()
