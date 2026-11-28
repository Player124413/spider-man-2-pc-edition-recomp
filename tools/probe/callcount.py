"""Самые вызываемые функции гостя по счётчикам входов (SNX_CALLSTATS=<файл>, сборка с SNX_TRACE): доля входов,
длина, лист ли, встраивается ли уже (fi_), почему нет. Входы встроенных копий тоже считаются (ENTER в теле fi_).

    tools/.venv/bin/python -I tools/probe/callcount.py --game game/System --kb build/kb/exports calls.txt [--n 60]
"""
import argparse
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "recomp"))
sys.path.insert(0, os.path.join(ROOT, "tools", "probe"))

from discover import Module, Program  # noqa: E402
from gen import Ctx, INLINE_MAX  # noqa: E402
from image import load_addrmap, load_image  # noqa: E402
from overrides import resolve as resolve_overrides  # noqa: E402
from prof import load_names  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", required=True)
    ap.add_argument("--kb", default=os.path.join(ROOT, "build", "kb", "exports"))
    ap.add_argument("--n", type=int, default=60)
    ap.add_argument("counts")
    args = ap.parse_args()
    counts = {}
    for ln in open(args.counts):
        a, n = ln.split()
        counts[int(a, 16)] = int(n)
    total = sum(counts.values())
    amap = load_addrmap()
    images = {n.lower(): load_image(os.path.join(args.game, n), b) for n, b in amap.items()}
    prog = Program(images)
    for name in images:
        prog.modules[name] = Module(images[name], prog)
    for mod in prog.modules.values():
        mod.run()
    ctx = Ctx(prog, resolve_overrides(prog))
    names = load_names(args.game, args.kb)
    cum = 0
    print(f"{total} входов; длина — инструкций x86, inl — уже встраивается (≤{INLINE_MAX}, лист)")
    for a, n in sorted(counts.items(), key=lambda kv: -kv[1])[:args.n]:
        cum += n
        mod = ctx.module_for(a)
        f = mod.functions.get(a) if mod is not None else None
        if f is None:
            desc = "?"
        else:
            why = []
            if f.calls:
                why.append(f"calls {len(f.calls)}")
            if f.tail_targets:
                why.append("tail")
            if f.unknown_indirect:
                why.append("ijmp")
            inl = ctx.inline_body(a) is not None
            desc = f"{len(f.insns):4d} {'inl' if inl else '   '} {' '.join(why)}"
        print(f"{100 * n / total:5.2f}% {100 * cum / total:5.1f}%  {a:08x} {desc:24s} {names.get(a, '')[:90]}")


if __name__ == "__main__":
    main()
