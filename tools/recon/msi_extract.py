"""Minimal MSI + external MSZIP CAB extractor (enough for InstallShield 'Shrek 2.msi').
Usage: python -I msi_extract.py <Shrek 2.msi> <dir with MainApp.cab> <outdir>  (needs olefile)"""
import sys, os, struct, zlib, olefile

def decode_name(n):
    out = []
    for ch in n:
        c = ord(ch)
        if 0x3800 <= c < 0x4800:
            c -= 0x3800; out.append(_ch(c & 0x3f)); out.append(_ch(c >> 6))
        elif 0x4800 <= c < 0x4840:
            out.append(_ch(c - 0x4800))
        elif c == 0x4840:
            out.append('!')
        else:
            out.append(ch)
    return ''.join(out)
def _ch(x):
    return '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz._'[x]

class MSI:
    def __init__(self, path):
        self.ole = olefile.OleFileIO(path)
        self.streams = {decode_name(e[0]): e[0] for e in self.ole.listdir()}
        pool = self.raw('!_StringPool'); data = self.raw('!_StringData')
        hdr = struct.unpack_from('<I', pool, 0)[0]
        self.codepage = hdr & 0xffff
        self.sref = 3 if hdr & 0x80000000 else 2
        enc = 'cp%d' % self.codepage if self.codepage else 'cp1252'
        self.strings = ['']
        off = 0; i = 4
        while i < len(pool):
            ln, rc = struct.unpack_from('<HH', pool, i); i += 4
            if ln == 0 and rc != 0:
                ln = struct.unpack_from('<I', pool, i)[0]; i += 4  # long string
            self.strings.append(data[off:off+ln].decode(enc, 'replace')); off += ln
        cols = self.table_raw('_Columns', [('s',), ('i',2), ('s',), ('i',2)])
        self.schema = {}
        for t, num, name, typ in cols:
            num ^= 0x8000; typ ^= 0x8000
            self.schema.setdefault(t, []).append((num, name, typ))
    def raw(self, name):
        return self.ole.openstream(self.streams[name]).read()
    def table_raw(self, name, coltypes):
        key = '!' + name
        if key not in self.streams: return []
        d = self.raw(key)
        widths = [self.sref if c[0] == 's' else c[1] for c in coltypes]
        rows = len(d) // sum(widths)
        colsdata = []; off = 0
        for c, w in zip(coltypes, widths):
            vals = []
            for r in range(rows):
                b = d[off + r*w: off + r*w + w]; v = int.from_bytes(b, 'little')
                vals.append(self.strings[v] if c[0] == 's' and v < len(self.strings) else (None if c[0]=='s' else v))
            colsdata.append(vals); off += rows * w
        return list(zip(*colsdata))
    def table(self, name):
        cols = sorted(self.schema[name])
        ct = []
        for num, cname, typ in cols:
            if typ & 0x4000: continue
            ct.append(('s',) if typ & 0x0800 else ('i', 4 if (typ & 0xff) == 4 else 2))
        rows = self.table_raw(name, ct)
        names = [c[1] for c in cols if not c[2] & 0x4000]
        out = []
        for r in rows:
            rec = {}
            for n, (k, v) in zip(names, zip(ct, r)):
                if k[0] == 'i':
                    v = None if v == 0 else v ^ (0x80000000 if k[1] == 4 else 0x8000)
                rec[n] = v
            out.append(rec)
        return out

def long_name(s):
    return s.split('|')[-1] if s else s

def dir_paths(msi):
    dirs = {d['Directory']: d for d in msi.table('Directory')}
    cache = {}
    def resolve(k):
        if k in cache: return cache[k]
        d = dirs[k]; dd = d['DefaultDir'].split(':')[0]
        name = long_name(dd)
        par = d['Directory_Parent']
        if not par or par == k:
            p = ''  # TARGETDIR root
        else:
            p = resolve(par)
            if name and name != '.': p = os.path.join(p, name)
        cache[k] = p; return p
    for k in dirs: resolve(k)
    return cache

def cab_extract(cabpath, wanted, outmap):
    f = open(cabpath, 'rb'); h = f.read(36)
    sig, _, cb, _, coffFiles, _, vmin, vmaj, nfold, nfile, flags, setid, icab = struct.unpack('<4sIIIIIBBHHHHH', h)
    assert sig == b'MSCF' and flags == 0, flags
    folders = [struct.unpack('<IHH', f.read(8)) for _ in range(nfold)]
    f.seek(coffFiles); files = []
    for _ in range(nfile):
        size, off, ifold, date, time, attr = struct.unpack('<IIHHHH', f.read(16))
        name = b''
        while True:
            c = f.read(1)
            if c == b'\0': break
            name += c
        files.append((name.decode('latin1'), size, off, ifold))
    byfold = {}
    for fl in files: byfold.setdefault(fl[3], []).append(fl)
    done = 0
    for fi, (coff, nblocks, typ) in enumerate(folders):
        if fi not in byfold: continue
        assert typ & 0xf == 1, typ
        f.seek(coff); buf = bytearray(); hist = b''
        for _ in range(nblocks):
            csum, cbd, cbu = struct.unpack('<IHH', f.read(8))
            blk = f.read(cbd); assert blk[:2] == b'CK'
            dec = zlib.decompressobj(-15, zdict=hist) if hist else zlib.decompressobj(-15)
            out = dec.decompress(blk[2:]); assert len(out) == cbu, (len(out), cbu)
            hist = (hist + out)[-32768:]; buf += out
        for name, size, off, _ in byfold[fi]:
            if name not in outmap: print('  [skip, no mapping]', name); continue
            dst = outmap[name]; os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, 'wb') as o: o.write(buf[off:off+size])
            done += 1
    return done

if __name__ == '__main__':
    msipath, srcdir, outdir = sys.argv[1:4]
    m = MSI(msipath)
    print('tables:', sorted(m.schema))
    dp = dir_paths(m)
    comps = {c['Component']: c for c in m.table('Component')}
    media = m.table('Media'); print('media:', media)
    outmap = {}
    for fr in m.table('File'):
        d = dp[comps[fr['Component_']]['Directory_']]
        outmap[fr['File']] = os.path.join(outdir, d, long_name(fr['FileName']))
    print('files in File table:', len(outmap))
    total = 0
    for md in media:
        cab = md.get('Cabinet')
        if cab and not cab.startswith('#'):
            total += cab_extract(os.path.join(srcdir, cab), None, outmap)
    print('extracted', total)
