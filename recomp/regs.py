"""Сводки по регистрам гостя для мест вызова: touched (что функция может тронуть) и out (что может измениться).

Тело функции грузит все регистры из CPU на входе и пишет в CPU на выходе. Раньше место вызова писало в CPU все
восемь (SPILL) и все восемь читало обратно (RELOAD). Теперь (emit.FuncCompiler, «чистые» регистры):
* перед вызовом пишутся только «грязные» регистры (локальная копия изменилась после последней записи/чтения CPU)
  из touched(цели) — регистр вне touched ни цель, ни её вызываемые не читают и не пишут, в CPU у неё лежит старое
  значение, его она и вернёт;
* после вызова читаются только out(цели); регистр вне out цель вернула таким же, каким получила (в CPU он равен
  локальной копии вызывающего) — ebx/esi/edi/ebp остаются в регистрах хоста.

* touched(F) — чтения и записи тела (effects) и touched вызываемых; незнакомая цель (HLE, оверрайд, косвенный
  вызов, setjmp) — все восемь.
* out(F) — eax/ecx/edx: записи тела и out вызываемых; ebx/esi/edi/ebp: не сохранённые хотя бы на одном выходе
  (поток сохранения по CFG — RegAnalysis). Незнакомая цель меняет eax/ecx/edx/esp: ABI MSVC сохраняет
  ebx/esi/edi/ebp (HLE — наш код, его RET пишет eax/esp; косвенные вызовы проверяет SNX_CHECK_ABI=1 в рантайме).
* Не разбираются (touched = out = все): функции с catch/__except и продолжения EH, setjmp, недокодированные.

Сводки — наибольшая неподвижная точка от «все регистры» вниз: любая промежуточная итерация — надмножество точного
ответа, обрывать итерации безопасно. Подробности и доказательство — AGENTS.md, «Сводки по регистрам».

SNX_REGS=0 при генерации — полный SPILL/RELOAD на каждом вызове (как до сводок, для сравнения сборок);
SNX_REGCHECK=1 — проверка out в рантайме (emit: RT_REGCHK после каждого прямого вызова).
"""
import os

from discover import is_nop_like
from x86 import REG32, reg_base

BIT = {r: 1 << i for i, r in enumerate(REG32)}  # порядок — как в CPU
ALL = 0xFF
ESP = BIT["esp"]
EAX = BIT["eax"]
ECX = BIT["ecx"]
EDX = BIT["edx"]
EBP = BIT["ebp"]
CALLEE_SAVED = BIT["ebx"] | BIT["esi"] | BIT["edi"] | EBP

# HLE (runtime/hle/*.c) и оверрайды (runtime/overrides_*.c) — наш C: возврат пишет eax/esp (edx, st_ret);
# ebx/esi/edi/ebp не пишет никто (RaiseException/RtlUnwind восстанавливают снимок). Косвенный вызов — ABI.
ABI_OUT = EAX | ECX | EDX | ESP
INTRINSIC_WRITES = {"_ftol": EAX | EDX, "_ftol2": EAX | EDX, "_ftol2_sse": EAX | EDX}  # emit.emit_intrinsic

ENABLED = os.environ.get("SNX_REGS") != "0"
CHECK = os.environ.get("SNX_REGCHECK") == "1"

RW_DST = {"inc", "dec", "neg", "not", "bswap", "bts", "btr", "btc", "rcl", "rcr", "xadd", "shld", "shrd"}
RD_DST = {"add", "adc", "sub", "sbb", "and", "or", "xor", "cmp", "test", "shl", "sal", "shr", "sar", "rol", "ror",
          "imul", "bt", "bsf", "bsr"}
MOV = {"mov", "movzx", "movsx"}
NOPISH = {"nop", "wait", "fwait", "cld", "std", "clc", "stc", "cmc", "hlt", "ud2", "int3", "into", "int1", "emms",
          "femms", "pause", "prefetchnta", "prefetcht0", "prefetcht1", "prefetcht2", "prefetch", "prefetchw",
          "sfence", "lfence", "mfence"}
SIMD = {
    "addpd", "addps", "addsd", "addss", "andnps", "andpd", "andps", "cmpeqsd", "cmpeqps", "cmpleps", "cmpltpd",
    "cmpltps", "cmpltss", "cmpneqpd", "cmpneqps", "cmpnlepd", "cmpnleps", "cmpnltps", "comisd", "comiss",
    "cvtdq2pd", "cvtpd2ps", "cvtpi2ps", "cvtps2pd", "cvtps2pi", "cvtsd2si", "cvtsi2sd", "cvttsd2si", "cvtdq2ps",
    "cvtsi2ss", "cvtss2si", "cvttss2si", "cvtss2sd", "cvtsd2ss", "cvttps2pi",
    "divpd", "divps", "divsd", "divss", "ldmxcsr", "maxpd", "maxps", "minpd", "minps", "movapd", "movaps",
    "movd", "movdqa", "movdqu", "movhlps", "movhpd", "movhps", "movlhps", "movlpd", "movlps", "movmskpd",
    "movmskps", "movntdq", "movnti", "movntps", "movntq", "movq", "movsd", "movss", "movups", "mulpd", "mulps",
    "mulsd", "mulss", "orpd", "orps", "packssdw", "packsswb", "packuswb", "paddb", "paddd", "paddq", "paddsb",
    "paddsw", "paddusb", "paddusw", "paddw", "pand", "pandn", "pavgb", "pcmpeqb", "pcmpeqd", "pcmpeqw",
    "pcmpgtb", "pcmpgtd", "pcmpgtw", "pextrw", "pf2id", "pf2iw", "pfacc", "pfadd", "pfcmpeq", "pfcmpge",
    "pfcmpgt", "pfmax", "pfmin", "pfmul", "pfnacc", "pfpnacc", "pfrcp", "pfrcpit1", "pfrcpit2", "pfrsqit1",
    "pfrsqrt", "pfsub", "pfsubr", "pi2fd", "pi2fw", "pinsrw", "pmaddwd", "pmaxsw", "pmaxub", "pminsw",
    "pminub", "pmovmskb", "pmulhuw", "pmullw", "pmuludq", "por", "psadbw", "pshufd", "pshufw", "pslld",
    "psllq", "psllw", "psrad", "psraw", "psrld", "psrlq", "psrlw", "psubb", "psubd", "psubq", "psubsb",
    "psubsw", "psubusb", "psubusw", "psubw", "pswapd", "punpckhbw", "punpckhdq", "punpckhwd", "punpcklbw",
    "punpckldq", "punpcklwd", "pxor", "rcpps", "rcpss", "rsqrtps", "rsqrtss", "shufpd", "shufps", "sqrtpd",
    "sqrtps", "sqrtsd", "sqrtss", "subpd", "subps", "subsd", "subss", "ucomisd", "ucomiss", "unpckhpd",
    "unpckhps", "unpcklpd", "unpcklps", "xorpd", "xorps", "movddup", "movshdup", "movsldup", "stmxcsr",
}


def gpr(o):
    """Имя 32-битного регистра операнда-регистра общего назначения (al/ax/eax -> eax) или None."""
    if o is None or o.kind != "r":
        return None
    return reg_base(o.reg)[0]


def reg32(o):
    """Бит регистра, если операнд — 32-битный регистр общего назначения целиком, иначе 0."""
    if o is None or o.kind != "r" or o.reg not in BIT:
        return 0
    return BIT[o.reg]


def effects(ins):
    """-> (reads, writes): маски регистров, которые инструкция может прочитать и записать (с неявными).

    Должна покрывать всё, что делает с регистрами сгенерированный для инструкции C (emit): по записям
    эмиттер решает, какие локальные копии «грязные». Незнакомое — (ALL, ALL).
    """
    r = w = 0
    for o in ins.ops:  # адресные регистры операндов в памяти
        if o.kind == "m":
            for x in (o.base, o.index):
                if x in BIT:
                    r |= BIT[x]
    m = ins.mnem
    ops = ins.ops
    if is_nop_like(ins):  # заполнители выравнивания MSVC: lea r, [r], mov r, r
        return 0, 0
    if ins.string_op:
        rep = ECX if ins.rep else 0
        si, di = BIT["esi"], BIT["edi"]
        op = ins.string_op
        if op == "movs":
            return r | si | di | rep, w | si | di | rep
        if op == "stos":
            return r | EAX | di | rep, w | di | rep
        if op == "lods":
            return r | EAX | si | rep, w | si | EAX | rep
        return r | EAX | di | si | rep, w | di | si | rep  # scas/cmps
    if ins.x87:
        if ins.x87 == (0xDF, 0xE0) or (m in ("fnstsw", "fstsw") and ops and gpr(ops[0])):
            return r | EAX, w | EAX  # fnstsw ax
        return r, w
    if m in ("fnstsw", "fstsw"):  # с префиксом wait capstone может отдать без опкода x87
        if ops and gpr(ops[0]):
            return r | EAX, w | EAX
        return r, w
    if m in MOV:
        if len(ops) > 1 and gpr(ops[1]):
            r |= BIT[gpr(ops[1])]
        if ops and gpr(ops[0]):
            w |= BIT[gpr(ops[0])]
        return r, w
    if m == "lea":
        if ops and gpr(ops[0]):
            w |= BIT[gpr(ops[0])]
        return r, w
    if m == "xchg":
        for o in ops:
            if gpr(o):
                r |= BIT[gpr(o)]
                w |= BIT[gpr(o)]
        return r, w
    if m == "push":
        if ops and gpr(ops[0]):
            r |= BIT[gpr(ops[0])]
        return r | ESP, w | ESP
    if m == "pop":
        if ops and gpr(ops[0]):
            w |= BIT[gpr(ops[0])]
        return r | ESP, w | ESP
    if m in ("pushal", "pushad"):
        return r | ALL, w | ESP
    if m in ("popal", "popad"):
        return r | ESP, w | ALL
    if m == "leave":
        return r | EBP | ESP, w | EBP | ESP
    if m == "enter":
        return r | ESP | EBP, w | ESP | EBP
    if m in ("mul", "div", "idiv") or (m == "imul" and len(ops) == 1):
        for o in ops:
            if gpr(o):
                r |= BIT[gpr(o)]
        return r | EAX | EDX, w | EAX | EDX
    if m == "imul":
        for o in ops:
            if gpr(o):
                r |= BIT[gpr(o)]
        if gpr(ops[0]):
            w |= BIT[gpr(ops[0])]
        return r, w
    if m in ("cdq", "cwd"):
        return r | EAX | EDX, w | EDX
    if m in ("cwde", "cbw"):
        return r | EAX, w | EAX
    if m == "cmpxchg":
        r |= EAX
        for o in ops:
            if gpr(o):
                r |= BIT[gpr(o)]
        w |= EAX
        if gpr(ops[0]):
            w |= BIT[gpr(ops[0])]
        return r, w
    if m in RW_DST:
        for i, o in enumerate(ops):
            if gpr(o):
                r |= BIT[gpr(o)]
                if i == 0 or m == "xadd":
                    w |= BIT[gpr(o)]
        return r, w
    if m in RD_DST or m.startswith("cmov"):
        for o in ops:
            if gpr(o):
                r |= BIT[gpr(o)]
        if m not in ("cmp", "test", "bt") and ops and gpr(ops[0]):
            w |= BIT[gpr(ops[0])]
        return r, w
    if m.startswith("set"):
        if ops and gpr(ops[0]):
            w |= BIT[gpr(ops[0])]
        return r, w
    if m in ("loop", "loope", "loopne"):
        return r | ECX, w | ECX
    if m == "jecxz":
        return r | ECX, w
    if m == "jmp" or ins.is_jcc():
        for o in ops:
            if gpr(o):
                r |= BIT[gpr(o)]
        return r, w
    if m in ("call", "ret"):  # цель вызова — отдельно (RegAnalysis), здесь только адрес/регистр и esp
        for o in ops:
            if gpr(o):
                r |= BIT[gpr(o)]
        return r | ESP, w | ESP
    if m == "cpuid":
        return r | EAX | ECX, w | EAX | BIT["ebx"] | ECX | EDX
    if m == "rdtsc":
        return r, w | EAX | EDX
    if m == "sahf":
        return r | EAX, w
    if m in ("lahf", "salc"):
        return r | EAX, w | EAX
    if m == "xlatb":
        return r | EAX | BIT["ebx"], w | EAX
    if m in ("pushfd", "pushf", "popfd", "popf"):
        return r | ESP, w | ESP
    if m in NOPISH:
        return r, w
    if m in SIMD:
        # регистры общего назначения у SIMD — адресные (учтены выше) или обмен с xmm/mm (movd, pextrw, cvt*si, ...)
        if ops and gpr(ops[0]):
            w |= BIT[gpr(ops[0])]
        for o in ops[1:]:
            if gpr(o):
                r |= BIT[gpr(o)]
        return r, w
    return ALL, ALL


VOLATILE = EAX | ECX | EDX

# Хелперы SEH MSVC (статические копии в каждом модуле): __SEH_prolog сохраняет ebp, ebx, esi, edi вызывающего в его
# кадре и ставит ebp на кадр, __SEH_epilog их восстанавливает (перед ret вызывающего). Для потока сохранения вызов —
# как push/pop этих регистров в самой функции.
SEH_PROLOG = ["mov eax, dword ptr fs:[0]", "push eax", "mov eax, dword ptr [esp + 0x10]",
              "mov dword ptr [esp + 0x10], ebp", "lea ebp, [esp + 0x10]", "sub esp, eax", "push ebx", "push esi",
              "push edi"]  # после push offset _except_handler3
SEH_EPILOG = ["mov ecx, dword ptr [ebp - 0x10]", "mov dword ptr fs:[0], ecx", "pop ecx", "pop edi", "pop esi",
              "pop ebx", "leave", "push ecx", "ret"]


def merge(a, b):
    """Слияние состояний сохранения (E, Sc, Sd) на стыке путей: разные — X (кроме Sc/Sd -> Sd)."""
    sc = a[1] & b[1]
    return a[0] & b[0], sc, (a[1] | a[2]) & (b[1] | b[2]) & ~sc


class RegAnalysis:
    """Сводки по всем функциям программы.

    Сохранение ebx/esi/edi/ebp — прямой поток данных по CFG тела, состояние регистра:
    E — равен входному, не сохранён; Sc — входное сохранено push и регистр не менялся; Sd — сохранено, регистр
    мог измениться; X — потерян. push R: E -> Sc (push в Sc/Sd — аргумент, без изменений); запись: E -> X,
    Sc -> Sd; вызов — запись out цели; pop R в эпилоге (дальше без ветвлений и вызовов до выхода) и leave:
    Sc/Sd -> E, E -> X; pop R посреди тела — X. На выходе (ret; хвостовой переход — после out цели) регистр
    сохранён, если он E или Sc на каждом выходе. Опора — дисциплина стека: pop эпилога снимает слот push пролога.
    """

    def __init__(self, ctx, log=print):
        self.ctx = ctx
        self.log = log
        self.skip = set()
        self.preserved = {}  # вход -> ebx/esi/edi/ebp, равные входным на каждом выходе
        self.local_rw = {}  # вход -> чтения и записи тела (без вызываемых)
        self.local_w = {}  # вход -> записи тела
        self.callees = {}  # вход -> [адрес цели] сгенерированных функций
        self.unknown = {}  # вход -> вклад незнакомых целей в (touched, out)
        self.flow = {}  # вход -> (succ, ops, exits) для потока сохранения
        self._helper = {}

    # ------------------------------------------------------------ цели вызовов
    def target(self, info):
        """-> адрес сгенерированной функции-цели или (touched, out) незнакомой цели; None — интринсик."""
        if info.intrinsic:
            return None
        if info.addr is not None:
            return info.addr
        return (ALL, ALL) if info.setjmp else (ALL, ABI_OUT)  # HLE, оверрайд, косвенный, «не функция»

    def callish(self, mod, f, x):
        """Инструкция — вызов или хвостовой переход (эффекты цели — через target)."""
        if x.mnem == "call":
            return True
        if x.mnem == "jmp" or x.is_jcc() or x.is_loop():
            t = x.branch_target()
            if t is None:
                return x.mnem == "jmp" and x.addr not in mod.jump_tables
            return mod.is_tail(f.entry, x, t)
        return False

    def is_exit(self, mod, f, x):
        """ret или безусловный хвостовой переход (на сильный вход, `jmp [IAT]`)."""
        if x.mnem == "ret":
            return True
        if x.mnem != "jmp":
            return False
        t = x.branch_target()
        if t is None:
            return x.addr not in mod.jump_tables
        return mod.is_tail(f.entry, x, t)

    def epilogue(self, mod, f, succ, a):
        """После инструкции a до выхода — без ветвлений (pop/leave на этом пути — восстановление, а не снятие
        аргумента). Прямые вызовы можно: __security_check_cookie (/GS) между pop и leave/ret; если вызов меняет
        регистр, поток сохранения это учтёт."""
        for _ in range(24):
            nx = succ.get(a, [])
            if len(nx) != 1:
                return False
            a = nx[0]
            x = mod.insn(a)
            if self.is_exit(mod, f, x):
                return True
            if x.is_jcc() or x.is_loop() or (x.mnem in ("call", "jmp") and x.branch_target() is None):
                return False
        return False

    def helper(self, t):
        """'sehp' / 'sehe' — функция по адресу t есть __SEH_prolog / __SEH_epilog, иначе None."""
        if t not in self._helper:
            mod = self.ctx.module_for(t)
            kind = None
            if mod is not None:
                seq, a = [], t
                for _ in range(len(SEH_PROLOG) + 1):
                    x = mod.insn(a)
                    if x is None:
                        break
                    seq.append(x.text)
                    a = x.next
                if seq[1:] == SEH_PROLOG and seq[0].startswith("push 0x"):
                    kind = "sehp"
                elif seq[:len(SEH_EPILOG)] == SEH_EPILOG:
                    kind = "sehe"
            self._helper[t] = kind
        return self._helper[t]

    # ------------------------------------------------------------ проходы
    def mark_skip(self, prog):
        for mod in prog.modules.values():
            for e, f in mod.functions.items():
                bad = f.invalid > 0 or any(mod.insn(a) is None for a in f.insns)
                if bad or e in mod.eh_targets or e in mod.eh_conts or self.ctx.is_special(e):
                    self.skip.add(e)

    def local_pass(self, mod):
        from emit import FuncCompiler  # emit импортирует regs
        for e, f in mod.functions.items():
            if e in self.skip:
                self.local_rw[e] = self.local_w[e] = ALL
                self.callees[e] = []
                self.unknown[e] = (ALL, ALL)
                continue
            fc = FuncCompiler(mod, f, self.ctx)
            succ = {a: [s for s in fc.succ_of(fc.ins[a]) if s in fc.ins] for a in f.insns}
            preds = {a: [] for a in f.insns}
            for a in f.insns:
                for s in succ[a]:
                    preds[s].append(a)
            idx = {a: i for i, a in enumerate(f.insns)}
            ops = []  # на инструкцию: None | ('w', маска) | ('push'|'epop'|'kill', бит) | ('call', цель)
            exits = []  # (номер инструкции, цель хвостового перехода или None)
            rw = wr = 0
            callees = []
            ut = uo = 0
            for i, a in enumerate(f.insns):
                x = mod.insn(a)
                r, w = effects(x)
                rw |= r | w
                wr |= w
                op = ("w", w & CALLEE_SAVED) if w & CALLEE_SAVED else None
                m = x.mnem
                if m == "push":
                    b = reg32(x.ops[0]) & CALLEE_SAVED
                    if b:
                        op = ("push", b)
                elif m == "pop" or m == "leave":
                    b = EBP if m == "leave" else reg32(x.ops[0]) & CALLEE_SAVED
                    if b and m == "pop" and len(preds[a]) == 1 and mod.insn(preds[a][0]).mnem == "push" \
                            and succ[preds[a][0]] == [a]:
                        op = ("w", b)  # push X; pop R — это mov R, X (MSVC /O1: push 2; pop edi)
                    elif b:
                        op = ("epop" if self.epilogue(mod, f, succ, a) else "kill", b)
                    elif w & CALLEE_SAVED:
                        op = ("kill", w & CALLEE_SAVED)  # pop bx и т. п.
                tail = None
                if self.callish(mod, f, x):
                    info = self.ctx.call_info(mod, x)
                    t = self.target(info)
                    if t is None:
                        w = INTRINSIC_WRITES.get(info.intrinsic, 0)
                        rw |= w
                        wr |= w
                        if m != "call":
                            exits.append((i, None))  # jmp на интринсик: исполнить и вернуться (emit.tail_intrinsic)
                    else:
                        if isinstance(t, tuple):
                            ut |= t[0]
                            uo |= t[1]
                        else:
                            callees.append(t)
                        if m == "call":
                            op = ("call", t)
                            if not isinstance(t, tuple) and self.helper(t):
                                op = (self.helper(t), CALLEE_SAVED)
                        elif x.branch_target() is None and mod.iat_import(x) is None:
                            # jmp reg/[mem] не по таблице: продолжение этой же функции (вычисляемый переход memcpy
                            # Core) или уход в чужой кадр (longjmp, _JumpToContinuation) — см. preserved_regs
                            tail = "ijmp"
                        else:
                            tail = t
                if m == "ret" or tail is not None:
                    exits.append((i, tail))
                ops.append(op)
            self.flow[e] = ([[idx[s] for s in succ[a]] for a in f.insns], ops, exits, idx[f.entry])
            self.local_rw[e] = rw | ESP
            self.local_w[e] = wr | ESP
            self.callees[e] = callees
            self.unknown[e] = (ut, uo)

    def out_of(self, t):
        if isinstance(t, tuple):
            return t[1]
        return self.ctx.reg_out.get(t, ALL)

    def apply(self, op, st):
        if op is None:
            return st
        e, sc, sd = st
        k, v = op
        if k == "call":
            k, v = "w", self.out_of(v) & CALLEE_SAVED
        if k == "w":
            return e & ~v, sc & ~v, sd | (sc & v)
        if k == "push":
            return (e & ~v, sc | (e & v), sd) if e & v else st
        if k == "epop":
            if (sc | sd) & v:
                return e | v, sc & ~v, sd & ~v
            return e & ~v, sc, sd
        if k == "sehp":  # push ebp; ebp = кадр; push ebx, esi, edi
            sc |= e & v
            e &= ~v
            if sc & EBP:
                sc, sd = sc & ~EBP, sd | EBP
            return e, sc, sd
        if k == "sehe":  # pop edi, esi, ebx; leave
            back = (sc | sd) & v
            return (e & ~v) | back, sc & ~v, sd & ~v
        return e & ~v, sc & ~v, sd & ~v  # kill

    def preserved_regs(self, e):
        """Поток сохранения по телу с текущими out вызываемых -> ebx/esi/edi/ebp, равные входным на выходах."""
        succ, ops, exits, entry = self.flow[e]
        if not exits:
            return CALLEE_SAVED  # не возвращается (throw, ExitProcess): код вызывающего после вызова недостижим
        state = [None] * len(ops)
        state[entry] = (CALLEE_SAVED, 0, 0)
        work = [entry]
        while work:
            i = work.pop()
            st = self.apply(ops[i], state[i])
            for j in succ[i]:
                old = state[j]
                new = st if old is None else merge(old, st)
                if new != old:
                    state[j] = new
                    work.append(j)
        pres = CALLEE_SAVED
        for i, tail in exits:
            st = state[i]
            if st is None:
                continue  # недостижимый выход
            e, sc, sd = st
            if tail is None:  # ret
                pres &= e | sc
                continue
            # переход в другую функцию: E — цель может поменять (out цели); сохранённые (Sc/Sd) ещё лежат в стеке —
            # значит, это не хвостовой вызов (цель увидела бы чужой адрес возврата), а продолжение этой функции
            # (вычисляемый переход memcpy Core, куски, ставшие отдельными функциями), его эпилог их и снимет
            o = 0 if tail == "ijmp" else self.out_of(tail) & CALLEE_SAVED
            pres &= (e & ~o) | sc | sd
        return pres

    def out_fixpoint(self):
        """out(F) = (записи тела | out вызываемых) & ~сохранённые; сверху вниз (от ALL), по очереди изменённых."""
        out = self.ctx.reg_out
        callers = {}
        for e, cs in self.callees.items():
            for t in cs:
                callers.setdefault(t, set()).add(e)
        for e in self.local_w:
            out[e] = ALL
        work = list(self.local_w)
        queued = set(work)
        evals = 0
        while work:
            e = work.pop()
            queued.discard(e)
            if e in self.skip:
                continue
            evals += 1
            vol = self.local_w[e] | self.unknown[e][1]
            for t in self.callees[e]:
                vol |= out.get(t, ALL)
            pres = self.preserved_regs(e)
            self.preserved[e] = pres
            new = (vol & VOLATILE) | (CALLEE_SAVED & ~pres) | ESP
            if new != out[e]:
                out[e] = new
                for c in callers.get(e, ()):
                    if c not in queued:
                        queued.add(c)
                        work.append(c)
        return evals

    def touched_fixpoint(self, max_iter=32):
        """touched(F) = чтения/записи тела | out(F) | touched вызываемых; сверху вниз (от ALL)."""
        tab, out = self.ctx.reg_touched, self.ctx.reg_out
        order = list(self.local_w)
        for e in order:
            tab[e] = ALL
        for it in range(1, max_iter + 1):
            changed = 0
            for e in order:
                acc = self.local_rw[e] | self.unknown[e][0] | out[e] | ESP
                if acc != ALL:
                    for t in self.callees[e]:
                        acc |= tab.get(t, ALL)
                if tab[e] != acc:
                    tab[e] = acc
                    changed += 1
            if not changed:
                return it
        return max_iter

    def run(self, prog):
        self.mark_skip(prog)
        for mod in prog.modules.values():
            self.local_pass(mod)
        for e in self.skip:
            self.preserved[e] = 0
        return self.out_fixpoint(), self.touched_fixpoint()


def compute(prog, ctx, log=print):
    if not ENABLED:
        log("regs: SNX_REGS=0 — полный SPILL/RELOAD на каждом вызове")
        return None
    ra = RegAnalysis(ctx, log)
    n_out, n_t = ra.run(prog)
    funcs = len(ra.local_w)
    cnt_out = {r: sum((m >> i) & 1 for m in ctx.reg_out.values()) for i, r in enumerate(REG32)}
    small = sum(1 for m in ctx.reg_touched.values() if m != ALL)
    log(f"regs: {funcs} функций, {len(ra.skip)} без сводок; разборов out {n_out}, итераций touched {n_t}; "
        f"touched не все у {small}; out содержит: " + " ".join(f"{r}={cnt_out[r]}" for r in REG32))
    return ra
