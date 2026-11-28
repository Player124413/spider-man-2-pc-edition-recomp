"""fdis.py <game/System> <module> <entry hex> [from hex] [n] — дизассемблер функции из discovery."""
import os, sys
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "recomp"))
from main import load_program
game, modname, e = sys.argv[1], sys.argv[2].lower(), int(sys.argv[3], 16)
prog = load_program(game, [modname])
mod = prog.modules[modname]
mod.run()
f = mod.functions.get(e)
if f is None:
    owners = [x for x, ff in mod.functions.items() if e in ff.insns]
    print("not an entry; owned by", [hex(x) for x in owners])
    f = mod.functions[owners[0]]
lo = int(sys.argv[4], 16) if len(sys.argv) > 4 else 0
n = int(sys.argv[5]) if len(sys.argv) > 5 else 400
callers = [x for x, ff in mod.functions.items() if f.entry in ff.calls]
print(f"entry {f.entry:08x} strong={f.entry in mod.strong} weak={f.entry in mod.weak} insns={len(f.insns)} callers={[hex(c) for c in callers[:8]]}")
k = 0
for a in f.insns:
    if a < lo: continue
    i = mod.insns[a]
    exp = prog.export_name_at(a)
    print(f"{a:08x}: {i.text}" + (f"   ; {exp}" if exp else ""))
    k += 1
    if k >= n: break
