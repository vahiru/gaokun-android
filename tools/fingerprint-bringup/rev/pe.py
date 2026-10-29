import struct

def pe(path):
    data = open(path, 'rb').read()
    e = struct.unpack_from('<I', data, 0x3c)[0]
    nsec = struct.unpack_from('<H', data, e + 6)[0]
    optsz = struct.unpack_from('<H', data, e + 20)[0]
    opt = e + 24
    ib = struct.unpack_from('<Q', data, opt + 24)[0]
    secs = []
    st = opt + optsz
    for i in range(nsec):
        o = st + i * 40
        name = data[o:o+8].rstrip(b'\0').decode('latin1')
        vsz, va, rawsz, rawptr = struct.unpack_from('<IIII', data, o + 8)
        ch = struct.unpack_from('<I', data, o + 36)[0]
        secs.append((name, ib + va, rawptr, rawsz, ch))
    return data, ib, secs

def va_of(secs, off):
    for name, sva, rawptr, rawsz, ch in secs:
        if rawptr <= off < rawptr + rawsz:
            return sva + (off - rawptr), name
    return None, '?'

def off_of(secs, va):
    for name, sva, rawptr, rawsz, ch in secs:
        if sva <= va < sva + rawsz:
            return rawptr + (va - sva), name
    return None, '?'
