"""Convert a raw MODE1/2352 CloneCD .img into a 2048-byte/sector ISO: python3 -I img2iso.py <img> <iso>"""
import sys
src, dst = sys.argv[1], sys.argv[2]
with open(src,'rb') as f, open(dst,'wb') as o:
    n=0
    while True:
        s=f.read(2352)
        if len(s)<2352: break
        assert s[:12]==b'\x00'+b'\xff'*10+b'\x00', n
        assert s[15]==1, (n, s[15])
        o.write(s[16:16+2048]); n+=1
print(n,'sectors')
