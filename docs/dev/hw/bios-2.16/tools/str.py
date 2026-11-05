import re,sys
NOISE=re.compile(r'(SafeString|String\.c|StrSize|AsciiStr|_gPcd_|InternalSafe|Destination|PrintLib|MemLibGuid|DebugLib|UefiLib|HobLib|DxeServicesTableLib|UefiBootServicesTableLib|UefiRuntimeServicesTableLib|ArmArchTimer|MemoryAllocationLib|ZeroMem|Unaligned|IoLibArm|DivU64|BaseLib|^\(|^\*|Lock->|^Warning |^[A-Z][a-z]+( [A-Za-z]+)*$)')
def run(path,minlen=6,all_=False):
    d=open(path,'rb').read()
    out=[]
    for m in re.finditer(rb'[\x20-\x7e]{%d,}'%minlen,d): out.append((m.start(),'A',m.group().decode()))
    for m in re.finditer(rb'(?:[\x20-\x7e]\x00){%d,}'%minlen,d): out.append((m.start(),'U',m.group().decode('utf-16le')))
    out.sort()
    for o,t,s in out:
        if not all_ and NOISE.search(s): continue
        print(f'{o:08x} {t} {s}')
if __name__=='__main__':
    for p in sys.argv[1:]:
        print('=====',p); run(p)
