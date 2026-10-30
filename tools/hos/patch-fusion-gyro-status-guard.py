#!/usr/bin/env python3
"""Offline R21-pinned guard: ignore UNRELIABLE gyro in fusion, retaining time."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct

spec=importlib.util.spec_from_file_location('fusion_study',Path(__file__).with_name('fusion-guard-study.py'))
common=importlib.util.module_from_spec(spec);spec.loader.exec_module(common)
PIN='3c54d8c74b94fa69e96f4c1dd5862983e7658e78ce81c2b3d234bf987dda2e21'
ENTRY=0x68524
GUARD=0x80a08
DROP=0x685f8
SIZE=16
RX_HEADER=0xb0
EXPECTED_RX=(1,5,0x3c000,0x3c000,0x3c000,0x44a08,0x44a08,0x4000)
ALLOWED=((0xd0,0xe0),(ENTRY,ENTRY+4),(GUARD,GUARD+SIZE))


def guard_bytes():
    # ldrsb w8,[x20,#0x24]; cbz w8,DROP; displaced ldr x8,[x20,#0x10]; b resume
    delta=DROP-(GUARD+4)
    assert delta%4==0 and -(1<<20)<=delta<(1<<20)
    return (struct.pack('<I',0x39c00000 | (0x24<<10) | (20<<5) | 8) +
            struct.pack('<I',0x34000000 | (((delta//4)&0x7ffff)<<5) | 8) +
            bytes.fromhex('880a40f9') + common.branch(GUARD+12,ENTRY+4))


def validate_layout(data):
    if data[:6]!=b'\x7fELF\x02\x01' or struct.unpack_from('<H',data,18)[0]!=183:
        raise ValueError('expected ELF64 AArch64 little endian')
    if struct.unpack_from('<Q',data,32)[0]!=64 or struct.unpack_from('<HH',data,54)!=(56,12):
        raise ValueError('unexpected program headers')
    if struct.unpack_from('<IIQQQQQQ',data,RX_HEADER)!=EXPECTED_RX:
        raise ValueError('R21 RX header mismatch')
    if data[ENTRY:ENTRY+4]!=bytes.fromhex('880a40f9'):
        raise ValueError('gyro timestamp-load patch site mismatch')
    if any(data[GUARD:GUARD+SIZE]):
        raise ValueError('guard space must be zero')
    if data[DROP:DROP+8]!=bytes.fromhex('880a40f9687a04f9'):
        raise ValueError('discard timestamp-update path mismatch')
    for name,sec in common.sections(data).items():
        if sec[2]&2 and sec[5] and sec[3]<GUARD+SIZE and sec[3]+sec[5]>GUARD:
            raise ValueError('allocated section overlaps guard: '+name)
    if struct.unpack_from('<IIQQ',data,64+3*56)!=(1,6,0x84000,0x84000):
        raise ValueError('next LOAD segment mismatch')


def patch(data):
    if hashlib.sha256(data).hexdigest()!=PIN:
        raise ValueError('requires exact deployed R21 library SHA256')
    validate_layout(data)
    result=bytearray(data)
    result[ENTRY:ENTRY+4]=common.branch(ENTRY,GUARD)
    result[GUARD:GUARD+SIZE]=guard_bytes()
    struct.pack_into('<QQ',result,0xd0,EXPECTED_RX[5]+SIZE,EXPECTED_RX[6]+SIZE)
    verify(data,result)
    return bytes(result)


def verify(before,after):
    if len(before)!=len(after):raise ValueError('file size changed')
    for i,(x,y) in enumerate(zip(before,after)):
        if x!=y and not any(lo<=i<hi for lo,hi in ALLOWED):
            raise ValueError(f'out-of-scope byte edit {i:#x}')
    expected=bytearray(before)
    expected[ENTRY:ENTRY+4]=common.branch(ENTRY,GUARD)
    expected[GUARD:GUARD+SIZE]=guard_bytes()
    struct.pack_into('<QQ',expected,0xd0,EXPECTED_RX[5]+SIZE,EXPECTED_RX[6]+SIZE)
    if after!=expected:raise ValueError('candidate differs from fixed guard')


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('library',type=Path)
    p.add_argument('--out',type=Path,required=True,help='new cache output directory')
    a=p.parse_args();original=a.library.read_bytes();candidate=patch(original)
    manifest={'schema':1,'input_sha256':PIN,'candidate_sha256':hashlib.sha256(candidate).hexdigest(),
              'guard_sha256':hashlib.sha256(guard_bytes()).hexdigest(),'guard_size':SIZE,
              'changed_ranges':[[hex(x),hex(y)] for x,y in ALLOWED],
              'entry':hex(ENTRY),'guard':hex(GUARD),'discard_timestamp_path':hex(DROP),
              'policy':'gyro status byte exactly zero skips fusion only; preserve raw event and advance mGyroTime',
              'preserved':'existing ACC guard, every other byte, symbols, dependencies, relocations and buildID',
              'unwind':'guard has no dedicated FDE; async unwind in its16 bytes may stop',
              'deployed':False}
    a.out.mkdir(parents=True,exist_ok=False)
    (a.out/'libsensorservice.so').write_bytes(candidate)
    (a.out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print(json.dumps(manifest,indent=2))


if __name__=='__main__':main()
