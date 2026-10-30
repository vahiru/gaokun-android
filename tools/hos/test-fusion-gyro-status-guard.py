#!/usr/bin/env python3
"""Static fail-closed checks for the independent R21 gyro-accuracy patch."""
import importlib.util
from pathlib import Path
import struct
import sys
from capstone import Cs,CS_ARCH_ARM64,CS_MODE_LITTLE_ENDIAN

spec=importlib.util.spec_from_file_location('patcher',Path(__file__).with_name('patch-fusion-gyro-status-guard.py'))
p=importlib.util.module_from_spec(spec);spec.loader.exec_module(p)


def reject(fn):
    try:fn()
    except ValueError:return
    raise AssertionError('invalid input accepted')


def main():
    if len(sys.argv)!=2:raise SystemExit('usage: test-fusion-gyro-status-guard.py R21_LIBRARY')
    base=Path(sys.argv[1]).read_bytes();candidate=p.patch(base)
    for at in (0,p.ENTRY,p.GUARD,0xd0,p.DROP):
        b=bytearray(base);b[at]^=1;reject(lambda:p.patch(b))
    for at in (p.ENTRY,p.GUARD,0xd0,p.DROP):
        b=bytearray(base);b[at]^=1;reject(lambda:p.validate_layout(b))
    for at in (0x1000,p.ENTRY,p.GUARD,0xd0,0x809c0):
        b=bytearray(candidate);b[at]^=1;reject(lambda:p.verify(base,b))
    md=Cs(CS_ARCH_ARM64,CS_MODE_LITTLE_ENDIAN)
    ins=list(md.disasm(p.guard_bytes(),p.GUARD))
    assert [(i.mnemonic,i.op_str) for i in ins]==[
        ('ldrsb','w8, [x20, #0x24]'),('cbz',f'w8, #{hex(p.DROP)}'),
        ('ldr','x8, [x20, #0x10]'),('b',f'#{hex(p.ENTRY+4)}')]
    assert candidate[0x809c0:0x80a08]==base[0x809c0:0x80a08]
    assert candidate[0x567d4:0x567dc]==base[0x567d4:0x567dc]
    ph=struct.unpack_from('<IIQQQQQQ',candidate,p.RX_HEADER)
    assert ph[1]==5 and ph[3]+ph[6]==p.GUARD+16 and ph[3]+ph[6]<0x84000
    assert candidate[0x684c4:0x68524]==base[0x684c4:0x68524]
    assert candidate[0x685f8:0x687b0]==base[0x685f8:0x687b0]
    print('PASS: exact R21 pin, entry/drop/RX/cave rejection, immutable ACC guard,16-byte branches, unchanged PAC/frame teardown and raw converter')


if __name__=='__main__':main()
