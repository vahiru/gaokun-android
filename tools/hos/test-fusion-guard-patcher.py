#!/usr/bin/env python3
"""Offline regression checks for the pinned ELF edit and ARM64 control flow."""
import importlib.util
from pathlib import Path
import struct
import sys
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN

spec=importlib.util.spec_from_file_location('patcher',Path(__file__).with_name('patch-fusion-zero-guard.py'))
p=importlib.util.module_from_spec(spec);spec.loader.exec_module(p)


def rejected(fn):
    try:
        fn()
    except ValueError:
        return
    raise AssertionError('unsafe input was accepted')


def main():
    if len(sys.argv)!=2:
        raise SystemExit('usage: test-fusion-guard-patcher.py ORIGINAL.so')
    original=Path(sys.argv[1]).read_bytes()
    guard=p.assemble_guard()
    candidate=p.patch(original,guard)
    p.verify_scope(original,candidate,guard)
    bad=bytearray(original);bad[-1]^=1
    rejected(lambda:p.patch(bad,guard))
    for offset in (p.RX_HEADER+4,p.ENTRY-4,p.GUARD):
        bad=bytearray(original);bad[offset]^=1
        rejected(lambda:p.validate_layout(bad))
    bad=bytearray(candidate);bad[0x1000]^=1
    rejected(lambda:p.verify_scope(original,bad,guard))
    bad=bytearray(candidate);bad[p.ENTRY]^=1
    rejected(lambda:p.verify_scope(original,bad,guard))
    bad=bytearray(candidate);bad[p.GUARD]^=1
    rejected(lambda:p.verify_scope(original,bad,guard))
    bad=bytearray(candidate);bad[p.RX_HEADER+40]^=1
    rejected(lambda:p.verify_scope(original,bad,guard))
    md=Cs(CS_ARCH_ARM64,CS_MODE_LITTLE_ENDIAN)
    ins=list(md.disasm(guard,p.GUARD))
    expected=['cmp','b.ne','ldr','ldr','fmul','faddp','fmul','fadd','fmov',
              'ubfx','cmp','b.eq','fcmp','b.le','ldr','b','mov','ret']
    assert [i.mnemonic for i in ins]==expected
    assert ins[1].op_str==f'#{hex(p.GUARD+56)}'
    assert ins[11].op_str==f'#{hex(p.GUARD+64)}'
    assert ins[13].op_str==f'#{hex(p.GUARD+64)}'
    assert ins[15].op_str==f'#{hex(p.ENTRY+4)}'
    assert ins[14].op_str=='w10, [x0, #0x1f4]'
    assert ins[16].op_str=='w0, #0'
    assert all('sp' not in i.op_str for i in ins)
    # Filesz/memsz now include the last guard byte but remain below the next
    # segment; no symbol/relocation/build-id or section header was changed.
    ph=struct.unpack_from('<IIQQQQQQ',candidate,p.RX_HEADER)
    assert ph[3]+ph[6]==p.GUARD+72 and ph[2]+ph[5]==p.GUARD+72
    assert ph[3]+ph[6]<0x84000 and ph[1]==5
    print('PASS: hash/layout/scope rejection, exact branch destinations, no SP changes, BTI and all unrelated ELF bytes preserved')


if __name__=='__main__':
    main()
