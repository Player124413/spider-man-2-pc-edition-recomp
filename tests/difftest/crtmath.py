"""Проверка нативных замен CRT (runtime/overrides_crt.c) против оригинального msvcr70 в unicorn.

python3 -I tests/difftest/crtmath.py --harness build/mac/tests/difftest/difftest_msvcr70 --game game/System

Каждая функция из recomp/overrides.py вызывается с осмысленными аргументами (числа разных
диапазонов, особые значения, строки по грамматике MSVC и мусор). Сравниваются ST0/eax и выходные
указатели. Простые вызовы kernel32 из CRT (Interlocked*, TLS для errno) эмулируются в unicorn;
если оригинал дошёл до другого импорта, случай пропускается (причины печатаются).

Эталон неточен там, где QEMU 5 (основа unicorn 2) считает x87 через double хоста: fprem при большом
частном, fsin/fcos/fptan при |x| >= 2^63, f2xm1 на крошечных аргументах. Такие случаи считаются
отдельно (unrel) и не сравниваются. Бессель (_y0/_y1/_yn) игра не импортирует, а приближения
MSVC грубее libm хоста — не проверяется.
"""
from collections import Counter
import argparse
import math
import os
import random
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from difftest import (FPUCW, REGS, SCRATCH, SCRATCH_SIZE, STACK, STACK_SIZE, TEB, THUNK_BASE, THUNK_SIZE,  # noqa: E402
                      Emu, Harness, load_program)
from unicorn import UcError  # noqa: E402
from unicorn.x86_const import UC_X86_REG_EFLAGS, UC_X86_REG_EIP, UC_X86_REG_FPCW, UC_X86_REG_FPSW, UC_X86_REG_FPTAG  # noqa: E402
from winhle import HEAP_BASE, HEAP_SIZE, Kernel32  # noqa: E402
from overrides import OVERRIDES  # noqa: E402

STUB = 0x00D01000
RES = SCRATCH          # сюда заглушка unicorn кладёт ST0
OUT = SCRATCH + 0x10   # выходной указатель (frexp/modf/endptr/_atodbl)
STR = SCRATCH + 0x100  # строка-аргумент
BESSEL = {"_y0", "_y1", "_yn"}


def unreliable(name, x, text=None):
    """Случаи, где эталон неточен (см. docstring модуля) или поведение MSVC не воспроизводится."""
    if name in STRFUNCS:  # порядок ниже -5000: знак нуля зависит от промежуточной арифметики CRT
        return any(int(m) > 5000 for m in re.findall(r"[eEdD]-(\d+)", text or ""))
    if name in ("sin", "cos", "tan"):
        return abs(x[0]) >= 2.0 ** 63
    if name == "fmod":
        return x[1] != 0 and math.isfinite(x[0]) and math.isfinite(x[1]) and \
            (abs(x[0]) >= abs(x[1]) * 2.0 ** 40 or abs(x[1]) < 2.0 ** -1000)  # частное в double QEMU
    if name in ("sinh", "tanh"):
        return 0 < abs(x[0]) < 2.0 ** -13
    if name == "pow":  # fscale с показателем вне int32
        return math.isfinite(x[0]) and math.isfinite(x[1]) and x[0] not in (0.0, 1.0, -1.0) and \
            abs(x[1] * math.log2(abs(x[0]))) > 2.0 ** 31
    return False

# сигнатуры: d — double, i — int, s — char*, w — wchar_t*, p — указатель на выход (OUT)
SIGS = {
    "atan2": "dd", "fmod": "dd", "pow": "dd", "_nextafter": "dd",
    "frexp": "dp", "modf": "dp", "ldexp": "di", "_yn": "id",
    "atof": "s", "strtod": "sp", "_wtof": "w", "wcstod": "wp", "_atodbl": "ps",
}
STRFUNCS = {"atof", "strtod", "_wtof", "wcstod", "_atodbl"}
STATEFUL = {"rand", "srand"}  # генератор с состоянием потока — tests/rt/crand_test.c
EXACT = {"fabs", "floor", "ceil", "fmod", "frexp", "ldexp", "modf", "sqrt", "_logb", "_nextafter",
         "atof", "strtod", "_wtof", "wcstod", "_atodbl"}

SPECIAL = [0.0, -0.0, 1.0, -1.0, 0.5, -0.5, 2.0, 1.5, -1.5, 2.5, 3.0, 10.0, 100.0, 1e-300, 5e-324, -5e-324,
           2.2250738585072014e-308, 1.7976931348623157e308, -1.7976931348623157e308, math.pi, -math.pi,
           math.pi / 2, math.pi / 4, 1e20, -1e20, 2.0 ** 63, 2.0 ** 64, 710.0, -745.0, 0.9999999999999999,
           -708.0, -709.0, -720.0, -740.0, -744.0, -744.3, -744.5, 709.7, 709.8,
           math.inf, -math.inf, math.nan, struct.unpack("<d", struct.pack("<Q", 0x7FF0000000000001))[0]]


def rand_double(rng):
    r = rng.random()
    if r < 0.3:
        return rng.choice(SPECIAL)
    if r < 0.6:
        return rng.uniform(-10, 10)
    if r < 0.7:
        return float(rng.randrange(-1000, 1000)) / rng.choice([1, 2, 4, 8])
    if r < 0.9:
        return rng.choice([-1, 1]) * 10 ** rng.uniform(-300, 300)
    return struct.unpack("<d", struct.pack("<Q", rng.getrandbits(64)))[0]


def rand_numstr(rng):
    if rng.random() < 0.15:
        return rng.choice(["", " ", "inf", "nan", "0x10", "1e", "1e+", "1E-", ".", "-.", "+", "-", "1.#INF", "1e400",
                           "-1e400", "1e-400", "-1e-400", "4.9e-324", "2.4e-324", "2.2250738585072014e-308",
                           "123456789012345678901234567890", "0.000000000000000000000000000001234567890123456789012345",
                           "0.1", "3.14159", "  -2.5e3xyz", "\t\n+7.", "1d5", "1D-2", "00000", "-0", "-0.0e5",
                           "1.7976931348623157e308", "1.7976931348623159e308", "9" * 400, "0." + "0" * 330 + "1",
                           "1e-310", "-2.5e-320", "-1e-4920", "1e-4931", "1e-4932", "-1e-4933", "1e-4940",
                           "1e-4960", "1e4931", "-1e4932", "1e4933", "1e5000", "-1e-5000"])
    s = rng.choice(["", "", " ", "\t", "  "])
    s += rng.choice(["", "", "+", "-"])
    s += "".join(rng.choice("0123456789") for _ in range(rng.choice([0, 1, 1, 2, 3, 5, 8, 17, 20, 21, 22, 25])))
    if rng.random() < 0.6:
        s += "." + "".join(rng.choice("0123456789") for _ in range(rng.choice([0, 1, 2, 4, 9, 17, 25])))
    if rng.random() < 0.4:
        s += rng.choice("eEdD") + rng.choice(["", "+", "-"])
        s += "".join(rng.choice("0123456789") for _ in range(rng.choice([0, 1, 2, 3, 4])))
    s += rng.choice(["", "", "x", " 1", "e", ".5", "f"])
    return s


def args_for(sig, rng):
    """-> (слова стека после адреса возврата, строка-аргумент или None, wide, числа для печати)."""
    words, text, wide, shown = b"", None, False, []
    for k in sig:
        if k == "d":
            shown.append(rand_double(rng))
            words += struct.pack("<d", shown[-1])
        elif k == "i":
            shown.append(rng.choice([0, 1, -1, 2, 5, 100, -1100, 1100, rng.randrange(-60, 60)]))
            words += struct.pack("<i", shown[-1])
        elif k == "p":
            words += struct.pack("<I", OUT)
        else:
            text, wide = rand_numstr(rng), k == "w"
            # _wtof переводит строку через WideCharToMultiByte: не-ASCII зависит от HLE, а не от CRT
            if wide and sig == "wp" and rng.random() < 0.2:
                pos = rng.randrange(len(text) + 1)
                text = text[:pos] + rng.choice(["é", "　", "١", "１"]) + text[pos:]
            words += struct.pack("<I", STR)
    return words, text, wide, shown


def run_uc(emu, regs, fn, until):
    """Исполнить оригинал до адреса until (заглушка после возврата); None — не дошёл (ошибка/HLE)."""
    uc = emu.uc
    for r, v in zip(REGS, regs):
        uc.reg_write(r, v)
    uc.reg_write(UC_X86_REG_EFLAGS, 0x202)
    uc.reg_write(UC_X86_REG_FPCW, FPUCW)
    uc.reg_write(UC_X86_REG_FPSW, 0)
    uc.reg_write(UC_X86_REG_FPTAG, 0xFFFF)
    emu.stop_reason = None
    emu.k32.hit = None
    try:
        uc.emu_start(fn, until, timeout=2_000_000, count=2_000_000)
    except UcError as e:
        emu.why = f"uc error {e}"
        return None
    if emu.stop_reason is not None or uc.reg_read(UC_X86_REG_EIP) != until:
        emu.why = emu.k32.hit or emu.stop_reason or f"eip {uc.reg_read(UC_X86_REG_EIP):08x}"
        return None
    return [uc.reg_read(r) for r in REGS]


def bits(v):
    return struct.unpack("<Q", struct.pack("<d", v))[0]


def close(a, b, exact):
    if bits(a) == bits(b):
        return True
    if abs(a) < 2.2250738585072014e-308 and abs(bits(a) - bits(b)) == 1:
        return True  # денормалы: x87 с PC=53 округляет дважды (мантисса, потом запись в память)
    if exact:
        return False
    if math.isnan(a) and math.isnan(b):
        return True
    if math.isinf(a) or math.isinf(b) or math.isnan(a) or math.isnan(b):
        return False
    return abs(a - b) <= 1e-12 * max(abs(a), abs(b))  # QEMU считает трансцендентные через double хоста


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--harness", required=True)
    ap.add_argument("--game", required=True)
    ap.add_argument("--cases", type=int, default=400)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--only", action="append", default=[])
    ap.add_argument("--show", type=int, default=5, help="сколько расхождений печатать на функцию")
    args = ap.parse_args()

    prog = load_program(args.game, ["msvcr70.dll"])
    img = prog.images["msvcr70.dll"]
    h = Harness(args.harness, args.game)
    emu = Emu([(img.base, img.size), (THUNK_BASE, THUNK_SIZE), (TEB, 0x1000), (STACK, STACK_SIZE),
               (SCRATCH, SCRATCH_SIZE), (STUB, 0x1000), (HEAP_BASE, HEAP_SIZE)])
    emu.uc.mem_write(img.base, h.read(img.base, img.size))
    emu.uc.mem_write(THUNK_BASE, h.read(THUNK_BASE, THUNK_SIZE))
    emu.uc.mem_write(STUB, b"\xdd\x1d" + struct.pack("<I", RES))  # fstp qword [RES]
    emu.k32 = Kernel32(emu, [img])
    teb = bytearray(0x1000)
    struct.pack_into("<III", teb, 0, 0xFFFFFFFF, STACK + STACK_SIZE, STACK)
    struct.pack_into("<I", teb, 0x18, TEB)
    # оригинальный CRT инициализируется своим DllMain(hinst, DLL_PROCESS_ATTACH, 0)
    esp = STACK + STACK_SIZE - 0x400
    emu.uc.mem_write(TEB, bytes(teb))
    emu.uc.mem_write(esp, struct.pack("<4I", STUB, img.base, 1, 0))
    out = run_uc(emu, [0, 0, 0, 0, esp, 0, 0, 0], img.entry, STUB)
    if out is None or out[0] != 1:
        print(f"msvcr70 DllMain failed: {getattr(emu, 'why', None)} eax={out and out[0]}")
        return 2

    rng = random.Random(args.seed)
    total_fail = 0
    for key in OVERRIDES:
        name = key.split("!")[1]
        if args.only and name not in args.only:
            continue
        if name not in img.exports or name in BESSEL or name in STATEFUL:
            continue
        fn = img.exports[name]
        sig = SIGS.get(name, "d")
        floatret = name != "_atodbl"
        st = {"pass": 0, "fail": 0, "skip": 0, "unrel": 0}
        why = Counter()
        shown = 0
        for _ in range(args.cases):
            words, text, wide, shown_args = args_for(sig, rng)
            esp = STACK + STACK_SIZE - 0x400
            regs = [0x11111111, 0x22222222, 0x33333333, 0x44444444, esp, 0x55555555, 0x66666666, 0x77777777]
            scratch = bytearray(0xA00)
            struct.pack_into("<QQ", scratch, 0x10, 0x5A5A5A5A5A5A5A5A, 0)
            if text is not None:
                enc = text.encode("utf-16-le") + b"\0\0" if wide else text.encode("latin-1") + b"\0"
                scratch[0x100:0x100 + len(enc)] = enc
                scratch = scratch.ljust(0x100 + len(enc) + 16, b"\0")
            frame = struct.pack("<I", STUB) + words
            for a, data in ((TEB, bytes(teb)), (SCRATCH, bytes(scratch)), (esp, frame)):
                h.write(a, data)
                emu.uc.mem_write(a, data)
            uout = run_uc(emu, regs, fn, STUB + 6 if floatret else STUB)
            hst, hout, st_ret, taddr, msg = h.run(regs, TEB, fn)
            if uout is None:
                st["skip"] += 1
                why[emu.why] += 1
                continue
            if unreliable(name, shown_args, text):
                st["unrel"] += 1
                continue
            if hst != 0:
                diffs = [f"harness trap {taddr:08x} {msg}"]
            else:
                diffs = []
                if floatret:
                    ur, = struct.unpack("<d", emu.uc.mem_read(RES, 8))
                    # у fprem в QEMU неверны знак нулевого остатка и NaN (берёт NaN хоста)
                    qemu_fprem = name == "fmod" and (ur == 0 == st_ret or math.isnan(ur) and math.isnan(st_ret))
                    # MSVC округляет десятичные ничьи (17+ цифр) к нулю, мы — к чётному: разница 1 ulp
                    tie = name in STRFUNCS and abs(bits(ur) - bits(st_ret)) == 1
                    if not qemu_fprem and not tie and not close(ur, st_ret, name in EXACT):
                        diffs.append(f"ret uc={ur!r} ({bits(ur):016x}) rc={st_ret!r} ({bits(st_ret):016x})")
                else:
                    if uout[0] != hout[0]:
                        diffs.append(f"eax uc={uout[0]} rc={hout[0]}")
                if uout[4] != hout[4]:
                    diffs.append(f"esp uc={uout[4]:08x} rc={hout[4]:08x}")
                if "p" in sig:
                    ub, hb = bytes(emu.uc.mem_read(OUT, 8)), h.read(OUT, 8)
                    if name in ("strtod", "wcstod", "frexp"):
                        ub, hb = ub[:4], hb[:4]
                    if name == "_atodbl" and abs(bits(struct.unpack("<d", ub)[0]) - bits(struct.unpack("<d", hb)[0])) == 1:
                        hb = ub  # ничья на 17 цифрах (см. tie ниже)
                    if ub != hb:
                        if name in ("modf", "_atodbl"):
                            u, = struct.unpack("<d", ub)
                            r, = struct.unpack("<d", hb)
                            desc = f"uc={u!r} rc={r!r}"
                        else:
                            u, = struct.unpack("<I", ub)
                            r, = struct.unpack("<I", hb)
                            desc = f"uc={u:#x} rc={r:#x}"
                        diffs.append(f"out {desc}")
            if diffs:
                st["fail"] += 1
                if shown < args.show:
                    shown += 1
                    a = repr(text) if text is not None else ", ".join(repr(v) for v in shown_args)
                    print(f"  {name}({a}): " + "; ".join(diffs))
            else:
                st["pass"] += 1
        total_fail += st["fail"]
        print(f"{name:12s} {st}" + ("  skip: " + ", ".join(f"{k} x{v}" for k, v in why.most_common(3)) if why else ""))
    h.p.stdin.write(b"Q")
    h.p.stdin.close()
    return 1 if total_fail else 0


if __name__ == "__main__":
    sys.exit(main())
