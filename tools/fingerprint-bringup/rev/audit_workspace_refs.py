#!/usr/bin/env python3
"""Bounded offline address-reference audit; not a whole-program alias proof."""
import argparse
import hashlib
import json
import struct
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN
from capstone.arm64 import ARM64_OP_IMM, ARM64_OP_MEM, ARM64_OP_REG
from replay_preprocess import HERE, TA_PIN


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target',type=lambda s:int(s,0),default=0x2a4638)
    args=parser.parse_args()
    target=args.target
    blob=(HERE/'cabs/qctree/fingerpr.mbn').read_bytes()
    assert hashlib.sha256(blob).hexdigest()==TA_PIN
    header=struct.unpack_from('<16sHHIQQQIHHHHHH',blob)
    segments=[struct.unpack_from('<IIQQQQQQ',blob,header[5]+i*header[9])
              for i in range(header[10])]
    dynamic=next(s for s in segments if s[0]==2)
    tags={}
    for offset in range(dynamic[2],dynamic[2]+dynamic[5],16):
        tag,value=struct.unpack_from('<qQ',blob,offset)
        if not tag:break
        tags[tag]=value
    relocations={}
    nearby=[]
    for tag,size_tag in ((7,8),(23,2)):
        for offset in range(tags[tag]+0x3000,tags[tag]+0x3000+tags[size_tag],24):
            address,info,addend=struct.unpack_from('<QQq',blob,offset)
            if info&0xffffffff==1027:
                relocations[address]=addend
                if target-0x200<=addend<target+0x200 or target-8<=address<target+8:
                    nearby.append({'location':hex(address),'addend':hex(addend)})
    md=Cs(CS_ARCH_ARM64,CS_MODE_LITTLE_ENDIAN);md.detail=True;md.skipdata=True
    def register(insn,reg):
        name=insn.reg_name(reg)
        return 'x'+name[1:] if name.startswith('w') else name
    hits=[];address_takes=[];page_seeds=[]
    for segment in segments:
        if segment[0]!=1 or not segment[1]&1:continue
        known={}
        for insn in md.disasm(blob[segment[2]:segment[2]+segment[5]],segment[3]):
            if not insn.id:known.clear();continue
            operands=insn.operands
            old=known.copy()
            for operand in operands:
                if operand.type!=ARM64_OP_MEM:continue
                mem=operand.mem
                base=old.get(register(insn,mem.base))
                if base is None or mem.index:continue
                address=base+mem.disp
                if target<=address<target+8 or (insn.mnemonic.startswith('st') and target-8<=address<target):
                    hits.append({'pc':hex(insn.address),'instruction':insn.mnemonic+' '+insn.op_str,
                                 'effective_address':hex(address),
                                 'access':'store' if insn.mnemonic.startswith('st') else 'load'})
            _,writes=insn.regs_access()
            for reg in writes:known.pop(register(insn,reg),None)
            value=None
            if insn.mnemonic in ('adrp','adr'):
                value=operands[1].imm
                if value==target&~4095:page_seeds.append(hex(insn.address))
            elif insn.mnemonic=='mov' and len(operands)==2:
                if operands[1].type==ARM64_OP_REG:value=old.get(register(insn,operands[1].reg))
                elif operands[1].type==ARM64_OP_IMM:value=operands[1].imm
            elif insn.mnemonic in ('add','sub') and len(operands)==3 and operands[2].type==ARM64_OP_IMM:
                base=old.get(register(insn,operands[1].reg))
                if base is not None:
                    delta=operands[2].imm<<operands[2].shift.value
                    value=base+(delta if insn.mnemonic=='add' else -delta)
            elif insn.mnemonic=='ldr' and len(operands)==2 and operands[1].type==ARM64_OP_MEM:
                mem=operands[1].mem;base=old.get(register(insn,mem.base))
                if base is not None and not mem.index:value=relocations.get(base+mem.disp)
            if value is not None and operands[0].type==ARM64_OP_REG:
                known[register(insn,operands[0].reg)]=value
                if target<=value<target+8:
                    address_takes.append({'pc':hex(insn.address),'instruction':insn.mnemonic+' '+insn.op_str})
            if insn.mnemonic in ('bl','blr'):
                for i in range(19):known.pop('x'+str(i),None)
            elif insn.mnemonic in ('b','br','ret') or insn.mnemonic.startswith('b.') or insn.mnemonic in ('cbz','cbnz','tbz','tbnz'):
                known.clear()
    print(json.dumps({'ta_sha256':TA_PIN,'target':hex(target),
        'direct_references_with_local_constant_propagation':hits,
        'address_constructions':address_takes,'nearby_relative_relocations':nearby,
        'page_seed_count':len(page_seeds),'page_seeds':page_seeds,
        'limitation':'Linear local tracking only; clears at branches, follows RELATIVE GOT loads. No full CFG, interprocedural alias, variable-index or range-store proof.'},indent=2))


if __name__=='__main__':main()
