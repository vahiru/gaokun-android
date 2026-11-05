import sys,struct
sys.path.insert(0,__import__('os').path.dirname(__file__))
from pe import PE
from capstone.arm64 import *
p=PE(sys.argv[1])
va,code=p.text()
p.md.skipdata=True
ins=list(p.md.disasm(code,va))
print(len(ins),file=sys.stderr)
targets={}
for name in sys.argv[2:]:
    for u in (True,False):
        for r in p.find_str(name,u): targets[r]=name
regs={}
out=[]
for k,i in enumerate(ins):
    m=i.mnemonic
    if m=='adrp':
        regs[i.operands[0].reg]=i.operands[1].imm; continue
    if m=='add' and len(i.operands)==3 and i.operands[2].type==ARM64_OP_IMM and i.operands[1].reg in regs:
        t=regs[i.operands[1].reg]+i.operands[2].imm
        if t in targets: out.append((i.address,targets[t]))
    if m in('b','ret','br') : regs={}
    if m.startswith('bl'): 
        for r in list(regs):
            if r not in (ARM64_REG_X19,ARM64_REG_X20,ARM64_REG_X21,ARM64_REG_X22,ARM64_REG_X23,ARM64_REG_X24,ARM64_REG_X25,ARM64_REG_X26,ARM64_REG_X27,ARM64_REG_X28): regs.pop(r)
for a,n in out: print(hex(a),hex(p.func_start(a)),n)
