import struct,sys,re
from capstone import *
from capstone.arm64 import *
class PE:
    def __init__(s,path):
        d=s.d=open(path,'rb').read()
        e=struct.unpack('<I',d[0x3c:0x40])[0]
        assert d[e:e+4]==b'PE\0\0'
        nsec=struct.unpack('<H',d[e+6:e+8])[0]; ohs=struct.unpack('<H',d[e+20:e+22])[0]
        opt=e+24
        s.entry=struct.unpack('<I',d[opt+16:opt+20])[0]
        s.secs=[]
        for i in range(nsec):
            o=opt+ohs+40*i
            name=d[o:o+8].rstrip(b'\0').decode(); vsz,va,rsz,rptr=struct.unpack('<IIII',d[o+8:o+24])
            s.secs.append((name,va,vsz,rptr,rsz))
        s.md=Cs(CS_ARCH_ARM64,CS_MODE_ARM); s.md.detail=True
    def off2rva(s,off):
        for n,va,vsz,rp,rs in s.secs:
            if rp<=off<rp+rs: return va+off-rp
    def rva2off(s,rva):
        for n,va,vsz,rp,rs in s.secs:
            if va<=rva<va+max(vsz,rs): return rp+rva-va
    def text(s):
        for n,va,vsz,rp,rs in s.secs:
            if n=='.text': return va,s.d[rp:rp+rs]
    def xrefs(s,target_rva):
        """find adrp+add pairs computing target"""
        va,code=s.text(); res=[]
        for i in range(0,len(code)-4,4):
            w=struct.unpack('<I',code[i:i+4])[0]
            if (w & 0x9f000000)==0x90000000:  # adrp
                rd=w&31; immlo=(w>>29)&3; immhi=(w>>5)&0x7ffff
                imm=((immhi<<2)|immlo)<<12
                if imm & (1<<32): imm-= (1<<33)
                pc=va+i; page=(pc & ~0xfff)+imm
                if page!=(target_rva & ~0xfff): continue
                for j in range(i+4,min(i+40,len(code)-4),4):
                    w2=struct.unpack('<I',code[j:j+4])[0]
                    if (w2 & 0xffc00000)==0x91000000 and ((w2>>5)&31)==rd:
                        if page+((w2>>10)&0xfff)==target_rva: res.append(va+i)
                        break
                    if (w2 & 0xffc00000)==0xf9400000 and ((w2>>5)&31)==rd:  # ldr x, [xn, #imm]
                        if page+((w2>>10)&0xfff)*8==target_rva: res.append(va+i)
                        break
        return res
    def find_str(s,txt,utf16=False):
        b=txt.encode('utf-16le') if utf16 else txt.encode()
        return [s.off2rva(m.start()) for m in re.finditer(re.escape(b),s.d) if m.start()==0 or s.d[m.start()-1]==0]
    def func_start(s,rva):
        va,code=s.text(); i=rva-va
        while i>0:
            w=struct.unpack('<I',code[i:i+4])[0]
            # stp x29,x30,[sp,#-N]! or sub sp,sp
            if (w & 0xffc07fff)==0xa9807bfd or (w&0xff8003ff)==0xd10003ff and False: 
                # look further back for sub sp / stp x2x pre-index
                k=i
                for b in range(1,8):
                    w0=struct.unpack('<I',code[k-4*b:k-4*b+4])[0]
                    if (w0 & 0xffc003e0)==0xa98003e0 or (w0&0xff0003ff)==0xd10003ff: i=k-4*b
                    else: break
                return va+i
            if (w & 0xffc003e0)==0xa98003e0 :  # stp xA,xB,[sp,#-N]!
                return va+i
            if (w & 0xffc003ff)==0xd10003ff and i>0:  # sub sp,sp,#
                prev=struct.unpack('<I',code[i-4:i])[0]
                if prev in (0xd65f03c0,) or (prev&0xfc000000)==0x14000000: return va+i
            i-=4
        return va
    def dis(s,rva,n=200,stop_ret=True):
        va,code=s.text(); i=rva-va; out=[]
        for ins in s.md.disasm(code[i:i+4*n],rva):
            out.append(ins)
            if stop_ret and ins.mnemonic=='ret' : break
        return out
    def cstr(s,rva):
        o=s.rva2off(rva)
        if o is None: return None
        e=s.d.find(b'\0',o); t=s.d[o:e]
        if len(t)>=3 and all(32<=c<127 or c in (9,10) for c in t): return t.decode()
        # utf16?
        e2=o
        while e2+1<len(s.d) and s.d[e2:e2+2]!=b'\0\0': e2+=2
        try:
            t2=s.d[o:e2].decode('utf-16le')
            if len(t2)>=3 and t2.isprintable(): return 'L"'+t2+'"'
        except: pass
        return None
    def annotate(s,inss):
        regs={}; lines=[]
        for ins in inss:
            c=''
            if ins.mnemonic=='adrp':
                regs[ins.operands[0].reg]=ins.operands[1].imm
            elif ins.mnemonic=='add' and len(ins.operands)==3 and ins.operands[2].type==ARM64_OP_IMM and ins.operands[1].reg in regs:
                t=regs[ins.operands[1].reg]+ins.operands[2].imm; regs[ins.operands[0].reg]=t
                st=s.cstr(t); c=f'  ; ={t:#x}'+(f' "{st}"' if st else '')
                g=s.guid_at(t)
                if g: c+=' GUID '+g
            elif ins.mnemonic=='ldr' and len(ins.operands)==2 and ins.operands[1].type==ARM64_OP_MEM and ins.operands[1].mem.base in regs:
                t=regs[ins.operands[1].mem.base]+ins.operands[1].mem.disp; c=f'  ; [{t:#x}]'
                g=s.guid_at(t)
                if g: c+=' GUID '+g
            lines.append(f'{ins.address:08x}: {ins.mnemonic:8s} {ins.op_str}{c}')
        return lines
    gdb=None
    def guid_at(s,rva):
        import uuid,json,os
        if PE.gdb is None:
            PE.gdb=json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)),'guiddb.json')))
        o=s.rva2off(rva)
        if o is None or o+16>len(s.d): return None
        g=str(uuid.UUID(bytes_le=s.d[o:o+16]))
        v=PE.gdb.get(g)
        return (v[0][0] if v else None) and f'{v[0][0]}({g})'
if __name__=='__main__':
    p=PE(sys.argv[1]); cmd=sys.argv[2]
    if cmd=='xs':  # xrefs to string
        for r in p.find_str(sys.argv[3], len(sys.argv)>4):
            for x in p.xrefs(r): print(f'str@{r:#x} xref {x:#x} func {p.func_start(x):#x}')
    elif cmd=='dis':
        a=int(sys.argv[3],16); n=int(sys.argv[4]) if len(sys.argv)>4 else 200
        print('\n'.join(p.annotate(p.dis(a,n,stop_ret=len(sys.argv)<=5))))
    elif cmd=='sec':
        print(p.secs, hex(p.entry))
