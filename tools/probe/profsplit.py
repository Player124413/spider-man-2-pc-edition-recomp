"""Доли времени потока гостя по слоям (профиль SNX_PROF, см. host/boot/main.c).

profsplit.py --bin build/mac/host/boot/snx_boot --objs build/mac/runtime prof.txt
Switch: --bin build/switch/host/boot/snx_boot.elf --objs build/switch/runtime (nm из devkitA64).
Слой выборки — по самому верхнему (ближнему к листу) кадру из известных групп:
  d3d8  — HLE Direct3D 8 (runtime/hle/d3d8_*.c) и всё, что под ним (драйвер GL, libsystem);
  cs    — очередь потока рендера (gl_cs.c): запись команд и ожидание потока рендера;
  hle   — остальной HLE (kernel32, user32, bink, openal, ...);
  heap  — runtime/heap.c и vmem.c;
  rt    — рантайм рекомпилятора (rt_call, rt_rep_*, x87-хелперы);
  guest — сгенерированный код (f_<адрес>);
  sys   — системные/сторонние библиотеки (на Switch — Mesa, SDL, libnx: всё в одном ELF) без нашего кадра выше.
Отдельно — включённое время D3D8 (кадр D3D8 где-то в стеке) и его лист в системных библиотеках (драйвер GL),
и доля, когда гость ждёт поток рендера (wait_tail в стеке).
Поток рендера (выборки r): idle — ждёт команды, swap — показ кадра (plat_swap: vsync, ожидание GPU),
driver — драйвер GL за исполнением команд, ours — наш код исполнения команд (gl_cs.c, d3d8_*.c).
"""
import argparse
import bisect
import collections
import glob
import os
import re
import subprocess
import sys


NM_ELF = os.path.join(os.environ.get("DEVKITPRO", "/opt/devkitpro"), "devkitA64", "bin", "aarch64-none-elf-nm")


def nm_defined(path):
    nm = NM_ELF if open(path, "rb").read(4) == b"\x7fELF" else "nm"
    out = subprocess.run([nm, "-n", "--defined-only", path], capture_output=True, text=True).stdout
    for ln in out.split("\n"):
        p = ln.split()
        if len(p) == 3 and p[1] in "tTwW":
            yield int(p[0], 16), p[2]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--objs", required=True, help="каталог сборки runtime (объектные файлы snx_hle/snx_runtime)")
    ap.add_argument("prof")
    args = ap.parse_args()

    group_of_name = {}
    for o in glob.glob(os.path.join(args.objs, "**", "*.o"), recursive=True):
        stem = re.sub(r"(\.c)?\.o$", "", os.path.basename(o))  # mac: x.c.o, Switch (devkitPro): x.o
        g = ("cs" if stem == "gl_cs" else
             "d3d8" if stem.startswith("d3d8") or stem == "window" else
             "heap" if stem in ("heap", "vmem") else
             "rt" if stem in ("rt", "process", "loader", "overrides_crt") else "hle")
        for _, name in nm_defined(o):
            group_of_name[name.lstrip("_")] = g

    lines = open(args.prof).read().split("\n")
    m = re.match(r"base ([0-9a-f]+) slide ([0-9a-f]+)", lines[0])
    slide = int(m.group(2), 16)
    syms = [(a + slide, n.lstrip("_")) for a, n in nm_defined(args.bin)]
    starts = [a for a, _ in syms]
    end = syms[-1][0] + 0x100000

    def name(addr):
        if addr < starts[0] or addr >= end:
            return None
        return syms[bisect.bisect_right(starts, addr) - 1][1]

    def group(addr):
        n = name(addr)
        if n is None:
            return "sys"
        if re.match(r"f_[0-9a-f]{8}$", n):
            return "guest"
        # не из объектников порта: на Switch это статически слинкованные Mesa, SDL, libnx, newlib
        return group_of_name.get(n, "rt" if n.startswith("rt_") else "sys")

    leaf = collections.Counter()
    render = collections.Counter()
    d3d_incl = d3d_drv = waits = total = rtotal = 0
    for ln in lines[1:]:
        p = ln.split()
        if not p or p[0] not in "gr":
            continue
        addrs = [int(x, 16) for x in p[1:]]
        addrs = [addrs[0]] + [a - 4 for a in addrs[1:] if a > 4]  # lr = 0 — начало потока
        ns = [name(a) for a in addrs]
        if p[0] == "r":
            rtotal += 1
            # ближний к листу кадр из объектников порта (на Switch libnx/newlib/Mesa — в том же ELF)
            top = next((n for a, n in zip(addrs, ns) if n and group(a) != "sys"), None)
            if "x_swap" in ns or "plat_swap" in ns:
                render["swap"] += 1
            elif top == "render_main":
                render["idle"] += 1
            elif group(addrs[0]) == "sys":
                render["driver"] += 1
            else:
                render["ours"] += 1
            continue
        total += 1
        gs = [group(a) for a in addrs]
        top = next((g for g in gs if g != "sys"), "sys")
        leaf[top] += 1
        if any(n and n.startswith("wait_tail") for n in ns):  # GCC: wait_tail.part.0
            waits += 1
        if "d3d8" in gs:
            d3d_incl += 1
            if gs[0] == "sys":
                d3d_drv += 1
    if total:
        print(f"{total} guest-thread samples")
        for k, v in leaf.most_common():
            print(f"  {k:6} {100 * v / total:6.2f}%")
        print(f"d3d8 inclusive {100 * d3d_incl / total:.2f}% (leaf in system libraries / GL driver: "
              f"{100 * d3d_drv / total:.2f}%); waiting for the render thread {100 * waits / total:.2f}%")
    if rtotal:
        print(f"{rtotal} render-thread samples")
        for k, v in render.most_common():
            print(f"  {k:6} {100 * v / rtotal:6.2f}%")


if __name__ == "__main__":
    sys.exit(main())
