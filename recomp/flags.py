"""Флаги EFLAGS: кто читает/пишет, живость и решения «слить с потребителем» / «материализовать»."""
C, P, A, Z, S, O = 1, 2, 4, 8, 16, 32
ALL = C | P | A | Z | S | O
NAMES = {C: "cf", P: "pf", A: "af", Z: "zf", S: "sf", O: "of"}

CC_READS = {
    "o": O, "no": O, "b": C, "ae": C, "e": Z, "ne": Z, "be": C | Z, "a": C | Z,
    "s": S, "ns": S, "p": P, "np": P, "l": S | O, "ge": S | O, "le": Z | S | O, "g": Z | S | O,
}
CC_ALIASES = {"c": "b", "nc": "ae", "nae": "b", "nb": "ae", "z": "e", "nz": "ne", "na": "be", "nbe": "a",
              "pe": "p", "po": "np", "nge": "l", "nl": "ge", "ng": "le", "nle": "g"}

FUSABLE = {"sub", "add", "logic", "inc", "dec"}


def cc_of(mnem, prefix):
    cc = mnem[len(prefix):]
    return CC_ALIASES.get(cc, cc)


def flag_info(ins):
    """-> (read, write, kind). kind — способ вычисления флагов для слияния/материализации."""
    m = ins.mnem
    ops = ins.ops
    if m in ("add", "xadd"):
        return 0, ALL, "add"
    if m in ("sub", "cmp", "cmpxchg"):
        return 0, ALL, "sub"
    if m == "neg":
        return 0, ALL, "sub"
    if m in ("adc",):
        return C, ALL, "adc"
    if m in ("sbb",):
        return C, ALL, "sbb"
    if m in ("and", "or", "xor", "test"):
        return 0, ALL, "logic"
    if m == "inc":
        return 0, P | A | Z | S | O, "inc"
    if m == "dec":
        return 0, P | A | Z | S | O, "dec"
    if m in ("shl", "sal", "shr", "sar"):
        if ops[1].kind == "i":
            if ops[1].imm & 31 == 0:
                return 0, 0, None
            return 0, ALL, "shift"
        return ALL, ALL, "shift"
    if m in ("rol", "ror"):
        if ops[1].kind == "i":
            return (0, C | O, "rot") if ops[1].imm & 31 else (0, 0, None)
        return C | O, C | O, "rot"
    if m in ("rcl", "rcr"):
        return C | O, C | O, "rot"
    if m in ("shld", "shrd"):
        if ops[2].kind == "i":
            return (0, ALL, "shift") if ops[2].imm & 31 else (0, 0, None)
        return ALL, ALL, "shift"
    if m in ("mul", "imul", "div", "idiv"):
        return 0, ALL, "other"
    if m in ("bt", "bts", "btr", "btc"):
        return 0, C | O | S | A | P, "other"
    if m in ("bsf", "bsr"):
        return 0, ALL, "other"
    if m in ("clc", "stc"):
        return 0, C, "other"
    if m == "cmc":
        return C, C, "other"
    if m == "sahf":
        return 0, C | P | A | Z | S, "other"
    if m == "lahf":
        return C | P | A | Z | S, 0, None
    if m in ("pushfd", "pushf"):
        return ALL, 0, None
    if m in ("popfd", "popf"):
        return 0, ALL, "other"
    if m in ("comiss", "ucomiss", "comisd", "ucomisd"):
        return 0, ALL, "other"
    if ins.string_op in ("cmps", "scas"):
        return (ALL, ALL, "other") if ins.rep else (0, ALL, "sub")
    if m.startswith("j") and cc_of(m, "j") in CC_READS:
        return CC_READS[cc_of(m, "j")], 0, "cond"
    if m.startswith("set") and cc_of(m, "set") in CC_READS:
        return CC_READS[cc_of(m, "set")], 0, "cond"
    if m.startswith("cmov") and cc_of(m, "cmov") in CC_READS:
        return CC_READS[cc_of(m, "cmov")], 0, "cond"
    if m in ("loope", "loopne"):
        return Z, 0, "cond"
    if m == "salc":
        return C, 0, None
    if m in ("daa", "das", "aaa", "aas", "aam", "aad"):
        return ALL, ALL, "other"
    if m == "call":
        return 0, ALL, "clobber"
    if ins.x87:
        opc, modrm = ins.x87
        if (opc in (0xDB, 0xDF) and (modrm >> 6) == 3 and ((modrm >> 3) & 7) in (5, 6)):
            return 0, ALL, "other"  # fcomi/fucomi(p)
        if opc in (0xDA, 0xDB) and (modrm >> 6) == 3 and ((modrm >> 3) & 7) <= 3 and modrm not in (0xE9,):
            reads = {0: C, 1: Z, 2: C | Z, 3: P}[(modrm >> 3) & 7]
            return reads, 0, "cond"  # fcmovcc
    return 0, 0, None


def jcc_cc(ins):
    m = ins.mnem
    for p in ("j", "set", "cmov"):
        if m.startswith(p):
            cc = cc_of(m, p)
            if cc in CC_READS:
                return cc
    if m == "loope":
        return "e"
    if m == "loopne":
        return "ne"
    return None


def liveness(addrs, succ, finfo):
    """Обратная живость по инструкциям. -> live_out: addr -> битовая маска."""
    live_in = {a: 0 for a in addrs}
    live_out = {a: 0 for a in addrs}
    preds = {a: [] for a in addrs}
    for a in addrs:
        for s in succ[a]:
            if s in preds:
                preds[s].append(a)
    work = list(addrs)
    inwork = set(work)
    while work:
        a = work.pop()
        inwork.discard(a)
        out = 0
        for s in succ[a]:
            out |= live_in.get(s, 0)
        r, w, _ = finfo[a]
        new_in = r | (out & ~w)
        live_out[a] = out
        if new_in != live_in[a]:
            live_in[a] = new_in
            for p in preds[a]:
                if p not in inwork:
                    inwork.add(p)
                    work.append(p)
    return live_in, live_out, preds


def plan(addrs, succ, finfo, live_in, live_out, preds, fusable_kinds=FUSABLE):
    """Для каждого сеттера — какие флаги материализовать; для потребителей — с каким сеттером слиты.

    -> (materialize: addr -> mask, fused: consumer_addr -> setter_addr, setter_used: set сеттеров,
        чьи fa/fb/fr нужны)
    """
    materialize = {}
    fused = {}
    used = set()
    for s in addrs:
        r, w, kind = finfo[s]
        if not w or kind == "clobber":
            continue
        pending = live_out[s] & w
        if not pending:
            continue
        mat = 0
        clean = True
        cur = s
        steps = 0
        while pending:
            steps += 1
            if steps > 64:
                mat |= pending
                break
            seq = succ[cur]
            if not seq:
                break
            # продолжение прямой цепочки: первый преемник (fallthrough идёт первым) с единственным предшественником
            cont = None
            for x in seq:
                if len(preds.get(x, ())) == 1 and x in finfo:
                    cont = x
                    break
            for x in seq:
                if x != cont:
                    mat |= live_in.get(x, 0) & pending
            if cont is None:
                break
            j = cont
            jr, jw, jkind = finfo[j]
            if jr & pending:
                if clean and kind in fusable_kinds and (jr & ~w) == 0 and jkind == "cond":
                    fused[j] = s
                    used.add(s)
                else:
                    mat |= jr & pending
            if jw:
                clean = False
                pending &= ~jw
            cur = j
        if mat:
            materialize[s] = mat
            used.add(s)
    return materialize, fused, used
