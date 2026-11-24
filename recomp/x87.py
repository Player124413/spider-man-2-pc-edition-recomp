"""x87: нормализованный разбор по опкодам D8–DF (не по мнемоникам capstone) и эффекты на стек.

Каждая инструкция превращается в X87Op:
  kind     — вид операции
  args     — параметры
  need     — сколько элементов стека должно быть (максимальный ST(i) + 1)
  delta    — изменение глубины (+1 push, -1 pop, ...)
"""
from dataclasses import dataclass

ARITH = ["add", "mul", "com", "comp", "sub", "subr", "div", "divr"]
IARITH = ["add", "mul", "com", "comp", "sub", "subr", "div", "divr"]


@dataclass
class X87Op:
    kind: str
    args: tuple
    need: int
    delta: int


def _mem_fmt(opc, reg):
    """Формат операнда в памяти для D8–DF /reg."""
    return {
        0xD8: "f32", 0xDC: "f64", 0xDA: "i32", 0xDE: "i16",
    }.get(opc)


def decode_x87(ins):
    opc, modrm = ins.x87
    mod = modrm >> 6
    reg = (modrm >> 3) & 7
    rm = modrm & 7
    mem = None
    if mod != 3:
        mem = next(o for o in ins.ops if o.kind == "m")

    if mod != 3:
        if opc in (0xD8, 0xDC, 0xDA, 0xDE):
            fmt = _mem_fmt(opc, reg)
            op = ARITH[reg]
            if op in ("com", "comp"):
                return X87Op("cmp_mem", (fmt, mem, op == "comp"), 1, -1 if op == "comp" else 0)
            return X87Op("arith_mem", (op, fmt, mem), 1, 0)
        if opc == 0xD9:
            if reg == 0:
                return X87Op("ld_mem", ("f32", mem), 0, 1)
            if reg in (2, 3):
                return X87Op("st_mem", ("f32", mem, reg == 3), 1, -1 if reg == 3 else 0)
            if reg == 5:
                return X87Op("fldcw", (mem,), 0, 0)
            if reg == 7:
                return X87Op("fnstcw", (mem,), 0, 0)
            if reg == 4:
                return X87Op("fldenv", (mem,), 0, 0)
            if reg == 6:
                return X87Op("fnstenv", (mem,), 0, 0)
        if opc == 0xDB:
            if reg == 0:
                return X87Op("ld_mem", ("i32", mem), 0, 1)
            if reg in (1, 2, 3):  # fisttp(SSE3)/fist/fistp
                return X87Op("st_mem", ("i32" if reg != 1 else "i32t", mem, reg != 2), 1, 0 if reg == 2 else -1)
            if reg == 5:
                return X87Op("ld_mem", ("f80", mem), 0, 1)
            if reg == 7:
                return X87Op("st_mem", ("f80", mem, True), 1, -1)
        if opc == 0xDD:
            if reg == 0:
                return X87Op("ld_mem", ("f64", mem), 0, 1)
            if reg in (1, 2, 3):
                return X87Op("st_mem", ("f64" if reg != 1 else "i64t", mem, reg != 2), 1, 0 if reg == 2 else -1)
            if reg == 4:
                return X87Op("frstor", (mem,), 0, 0)
            if reg == 6:
                return X87Op("fnsave", (mem,), 0, 0)
            if reg == 7:
                return X87Op("fnstsw_mem", (mem,), 0, 0)
        if opc == 0xDF:
            if reg == 0:
                return X87Op("ld_mem", ("i16", mem), 0, 1)
            if reg in (1, 2, 3):
                return X87Op("st_mem", ("i16" if reg != 1 else "i16t", mem, reg != 2), 1, 0 if reg == 2 else -1)
            if reg == 4:
                return X87Op("unknown", ("fbld",), 0, 1)
            if reg == 5:
                return X87Op("ld_mem", ("i64", mem), 0, 1)
            if reg == 6:
                return X87Op("unknown", ("fbstp",), 1, -1)
            if reg == 7:
                return X87Op("st_mem", ("i64", mem, True), 1, -1)
        return X87Op("unknown", (ins.text,), 0, 0)

    i = rm
    if opc == 0xD8:
        op = ARITH[reg]
        if op in ("com", "comp"):
            return X87Op("cmp_st", (i, 1 if op == "comp" else 0, False), max(1, i + 1), -1 if op == "comp" else 0)
        return X87Op("arith_st", (op, 0, i, False), max(1, i + 1), 0)  # ST0 = ST0 op STi
    if opc == 0xDC:
        # DC: STi = STi op ST0; для sub/div мнемоники «перевёрнуты»: E0+i FSUBR, E8+i FSUB, F0+i FDIVR, F8+i FDIV
        op = {0: "add", 1: "mul", 2: "com", 3: "comp", 4: "subr", 5: "sub", 6: "divr", 7: "div"}[reg]
        if op in ("com", "comp"):
            return X87Op("cmp_st", (i, 1 if op == "comp" else 0, False), max(1, i + 1), -1 if op == "comp" else 0)
        # STi = STi op ST0 для sub/div; subr: STi = ST0 - STi
        return X87Op("arith_st", (op, i, 0, False), max(1, i + 1), 0)
    if opc == 0xDE:
        if reg == 3 and i == 1:
            return X87Op("cmp_st", (1, 2, False), 2, -2)  # fcompp
        op = {0: "add", 1: "mul", 2: "com", 3: "comp", 4: "subr", 5: "sub", 6: "divr", 7: "div"}[reg]
        if op in ("com", "comp"):
            return X87Op("cmp_st", (i, 1, False), max(1, i + 1), -1)
        return X87Op("arith_st", (op, i, 0, True), max(2, i + 1), -1)  # STi = STi op ST0; pop
    if opc == 0xD9:
        if reg == 0:
            return X87Op("ld_st", (i,), i + 1, 1)
        if reg == 1:
            return X87Op("fxch", (i,), max(1, i + 1), 0)
        if modrm == 0xD0:
            return X87Op("nop", (), 0, 0)
        simple = {
            0xE0: ("unary", "chs", 1, 0), 0xE1: ("unary", "abs", 1, 0), 0xE4: ("ftst", None, 1, 0),
            0xE5: ("fxam", None, 1, 0), 0xE8: ("ld_const", "1", 0, 1), 0xE9: ("ld_const", "l2t", 0, 1),
            0xEA: ("ld_const", "l2e", 0, 1), 0xEB: ("ld_const", "pi", 0, 1), 0xEC: ("ld_const", "lg2", 0, 1),
            0xED: ("ld_const", "ln2", 0, 1), 0xEE: ("ld_const", "0", 0, 1),
            0xF0: ("unary", "f2xm1", 1, 0), 0xF1: ("binpop", "fyl2x", 2, -1), 0xF2: ("fptan", None, 1, 1),
            0xF3: ("binpop", "fpatan", 2, -1), 0xF4: ("unknown", "fxtract", 1, 1), 0xF5: ("bin", "fprem1", 2, 0),
            0xF6: ("unknown", "fdecstp", 0, 0), 0xF7: ("unknown", "fincstp", 0, 0), 0xF8: ("bin", "fprem", 2, 0),
            0xF9: ("binpop", "fyl2xp1", 2, -1), 0xFA: ("unary", "sqrt", 1, 0), 0xFB: ("fsincos", None, 1, 1),
            0xFC: ("unary", "rndint", 1, 0), 0xFD: ("bin", "fscale", 2, 0), 0xFE: ("unary", "sin", 1, 0),
            0xFF: ("unary", "cos", 1, 0),
        }
        if modrm in simple:
            k, a, need, delta = simple[modrm]
            return X87Op(k, (a,), need, delta)
    if opc == 0xDA:
        if modrm == 0xE9:
            return X87Op("cmp_st", (1, 2, True), 2, -2)  # fucompp
        cc = {0: "b", 1: "e", 2: "be", 3: "p"}.get(reg)
        if cc:
            return X87Op("fcmov", (cc, i), max(1, i + 1), 0)
    if opc == 0xDB:
        cc = {0: "ae", 1: "ne", 2: "a", 3: "np"}.get(reg)
        if cc:
            return X87Op("fcmov", (cc, i), max(1, i + 1), 0)
        if modrm == 0xE2:
            return X87Op("nop", ("fnclex",), 0, 0)
        if modrm == 0xE3:
            return X87Op("fninit", (), 0, 0)
        if reg == 5:
            return X87Op("comi", (i, False), max(1, i + 1), 0)  # fucomi
        if reg == 6:
            return X87Op("comi", (i, False), max(1, i + 1), 0)  # fcomi
    if opc == 0xDD:
        if reg == 0:
            return X87Op("ffree", (i,), 0, 0)
        if reg == 2:
            return X87Op("st_st", (i, False), max(1, i + 1), 0)
        if reg == 3:
            return X87Op("st_st", (i, True), max(1, i + 1), -1)
        if reg == 4:
            return X87Op("cmp_st", (i, 0, True), max(1, i + 1), 0)
        if reg == 5:
            return X87Op("cmp_st", (i, 1, True), max(1, i + 1), -1)
    if opc == 0xDF:
        if modrm == 0xE0:
            return X87Op("fnstsw_ax", (), 0, 0)
        if reg == 5:
            return X87Op("comi", (i, True), max(1, i + 1), -1)  # fucomip
        if reg == 6:
            return X87Op("comi", (i, True), max(1, i + 1), -1)  # fcomip
    return X87Op("unknown", (ins.text,), 0, 0)


# Встроенные функции CRT, получающие аргументы через стек x87: имя -> (сколько снимают, сколько кладут).
X87_INTRINSICS = {
    "_ftol": (1, 0), "_ftol2": (1, 0), "_ftol2_sse": (1, 0),
    "_CIpow": (2, 1), "_CIfmod": (2, 1), "_CIatan2": (2, 1),
    "_CIsin": (1, 1), "_CIcos": (1, 1), "_CItan": (1, 1), "_CIasin": (1, 1), "_CIacos": (1, 1),
    "_CIatan": (1, 1), "_CIsinh": (1, 1), "_CIcosh": (1, 1), "_CItanh": (1, 1), "_CIexp": (1, 1),
    "_CIlog": (1, 1), "_CIlog10": (1, 1), "_CIsqrt": (1, 1),
}
