"""iat.py <pe> <base hex> [addr hex ...] — имена импортов по адресам слотов IAT (по нашей карте)."""
import sys, pefile
pe = pefile.PE(sys.argv[1]); base = int(sys.argv[2], 16)
m = {}
for d in pe.DIRECTORY_ENTRY_IMPORT:
    for i in d.imports:
        m[i.address - pe.OPTIONAL_HEADER.ImageBase + base] = f"{d.dll.decode()}!{i.name.decode() if i.name else '#%d' % i.ordinal}"
for a in sys.argv[3:]:
    print(a, m.get(int(a, 16), '?'))
