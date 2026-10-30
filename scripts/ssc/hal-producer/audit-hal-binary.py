#!/usr/bin/env python3
"""Read-only assertions on the exact deployed HOS HAL and its mini-debug symbols.
No ADB, service control, firmware or sensor access. Prints code facts, not samples.
"""
import argparse
import hashlib
import json
import lzma
from pathlib import Path
import struct
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN

SHA = 'ffc36303c168796322ec5483cd73bea7abc111b208ab1c98bdcd99725850687e'
DEFAULT = Path.home()/'.cache/hos-r19-fusion-probe/producer-audit/hos-vendor-r4-sensors-hal'


def elf_sections(data):
    if data[:6] != b'\x7fELF\x02\x01': raise ValueError('expected little-endian ELF64')
    offset=struct.unpack_from('<Q',data,40)[0]
    size,count,strings=struct.unpack_from('<HHH',data,58)
    if size != 64: raise ValueError('unexpected ELF section layout')
    headers=[struct.unpack_from('<IIQQQQIIQQ',data,offset+i*size) for i in range(count)]
    h=headers[strings];names=data[h[4]:h[4]+h[5]]
    return [(names[h[0]:].split(b'\0',1)[0].decode(),h) for h in headers]


def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('binary',type=Path,nargs='?',default=DEFAULT);a=ap.parse_args()
    data=a.binary.read_bytes()
    if hashlib.sha256(data).hexdigest()!=SHA: raise ValueError('HAL SHA differs from reviewed/deployed binary')
    sections=elf_sections(data)
    mini_hdr=next(h for name,h in sections if name=='.gnu_debugdata')
    mini=lzma.decompress(data[mini_hdr[4]:mini_hdr[4]+mini_hdr[5]])
    mini_sections=elf_sections(mini);symbols={}
    for name,h in mini_sections:
        if h[1]!=2: continue
        strings_hdr=mini_sections[h[6]][1]
        strings=mini[strings_hdr[4]:strings_hdr[4]+strings_hdr[5]]
        for offset in range(h[4],h[4]+h[5],h[9]):
            n,info,other,index,value,size=struct.unpack_from('<IBBHQQ',mini,offset)
            symbols[strings[n:].split(b'\0',1)[0].decode()]=(value,size)
    def symbol(fragment):
        matches=[(n,v) for n,v in symbols.items() if fragment in n]
        if len(matches)!=1: raise ValueError(('ambiguous/missing symbol',fragment,matches))
        return matches[0]
    wanted={
        'readEvents':'6Sensor10readEventsEv',
        'timerThread':'6Sensor11startThreadEPS3_',
        'accelPayload':'11AccelSensor16readEventPayload',
        'gyroPayload':'10GyroSensor16readEventPayload',
        'hubAccel':'6SscHub5AccelEv',
        'hubGyro':'6SscHub4GyroEv',
        'hubGet':'6SscHub3GetEv',
        'clock':'elapsedRealtimeNanoEv',
    }
    # Clang may use a different substitution index in startThread's mangling;
    # use its unique name component when the exact substitution is absent.
    wanted['timerThread']='6Sensor11startThread'
    functions={key:symbol(fragment) for key,fragment in wanted.items()}
    md=Cs(CS_ARCH_ARM64,CS_MODE_LITTLE_ENDIAN)
    instructions={}
    for _,(start,size) in functions.values():
        name,h=next((n,h) for n,h in sections if h[1]!=8 and h[3]<=start and start+size<=h[3]+h[5])
        off=h[4]+start-h[3]
        instructions.update({i.address:i for i in md.disasm(data[off:off+size],start)})
    verified=[]
    def check(addr,mnemonic,operands):
        i=instructions[addr]
        if (i.mnemonic,i.op_str)!=(mnemonic,operands):
            raise ValueError(f'unexpected instruction{addr:#x}: {i.mnemonic} {i.op_str}')
        verified.append({'address':hex(addr),'instruction':i.mnemonic+' '+i.op_str})
    assert functions['readEvents'][1]==(0x1eb6c,0xcc)
    assert functions['accelPayload'][1]==(0x1eec4,0x88)
    assert functions['gyroPayload'][1]==(0x1f068,0x88)
    check(0x1eba0,'bl','#'+hex(functions['clock'][1][0]))
    check(0x1ebd4,'blr','x8') # void payload callback; no presence return value.
    check(0x1ebd8,'mov','w0, #0x60')
    check(0x1ebe8,'add','x8, x0, #0x60')
    check(0x1ebf0,'stp','x8, x8, [x19, #8]') # one96B event end/capacity.
    for key,hub in [('accelPayload','hubAccel'),('gyroPayload','hubGyro')]:
        start=functions[key][1][0]
        check(start+0x28,'bl','#'+hex(functions['hubGet'][1][0]))
        check(start+0x30,'bl','#'+hex(functions[hub][1][0]))
        check(start+0x38,'ldrb','w9, [sp, #0x10]') # Sample.valid.
        check(start+0x4c,'str','d0, [x19, #8]') # xyz copied irrespective ofvalid.
        check(start+0x54,'str','s1, [x19, #0x10]')
        check(start+0x58,'tst','w9, w10')
        check(start+0x5c,'csel','w8, w8, wzr, ne') # valid only selectsaccuracy.
        check(start+0x60,'strb','w8, [x19, #0x14]')
    check(0x1f448,'stp','xzr, xzr, [x19, #0x78]') # initialAccel v+accuracy zeros.
    check(0x1f44c,'strb','wzr, [x19, #0x88]') # initialAccel valid=false.
    check(0x1f55c,'stp','xzr, xzr, [x20]') # expiredFresh zeroSample.
    check(0x1f560,'str','wzr, [x20, #0x10]')
    check(0x1f630,'stp','xzr, xzr, [x20]') # gyro sameexpiry behavior.
    check(0x1f634,'str','wzr, [x20, #0x10]')
    check(0x1e834,'mov','w0, #7') # CLOCK_BOOTTIME.
    check(0x1e84c,'ldp','x26, x8, [x19, #0x10]') # period/lasttick.
    check(0x1e850,'add','x8, x26, x8')
    check(0x1e858,'b.lt','#0x1e8b0')
    check(0x1e860,'str','x25, [x19, #0x18]') # lasttick=now.
    check(0x1e874,'blr','x9') # virtualreadEvents.
    check(0x1e890,'blr','x8') # callbackpostEvents.
    print(json.dumps({'binary_sha256':SHA,'mini_debug_sha256':hashlib.sha256(mini).hexdigest(),
        'functions':{k:{'symbol':n,'address':hex(v[0]),'size':v[1]} for k,(n,v) in functions.items()},
        'verified_instructions':verified,
        'finding':'Absent/expired SSC cache haszero payload; periodic event builder emits it with new timestamp; validity only affects accuracy.',
        'limit':'Confirms deployed binary behavior and matching inspected source functions, not whole historical source/build reproducibility.'},indent=2))


if __name__=='__main__':main()
