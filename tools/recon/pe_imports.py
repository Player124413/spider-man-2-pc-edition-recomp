"""Dump imports of PE files grouped by module (non-UE DLLs only by default)."""
import sys, os, pefile
UE = {'core.dll', 'engine.dll', 'window.dll', 'windrv.dll', 'fire.dll', 'gui.dll', 'kwgame.dll', 'ipdrv.dll', 'd3ddrv.dll', 'alaudio.dll', 'editor.dll'}
show_ue = '--all' in sys.argv
for p in [a for a in sys.argv[1:] if not a.startswith('--')]:
    pe = pefile.PE(p)
    print('==', os.path.basename(p))
    for d in getattr(pe, 'DIRECTORY_ENTRY_IMPORT', []):
        dll = d.dll.decode().lower()
        if dll in UE and not show_ue:
            print('   %-14s %d symbols (UE module)' % (dll, len(d.imports))); continue
        names = sorted(i.name.decode() if i.name else '#%d' % i.ordinal for i in d.imports)
        print('   %-14s %s' % (dll, ' '.join(names)))
