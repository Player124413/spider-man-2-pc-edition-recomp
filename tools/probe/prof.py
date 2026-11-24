"""Сводка профиля snx_boot (SNX_PROF=<файл>, см. host/boot/main.c).

prof.py --bin build/mac/host/boot/snx_boot --game game/System [--kb build/kb/exports] [--top 40] [--thread g|r] prof.txt
Профиль со Switch: --bin build/switch/host/boot/snx_boot.elf (имена — по nm из devkitA64, без строк).
Печатает функции по собственному времени (вершина стека) и по включённому (функция где-то в стеке).
Имена f_<адрес> переводятся в экспорт модуля (с демэнглом, если есть каталог build/kb/exports): переходник
инкрементальной линковки (jmp rel32 на экспорте) ведёт к телу — тело получает имя экспорта.
"""
import argparse
import bisect
import collections
import json
import os
import re
import struct
import subprocess
import sys

import pefile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def load_names(game, kb):
    """адрес гостя -> имя (экспорты и тела за переходниками)"""
    amap = json.load(open(os.path.join(ROOT, "recomp", "addrmap.json")))["modules"]
    names = {}
    for mod, base in amap.items():
        base = int(base, 16)
        stem = mod.replace(".", "_").lower()
        tsv = os.path.join(kb, stem + ".tsv") if kb else None
        path = os.path.join(game, mod)
        if not os.path.exists(path):
            continue
        pe = pefile.PE(path, fast_load=True)
        pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXPORT"]])
        img = pe.get_memory_mapped_image()
        demangled = {}
        if tsv and os.path.exists(tsv):
            for ln in open(tsv, encoding="utf-8"):
                p = ln.rstrip("\n").split("\t")
                if len(p) >= 4 and p[0] != "addr":
                    demangled[int(p[0], 16)] = p[3] or p[2]
        exp = getattr(pe, "DIRECTORY_ENTRY_EXPORT", None)
        for e in exp.symbols if exp else []:
            a = base + e.address
            nm = demangled.get(a) or (e.name.decode() if e.name else f"#{e.ordinal}")
            nm = f"{mod}!{nm}"
            names.setdefault(a, nm)
            rva = e.address
            for _ in range(3):  # переходники: jmp rel32 (может быть цепочка)
                if rva + 5 <= len(img) and img[rva] == 0xE9:
                    rva = (rva + 5 + struct.unpack_from("<i", img, rva + 1)[0]) & 0xFFFFFFFF
                    names.setdefault(base + rva, nm)
                else:
                    break
        if mod == "game.exe" or not exp:
            names.setdefault(base + pe.OPTIONAL_HEADER.AddressOfEntryPoint, f"{mod}!entry")
    return names


NM_ELF = os.path.join(os.environ.get("DEVKITPRO", "/opt/devkitpro"), "devkitA64", "bin", "aarch64-none-elf-nm")


def elf_symbols(path, slide, addrs):
    """адрес выполнения -> имя функции ELF (NRO: адрес в ELF = адрес - начало образа)"""
    out = subprocess.run([NM_ELF, "-n", "--defined-only", path], capture_output=True, text=True).stdout
    syms = []
    for ln in out.split("\n"):
        p = ln.split()
        if len(p) == 3 and p[1] in "tTwW":
            syms.append((int(p[0], 16) + slide, p[2]))
    starts = [a for a, _ in syms]
    res = {}
    for a in addrs:
        i = bisect.bisect_right(starts, a) - 1
        res[a] = syms[i][1] if i >= 0 else hex(a)
    return res


def module_of(addr):
    amap = json.load(open(os.path.join(ROOT, "recomp", "addrmap.json")))["modules"]
    best = None
    for mod, base in amap.items():
        b = int(base, 16)
        if b <= addr and (best is None or b > best[1]):
            best = (mod, b)
    return best[0] if best else "?"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True)
    ap.add_argument("--game", required=True)
    ap.add_argument("--kb", default=os.path.join(ROOT, "build", "kb", "exports"))
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--thread", choices=("g", "r"), help="только выборки потока: g — гостя, r — рендера")
    ap.add_argument("--guest", dest="thread", action="store_const", const="g", help="то же, что --thread g")
    ap.add_argument("prof")
    args = ap.parse_args()
    lines = open(args.prof).read().split("\n")
    m = re.match(r"base ([0-9a-f]+) slide ([0-9a-f]+)", lines[0])
    base = m.group(1)
    samples = []
    for ln in lines[1:]:
        if not ln:
            continue
        p = ln.split()
        if p[0] not in ("g", "r") or (args.thread and p[0] != args.thread):  # m — отметка кадра
            continue
        addrs = [int(x, 16) for x in p[1:]]
        # pc как есть, адреса возврата — на инструкцию вызова
        samples.append([addrs[0]] + [a - 4 for a in addrs[1:] if a > 4])  # lr = 0 — начало потока
    uniq = sorted({a for s in samples for a in s})
    sym = {}
    if open(args.bin, "rb").read(4) == b"\x7fELF":
        sym = elf_symbols(args.bin, int(m.group(2), 16), uniq)
    chunk = 2000
    for i in range(0, len(uniq), chunk) if not sym else []:
        part = uniq[i:i + chunk]
        out = subprocess.run(["atos", "-o", args.bin, "-l", "0x" + base] + [hex(a) for a in part],
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

    self_c = collections.Counter()
    incl_c = collections.Counter()
    for s in samples:
        # лист без кадра: pc и lr в одной функции — lr не считаем отдельно
        syms = [sym.get(a, hex(a)) for a in s]
        self_c[syms[0]] += 1
        for x in set(syms):
            incl_c[x] += 1
    n = len(samples)
    who = {"g": " (guest thread)", "r": " (render thread)"}.get(args.thread, "")
    print(f"{n} samples{who}\n\nself:")
    for k, v in self_c.most_common(args.top):
        print(f"{100 * v / n:6.2f}%  {pretty(k)}")
    print("\ninclusive:")
    for k, v in incl_c.most_common(args.top):
        print(f"{100 * v / n:6.2f}%  {pretty(k)}")


if __name__ == "__main__":
    sys.exit(main())
