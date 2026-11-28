"""Extract a zip whose entry names are Cyrillic (CP866) safely: python3 -I unzip_zip.py <zip> <outdir>.
Python >= 3.11 decodes the legacy names itself; we only guard against path traversal."""
import sys, zipfile, os
src, dst = sys.argv[1], os.path.abspath(sys.argv[2])
with zipfile.ZipFile(src) as z:
    for info in z.infolist():
        out = os.path.normpath(os.path.join(dst, info.filename))
        assert out.startswith(dst), info.filename
        if info.filename.endswith('/'):
            os.makedirs(out, exist_ok=True); continue
        os.makedirs(os.path.dirname(out), exist_ok=True)
        with z.open(info) as f, open(out, 'wb') as o:
            while b := f.read(1 << 20):
                o.write(b)
        print(info.filename)
