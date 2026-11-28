"""Долгие кадры по профилю SNX_PROF с отметками кадров («m <кадр>», host/boot/prof.c): на что ушло их время.

hitchprof.py --bin build/switch/host/boot/snx_boot.elf --game game/System prof.txt [--min-ms 40] [--top 12]
Длина кадра — число выборок потока гостя между отметками (1 кГц: выборка ≈ мс). Долгий кадр — дольше --min-ms и
вдвое дольше медианы. По каждому — свои (лист) и включённые функции; в конце — сводка по всем долгим кадрам.
"""
import argparse
import collections
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from prof import elf_symbols, load_names, module_of  # noqa: E402

# корни стека — у всех выборок одни и те же, в включённом времени не интересны
ROOTS = re.compile(r"^(boot|__thread_entry|_EntryWrap|rt_pin_enter|rt_guest_call|rt_icall_slow|fb_.*|thread_start|"
                   r"_pthread_start|f_1091aacb|f_10913230|f_1090ef00|f_1090dff0|f_12122cb0)$")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--game", required=True)
    ap.add_argument("--kb", default=os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__)))), "build", "kb", "exports"))
    ap.add_argument("--min-ms", type=float, default=40)
    ap.add_argument("--top", type=int, default=12)
    ap.add_argument("prof")
    args = ap.parse_args()
    lines = open(args.prof).read().split("\n")
    m = re.match(r"base ([0-9a-f]+) slide ([0-9a-f]+)", lines[0])
    frames = []  # (кадр, [выборки потока гостя])
    cur = None
    for ln in lines[1:]:
        p = ln.split()
        if not p:
            continue
        if p[0] == "m":
            cur = (int(p[1]), [])
            frames.append(cur)
        elif p[0] == "g" and cur is not None:
            a = [int(x, 16) for x in p[1:]]
            cur[1].append([a[0]] + [x - 4 for x in a[1:] if x > 4])
    if not frames:
        raise SystemExit("в профиле нет отметок кадров")
    lens = sorted(len(s) for _, s in frames)
    med = lens[len(lens) // 2]
    long_ = [(f, s) for f, s in frames if len(s) >= args.min_ms and len(s) >= 2 * med]
    print(f"{len(frames)} кадров, медиана {med} мс; долгих (≥ {args.min_ms:.0f} мс и ≥ 2 медианы): {len(long_)}")
    uniq = sorted({a for _, s in long_ for smp in s for a in smp})
    if open(args.bin, "rb").read(4) == b"\x7fELF":
        sym = elf_symbols(args.bin, int(m.group(2), 16), uniq)
    else:
        sym = {}
        for i in range(0, len(uniq), 2000):
            part = uniq[i:i + 2000]
            out = subprocess.run(["atos", "-o", args.bin, "-l", "0x" + m.group(1)] + [hex(a) for a in part],
                                 capture_output=True, text=True).stdout.split("\n")
            for a, o in zip(part, out):
                sym[a] = re.sub(r" \(in [^)]*\)", "", o).split(" (")[0].strip()
    names = load_names(args.game, args.kb)

    def pretty(s):
        mm = re.match(r"f_([0-9a-f]{8})$", s)
        if mm:
            a = int(mm.group(1), 16)
            return f"{s} {names.get(a) or module_of(a)}"
        return s

    def report(samples, top, indent="  "):
        self_c, incl_c = collections.Counter(), collections.Counter()
        for smp in samples:
            ns = [sym.get(a, hex(a)) for a in smp]
            self_c[ns[0]] += 1
            for x in set(ns):
                if not ROOTS.match(x):
                    incl_c[x] += 1
        n = len(samples)
        print(f"{indent}свои: " + "; ".join(f"{100 * v / n:.0f}% {pretty(k)}" for k, v in self_c.most_common(top // 2)))
        print(f"{indent}включённые:")
        for k, v in incl_c.most_common(top):
            print(f"{indent}  {100 * v / n:5.1f}%  {pretty(k)}")

    allsmp = []
    for f, s in long_:
        print(f"\nкадр {f}: {len(s)} мс")
        report(s, args.top)
        allsmp.extend(s)
    if len(long_) > 1:
        print(f"\nвсе долгие кадры вместе ({len(allsmp)} мс):")
        report(allsmp, args.top * 2)


if __name__ == "__main__":
    sys.exit(main())
