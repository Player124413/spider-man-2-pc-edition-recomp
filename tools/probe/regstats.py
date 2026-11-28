"""Отладка сводок по регистрам (recomp/regs.py): сколько функций сохраняют ebx/esi/edi/ebp, у скольких touched
не все регистры, примеры функций, у которых callee-saved регистр попал в out.

    tools/.venv/bin/python -I tools/probe/regstats.py --game game/System [--mod Core.dll] [--n 8]
"""
import argparse
import collections
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "recomp"))

from discover import Module, Program  # noqa: E402
from gen import Ctx  # noqa: E402
from image import load_addrmap, load_image  # noqa: E402
from overrides import resolve as resolve_overrides  # noqa: E402
from regs import ALL, BIT, RegAnalysis  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", required=True)
    ap.add_argument("--mod", default="Core.dll")
    ap.add_argument("--n", type=int, default=6)
    args = ap.parse_args()
    amap = load_addrmap()
    images = {n.lower(): load_image(os.path.join(args.game, n), b) for n, b in amap.items()}
    prog = Program(images)
    mod = Module(images[args.mod.lower()], prog)  # только один модуль: вызовы в другие — незнакомые цели
    prog.modules[args.mod.lower()] = mod
    mod.run()
    ctx = Ctx(prog, resolve_overrides(prog))
    ra = RegAnalysis(ctx)
    ra.run(prog)
    cnt = collections.Counter()
    examples = collections.defaultdict(list)
    for e, f in mod.functions.items():
        if e in ra.skip:
            cnt["без сводок"] += 1
            continue
        cnt["всего"] += 1
        if ctx.reg_touched[e] != ALL:
            cnt["touched не все"] += 1
        for r in ("ebx", "esi", "edi", "ebp"):
            b = BIT[r]
            if ra.preserved[e] & b:
                cnt[f"сохраняет {r}"] += 1
            elif ctx.reg_out[e] & b:
                cnt[f"{r} в out"] += 1
                if ra.local_w[e] & b and len(examples[r]) < args.n:
                    examples[r].append(e)
    for k, v in sorted(cnt.items()):
        print(f"{k:24s} {v}")
    for r, addrs in examples.items():
        print(f"пишет {r}, но не сохранён:")
        for a in addrs:
            f = mod.functions[a]
            exits = [f"{f.insns[i]:08x}" for i, _ in ra.flow[a][2]][:6]
            print(f"  {a:08x}: insns={len(f.insns)} exits={exits}")


if __name__ == "__main__":
    main()
