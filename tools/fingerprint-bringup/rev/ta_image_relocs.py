#!/usr/bin/env python3
"""Inventory the fingerpr.mbn load image: PT_LOAD extents, dynamic relocations
and imported symbols.  Pure static parse; no execution and no device access."""
import argparse
import hashlib
import json
import struct
from pathlib import Path

from replay_preprocess import HERE, TA_PIN

R_RELATIVE=1027
R_ABS64=257
R_GLOB_DAT=1025
R_JUMP_SLOT=1026
NAMES={R_RELATIVE:'RELATIVE',R_ABS64:'ABS64',R_GLOB_DAT:'GLOB_DAT',R_JUMP_SLOT:'JUMP_SLOT'}


def parse(blob):
    e_phoff=struct.unpack_from('<Q',blob,0x20)[0]
    e_phentsize=struct.unpack_from('<H',blob,0x36)[0]
    e_phnum=struct.unpack_from('<H',blob,0x38)[0]
    loads=[];dynamic=None
    for i in range(e_phnum):
        p=e_phoff+i*e_phentsize
        typ,flags=struct.unpack_from('<II',blob,p)
        off,vaddr,paddr,filesz,memsz,align=struct.unpack_from('<QQQQQQ',blob,p+8)
        if typ==1:loads.append({'offset':off,'vaddr':vaddr,'filesz':filesz,'memsz':memsz,
                                'flags':flags})
        elif typ==2:dynamic=(off,filesz)
    assert dynamic,'expected PT_DYNAMIC'

    def to_offset(va):
        for seg in loads:
            if seg['vaddr']<=va<seg['vaddr']+seg['filesz']:
                return seg['offset']+(va-seg['vaddr'])
        raise ValueError(f'address {va:#x} is not backed by a PT_LOAD segment')

    tags={}
    for i in range(0,dynamic[1],16):
        tag,val=struct.unpack_from('<QQ',blob,dynamic[0]+i)
        if tag==0:break
        tags.setdefault(tag,[]).append(val)
    rela,relasz,relaent=tags[7][0],tags[8][0],tags[9][0]
    jmprel,pltrelsz=tags[23][0],tags[2][0]
    strtab=tags[5][0]
    symtab,syment=tags[6][0],tags[11][0]

    def symbol_name(index):
        sym=symtab+index*syment
        nameoff=struct.unpack_from('<I',blob,to_offset(sym))[0]
        start=to_offset(strtab)+nameoff
        return blob[start:blob.index(b'\0',start)].decode('latin1')

    relocations=[]
    for base,size in ((rela,relasz),(jmprel,pltrelsz)):
        for i in range(0,size,relaent):
            target,info,addend=struct.unpack_from('<QQq',blob,to_offset(base+i))
            kind=info&0xffffffff;sym=info>>32
            relocations.append({'kind':kind,'target':target,'addend':addend,'symbol':sym,
                                'name':symbol_name(sym) if sym else None})
    return loads,relocations


def inventory(blob):
    loads,relocations=parse(blob)
    counts={}
    for r in relocations:counts[NAMES.get(r['kind'],hex(r['kind']))]=counts.get(
        NAMES.get(r['kind'],hex(r['kind'])),0)+1
    imports={}
    for r in relocations:
        if r['kind'] in (R_ABS64,R_GLOB_DAT,R_JUMP_SLOT) and r['name']:
            imports.setdefault(r['name'],[]).append(
                {'kind':NAMES[r['kind']],'slot':r['target'],'addend':r['addend']})
    return {
        'sha256':hashlib.sha256(blob).hexdigest(),
        'load_segments':loads,
        'relocation_counts':counts,
        'relative_relocations':[{'slot':r['target'],'addend':r['addend']}
                                for r in relocations if r['kind']==R_RELATIVE],
        'imports':dict(sorted(imports.items())),
    }


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ta',type=Path,default=HERE/'cabs/qctree/fingerpr.mbn')
    parser.add_argument('--out',type=Path)
    args=parser.parse_args()
    blob=args.ta.read_bytes()
    assert hashlib.sha256(blob).hexdigest()==TA_PIN,'unreviewed TA image'
    report=inventory(blob)
    text=json.dumps(report,indent=2)+'\n'
    if args.out:args.out.write_text(text)
    else:print(text,end='')


if __name__=='__main__':main()
