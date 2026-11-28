import sys, os, pefile, datetime
for p in sys.argv[1:]:
    try: pe = pefile.PE(p)
    except Exception as e: print(os.path.basename(p), 'ERR', e); continue
    fh, oh = pe.FILE_HEADER, pe.OPTIONAL_HEADER
    secs = ','.join(s.Name.rstrip(b'\0').decode(errors='replace') for s in pe.sections)
    text = sum(s.Misc_VirtualSize for s in pe.sections if s.Characteristics & 0x20)
    nexp = len(pe.DIRECTORY_ENTRY_EXPORT.symbols) if hasattr(pe, 'DIRECTORY_ENTRY_EXPORT') else 0
    imps = []
    if hasattr(pe, 'DIRECTORY_ENTRY_IMPORT'):
        for d in pe.DIRECTORY_ENTRY_IMPORT:
            imps.append('%s:%d' % (d.dll.decode().lower().replace('.dll',''), len(d.imports)))
    reloc = 'reloc' if hasattr(pe, 'DIRECTORY_ENTRY_BASERELOC') else 'NOREL'
    print('%-22s %s link%d.%d base=%08x code=%7d exp=%5d %s [%s] imps: %s' % (os.path.basename(p), datetime.datetime.utcfromtimestamp(fh.TimeDateStamp).date(), oh.MajorLinkerVersion, oh.MinorLinkerVersion, oh.ImageBase, text, nexp, reloc, secs, ' '.join(imps)))
