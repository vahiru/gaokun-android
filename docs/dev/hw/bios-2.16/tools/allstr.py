import re,sys
for path in sys.argv[2:]:
    d=open(path,'rb').read()
    pat=re.compile(sys.argv[1],re.I)
    for m in re.finditer(rb'[\x20-\x7e]{5,}',d):
        s=m.group().decode()
        if pat.search(s): print(path,hex(m.start()),'A',s)
    for m in re.finditer(rb'(?:[\x20-\x7e]\x00){5,}',d):
        s=m.group().decode('utf-16le')
        if pat.search(s): print(path,hex(m.start()),'U',s)
