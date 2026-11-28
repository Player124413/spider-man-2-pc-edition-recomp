"""Каталог экспортов модулей с адресами по карте recomp/addrmap.json и демэнглингом llvm-undname.

    python3 -I tools/recon/exports.py --game game/System --undname tools/llvm/bin/llvm-undname --out build/kb/exports

Пишет <out>/<module>.tsv (адрес, ординал, имя, демэнгл) и печатает сводку по классам.
Вывод — производное от бинарников игры: только в build/, не в git.
"""
import argparse
import collections
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "recomp"))

from image import load_addrmap, load_image  # noqa: E402


def demangle(undname, names):
    """{mangled: demangled} для имён MSVC (начинаются с '?')."""
    todo = [n for n in names if n.startswith("?")]
    if not todo:
        return {}
    p = subprocess.run([undname], input="\n".join(todo) + "\n", capture_output=True, text=True, check=False)
    out = {}
    for group in p.stdout.split("\n\n"):
        lines = group.strip("\n").split("\n")
        if len(lines) >= 2 and lines[0].startswith("?"):
            out[lines[0]] = lines[1]
    return out


CLASS_RE = re.compile(r"(?:__thiscall|__cdecl|__stdcall) ([A-Za-z_][A-Za-z0-9_]*)::")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", required=True)
    ap.add_argument("--undname", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    for name, base in load_addrmap().items():
        img = load_image(os.path.join(args.game, name), base)
        by_va = collections.defaultdict(list)
        for n, va in img.exports.items():
            by_va[va].append(n)
        ords = {va: o for o, va in img.export_ordinals.items()}
        dm = demangle(args.undname, list(img.exports))
        classes = collections.Counter()
        with open(os.path.join(args.out, name.lower().replace(".", "_") + ".tsv"), "w") as f:
            f.write("addr\tordinal\tname\tdemangled\n")
            for va in sorted(by_va):
                for n in sorted(by_va[va]):
                    d = dm.get(n, "")
                    if m := CLASS_RE.search(d):
                        classes[m.group(1)] += 1
                    f.write(f"{va:08x}\t{ords.get(va, '')}\t{n}\t{d}\n")
        top = ", ".join(f"{c} {k}" for c, k in classes.most_common(8))
        print(f"{name:12s} exports={len(img.exports):6d} demangled={len(dm):6d} classes={len(classes):5d}  {top}")


if __name__ == "__main__":
    main()
