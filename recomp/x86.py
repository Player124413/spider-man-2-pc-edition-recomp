"""Декодер x86-32: capstone + собственная нормализация (префиксы, строковые операции, x87 по опкодам)."""
from dataclasses import dataclass, field

import capstone
from capstone import x86 as X

_md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
_md.detail = True

REG32 = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
REG16 = {"ax": "eax", "cx": "ecx", "dx": "edx", "bx": "ebx", "sp": "esp", "bp": "ebp", "si": "esi", "di": "edi"}
REG8L = {"al": "eax", "cl": "ecx", "dl": "edx", "bl": "ebx"}
REG8H = {"ah": "eax", "ch": "ecx", "dh": "edx", "bh": "ebx"}

STRING_OPS = {0xA4: "movs", 0xA5: "movs", 0xA6: "cmps", 0xA7: "cmps", 0xAA: "stos", 0xAB: "stos",
              0xAC: "lods", 0xAD: "lods", 0xAE: "scas", 0xAF: "scas"}

JCC = {"jo", "jno", "jb", "jae", "je", "jne", "jbe", "ja", "js", "jns", "jp", "jnp", "jl", "jge", "jle", "jg"}
LOOPS = {"loop", "loope", "loopne", "jecxz"}


@dataclass(slots=True)
class Op:
    kind: str  # 'r' регистр, 'i' непосредственное, 'm' память
    size: int
    reg: str | None = None
    imm: int = 0
    seg: str | None = None
    base: str | None = None
    index: str | None = None
    scale: int = 1
    disp: int = 0

    def __str__(self):
        if self.kind == "r":
            return self.reg
        if self.kind == "i":
            return hex(self.imm)
        parts = []
        if self.base:
            parts.append(self.base)
        if self.index:
            parts.append(f"{self.index}*{self.scale}")
        if self.disp or not parts:
            parts.append(hex(self.disp & 0xFFFFFFFF))
        s = "+".join(parts)
        return f"{self.seg + ':' if self.seg else ''}[{s}]/{self.size}"


@dataclass(slots=True)
class Insn:
    addr: int
    size: int
    mnem: str  # без префиксов rep/lock
    ops: list
    text: str  # дизассемблер для комментариев
    raw: bytes
    rep: str | None = None  # 'rep' | 'repne' (для строковых операций)
    lock: bool = False
    opsize16: bool = False
    string_op: str | None = None  # movs/stos/... для A4–AF
    x87: tuple | None = None  # (opcode, modrm) для D8–DF
    seg_override: str | None = None
    extra: dict = field(default_factory=dict)

    @property
    def next(self):
        return (self.addr + self.size) & 0xFFFFFFFF

    def is_jcc(self):
        return self.mnem in JCC

    def is_loop(self):
        return self.mnem in LOOPS

    def branch_target(self):
        if self.ops and self.ops[0].kind == "i":
            return self.ops[0].imm & 0xFFFFFFFF
        return None


def _op(insn, o):
    if o.type == X.X86_OP_REG:
        return Op("r", o.size, reg=insn.reg_name(o.reg))
    if o.type == X.X86_OP_IMM:
        return Op("i", o.size, imm=o.imm)
    m = o.mem
    return Op("m", o.size,
              seg=insn.reg_name(m.segment) if m.segment else None,
              base=insn.reg_name(m.base) if m.base else None,
              index=insn.reg_name(m.index) if m.index else None,
              scale=m.scale, disp=m.disp & 0xFFFFFFFF)


def decode(buf, addr, offset=0):
    """Декодирует одну инструкцию из buf[offset:] по адресу addr. None — если байты невалидны."""
    for ci in _md.disasm(bytes(buf[offset:offset + 16]), addr, 1):
        break
    else:
        return None
    mnem = ci.mnemonic
    pfx = [p for p in ci.prefix if p]
    rep = None
    lock = 0xF0 in pfx
    words = mnem.split()
    if len(words) > 1 and words[0] in ("rep", "repe", "repz", "repne", "repnz", "lock", "bnd", "notrack", "xacquire", "xrelease"):
        mnem = words[-1]
    raw = bytes(ci.bytes)
    opc = ci.opcode[0]
    string_op = None
    if opc in STRING_OPS and ci.opcode[1] == 0 and not (len(raw) > 1 and _is_0f(raw)):
        string_op = STRING_OPS[opc]
        if 0xF3 in pfx:
            rep = "rep"
        elif 0xF2 in pfx:
            rep = "repne"
    x87 = None
    if 0xD8 <= opc <= 0xDF and not _is_0f(raw):
        modrm = ci.modrm
        x87 = (opc, modrm)
    seg = None
    for p, s in ((0x64, "fs"), (0x65, "gs"), (0x2E, "cs"), (0x36, "ss"), (0x3E, "ds"), (0x26, "es")):
        if p in pfx:
            seg = s
    ops = [_op(ci, o) for o in ci.operands]
    return Insn(addr=ci.address, size=ci.size, mnem=mnem, ops=ops, text=f"{ci.mnemonic} {ci.op_str}".strip(),
                raw=raw, rep=rep, lock=lock, opsize16=0x66 in pfx, string_op=string_op, x87=x87,
                seg_override=seg)


def _is_0f(raw):
    i = 0
    while i < len(raw) and raw[i] in (0x66, 0x67, 0xF2, 0xF3, 0xF0, 0x2E, 0x36, 0x3E, 0x26, 0x64, 0x65):
        i += 1
    return i < len(raw) and raw[i] == 0x0F


def reg_base(name):
    """eax/ax/al/ah -> ('eax', kind) где kind: 32, 16, 'lo', 'hi'."""
    if name in REG32:
        return name, 32
    if name in REG16:
        return REG16[name], 16
    if name in REG8L:
        return REG8L[name], "lo"
    if name in REG8H:
        return REG8H[name], "hi"
    return None, None
