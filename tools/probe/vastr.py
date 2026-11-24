"""vastr.py <pe> <base hex> <va hex...> — строки (ASCII/UTF-16) по виртуальным адресам образа."""
import struct, sys
d = open(sys.argv[1], 'rb').read()
base = int(sys.argv[2], 16)
pe = struct.unpack_from('<I', d, 0x3c)[0]
nsec = struct.unpack_from('<H', d, pe + 6)[0]
opt = struct.unpack_from('<H', d, pe + 20)[0]
secs = []
for i in range(nsec):
    o = pe + 24 + opt + 40 * i
    vsz, va, rsz, raw = struct.unpack_from('<IIII', d, o + 8)
    secs.append((va, max(vsz, rsz), raw))
for a in sys.argv[3:]:
    rva = int(a, 16) - base
    for va, sz, raw in secs:
        if va <= rva < va + sz:
            off = raw + rva - va
            s = d[off:off + 200]
            if len(s) > 1 and s[1] == 0:
                t = s.decode('utf-16le', 'replace').split('\0')[0]
            else:
                t = s.split(b'\0')[0].decode('latin1')
            print(a, repr(t))
