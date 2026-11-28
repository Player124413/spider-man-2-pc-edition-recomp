"""Статистика мест вызова: сколько прямых вызовов, сколько встраивается и почему остальные — нет.

    tools/.venv/bin/python -I tools/probe/callstats.py --game game/System [модули]
"""
import argparse
import collections
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "recomp"))

from discover import Module, Program  # noqa: E402
from gen import Ctx, INLINE_MAX  # noqa: E402
from image import load_addrmap, load_image  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", required=True)
    ap.add_argument("modules", nargs="*")
    args = ap.parse_args()
    amap = load_addrmap()
    only = {m.lower() for m in args.modules} if args.modules else None
    images = {n.lower(): load_image(os.path.join(args.game, n), b) for n, b in amap.items()}
    prog = Program(images)
    for name in images:
        if only and name not in only:
            continue
        prog.modules[name] = Module(images[name], prog)
    for mod in prog.modules.values():
        mod.run()
    ctx = Ctx(prog)
    why = collections.Counter()
    npairs = 0
    ncall = 0
    for mod in prog.modules.values():
        for e, f in mod.functions.items():
            for a in f.insns:
                ins = mod.insn(a)
                if ins.mnem != "call" or ins.branch_target() is None:
                    continue
                t = ins.branch_target()
                tmod = ctx.module_for(t)
                tf = tmod.functions.get(t) if tmod is not None else None
                ncall += 1
                if tf is None:
                    why["не функция"] += 1
                    continue
                if ctx.inline_body(t):
                    why["встраивается"] += 1
                    continue
                if len(tf.insns) > INLINE_MAX:
                    why["длинная > INLINE_MAX"] += 1
                elif tf.calls:
                    why["внутри есть вызовы"] += 1
                elif tf.tail_targets:
                    why["хвостовые вызовы"] += 1
                elif tf.unknown_indirect:
                    why["косвенный вызов"] += 1
                elif t in mod.eh_conts or t in mod.eh_targets:
                    why["EH"] += 1
                elif t in ctx.overrides:
                    why["оверрайд"] += 1
                elif tf.invalid:
                    why["invalid"] += 1
                else:
                    why["прочее (сам не встроился)"] += 1
                if len(tf.insns) <= INLINE_MAX and tf.calls:
                    npairs += 1
    print("прямых вызовов:", ncall)
    for k, v in why.most_common():
        print(f"  {k:26s} {v}")
    print("из них мелкие (<= INLINE_MAX) с вызовами внутри:", npairs)


if __name__ == "__main__":
    main()
