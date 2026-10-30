#!/usr/bin/env python3
"""Enumerate the shipped Windows client's complete TA wire-command surface.

Reads FtWbioDriverUmdf.dll (SHA-pinned), finds every direct call to the single
QSEECOM transport wrapper, and recovers the command word from the local
register setup.  Offline only; no device, no TA command is executed.
"""
import bisect
import hashlib
import json
import struct
from pathlib import Path

import pefile
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN
from capstone.arm64 import ARM64_OP_IMM, ARM64_OP_REG

HERE = Path(__file__).resolve().parent
DLL = HERE / 'cabs/ftwbio/FtWbioDriverUmdf.dll'
DLL_PIN = '90dfeb2c23523a382f9310c16e6ad5d4ed33247b7868a955521ce7b70a710fb0'
WRAPPER = 0x18000c5a8          # ff_trustlet_RWQSEE(cmd, wbuf, wlen, rbuf, rlen)
TA_EXCHANGE = 0x18000c388      # ff_trustlet_client_exchange_message


def reg(insn, operand):
    name = insn.reg_name(operand.reg)
    return 'x' + name[1:] if name.startswith('w') else name


def imm_written_to(insn):
    """Return (canonical_reg, immediate_or_None, source_reg_or_None) for insn."""
    if not insn.operands or insn.operands[0].type != ARM64_OP_REG:
        return None
    dst = reg(insn, insn.operands[0])
    if insn.mnemonic in ('movz', 'mov') and len(insn.operands) >= 2:
        src = insn.operands[1]
        if src.type == ARM64_OP_IMM:
            return dst, src.imm & 0xffffffff, None
        if src.type == ARM64_OP_REG:
            return dst, None, reg(insn, src)
    if insn.mnemonic == 'movk' and len(insn.operands) >= 2 and insn.operands[1].type == ARM64_OP_IMM:
        return dst, ('movk', insn.operands[1].imm, insn.operands[1].shift.value), None
    return None


def command_at(insns, call_index):
    """Nearest w0/x0 assignment before a call, with one register hop."""
    def lookup(target, start, depth):
        for j in range(start - 1, max(start - 40, -1), -1):
            wr = imm_written_to(insns[j])
            if wr and wr[0] == target:
                if wr[1] is not None:
                    return wr[1]
                if wr[2] is not None and depth:
                    return lookup(wr[2], j, depth - 1)
                return None
            if insns[j].mnemonic in ('bl', 'blr'):
                return None
        return None
    return lookup('x0', call_index, 1)


def main():
    blob = DLL.read_bytes()
    if hashlib.sha256(blob).hexdigest() != DLL_PIN:
        raise SystemExit('unreviewed driver hash')
    pe = pefile.PE(str(DLL))
    base = pe.OPTIONAL_HEADER.ImageBase
    text = next(s for s in pe.sections if s.Name.startswith(b'.text'))
    va = base + text.VirtualAddress
    md = Cs(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN)
    md.detail = True
    md.skipdata = True
    insns = [i for i in md.disasm(text.get_data(), va) if i.id]

    starts = [i.address for i in insns
              if (i.mnemonic == 'stp' and i.op_str.startswith('x29, x30, [sp') and '!' in i.op_str)
              or i.mnemonic == 'paciasp']
    starts.sort()

    def func_of(addr):
        j = bisect.bisect_right(starts, addr) - 1
        return starts[j] if j >= 0 else None

    call_sites = []
    for idx, insn in enumerate(insns):
        if insn.mnemonic != 'bl' or not insn.operands:
            continue
        if insn.operands[0].type != ARM64_OP_IMM or insn.operands[0].imm != WRAPPER:
            continue
        call_sites.append({
            'call_site': hex(insn.address),
            'function': hex(func_of(insn.address)) if func_of(insn.address) else None,
            'command': command_at(insns, idx),
        })

    wrapper_data_refs = [hex(i) for i in range(len(blob))
                         if blob.startswith(struct.pack('<Q', WRAPPER), i)]

    commands = sorted({c['command'] for c in call_sites if isinstance(c['command'], int)})
    report = {
        'dll_sha256': DLL_PIN,
        'transport_wrapper': hex(WRAPPER),
        'ta_exchange': hex(TA_EXCHANGE),
        'direct_wrapper_call_sites': call_sites,
        'commands': [hex(c) for c in commands],
        'wrapper_data_references': wrapper_data_refs,
        'unexpected_command': any(c is None or not 0x8000 <= c <= 0x80ff for c in commands),
        'limitation': ('Direct BL call sites plus raw 8-byte pointer references only. '
                       'Indirect dispatch through the wrapper was not observed; this is not a '
                       'whole-program alias proof.'),
    }
    print(json.dumps(report, indent=2))
    expected = {0x8020, 0x8021, 0x8029, 0x8031}
    if set(commands) != expected or len(call_sites) != 7 or wrapper_data_refs:
        raise SystemExit('command surface changed from the reviewed set')


if __name__ == '__main__':
    main()
