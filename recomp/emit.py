"""Генерация C из функции x86-32.

Модель:
* регистры гостя — локальные u32 (eax..edi), сбрасываются в CPU на вызовах/выходах (SPILL/RELOAD);
* гостевая память — g_mem + addr (локальная копия m);
* флаги — сливаются с jcc/setcc/cmovcc (fa/fb/fr сеттера) или материализуются в cf/zf/... по живости;
* x87 — статическая глубина стека, ST(i) — локальные double f0..f7.
"""
import os

from discover import TERMINATORS
from flags import (ALL, A, C, CC_READS, FUSABLE, NAMES, O, P, S, Z, flag_info, jcc_cc, liveness, plan)
import regs
from x86 import REG32, reg_base
from x87 import X87_INTRINSICS, decode_x87

MASK = {1: 0xFF, 2: 0xFFFF, 4: 0xFFFFFFFF}
SIGNED = {1: "s8", 2: "s16", 4: "s32"}
UNSIGNED = {1: "u8", 2: "u16", 4: "u32"}
SIGNBIT = {1: 0x80, 2: 0x8000, 4: 0x80000000}
# [рег ± d] с |d| меньше — поле структуры или слот кадра: адрес без заворота (FuncCompiler.ha); больше — адрес таблицы
SMALL_DISP = 0x10000
REP_INLINE_MAX = 16  # согласовано с REP_INLINE_MAX в runtime/gen_rt.h


def fpu_single_mode():
    """Режим 24-битной мантиссы x87 (SNX_FPU_SINGLE=1, как на ПК после создания D3D8 без FPU_PRESERVE)."""
    return os.environ.get("SNX_FPU_SINGLE") == "1"


def fpu_float32_mode():
    """Нативные float f0..f7 в чистых f32-функциях (опция SNX_FPU_FLOAT32=1 для Switch/релиза).
    По умолчанию выключена (0), чтобы не давать 24-бит vs 53-бит расхождений в difftest (same_float 1e-12)."""
    if fpu_single_mode():
        return False
    return os.environ.get("SNX_FPU_FLOAT32") == "1"


# Биты отношения (fx, fy) для слияния x87-сравнений: 1=GT (fx>fy), 2=LT (fx<fy), 4=EQ (fx==fy), 8=UN (NaN).
# Каждая комбинация отображается в точное C-выражение по семантике IEEE-754 (без PARITY/NEON-popcnt).
_X87_REL_EXPR = {
    0x0: "0",
    0x1: "(fx > fy)",
    0x2: "(fx < fy)",
    0x3: "(fx < fy || fx > fy)",
    0x4: "(fx == fy)",
    0x5: "(fx >= fy)",
    0x6: "(fx <= fy)",
    0x7: "(!__builtin_isunordered(fx, fy))",
    0x8: "(__builtin_isunordered(fx, fy))",
    0x9: "(!(fx <= fy))",
    0xA: "(!(fx >= fy))",
    0xB: "(fx != fy)",
    0xC: "(!(fx < fy) && !(fx > fy))",
    0xD: "(!(fx < fy))",
    0xE: "(!(fx > fy))",
    0xF: "1",
}

# Значения AH = fsw >> 8 после FCOM(fx, fy): GT=0x00, LT=0x01 (C0), EQ=0x40 (C3), UN=0x45 (C3|C2|C0).
_X87_AH_RELS = ((1, 0x00), (2, 0x01), (4, 0x40), (8, 0x45))


def _eval_cc(cc, cf, pf, zf, sf, of):
    return {
        "o": bool(of), "no": not of, "b": bool(cf), "ae": not cf,
        "e": bool(zf), "ne": not zf, "be": bool(cf or zf), "a": not (cf or zf),
        "s": bool(sf), "ns": not sf, "p": bool(pf), "np": not pf,
        "l": sf != of, "ge": sf == of, "le": bool(zf or (sf != of)), "g": (not zf) and (sf == of),
    }[cc]


def _x87_test_cond(mask, cc):
    """Точное C-условие для `fcom*; fnstsw ax; test ah, mask; jcc/setcc/cmovcc` (при mask & ~0x45 == 0)."""
    if (mask & ~0x45) != 0 or cc not in CC_READS:
        return None
    bits = 0
    for rel_bit, ah in _X87_AH_RELS:
        v = ah & mask
        zf = int(v == 0)
        pf = int(bin(v).count("1") % 2 == 0)
        if _eval_cc(cc, 0, pf, zf, 0, 0):
            bits |= rel_bit
    return _X87_REL_EXPR[bits]


def _x87_sahf_cond(cc, allow_of=False):
    """Точное C-условие для `fcom*; fnstsw ax; sahf; jcc` (allow_of=False) и `fcomi*; jcc` (allow_of=True)."""
    if cc not in CC_READS or (not allow_of and (CC_READS[cc] & O)):
        return None
    bits = 0
    for rel_bit, ah in _X87_AH_RELS:
        cf = ah & 1
        pf = (ah >> 2) & 1
        zf = (ah >> 6) & 1
        if _eval_cc(cc, cf, pf, zf, 0, 0):
            bits |= rel_bit
    return _X87_REL_EXPR[bits]


class EmitError(Exception):
    pass


def hx(v):
    return f"0x{v & 0xFFFFFFFF:x}u"


class FuncCompiler:
    def __init__(self, mod, func, ctx, fpu_single=None, fpu_float32=None):
        self.mod = mod
        self.f = func
        self.ctx = ctx  # общий контекст: резолв вызовов, сводки x87, оверрайды
        self.fpu_single = fpu_single_mode() if fpu_single is None else bool(fpu_single)
        self.fpu_float32 = (fpu_float32_mode() if fpu_float32 is None else bool(fpu_float32)) and not self.fpu_single
        self.entry = func.entry
        self.addrs = func.insns
        self.ins = {a: mod.insns[a] for a in self.addrs}
        self.out = []
        self.errors = []
        self.uses = set()  # какие локальные нужны (флаги, fa/fb/fr, df, fsw, f0..)
        self.max_depth = 0
        self.has_switch = False
        self.const_regs = {}
        self.use_f32 = False
        self.x87_fused_cmp = {}  # addr fcom*/comi -> need_fsw (bool)
        self.x87_skip = set()    # адреса fnstsw/test ah/sahf, полностью убранных слиянием
        self.x87_cond = {}       # addr потребителя (jcc/setcc/cmovcc/fcmov) -> C-выражение на fx/fy

    # ================================================================ анализ
    def succ_of(self, ins):
        m = ins.mnem
        mod = self.mod
        if m == "call":
            if mod.is_noreturn_call(ins) or self.call_is_noreturn(ins):
                return []
            return [ins.next]
        if m == "jmp":
            t = ins.branch_target()
            if t is not None:
                return [] if mod.is_tail(self.entry, ins, t) else [t]
            jt = mod.jump_tables.get(ins.addr)
            return list(dict.fromkeys(jt.targets)) if jt else []
        if ins.is_jcc() or ins.is_loop():
            t = ins.branch_target()
            if mod.is_tail(self.entry, ins, t):
                return [ins.next]
            return [ins.next, t] if t != ins.next else [ins.next]
        if m in TERMINATORS:
            return []
        return [ins.next]

    def call_is_noreturn(self, ins):
        return False

    def analyze(self):
        self.succ = {a: [s for s in self.succ_of(self.ins[a])] for a in self.addrs}
        self.finfo = {a: flag_info(self.ins[a]) for a in self.addrs}
        # вызовы x87-интринсиков не трогают флаги так, как обычный call: считаем clobber всё равно.
        self.live_in, self.live_out, self.preds = liveness(self.addrs, self.succ, self.finfo)
        self.mat, self.fused, self.fused_used = plan(self.addrs, self.succ, self.finfo, self.live_in,
                                                     self.live_out, self.preds)
        self.x87_analyze()
        self.use_f32 = self.detect_pure_f32()
        self.plan_x87_fusion()

    def detect_pure_f32(self):
        """Чистая f32-функция: x87 работает только с float (dword/i16/i32) и базовой арифметикой без f64/f80/i64/libm."""
        if not self.fpu_float32 or self.max_depth == 0 or self.x87_bad:
            return False
        if self.mod.img.name.lower() == "msvcr71.dll":
            return False
        f32_fmts = ("f32", "i16", "i32")
        f32_st_fmts = ("f32", "i16", "i32", "i16t", "i32t")
        f32_intr = ("_CIsqrt", "_ftol", "_ftol2", "_ftol2_sse")
        for a in self.addrs:
            ins = self.ins[a]
            if ins.x87:
                op = self.x87op(ins)
                k = op.kind
                if k == "ld_mem" or k == "cmp_mem":
                    if op.args[0] not in f32_fmts:
                        return False
                elif k == "arith_mem":
                    if op.args[1] not in f32_fmts:
                        return False
                elif k == "st_mem":
                    if op.args[0] not in f32_st_fmts:
                        return False
                elif k == "ld_const":
                    if op.args[0] not in ("0", "1"):
                        return False
                elif k == "unary":
                    if op.args[0] not in ("chs", "abs", "sqrt"):
                        return False
                elif k not in ("ld_st", "st_st", "fxch", "arith_st", "cmp_st", "ftst", "comi",
                               "fnstsw_ax", "fnstsw_mem", "fcmov", "nop", "ffree"):
                    return False
            elif ins.mnem == "call":
                info = self.ctx.call_info(self.mod, ins)
                if info.intrinsic and info.intrinsic not in f32_intr:
                    return False
            elif ins.mnem == "jmp" or ins.is_jcc():
                t = ins.branch_target()
                if t is not None and self.mod.is_tail(self.entry, ins, t):
                    info = self.ctx.target_info(self.mod, t)
                    if info.intrinsic and info.intrinsic not in f32_intr:
                        return False
                elif t is None and ins.addr not in self.mod.jump_tables:
                    imp = self.mod.iat_import(ins)
                    if imp is not None:
                        info = self.ctx.import_info(self.mod, imp)
                        if info.intrinsic and info.intrinsic not in f32_intr:
                            return False
        return True

    def _is_fsw_writer(self, ins):
        if not ins.x87:
            return False
        op = self.x87op(ins)
        return op.kind in ("cmp_mem", "cmp_st", "ftst", "fxam", "comi") or (
            op.kind == "bin" and op.args[0] in ("fprem", "fprem1")
        )

    def _is_fsw_reader(self, ins):
        if not ins.x87:
            return False
        return self.x87op(ins).kind in ("fnstsw_ax", "fnstsw_mem")

    def _step_linear(self, a):
        """Следующая инструкция линейного участка (один преемник в функции с единственным предшественником)."""
        ins = self.ins[a]
        if ins.mnem in ("call", "jmp", "ret") or ins.mnem in TERMINATORS or ins.is_jcc() or ins.is_loop():
            return None
        nx = self.succ[a]
        if len(nx) != 1 or nx[0] not in self.ins:
            return None
        if len(self.preds.get(nx[0], ())) != 1:
            return None
        return nx[0]

    def _cond_cc_of(self, ins):
        cc = jcc_cc(ins)
        if cc is not None:
            return cc
        if ins.x87:
            op = self.x87op(ins)
            if op.kind == "fcmov":
                return op.args[0]
        return None

    def _liveness_1bit(self, rw_fn, exit_live):
        """Обратная 1-битная живость по CFG функции -> live_out: addr -> bool."""
        lin = {a: False for a in self.addrs}
        lout = {a: False for a in self.addrs}
        work = list(self.addrs)
        inw = set(work)
        while work:
            a = work.pop()
            inw.discard(a)
            seq = self.succ[a]
            out = exit_live if not seq else False
            for s in seq:
                if s not in lin:
                    if exit_live:
                        out = True
                elif lin[s]:
                    out = True
            rd, kill = rw_fn(self.ins[a])
            ni = rd or (out and not kill)
            lout[a] = out
            if ni != lin[a]:
                lin[a] = ni
                for p in self.preds.get(a, ()):
                    if p not in inw:
                        inw.add(p)
                        work.append(p)
        return lout

    def _ax_rw(self, ins):
        """Консервативное (reads_ax, kills_all_16bits_of_ax) для безопасного удаления fnstsw ax (пишет только ax)."""
        m = ins.mnem
        ops = ins.ops
        if m == "call":
            return True, False
        if ins.x87 and self.x87op(ins).kind == "fnstsw_ax":
            return False, True
        if m in ("xor", "sub") and len(ops) == 2 and ops[0].kind == "r" and ops[1].kind == "r":
            if ops[0].reg in ("eax", "ax") and ops[0].reg == ops[1].reg:
                return False, True
        r, _ = regs.effects(ins)
        rd = bool(r & regs.EAX)
        if m in ("mov", "movzx", "movsx", "lea", "pop") and ops and ops[0].kind == "r" and ops[0].reg in ("eax", "ax"):
            return rd, True
        if m == "rdtsc":
            return False, True
        return rd, False

    def plan_x87_fusion(self):
        """Слияние цепочек `fcom*/ftst -> fnstsw ax -> (test ah, imm | sahf) -> jcc/setcc/cmovcc` и `fcomi* -> jcc`
        в прямые C-сравнения `fx/fy` (убирает FCOM + PARITY/NEON-popcnt; fnstsw ax опускается, только если eax мёртв)."""
        self.x87_fused_cmp = {}
        self.x87_skip = set()
        self.x87_cond = {}
        if self.x87_bad or self.max_depth == 0:
            return
        cmps = []
        for a in self.addrs:
            ins = self.ins[a]
            if ins.x87:
                k = self.x87op(ins).kind
                if k in ("cmp_mem", "cmp_st", "ftst", "comi"):
                    cmps.append((a, k))
        if not cmps:
            return
        fsw_out = self._liveness_1bit(lambda i: (self._is_fsw_reader(i), self._is_fsw_writer(i)), False)
        ax_out = self._liveness_1bit(self._ax_rw, True)

        def collect_linear_consumers(start_a, mask_written, allow_of):
            """Собрать все потребители флагов из mask_written, достижимые от start_a по линейной цепочке."""
            pending = self.live_out[start_a] & mask_written
            if not pending:
                return None
            cur = start_a
            cons = []
            for _ in range(16):
                if not pending:
                    break
                live_succs = [x for x in self.succ[cur] if self.live_in.get(x, 0) & pending]
                if not live_succs:
                    pending = 0
                    break
                if len(live_succs) != 1:
                    return None  # флаги уходят в несколько веток
                cont = live_succs[0]
                if len(self.preds.get(cont, ())) != 1 or cont not in self.finfo:
                    return None
                ins_j = self.ins[cont]
                if self._is_fsw_writer(ins_j):
                    return None
                jr, jw, jkind = self.finfo[cont]
                if jr & pending:
                    if jkind != "cond" or (jr & ~mask_written) != 0:
                        return None
                    cc = self._cond_cc_of(ins_j)
                    expr = _x87_sahf_cond(cc, allow_of=allow_of)
                    if expr is None:
                        return None
                    cons.append((cont, expr))
                if jw:
                    pending &= ~jw
                pending &= self.live_out[cont]
                cur = cont
            if pending or not cons:
                return None
            return cons

        for a, kind in cmps:
            if kind == "comi":
                cons = collect_linear_consumers(a, ALL, allow_of=True)
                if cons:
                    self.x87_fused_cmp[a] = False
                    self.mat.pop(a, None)
                    for ca, expr in cons:
                        self.x87_cond[ca] = expr
                        self.fused.pop(ca, None)
                continue

            # 1) Найти fnstsw ax в том же базовом блоке
            cur = a
            a_fn = None
            for _ in range(8):
                nx = self._step_linear(cur)
                if nx is None:
                    break
                ni = self.ins[nx]
                if ni.x87 and self.x87op(ni).kind == "fnstsw_ax":
                    a_fn = nx
                    break
                if self._is_fsw_writer(ni) or self._is_fsw_reader(ni):
                    break
                cur = nx
            if a_fn is None:
                continue

            # 2) Найти test ah, imm8 или sahf после fnstsw ax в том же базовом блоке
            cur = a_fn
            a_tst = None
            tst_mask = None
            mid_reads_eax = False
            for _ in range(8):
                nx = self._step_linear(cur)
                if nx is None:
                    break
                ni = self.ins[nx]
                if (ni.mnem == "test" and len(ni.ops) == 2 and ni.ops[0].kind == "r" and ni.ops[0].reg == "ah"
                        and ni.ops[1].kind == "i" and ((ni.ops[1].imm & 0xFF) & ~0x45) == 0):
                    a_tst = nx
                    tst_mask = ni.ops[1].imm & 0xFF
                    break
                if ni.mnem == "sahf":
                    a_tst = nx
                    tst_mask = None
                    break
                if self._is_fsw_writer(ni) or self._is_fsw_reader(ni):
                    break
                er, ew = regs.effects(ni)
                if ew & regs.EAX:
                    break
                if er & regs.EAX:
                    mid_reads_eax = True
                cur = nx
            if a_tst is None:
                continue

            # 3) Проверить потребителей флагов после test ah / sahf
            if tst_mask is not None:
                if a_tst in self.mat:
                    continue
                # Идём по той же линейной цепочке, по которой flags.plan() привязал fused-потребителей
                cons = []
                fcons = {c for c, s in self.fused.items() if s == a_tst}
                if not fcons:
                    continue
                cur = a_tst
                ok = True
                remaining = set(fcons)
                for _ in range(64):
                    if not remaining:
                        break
                    seq = self.succ[cur]
                    cont = None
                    for x in seq:
                        if len(self.preds.get(x, ())) == 1 and x in self.finfo:
                            cont = x
                            break
                    if cont is None:
                        ok = False
                        break
                    ins_j = self.ins[cont]
                    if self._is_fsw_writer(ins_j):
                        ok = False
                        break
                    if cont in remaining:
                        cc = self._cond_cc_of(ins_j)
                        expr = _x87_test_cond(tst_mask, cc)
                        if expr is None:
                            ok = False
                            break
                        cons.append((cont, expr))
                        remaining.discard(cont)
                    cur = cont
                if not ok or remaining:
                    continue
            else:
                cons = collect_linear_consumers(a_tst, C | P | A | Z | S, allow_of=False)
                if not cons:
                    continue

            # 4) Применить слияние; fnstsw ax опускаем, только если ax гарантированно мёртв после a_tst
            drop_fnstsw = (not mid_reads_eax) and (not ax_out[a_tst])
            need_fsw = (not drop_fnstsw) or fsw_out[a_fn]
            self.x87_fused_cmp[a] = need_fsw
            if drop_fnstsw:
                self.x87_skip.add(a_fn)
            self.x87_skip.add(a_tst)
            self.fused_used.discard(a_tst)
            self.mat.pop(a_tst, None)
            for ca, expr in cons:
                self.x87_cond[ca] = expr
                self.fused.pop(ca, None)

    # ---------------------------------------------------------------- x87
    def x87_effect(self, ins, depth):
        """-> (need, delta) или None, если инструкция не x87 и не вызов."""
        if ins.x87:
            op = self.x87op(ins)
            return op.need, op.delta
        if ins.mnem == "call":
            info = self.ctx.call_info(self.mod, ins)
            if info.intrinsic:
                pops, pushes = X87_INTRINSICS[info.intrinsic]
                return pops, pushes - pops
            return 0, None  # None -> глубина после вызова определяется ret_depth
        return None

    def x87op(self, ins):
        op = ins.extra.get("x87op")
        if op is None:
            op = decode_x87(ins)
            ins.extra["x87op"] = op
        return op

    def call_ret_depth(self, ins):
        info = self.ctx.call_info(self.mod, ins)
        if info.ret_depth is not None:
            return info.ret_depth
        # неизвестный (косвенный) вызов: смотрим, сколько элементов x87 требуется дальше
        rd = self.lookahead_ret_depth(ins.next)
        if rd is None:
            # результат уходит прямо в ret: float ли это, решают остальные выходы функции (x87_analyze)
            self.passthrough_calls.add(ins.addr)
            return self.passthrough_depth
        return rd

    def lookahead_ret_depth(self, a):
        d = 0
        extra = 0
        steps = 0
        touched = False
        while a in self.ins and steps < 40:
            steps += 1
            ins = self.ins[a]
            if ins.x87:
                touched = True
                op = self.x87op(ins)
                if op.need > d + extra:
                    extra = op.need - d
                d += op.delta
            elif ins.mnem == "ret" and not touched:
                return None
            elif ins.mnem == "jmp":
                # значение из ST(0) часто потребляется за переходом на общий хвост (ветка с fld константы)
                t = ins.branch_target()
                if t is None or t not in self.ins or self.mod.is_tail(self.entry, ins, t):
                    break
                a = t
                continue
            elif ins.mnem in ("call", "ret") or ins.mnem in TERMINATORS:
                break
            elif ins.is_jcc():
                pass
            a = ins.next
        return min(extra, 1)

    def ffree_run(self, a):
        """Номера st(i), освобождённые непрерывной серией ffree непосредственно перед a."""
        freed = set()
        i = self.addrs.index(a)
        while i > 0:
            prev = self.ins[self.addrs[i - 1]]
            if prev.next != self.addrs[i] or not prev.x87 or self.x87op(prev).kind != "ffree":
                break
            freed.add(self.x87op(prev).args[0])
            i -= 1
        return freed

    def x87_analyze(self):
        """Прямой анализ глубины стека x87. Заполняет self.depth_in и self.exit_depth."""
        self.passthrough_depth = 0
        self.passthrough_calls = set()
        errors = list(self.errors)
        self._x87_pass()
        if self.x87_bad and self.passthrough_calls and self.exit_depths == {0, 1}:
            # косвенный вызов прямо перед ret, а другие выходы возвращают float: вызов вернул float
            self.errors = errors
            self.passthrough_depth = 1
            self.max_depth = 0
            self._x87_pass()

    def _x87_pass(self):
        depth_in = {self.entry: 0}
        work = [self.entry]
        self.call_depth = {}
        exit_depths = set()
        bad = False
        while work:
            a = work.pop()
            d = depth_in[a]
            ins = self.ins[a]
            nd = d
            if ins.x87:
                op = self.x87op(ins)
                if op.need > d:
                    self.errors.append(f"{a:08x}: x87 underflow ({ins.text}, depth {d})")
                    bad = True
                nd = d + op.delta
                if op.kind == "ffree" and op.args == (0,):
                    # ассемблерные вставки D3DDrv: ffree st(d-1) ... ffree st(0) вместо снятия со стека.
                    # TOP на ПК не возвращается, но все регистры помечены пустыми — стек логически пуст
                    if self.ffree_run(a) != set(range(1, d)):
                        self.errors.append(f"{a:08x}: ffree st(0) with live values below (depth {d})")
                        bad = True
                    nd = 0
            elif ins.mnem == "call":
                info = self.ctx.call_info(self.mod, ins)
                if info.intrinsic:
                    pops, pushes = X87_INTRINSICS[info.intrinsic]
                    if d < pops:
                        self.errors.append(f"{a:08x}: intrinsic {info.intrinsic} underflow (depth {d})")
                        bad = True
                    nd = d - pops + pushes
                else:
                    # Прямой вызов при непустом стеке: так делает MSVC с /GL, зная, что вызываемая функция
                    # x87 не трогает. Значения вызывающего остаются в его локалях, результат ложится сверху.
                    if d != 0 and info.sym is None:
                        self.errors.append(f"{a:08x}: x87 stack not empty at indirect call (depth {d})")
                        bad = True
                    rd = self.call_ret_depth(ins)
                    self.call_depth[a] = rd
                    nd = d + rd
            elif ins.mnem == "ret":
                exit_depths.add(d)
            elif ins.mnem == "jmp" or ins.is_jcc():
                t = ins.branch_target()
                if t is not None and self.mod.is_tail(self.entry, ins, t):
                    tinfo = self.ctx.target_info(self.mod, t)
                    if tinfo.intrinsic:
                        pops, pushes = X87_INTRINSICS[tinfo.intrinsic]
                        exit_depths.add(d - pops + pushes)
                    else:
                        exit_depths.add(d + (tinfo.ret_depth or 0))
                elif t is None and ins.addr not in self.mod.jump_tables:
                    imp = self.mod.iat_import(ins)
                    iinfo = self.ctx.import_info(self.mod, imp) if imp is not None else None
                    if iinfo is not None and iinfo.intrinsic:
                        pops, pushes = X87_INTRINSICS[iinfo.intrinsic]
                        exit_depths.add(d - pops + pushes)
                    else:
                        exit_depths.add(d + (self.ctx.import_ret_depth(imp) if imp is not None else 0))
            if nd < 0 or nd > 8:
                self.errors.append(f"{a:08x}: x87 depth out of range ({nd})")
                bad = True
                nd = max(0, min(8, nd))
            self.max_depth = max(self.max_depth, nd, d)
            for s in self.succ[a]:
                if s not in self.ins:
                    continue
                if s in depth_in:
                    if depth_in[s] != nd:
                        self.errors.append(f"{s:08x}: x87 depth mismatch {depth_in[s]} vs {nd}")
                        bad = True
                else:
                    depth_in[s] = nd
                    work.append(s)
        self.depth_in = depth_in
        self.exit_depths = exit_depths
        if len(exit_depths) > 1:
            self.errors.append(f"x87 exit depth ambiguous {sorted(exit_depths)}")
            bad = True
        self.exit_depth = max(exit_depths) if exit_depths else 0
        self.x87_bad = bad

    # ================================================================ операнды
    def r32(self, name):
        b, k = reg_base(name)
        if k != 32:
            raise EmitError(f"expected 32-bit reg, got {name}")
        return b

    def ea(self, o):
        parts = []
        if o.base:
            parts.append(self.r32(o.base))
        if o.index:
            idx = self.r32(o.index)
            parts.append(idx if o.scale == 1 else f"{idx}*{o.scale}u")
        if o.disp or not parts:
            parts.append(hx(o.disp))
        e = " + ".join(parts)
        if o.seg == "fs":
            e = f"c->fs_base + {e}"
        elif o.seg == "gs":
            raise EmitError("gs segment")
        return f"(u32)({e})"

    def ha(self, o):
        """Указатель хоста на операнд-память. [рег ± небольшое смещение] — без заворота через 2^32 (m + рег ± d):
        m + рег компилятор считает раз на значение регистра, поля берёт непосредственным смещением инструкции
        (иначе на каждый доступ — 32-битное сложение). С x86 расходится только у доступа через 0 или 4 ГБ: это
        страница NULL или адреса вне окна гостя. Индекс, большое смещение (адрес таблицы), fs: — с заворотом."""
        if o.base and not o.index and not o.seg:
            d = o.disp - (1 << 32) if o.disp & 0x80000000 else o.disp
            if -SMALL_DISP < d < SMALL_DISP:
                b = self.r32(o.base)
                return f"m + {b}" + (f" + {d:#x}u" if d > 0 else f" - {-d:#x}u" if d < 0 else "")
        return f"m + {self.ea(o)}"

    def rd(self, o, size=None, AX=None):
        size = size or o.size
        if o.kind == "i":
            return hx(o.imm & MASK[size])
        if o.kind == "r":
            b, k = reg_base(o.reg)
            if b is None:
                raise EmitError(f"reg {o.reg}")
            if k == 32:
                return b
            if k == 16:
                return f"({b} & 0xFFFFu)"
            if k == "lo":
                return f"({b} & 0xFFu)"
            return f"(({b} >> 8) & 0xFFu)"
        a = AX or self.ha(o)
        if size == 1:
            return f"(u32)*({a})"
        if size == 2:
            return f"(u32)ld16({a})"
        if size == 4:
            return f"ld32({a})"
        raise EmitError(f"read size {size}")

    def wr(self, o, v, size=None, AX=None):
        size = size or o.size
        if o.kind == "r":
            b, k = reg_base(o.reg)
            if k == 32:
                return f"esp = (u32)({v});" if b == "esp" else f"{b} = {v};"
            if k == 16:
                return f"{b} = ({b} & 0xFFFF0000u) | (({v}) & 0xFFFFu);"
            if k == "lo":
                return f"{b} = ({b} & 0xFFFFFF00u) | (({v}) & 0xFFu);"
            if k == "hi":
                return f"{b} = ({b} & 0xFFFF00FFu) | ((({v}) & 0xFFu) << 8);"
            raise EmitError(f"write reg {o.reg}")
        if o.kind != "m":
            raise EmitError("write to imm")
        a = AX or self.ha(o)
        if size == 1:
            return f"*({a}) = (u8)({v});"
        if size == 2:
            return f"st16({a}, (u16)({v}));"
        if size == 4:
            return f"st32({a}, {v});"
        raise EmitError(f"write size {size}")

    # ================================================================ флаги
    def flag_exprs(self, kind, n):
        sb = n * 8 - 1
        par = "PARITY(fr)"
        if kind == "sub":
            return {C: "(fa < fb)", Z: "(fr == 0)", S: f"((fr >> {sb}) & 1)",
                    O: f"((((fa ^ fb) & (fa ^ fr)) >> {sb}) & 1)", P: par, A: "(((fa ^ fb ^ fr) >> 4) & 1)"}
        if kind == "add":
            return {C: "(fr < fa)", Z: "(fr == 0)", S: f"((fr >> {sb}) & 1)",
                    O: f"(((~(fa ^ fb) & (fa ^ fr)) >> {sb}) & 1)", P: par, A: "(((fa ^ fb ^ fr) >> 4) & 1)"}
        if kind == "logic":
            return {C: "0", Z: "(fr == 0)", S: f"((fr >> {sb}) & 1)", O: "0", P: par, A: "0"}
        if kind == "inc":
            return {Z: "(fr == 0)", S: f"((fr >> {sb}) & 1)", O: f"(fr == {hx(SIGNBIT[n])})", P: par,
                    A: "((fr & 0xF) == 0)"}
        if kind == "dec":
            return {Z: "(fr == 0)", S: f"((fr >> {sb}) & 1)", O: f"(fa == {hx(SIGNBIT[n])})", P: par,
                    A: "((fa & 0xF) == 0)"}
        raise EmitError(kind)

    def emit_setter_flags(self, a, kind, n, lines):
        """После вычисления fa/fb/fr — материализовать нужные флаги."""
        mask = self.mat.get(a, 0)
        if not mask:
            return
        ex = self.flag_exprs(kind, n)
        for bit in (C, P, A, Z, S, O):
            if mask & bit and bit in ex:
                lines.append(f"{NAMES[bit]} = {ex[bit]};")
                self.uses.add(NAMES[bit])

    def setter_needed(self, a):
        return a in self.fused_used or a in self.mat

    def cond(self, a, cc):
        """C-выражение условия cc для потребителя a."""
        xc = self.x87_cond.get(a)
        if xc is not None:
            return xc
        s = self.fused.get(a)
        if s is not None:
            sins = self.ins[s]
            kind = self.finfo[s][2]
            n = self.setter_size(sins)
            return self.fused_cond(kind, n, cc)
        return self.flag_cond(cc)

    def flag_cond(self, cc):
        for bit in (C, P, Z, S, O):
            if CC_READS[cc] & bit:
                self.uses.add(NAMES[bit])
        return {
            "o": "of", "no": "!of", "b": "cf", "ae": "!cf", "e": "zf", "ne": "!zf", "be": "(cf | zf)",
            "a": "!(cf | zf)", "s": "sf", "ns": "!sf", "p": "pf", "np": "!pf", "l": "(sf != of)",
            "ge": "(sf == of)", "le": "(zf | (sf != of))", "g": "(!zf & (sf == of))",
        }[cc]

    def fused_cond(self, kind, n, cc):
        st = SIGNED[n]
        if kind == "sub":
            d = {"b": "fa < fb", "ae": "fa >= fb", "e": "fa == fb", "ne": "fa != fb", "be": "fa <= fb",
                 "a": "fa > fb", "l": f"({st})fa < ({st})fb", "ge": f"({st})fa >= ({st})fb",
                 "le": f"({st})fa <= ({st})fb", "g": f"({st})fa > ({st})fb"}
            if cc in d:
                return f"({d[cc]})"
        if kind == "logic":
            d = {"b": "0", "ae": "1", "e": "fr == 0", "ne": "fr != 0", "be": "fr == 0", "a": "fr != 0",
                 "l": f"({st})fr < 0", "ge": f"({st})fr >= 0", "le": f"({st})fr <= 0", "g": f"({st})fr > 0",
                 "o": "0", "no": "1"}
            if cc in d:
                return f"({d[cc]})"
        if cc in ("e", "ne") and kind in ("add", "inc", "dec"):
            return "(fr == 0)" if cc == "e" else "(fr != 0)"
        ex = self.flag_exprs(kind, n)
        fl = {bit: ex[bit] for bit in ex}
        comb = {
            "o": fl.get(O), "no": f"!{fl.get(O)}", "b": fl.get(C), "ae": f"!{fl.get(C)}",
            "e": fl.get(Z), "ne": f"!{fl.get(Z)}", "be": f"({fl.get(C)} | {fl.get(Z)})",
            "a": f"!({fl.get(C)} | {fl.get(Z)})", "s": fl.get(S), "ns": f"!{fl.get(S)}",
            "p": fl.get(P), "np": f"!{fl.get(P)}", "l": f"({fl.get(S)} != {fl.get(O)})",
            "ge": f"({fl.get(S)} == {fl.get(O)})", "le": f"({fl.get(Z)} | ({fl.get(S)} != {fl.get(O)}))",
            "g": f"(!{fl.get(Z)} & ({fl.get(S)} == {fl.get(O)}))",
        }
        e = comb[cc]
        if e is None or "None" in e:
            raise EmitError(f"fused cond {kind} {cc}")
        return e

    def setter_size(self, ins):
        if ins.string_op:
            return ins.ops[0].size if ins.ops else 1
        o = ins.ops[0]
        return o.size if o.size in (1, 2, 4) else 4

    # ================================================================ генерация
    def compile(self):
        self.analyze()
        self.regs_analyze()
        self.setjmps = 0
        self.has_switch = False
        self.const_regs = {}
        body = []
        order = self.addrs
        pos = {a: i for i, a in enumerate(order)}
        labels = set()
        for a in order:
            ins = self.ins[a]
            if ins.is_jcc() or ins.is_loop() or ins.mnem == "jmp":
                t = ins.branch_target()
                if t is not None and t in self.ins:
                    labels.add(t)
            for s in self.succ[a]:
                if s == ins.next and self.falls_through(ins) and pos.get(s) == pos[a] + 1:
                    continue
                labels.add(s)
        # продолжения catch (discover.Module._eh_continuations): в теле — метки, вне — отдельные функции
        conts = self.mod.eh_conts.get(self.entry, [])
        self.eh_in = [v for v in conts if v in self.ins]
        self.eh_out = [v for v in conts if v not in self.ins and v in self.mod.functions]
        labels.update(self.eh_in)
        if order and order[0] != self.entry:
            # часть тела лежит ниже входа: начинаем с перехода на вход
            labels.add(self.entry)
            body.append(f"    goto L_{self.entry:08x};")
        self.labels = labels
        prev_fall = False
        for i, a in enumerate(order):
            ins = self.ins[a]
            if a in labels:
                body.append(f"L_{a:08x}:; BLK({hx(a)});")
                self.const_regs.clear()
            elif not prev_fall:
                self.const_regs.clear()
            self.cur = a
            self.depth = self.depth_in.get(a, 0)
            try:
                lines = self.emit_insn(ins)
            except EmitError as e:
                self.errors.append(f"{a:08x}: {ins.text}: {e}")
                lines = [f'rt_unimpl(c, {hx(a)}, "{ins.text}"); return;']
            self.update_const_regs(ins)
            body.append(f"/* {a:08x}: {ins.text} */")
            body.extend("    " + ln for ln in lines)
            # fallthrough
            nxt = ins.next
            falls = self.falls_through(ins)
            prev_fall = falls and nxt in self.ins and i + 1 < len(order) and order[i + 1] == nxt
            if falls:
                if nxt not in self.ins:
                    body.append(f'    SPILL(); rt_trap(c, {hx(nxt)}, "fell off decoded code"); return;')
                elif i + 1 >= len(order) or order[i + 1] != nxt:
                    body.append(f"    goto L_{nxt:08x};")
        return self.wrap(body)

    def update_const_regs(self, ins):
        """Узкое отслеживание констант в 32-битных регистрах внутри базового блока: нужно, чтобы
        rep movs/stos под SNX_PIN (где ecx — глобальный регистр x20) получали литеральную длину."""
        m = ins.mnem
        ops = ins.ops
        if m in ("call", "jmp", "ret") or m in TERMINATORS:
            self.const_regs.clear()
            return
        if ins.string_op:
            w = regs.effects(ins)[1]
            for r in REG32:
                if w & regs.BIT[r]:
                    self.const_regs.pop(r, None)
            if ins.rep and ins.string_op in ("movs", "stos"):
                self.const_regs["ecx"] = 0
            return
        if m == "mov" and len(ops) == 2 and ops[0].kind == "r" and ops[0].reg in regs.BIT:
            d, s = ops[0].reg, ops[1]
            if s.kind == "i":
                self.const_regs[d] = s.imm & 0xFFFFFFFF
                return
            if s.kind == "r" and s.reg in self.const_regs:
                self.const_regs[d] = self.const_regs[s.reg]
                return
        elif m in ("xor", "sub") and len(ops) == 2 and ops[0].kind == "r" and ops[1].kind == "r":
            if ops[0].reg == ops[1].reg and ops[0].reg in regs.BIT:
                self.const_regs[ops[0].reg] = 0
                return
        elif (m == "shr" and len(ops) == 2 and ops[0].kind == "r" and ops[0].reg in self.const_regs
              and ops[1].kind == "i"):
            self.const_regs[ops[0].reg] >>= ops[1].imm & 31
            return
        elif (m == "and" and len(ops) == 2 and ops[0].kind == "r" and ops[0].reg in self.const_regs
              and ops[1].kind == "i"):
            self.const_regs[ops[0].reg] &= ops[1].imm & 0xFFFFFFFF
            return
        w = regs.effects(ins)[1]
        if w:
            for r in REG32:
                if w & regs.BIT[r]:
                    self.const_regs.pop(r, None)

    def falls_through(self, ins):
        m = ins.mnem
        if m in TERMINATORS or m == "jmp" or m == "ret":
            return False
        if m == "call":
            return ins.next in self.succ[ins.addr]
        return True

    def wrap(self, body):
        code = self.wrap_body(body)
        if self.entry not in self.mod.eh_targets:
            return code
        # адрес продолжения catch с собственной функцией (на него ссылается mov eax, offset в блоке catch):
        # из _JumpToContinuation (jmp eax) — не вложенный вызов, а longjmp в кадр функции с catch
        e = self.entry
        return "\n".join([code, f"void {self.ctx.fname(e)}(SNX_C_PARAM)", "{", "    P_(CPU *restrict c = g_pin_c;)",
                          "    P_(SPILL();)", f"    rt_eh_continue(c, {hx(e)});",
                          f"    fc_{e:08x}(SNX_C_ARG);", "}"])

    def wrap_body(self, body):
        # restrict: CPU лежит в памяти хоста, память гостя (m) с ним не пересекается — компилятор может не
        # перечитывать регистры гостя после записей в память гостя и убирать лишние сохранения в c
        name = self.ctx.fname(self.entry)
        if self.entry in self.mod.eh_targets:
            name = f"fc_{self.entry:08x}"
        # esp — 64-битный: push/pop/call/ret/add/sub esp, imm меняют его без заворота через 2^32 (стек гостя через
        # 0 не переходит), и m + esp компилятор считает раз на серию push — каждый слот — смещением инструкции.
        # Любая другая запись в esp — (u32)(...) (wr), чтение в 32-битные переменные усекает само.
        # G_(...): с закреплёнными регистрами (SNX_PIN, runtime/gen_rt.h) регистры гостя и m — общие, копий нет;
        # P_(...): под SNX_PIN указатель CPU *c закреплён в x28 (g_pin_c) и не передаётся через x0.
        decl = ["P_(CPU *restrict c = g_pin_c;)",
                "G_(u8 *const m = g_mem;)",
                "G_(u32 eax = c->eax, ecx = c->ecx, edx = c->edx, ebx = c->ebx, ebp = c->ebp, esi = c->esi, edi = c->edi;)",
                "G_(u64 esp = c->esp;)"]
        if self.uses & {"fa", "fb", "fr"} or self.fused_used:
            decl.append("u32 fa = 0, fb = 0, fr = 0;")
        fl = [n for n in ("cf", "pf", "af", "zf", "sf", "of") if n in self.uses]
        if fl:
            decl.append("u32 " + ", ".join(f"{n} = 0" for n in fl) + ";")
        if "df" in self.uses:
            decl.append("u32 df = 0;")
        if "fsw" in self.uses:
            decl.append("u32 fsw = 0;")
        nf = max(self.max_depth, 0)
        ft = "float" if self.use_f32 else "double"
        if nf:
            decl.append(f"{ft} " + ", ".join(f"f{i} = 0" for i in range(min(nf, 8))) + ";")
        if "fxy" in self.uses:
            decl.append(f"{ft} fx = 0, fy = 0;")
        if self.setjmps:
            decl.append("rt_jmp " + ", ".join(f"sj{i}" for i in range(self.setjmps)) + ";")
        decl.append(f"ENTER({hx(self.entry)});")
        if not (self.eh_in or self.eh_out):
            attr = "SNX_NO_JT " if self.has_switch else ""
            out = [f"{attr}void {name}(SNX_C_PARAM)", "{"]
            out += ["    " + d for d in decl]
            out += body
            out.append("}")
            return "\n".join(out)
        # функция с продолжениями catch: тело — со входом «с продолжения» (регистры — из c после
        # _JumpToContinuation), обёртка держит хостовый кадр для longjmp из rt_call
        cases = [f"case {hx(v)}: goto L_{v:08x};" for v in self.eh_in]
        cases += [f"case {hx(v)}: G_(SPILL();) fc_{v:08x}(SNX_C_ARG); return;" for v in self.eh_out]
        if len(cases) >= 4:
            self.has_switch = True
        bname = f"fb_{self.entry:08x}"
        attr = "SNX_NO_JT " if self.has_switch else ""
        out = [f"{attr}static void {bname}(SNX_C_PARAM_COMMA u32 eh_cont)", "{"]
        out += ["    " + d for d in decl]
        # вход с продолжения (после longjmp): закреплённые регистры вернулись к значениям на setjmp — из CPU
        out.append(f"    if (eh_cont) {{ P_(RELOAD();) switch (eh_cont) {{ {' '.join(cases)} }} "
                   f'SPILL(); rt_trap(c, eh_cont, "unknown catch continuation"); return; }}')
        out += body
        out.append("}")
        out += [f"void {name}(SNX_C_PARAM)", "{", "    P_(CPU *restrict c = g_pin_c;)", "    rt_eh_frame eh;",
                "    P_(SPILL();)", "    rt_eh_enter(c, &eh);",
                f"    {bname}(SNX_C_ARG_COMMA RT_SETJMP(&eh.j) ? eh.target : 0);", "    rt_eh_leave(&eh);", "}"]
        return "\n".join(out)

    # ------------------------------------------------------------- SPILL/RELOAD
    # «Чистый» регистр: локальная копия равна значению в CPU. На входе чисты все (копии только что прочитаны);
    # запись делает регистр грязным; после прямого вызова чисты touched цели (грязные из них записаны перед
    # вызовом, out прочитаны после, остальные цель вернула такими же), прочие — как были: цель их не трогает.
    # После HLE/косвенного вызова чисты все. Перед вызовом и на выходе пишутся только грязные (recomp/regs.py).
    def regs_analyze(self):
        self.clean = {}
        if not regs.ENABLED:
            return
        roots = [self.entry] + [v for v in self.mod.eh_conts.get(self.entry, []) if v in self.ins]
        work = []
        for a in roots:  # продолжения catch входят из CPU — всё чисто
            self.clean[a] = regs.ALL
            work.append(a)
        while work:
            a = work.pop()
            ins = self.ins[a]
            cin = self.clean[a]
            if ins.mnem == "call":
                info = self.ctx.call_info(self.mod, ins)
                if info.intrinsic:
                    cout = cin & ~regs.INTRINSIC_WRITES.get(info.intrinsic, 0)
                else:
                    cout = cin | self.ctx.reg_masks(info)[0]
            else:
                cout = cin & ~regs.effects(ins)[1]
            for s in self.succ[a]:
                if s not in self.ins:
                    continue
                old = self.clean.get(s)
                new = cout if old is None else old & cout
                if new != old:
                    self.clean[s] = new
                    work.append(s)

    def dirty(self, mask=0xFF):
        """Грязные регистры из mask перед текущей инструкцией (esp — всегда: call/ret его меняют)."""
        if not regs.ENABLED:
            return regs.ALL
        return (mask & ~self.clean.get(self.cur, regs.ALL)) | regs.ESP

    def sp(self, mask):
        """Запись в CPU перечисленных регистров (SPILL — все)."""
        if mask == regs.ALL:
            return "SPILL();"
        return "".join(f"c->{r} = {r}; " for r in REG32 if mask & regs.BIT[r]).rstrip()

    def rl(self, mask):
        """Чтение из CPU перечисленных регистров (RELOAD — все)."""
        if mask == regs.ALL:
            return "RELOAD();"
        return "".join(f"{r} = c->{r}; " for r in REG32 if mask & regs.BIT[r]).rstrip()

    @staticmethod
    def g_(stmt):
        """Обмен с CPU, нужный только без закреплённых регистров (runtime/gen_rt.h: G_)."""
        return f"G_({stmt})" if stmt else ""

    def host_sp(self, mask):
        """Перед вызовом функции хоста (HLE, оверрайд): без закрепления — грязные из mask, с ним — все (CPU не ведётся)."""
        return " ".join(p for p in ("P_(SPILL();)", self.g_(self.sp(mask))) if p)

    def call_stmt(self, info, sym):
        """Прямой вызов сгенерированной функции, HLE или оверрайда: грязные из touched цели — в CPU, out — обратно.
        С закреплёнными регистрами вызов функции гостя обходится без CPU; функции хоста — всегда через CPU."""
        touched, out = self.ctx.reg_masks(info)
        dirty = self.dirty(regs.ALL if regs.CHECK else touched)
        if info.addr is None:
            return " ".join(p for p in (self.host_sp(dirty), f"{sym}(c);", self.rl(out)) if p)
        parts = [self.g_(self.sp(dirty)), f"{sym}(SNX_C_ARG);"]
        if regs.CHECK:
            # проверка сводки: регистры вне out цель обязана вернуть такими же (rt_regchk_fail в runtime/rt.c)
            chk = " | ".join(f"(c->{r} != {r} ? {regs.BIT[r]:#x}u : 0u)" for r in REG32
                             if not (out | regs.ESP) & regs.BIT[r])
            if chk:
                parts.append(self.g_(f"RT_REGCHK({hx(self.cur)}, {hx(info.addr)}, {chk});"))
        parts.append(self.g_(self.rl(out)))
        return " ".join(p for p in parts if p)

    # ------------------------------------------------------------- dispatch
    def emit_insn(self, ins):
        if ins.addr in self.x87_skip:
            return []
        if ins.x87:
            return self.emit_x87(ins)
        m = ins.mnem
        h = getattr(self, "i_" + m, None)
        if h is None:
            if m.startswith("set"):
                return self.i_setcc(ins)
            if m.startswith("cmov"):
                return self.i_cmovcc(ins)
            if ins.is_jcc():
                return self.i_jcc(ins)
            if ins.string_op:
                return self.i_string(ins)
            return [f'SPILL(); rt_unimpl(c, {hx(ins.addr)}, "{ins.text}"); return;']
        return h(ins)

    # ---- SIMD: только обнуление регистра (xorps xmm0, xmm0 — проба поддержки SSE в ОС у Engine,
    # вызывается безусловно; решение «есть SSE» берётся из CPUID). Остальное — rt_unimpl.
    def i_xorps(self, ins):
        d, s = ins.ops
        if d.kind == "r" and s.kind == "r" and d.reg == s.reg:
            if d.reg.startswith("xmm"):
                return [f"memset(&c->xmm[{int(d.reg[3:])}], 0, sizeof(xmm_t));"]
            if d.reg.startswith("mm"):
                return [f"c->mm[{int(d.reg[2:])}] = 0;"]
        return [f'SPILL(); rt_unimpl(c, {hx(ins.addr)}, "{ins.text}"); return;']

    i_xorpd = i_xorps
    i_pxor = i_xorps

    # ---- пересылки
    def i_mov(self, ins):
        d, s = ins.ops
        if d.kind == "r" and d.reg in ("es", "ds", "ss", "cs", "fs", "gs") or s.kind == "r" and s.reg in (
                "es", "ds", "ss", "cs", "fs", "gs"):
            return [f'SPILL(); rt_unimpl(c, {hx(ins.addr)}, "{ins.text}"); return;']
        return [self.wr(d, self.rd(s, d.size))]

    def i_movzx(self, ins):
        d, s = ins.ops
        return [self.wr(d, self.rd(s))]

    def i_movsx(self, ins):
        d, s = ins.ops
        v = f"(u32)(s32)({SIGNED[s.size]}){self.rd(s)}"
        return [self.wr(d, v)]

    def i_lea(self, ins):
        d, s = ins.ops
        e = self.ea(s).replace("c->fs_base + ", "")
        return [self.wr(d, e)]

    def i_xchg(self, ins):
        d, s = ins.ops
        if d.kind == "r" and s.kind == "r":
            if d.reg == s.reg:
                return []
            return [f"{{ u32 t = {self.rd(d)}; {self.wr(d, self.rd(s))} {self.wr(s, 't')} }}"]
        mo, ro = (d, s) if d.kind == "m" else (s, d)
        A = self.ha(mo)
        return [f"{{ u8 *Ap = {A}; u32 t = {self.rd(mo, AX='Ap')}; {self.wr(mo, self.rd(ro), AX='Ap')} "
                f"{self.wr(ro, 't')} }}"]

    def i_push(self, ins):
        o = ins.ops[0]
        if o.kind == "r" and o.reg in ("es", "ds", "ss", "cs", "fs", "gs"):
            return [f'SPILL(); rt_unimpl(c, {hx(ins.addr)}, "{ins.text}"); return;']
        if o.size == 2:
            return [f"{{ u32 v = {self.rd(o)}; esp -= 2; st16(m + esp, (u16)v); }}"]
        if o.kind == "i":
            return [f"esp -= 4; st32(m + esp, {hx(o.imm)});"]
        return [f"{{ u32 v = {self.rd(o, 4)}; esp -= 4; st32(m + esp, v); }}"]

    def i_pop(self, ins):
        o = ins.ops[0]
        if o.kind == "r" and o.reg in ("es", "ds", "ss", "cs", "fs", "gs"):
            return [f'SPILL(); rt_unimpl(c, {hx(ins.addr)}, "{ins.text}"); return;']
        if o.size == 2:
            return [f"{{ u32 v = ld16(m + esp); esp += 2; {self.wr(o, 'v')} }}"]
        if o.kind == "r" and o.reg == "esp":
            return ["esp = ld32(m + esp);"]
        if o.kind == "m":
            # адрес считается после увеличения esp (если использует esp)
            return [f"{{ u32 v = ld32(m + esp); esp += 4; {self.wr(o, 'v')} }}"]
        return [f"{{ u32 v = ld32(m + esp); esp += 4; {self.wr(o, 'v')} }}"]

    def i_pushal(self, ins):
        return ["{ u32 t = esp; esp -= 32; wr32(m, esp + 28, eax); wr32(m, esp + 24, ecx); "
                "wr32(m, esp + 20, edx); wr32(m, esp + 16, ebx); wr32(m, esp + 12, t); wr32(m, esp + 8, ebp); "
                "wr32(m, esp + 4, esi); wr32(m, esp, edi); }"]

    def i_popal(self, ins):
        return ["edi = rd32(m, esp); esi = rd32(m, esp + 4); ebp = rd32(m, esp + 8); ebx = rd32(m, esp + 16); "
                "edx = rd32(m, esp + 20); ecx = rd32(m, esp + 24); eax = rd32(m, esp + 28); esp += 32;"]

    def i_leave(self, ins):
        return ["esp = ebp; ebp = ld32(m + esp); esp += 4;"]

    def i_enter(self, ins):
        size = ins.ops[0].imm & 0xFFFF
        lvl = ins.ops[1].imm & 31
        if lvl:
            raise EmitError("enter with nesting level")
        return [f"esp -= 4; st32(m + esp, ebp); ebp = (u32)esp; esp -= {size}u;"]

    def i_cdq(self, ins):
        return ["edx = (u32)((s32)eax >> 31);"]

    def i_cwde(self, ins):
        return ["eax = (u32)(s32)(s16)(eax & 0xFFFFu);"]

    def i_cbw(self, ins):
        return ["eax = (eax & 0xFFFF0000u) | ((u32)(s16)(s8)(eax & 0xFFu) & 0xFFFFu);"]

    def i_cwd(self, ins):
        return ["edx = (edx & 0xFFFF0000u) | (((s16)(eax & 0xFFFFu) < 0) ? 0xFFFFu : 0u);"]

    def i_bswap(self, ins):
        o = ins.ops[0]
        return [self.wr(o, f"__builtin_bswap32({self.rd(o)})")]

    def i_nop(self, ins):
        return []

    i_pause = i_nop
    i_wait = i_nop
    i_fwait = i_nop
    i_prefetchnta = i_nop
    i_prefetcht0 = i_nop
    i_prefetcht1 = i_nop
    i_prefetcht2 = i_nop
    i_sfence = i_nop
    i_lfence = i_nop
    i_mfence = i_nop

    # ---- арифметика с флагами
    def alu(self, ins, kind, op):
        d, s = ins.ops[0], ins.ops[1]
        n = d.size
        mk = MASK[n]
        need = self.setter_needed(ins.addr)
        lines = []
        AX = None
        pre = ""
        if d.kind == "m":
            pre = f"u8 *Ap = {self.ha(d)}; "
            AX = "Ap"
        a_ = self.rd(d, AX=AX)
        b_ = self.rd(s, n)
        writes = ins.mnem not in ("cmp", "test")
        if op == "^" and d.kind == "r" and s.kind == "r" and d.reg == s.reg and not need:
            return [self.wr(d, "0u")]
        if need:
            self.uses |= {"fa", "fb", "fr"}
            expr = f"(fa {op} fb)" if mk == 0xFFFFFFFF else f"((fa {op} fb) & {hx(mk)})"
            lines.append(f"{pre}fa = {a_}; fb = {b_}; fr = {expr};")
            if writes:
                lines.append(self.wr(d, "fr", AX=AX))
            self.emit_setter_flags(ins.addr, kind, n, lines)
            return ["{ " + " ".join(lines) + " }"] if pre else lines
        if not writes:
            return []
        if op in ("+", "-") and d.kind == "r" and d.reg == "esp" and s.kind == "i":
            v = s.imm & 0xFFFFFFFF
            v = v - (1 << 32) if v & 0x80000000 else v
            if op == "-":
                v = -v
            if abs(v) < SMALL_DISP:  # кадр/аргументы: без заворота, как push/pop
                return [f"esp += {v}u;" if v >= 0 else f"esp -= {-v}u;"]
        expr = f"({a_} {op} {b_})"
        if mk != 0xFFFFFFFF and op in ("+", "-"):
            expr = f"({expr} & {hx(mk)})"
        return ["{ " + pre + self.wr(d, expr, AX=AX) + " }"] if pre else [self.wr(d, expr, AX=AX)]

    def i_add(self, ins):
        return self.alu(ins, "add", "+")

    def i_sub(self, ins):
        return self.alu(ins, "sub", "-")

    def i_cmp(self, ins):
        return self.alu(ins, "sub", "-")

    def i_and(self, ins):
        return self.alu(ins, "logic", "&")

    def i_or(self, ins):
        return self.alu(ins, "logic", "|")

    def i_xor(self, ins):
        return self.alu(ins, "logic", "^")

    def i_test(self, ins):
        return self.alu(ins, "logic", "&")

    def i_adc(self, ins):
        return self.adc_sbb(ins, "+")

    def i_sbb(self, ins):
        return self.adc_sbb(ins, "-")

    def adc_sbb(self, ins, op):
        d, s = ins.ops
        n = d.size
        mk = MASK[n]
        sb = n * 8 - 1
        self.uses.add("cf")
        AX = None
        pre = ""
        if d.kind == "m":
            pre = f"u8 *Ap = {self.ha(d)}; "
            AX = "Ap"
        lines = [f"{pre}u32 a = {self.rd(d, AX=AX)}, b = {self.rd(s, n)}, ci = cf;",
                 f"u32 r = (a {op} b {op} ci) & {hx(mk)};", self.wr(d, "r", AX=AX)]
        mask = self.mat.get(ins.addr, 0)
        if op == "+":
            ex = {C: "(ci ? r <= a : r < a)", O: f"(((~(a ^ b) & (a ^ r)) >> {sb}) & 1)"}
        else:
            ex = {C: "(ci ? a <= b : a < b)", O: f"((((a ^ b) & (a ^ r)) >> {sb}) & 1)"}
        ex.update({Z: "(r == 0)", S: f"((r >> {sb}) & 1)", P: "PARITY(r)", A: "(((a ^ b ^ r) >> 4) & 1)"})
        for bit in (C, P, A, Z, S, O):
            if mask & bit:
                lines.append(f"{NAMES[bit]} = {ex[bit]};")
                self.uses.add(NAMES[bit])
        return ["{ " + " ".join(lines) + " }"]

    def i_inc(self, ins):
        return self.incdec(ins, "inc", "+")

    def i_dec(self, ins):
        return self.incdec(ins, "dec", "-")

    def incdec(self, ins, kind, op):
        d = ins.ops[0]
        n = d.size
        mk = MASK[n]
        AX = None
        pre = ""
        if d.kind == "m":
            pre = f"u8 *Ap = {self.ha(d)}; "
            AX = "Ap"
        if self.setter_needed(ins.addr):
            self.uses |= {"fa", "fb", "fr"}
            lines = [f"{pre}fa = {self.rd(d, AX=AX)}; fb = 1; fr = (fa {op} 1) & {hx(mk)};", self.wr(d, "fr", AX=AX)]
            self.emit_setter_flags(ins.addr, kind, n, lines)
            return ["{ " + " ".join(lines) + " }"]
        if d.kind == "r" and n == 4:
            return [f"{d.reg}{op}{op};"]
        return ["{ " + pre + self.wr(d, f"({self.rd(d, AX=AX)} {op} 1) & {hx(mk)}", AX=AX) + " }"]

    def i_neg(self, ins):
        d = ins.ops[0]
        n = d.size
        mk = MASK[n]
        AX = None
        pre = ""
        if d.kind == "m":
            pre = f"u8 *Ap = {self.ha(d)}; "
            AX = "Ap"
        if self.setter_needed(ins.addr):
            self.uses |= {"fa", "fb", "fr"}
            lines = [f"{pre}fa = 0; fb = {self.rd(d, AX=AX)}; fr = (0u - fb) & {hx(mk)};", self.wr(d, "fr", AX=AX)]
            self.emit_setter_flags(ins.addr, "sub", n, lines)
            return ["{ " + " ".join(lines) + " }"]
        return ["{ " + pre + self.wr(d, f"(0u - {self.rd(d, AX=AX)}) & {hx(mk)}", AX=AX) + " }"]

    def i_not(self, ins):
        d = ins.ops[0]
        mk = MASK[d.size]
        AX = None
        pre = ""
        if d.kind == "m":
            pre = f"u8 *Ap = {self.ha(d)}; "
            AX = "Ap"
        return ["{ " + pre + self.wr(d, f"(~{self.rd(d, AX=AX)}) & {hx(mk)}", AX=AX) + " }"]

    # ---- сдвиги
    def shift(self, ins, kind):
        d, s = ins.ops[0], ins.ops[1]
        n = d.size
        bits = n * 8
        mk = MASK[n]
        sb = bits - 1
        st = SIGNED[n]
        AX = None
        pre = ""
        if d.kind == "m":
            pre = f"u8 *Ap = {self.ha(d)}; "
            AX = "Ap"
        mask = self.mat.get(ins.addr, 0)
        const = s.kind == "i"
        k = f"{s.imm & 31}u" if const else f"({self.rd(s)} & 31u)"
        if const and (s.imm & 31) == 0:
            return []
        lines = [f"{pre}u32 a = {self.rd(d, AX=AX)}, k = {k}, r;"]
        if kind == "shl":
            lines.append(f"r = (k < {bits}) ? (a << k) & {hx(mk)} : 0;")
            cf = f"(k <= {bits} ? (a >> ({bits} - k)) & 1 : 0)"
            of = f"(((r >> {sb}) & 1) ^ {cf})"
        elif kind == "shr":
            lines.append(f"r = (k < {bits}) ? a >> k : 0;")
            cf = f"(k <= {bits} ? (a >> (k - 1)) & 1 : 0)"
            of = f"((a >> {sb}) & 1)"
        else:  # sar
            lines.append(f"r = (u32)(({st})a >> (k < {bits} ? k : {sb})) & {hx(mk)};")
            cf = f"(u32)((({st})a >> (k <= {bits} ? k - 1 : {sb})) & 1)"
            of = "0"
        lines.append(self.wr(d, "r", AX=AX))
        fl = []
        ex = {C: cf, O: of, Z: "(r == 0)", S: f"((r >> {sb}) & 1)", P: "PARITY(r)", A: "0"}
        for bit in (C, P, A, Z, S, O):
            if mask & bit:
                fl.append(f"{NAMES[bit]} = {ex[bit]};")
                self.uses.add(NAMES[bit])
        if fl:
            lines.append(" ".join(fl) if const else "if (k) { " + " ".join(fl) + " }")
        return ["{ " + " ".join(lines) + " }"]

    def i_shl(self, ins):
        return self.shift(ins, "shl")

    i_sal = i_shl

    def i_shr(self, ins):
        return self.shift(ins, "shr")

    def i_sar(self, ins):
        return self.shift(ins, "sar")

    def rotate(self, ins, left):
        d, s = ins.ops
        n = d.size
        bits = n * 8
        mk = MASK[n]
        sb = bits - 1
        AX = None
        pre = ""
        if d.kind == "m":
            pre = f"u8 *Ap = {self.ha(d)}; "
            AX = "Ap"
        k = f"{s.imm & 31}u" if s.kind == "i" else f"({self.rd(s)} & 31u)"
        lines = [f"{pre}u32 a = {self.rd(d, AX=AX)}, k = {k} % {bits}u, r;"]
        if left:
            lines.append(f"r = k ? ((a << k) | (a >> ({bits} - k))) & {hx(mk)} : a;")
            cf, of = "(r & 1)", f"(((r >> {sb}) & 1) ^ (r & 1))"
        else:
            lines.append(f"r = k ? ((a >> k) | (a << ({bits} - k))) & {hx(mk)} : a;")
            cf, of = f"((r >> {sb}) & 1)", f"(((r >> {sb}) ^ (r >> {sb - 1})) & 1)"
        lines.append(self.wr(d, "r", AX=AX))
        mask = self.mat.get(ins.addr, 0)
        fl = []
        if mask & C:
            fl.append(f"cf = {cf};")
            self.uses.add("cf")
        if mask & O:
            fl.append(f"of = {of};")
            self.uses.add("of")
        if fl:
            raw = s.imm & 31 if s.kind == "i" else None
            lines.append(" ".join(fl) if raw else f"if ({k}) {{ " + " ".join(fl) + " }")
        return ["{ " + " ".join(lines) + " }"]

    def i_rol(self, ins):
        return self.rotate(ins, True)

    def i_ror(self, ins):
        return self.rotate(ins, False)

    def rc(self, ins, left):
        d, s = ins.ops
        n = d.size
        bits = n * 8
        mk = MASK[n]
        sb = bits - 1
        self.uses.add("cf")
        AX = None
        pre = ""
        if d.kind == "m":
            pre = f"u8 *Ap = {self.ha(d)}; "
            AX = "Ap"
        k = f"{s.imm & 31}u" if s.kind == "i" else f"({self.rd(s)} & 31u)"
        if left:
            step = f"{{ u32 nc = (a >> {sb}) & 1; a = ((a << 1) | cf) & {hx(mk)}; cf = nc; }}"
        else:
            step = f"{{ u32 nc = a & 1; a = (a >> 1) | (cf << {sb}); cf = nc; }}"
        lines = [f"{pre}u32 a = {self.rd(d, AX=AX)}, k = {k} % {bits + 1}u;", f"while (k--) {step}",
                 self.wr(d, "a", AX=AX)]
        if self.mat.get(ins.addr, 0) & O:
            self.uses.add("of")
            lines.append(f"of = ((a >> {sb}) ^ cf) & 1;" if left else f"of = ((a >> {sb}) ^ (a >> {sb - 1})) & 1;")
        return ["{ " + " ".join(lines) + " }"]

    def i_rcl(self, ins):
        return self.rc(ins, True)

    def i_rcr(self, ins):
        return self.rc(ins, False)

    def shxd(self, ins, left):
        d, s, cnt = ins.ops
        n = d.size
        if n != 4:
            raise EmitError("16-bit shld/shrd")
        AX = None
        pre = ""
        if d.kind == "m":
            pre = f"u8 *Ap = {self.ha(d)}; "
            AX = "Ap"
        k = f"{cnt.imm & 31}u" if cnt.kind == "i" else f"({self.rd(cnt)} & 31u)"
        if left:
            r = "k ? (a << k) | (b >> (32 - k)) : a"
            cf = "(a >> (32 - k)) & 1"
        else:
            r = "k ? (a >> k) | (b << (32 - k)) : a"
            cf = "(a >> (k - 1)) & 1"
        lines = [f"{pre}u32 a = {self.rd(d, AX=AX)}, b = {self.rd(s)}, k = {k}, r = {r};", self.wr(d, "r", AX=AX)]
        mask = self.mat.get(ins.addr, 0)
        fl = []
        ex = {C: cf, O: "((r ^ a) >> 31) & 1", Z: "(r == 0)", S: "(r >> 31)", P: "PARITY(r)", A: "0"}
        for bit in (C, P, A, Z, S, O):
            if mask & bit:
                fl.append(f"{NAMES[bit]} = {ex[bit]};")
                self.uses.add(NAMES[bit])
        if fl:
            lines.append("if (k) { " + " ".join(fl) + " }")
        return ["{ " + " ".join(lines) + " }"]

    def i_shld(self, ins):
        return self.shxd(ins, True)

    def i_shrd(self, ins):
        return self.shxd(ins, False)

    # ---- умножение/деление
    def mulflags(self, ins, cond_expr, r_expr, n):
        mask = self.mat.get(ins.addr, 0)
        out = []
        sb = n * 8 - 1
        for bit, e in ((C, cond_expr), (O, cond_expr), (Z, f"({r_expr} == 0)"), (S, f"(({r_expr} >> {sb}) & 1)"),
                       (P, f"PARITY({r_expr})"), (A, "0")):
            if mask & bit:
                out.append(f"{NAMES[bit]} = {e};")
                self.uses.add(NAMES[bit])
        return out

    def i_mul(self, ins):
        s = ins.ops[0]
        n = s.size
        if n == 4:
            lines = [f"u64 p = (u64)eax * {self.rd(s)};", "eax = (u32)p; edx = (u32)(p >> 32);"]
            lines += self.mulflags(ins, "(edx != 0)", "eax", 4)
        elif n == 2:
            lines = [f"u32 p = (eax & 0xFFFFu) * {self.rd(s)};",
                     "eax = (eax & 0xFFFF0000u) | (p & 0xFFFFu); edx = (edx & 0xFFFF0000u) | (p >> 16);"]
            lines += self.mulflags(ins, "((p >> 16) != 0)", "(p & 0xFFFFu)", 2)
        else:
            lines = [f"u32 p = (eax & 0xFFu) * {self.rd(s)};", "eax = (eax & 0xFFFF0000u) | (p & 0xFFFFu);"]
            lines += self.mulflags(ins, "((p >> 8) != 0)", "(p & 0xFFu)", 1)
        return ["{ " + " ".join(lines) + " }"]

    def i_imul(self, ins):
        ops = ins.ops
        if len(ops) == 1:
            s = ops[0]
            n = s.size
            if n == 4:
                lines = [f"s64 p = (s64)(s32)eax * (s32){self.rd(s)};", "eax = (u32)p; edx = (u32)((u64)p >> 32);"]
                lines += self.mulflags(ins, "(p != (s64)(s32)p)", "eax", 4)
            elif n == 2:
                lines = [f"s32 p = (s32)(s16)(eax & 0xFFFFu) * (s16){self.rd(s)};",
                         "eax = (eax & 0xFFFF0000u) | ((u32)p & 0xFFFFu); "
                         "edx = (edx & 0xFFFF0000u) | (((u32)p >> 16) & 0xFFFFu);"]
                lines += self.mulflags(ins, "(p != (s32)(s16)p)", "((u32)p & 0xFFFFu)", 2)
            else:
                lines = [f"s32 p = (s32)(s8)(eax & 0xFFu) * (s8){self.rd(s)};",
                         "eax = (eax & 0xFFFF0000u) | ((u32)p & 0xFFFFu);"]
                lines += self.mulflags(ins, "(p != (s32)(s8)p)", "((u32)p & 0xFFu)", 1)
            return ["{ " + " ".join(lines) + " }"]
        if len(ops) == 2:
            d, s = ops
            a, b = self.rd(d), self.rd(s, d.size)
        else:
            d, s, imm = ops
            a, b = self.rd(s), hx(imm.imm & MASK[d.size])
        n = d.size
        st = SIGNED[n]
        lines = [f"s64 p = (s64)({st}){a} * (s64)({st}){b}; u32 r = (u32)p & {hx(MASK[n])};", self.wr(d, "r")]
        lines += self.mulflags(ins, f"(p != (s64)({st})p)", "r", n)
        return ["{ " + " ".join(lines) + " }"]

    def i_div(self, ins):
        s = ins.ops[0]
        n = s.size
        a = hx(ins.addr)
        self.uses.update(())
        if n == 4:
            lines = [f"u32 d = {self.rd(s)};", f"if (!d) {{ SPILL(); rt_divzero(c, {a}); return; }}",
                     "u64 x = ((u64)edx << 32) | eax; u64 q = x / d;",
                     f"if (q >> 32) {{ SPILL(); rt_divzero(c, {a}); return; }}",
                     "eax = (u32)q; edx = (u32)(x % d);"]
        elif n == 2:
            lines = [f"u32 d = {self.rd(s)};", f"if (!d) {{ SPILL(); rt_divzero(c, {a}); return; }}",
                     "u32 x = ((edx & 0xFFFFu) << 16) | (eax & 0xFFFFu); u32 q = x / d;",
                     f"if (q > 0xFFFFu) {{ SPILL(); rt_divzero(c, {a}); return; }}",
                     "eax = (eax & 0xFFFF0000u) | q; edx = (edx & 0xFFFF0000u) | (x % d);"]
        else:
            lines = [f"u32 d = {self.rd(s)};", f"if (!d) {{ SPILL(); rt_divzero(c, {a}); return; }}",
                     "u32 x = eax & 0xFFFFu; u32 q = x / d;",
                     f"if (q > 0xFFu) {{ SPILL(); rt_divzero(c, {a}); return; }}",
                     "eax = (eax & 0xFFFF0000u) | ((x % d) << 8) | q;"]
        return ["{ " + " ".join(lines) + " }"]

    def i_idiv(self, ins):
        s = ins.ops[0]
        n = s.size
        a = hx(ins.addr)
        if n == 4:
            lines = [f"s32 d = (s32){self.rd(s)};", f"if (!d) {{ SPILL(); rt_divzero(c, {a}); return; }}",
                     "s64 x = (s64)(((u64)edx << 32) | eax);",
                     f"if (d == -1 && x == INT64_MIN) {{ SPILL(); rt_divzero(c, {a}); return; }}",
                     "s64 q = x / d;",
                     f"if (q != (s32)q) {{ SPILL(); rt_divzero(c, {a}); return; }}",
                     "eax = (u32)q; edx = (u32)(s32)(x % d);"]
        elif n == 2:
            lines = [f"s32 d = (s16){self.rd(s)};", f"if (!d) {{ SPILL(); rt_divzero(c, {a}); return; }}",
                     "s32 x = (s32)(((edx & 0xFFFFu) << 16) | (eax & 0xFFFFu)); s32 q = x / d;",
                     f"if (q != (s16)q) {{ SPILL(); rt_divzero(c, {a}); return; }}",
                     "eax = (eax & 0xFFFF0000u) | ((u32)q & 0xFFFFu); edx = (edx & 0xFFFF0000u) | ((u32)(x % d) & 0xFFFFu);"]
        else:
            lines = [f"s32 d = (s8){self.rd(s)};", f"if (!d) {{ SPILL(); rt_divzero(c, {a}); return; }}",
                     "s32 x = (s16)(eax & 0xFFFFu); s32 q = x / d;",
                     f"if (q != (s8)q) {{ SPILL(); rt_divzero(c, {a}); return; }}",
                     "eax = (eax & 0xFFFF0000u) | (((u32)(x % d) & 0xFFu) << 8) | ((u32)q & 0xFFu);"]
        return ["{ " + " ".join(lines) + " }"]

    # ---- биты
    def bt_common(self, ins, op):
        d, s = ins.ops
        n = d.size
        bits = n * 8
        if d.kind == "m" and s.kind == "r":
            # битовый адрес может выходить за операнд
            lines = [f"s32 bo = (s32){self.rd(s)}; u8 *Ap = m + (u32)({self.ea(d)} + (u32)((bo >> 5) * 4)); "
                     "u32 k = (u32)bo & 31;", "u32 a = ld32(Ap);"]
            wa = "st32(Ap, r);"
        else:
            AX = None
            pre = ""
            if d.kind == "m":
                pre = f"u8 *Ap = {self.ha(d)}; "
                AX = "Ap"
            k = f"({s.imm & (bits - 1)}u)" if s.kind == "i" else f"({self.rd(s)} & {bits - 1}u)"
            lines = [f"{pre}u32 k = {k}; u32 a = {self.rd(d, AX=AX)};"]
            wa = self.wr(d, "r", AX=AX)
        if self.mat.get(ins.addr, 0) & C:
            lines.append("cf = (a >> k) & 1;")
            self.uses.add("cf")
        if op:
            lines.append({"bts": "u32 r = a | (1u << k);", "btr": "u32 r = a & ~(1u << k);",
                          "btc": "u32 r = a ^ (1u << k);"}[op])
            lines.append(wa)
        return ["{ " + " ".join(lines) + " }"]

    def i_bt(self, ins):
        return self.bt_common(ins, None)

    def i_bts(self, ins):
        return self.bt_common(ins, "bts")

    def i_btr(self, ins):
        return self.bt_common(ins, "btr")

    def i_btc(self, ins):
        return self.bt_common(ins, "btc")

    def i_bsf(self, ins):
        d, s = ins.ops
        lines = [f"u32 v = {self.rd(s)};"]
        if self.mat.get(ins.addr, 0) & Z:
            lines.append("zf = (v == 0);")
            self.uses.add("zf")
        lines.append("if (v) " + self.wr(d, "(u32)__builtin_ctz(v)"))
        return ["{ " + " ".join(lines) + " }"]

    def i_bsr(self, ins):
        d, s = ins.ops
        lines = [f"u32 v = {self.rd(s)};"]
        if self.mat.get(ins.addr, 0) & Z:
            lines.append("zf = (v == 0);")
            self.uses.add("zf")
        lines.append("if (v) " + self.wr(d, "(u32)(31 - __builtin_clz(v))"))
        return ["{ " + " ".join(lines) + " }"]

    # ---- атомарные
    def i_xadd(self, ins):
        d, s = ins.ops
        n = d.size
        AX = None
        pre = ""
        if d.kind == "m":
            pre = f"u8 *Ap = {self.ha(d)}; "
            AX = "Ap"
        need = self.setter_needed(ins.addr)
        if need:
            self.uses |= {"fa", "fb", "fr"}
        if d.kind == "m" and n == 4 and ins.lock:
            lines = [f"{pre}u32 b = {self.rd(s)}; u32 a = __atomic_fetch_add((u32 *)Ap, b, __ATOMIC_SEQ_CST);",
                     "u32 r = a + b;", self.wr(s, "a")]
        else:
            lines = [f"{pre}u32 a = {self.rd(d, AX=AX)}, b = {self.rd(s)}; u32 r = (a + b) & {hx(MASK[n])};",
                     self.wr(s, "a"), self.wr(d, "r", AX=AX)]
        if need:
            lines.append("fa = a; fb = b; fr = r;")
            self.emit_setter_flags(ins.addr, "add", n, lines)
        return ["{ " + " ".join(lines) + " }"]

    def i_cmpxchg(self, ins):
        d, s_ = ins.ops
        n = d.size
        acc = {4: "eax", 2: "(eax & 0xFFFFu)", 1: "(eax & 0xFFu)"}[n]
        AX = None
        pre = ""
        if d.kind == "m":
            pre = f"u8 *Ap = {self.ha(d)}; "
            AX = "Ap"
        if d.kind == "m" and n == 4:
            lines = [f"{pre}u32 accv = eax, dv = accv;",
                     f"if (!__atomic_compare_exchange_n((u32 *)Ap, &dv, {self.rd(s_)}, 0, __ATOMIC_SEQ_CST, "
                     "__ATOMIC_SEQ_CST)) eax = dv;"]
        else:
            lines = [f"{pre}u32 accv = {acc}, dv = {self.rd(d, AX=AX)};",
                     f"if (accv == dv) {{ {self.wr(d, self.rd(s_), AX=AX)} }} else {{ {self.wr_acc(n, 'dv')} }}"]
        if self.setter_needed(ins.addr):
            self.uses |= {"fa", "fb", "fr"}
            lines.append(f"fa = accv; fb = dv; fr = (fa - fb) & {hx(MASK[n])};")
            self.emit_setter_flags(ins.addr, "sub", n, lines)
        return ["{ " + " ".join(lines) + " }"]

    def wr_acc(self, n, v):
        if n == 4:
            return f"eax = {v};"
        if n == 2:
            return f"eax = (eax & 0xFFFF0000u) | (({v}) & 0xFFFFu);"
        return f"eax = (eax & 0xFFFFFF00u) | (({v}) & 0xFFu);"

    # ---- флаговые мелочи
    def i_clc(self, ins):
        if self.mat.get(ins.addr, 0) & C:
            self.uses.add("cf")
            return ["cf = 0;"]
        return []

    def i_stc(self, ins):
        if self.mat.get(ins.addr, 0) & C:
            self.uses.add("cf")
            return ["cf = 1;"]
        return []

    def i_cmc(self, ins):
        self.uses.add("cf")
        return ["cf ^= 1;"]

    def i_cld(self, ins):
        self.uses.add("df")
        return ["df = 0;"]

    def i_std(self, ins):
        self.uses.add("df")
        return ["df = 1;"]

    def i_sahf(self, ins):
        mask = self.mat.get(ins.addr, 0)
        out = []
        for bit, sh in ((C, 0), (P, 2), (A, 4), (Z, 6), (S, 7)):
            if mask & bit:
                out.append(f"{NAMES[bit]} = (eax >> {8 + sh}) & 1;")
                self.uses.add(NAMES[bit])
        return out

    def i_lahf(self, ins):
        self.uses |= {"cf", "pf", "af", "zf", "sf"}
        return ["eax = (eax & 0xFFFF00FFu) | ((cf | 2u | (pf << 2) | (af << 4) | (zf << 6) | (sf << 7)) << 8);"]

    def i_pushfd(self, ins):
        self.uses |= {"cf", "pf", "af", "zf", "sf", "of", "df"}
        return ["esp -= 4; wr32(m, esp, cf | 2u | (pf << 2) | (af << 4) | (zf << 6) | (sf << 7) | (df << 10) | (of << 11) | 0x200u);"]

    def i_popfd(self, ins):
        self.uses |= {"cf", "pf", "af", "zf", "sf", "of", "df"}
        return ["{ u32 v = rd32(m, esp); esp += 4; cf = v & 1; pf = (v >> 2) & 1; af = (v >> 4) & 1; "
                "zf = (v >> 6) & 1; sf = (v >> 7) & 1; df = (v >> 10) & 1; of = (v >> 11) & 1; "
                "if (v & 0x200000u) { /* ID-бит: эмулируем отсутствие переключения */ } }"]

    def i_salc(self, ins):
        self.uses.add("cf")
        return ["eax = (eax & 0xFFFFFF00u) | (cf ? 0xFFu : 0u);"]

    # ---- условия
    def i_setcc(self, ins):
        cc = jcc_cc(ins)
        return [self.wr(ins.ops[0], f"({self.cond(ins.addr, cc)} ? 1u : 0u)")]

    def i_cmovcc(self, ins):
        cc = jcc_cc(ins)
        d, s = ins.ops
        return [f"if ({self.cond(ins.addr, cc)}) {self.wr(d, self.rd(s))}"]

    def i_jcc(self, ins):
        cc = jcc_cc(ins)
        t = ins.branch_target()
        cond = self.cond(ins.addr, cc)
        if self.mod.is_tail(self.entry, ins, t):
            return [f"if ({cond}) {{ {self.tail_call(ins, t)} }}"]
        return [f"if ({cond}) {self.goto(t)}"]

    def goto(self, t):
        if t in self.ins:
            return f"goto L_{t:08x};"
        return f'{{ SPILL(); rt_trap(c, {hx(t)}, "jump to undecoded code"); return; }}'

    def i_loop(self, ins):
        t = ins.branch_target()
        return [f"if (--ecx) {self.goto(t)}"]

    def i_loope(self, ins):
        t = ins.branch_target()
        return [f"if (--ecx && {self.cond(ins.addr, 'e')}) {self.goto(t)}"]

    def i_loopne(self, ins):
        t = ins.branch_target()
        return [f"if (--ecx && {self.cond(ins.addr, 'ne')}) {self.goto(t)}"]

    def i_jecxz(self, ins):
        t = ins.branch_target()
        return [f"if (!ecx) {self.goto(t)}"]

    # ---- поток управления
    def x87_before_call(self):
        pass

    def tail_intrinsic(self, ins, name):
        # jmp на интринсик: выполнить его и вернуться за вызывающего (его ret снял бы адрес возврата)
        body = " ".join(self.emit_intrinsic(ins, name))
        pops, pushes = X87_INTRINSICS[name]
        st = (" c->st_ret = (double)f0;" if self.use_f32 else " c->st_ret = f0;") if self.depth - pops + pushes >= 1 else ""
        written = regs.INTRINSIC_WRITES.get(name, 0)  # интринсик пишет eax/edx в этом же операторе
        return f"{body} esp += 4u;{st} {self.g_(self.sp(self.dirty() | written))} return;"

    def tail_call(self, ins, t):
        info = self.ctx.target_info(self.mod, t)
        if info.intrinsic:
            return self.tail_intrinsic(ins, info.intrinsic)
        if info.sym is None:
            return f"SPILL(); rt_call(c, {hx(t)}); P_(RELOAD();) return;"
        return self.tail_stmt(info, self.ctx.inline_sym(info.sym))

    def tail_stmt(self, info, sym):
        # хвостовой вызов: вызывающий прочитает out этой функции — пишем все грязные, не только touched цели
        if info.addr is None:  # функция хоста: с закреплёнными регистрами вызывающий ждёт их в регистрах
            return f"{self.host_sp(self.dirty())} {sym}(c); P_(RELOAD();) return;"
        return f"{self.g_(self.sp(self.dirty()))} {sym}(SNX_C_ARG); return;"

    def i_jmp(self, ins):
        t = ins.branch_target()
        if t is not None:
            if self.mod.is_tail(self.entry, ins, t):
                return [self.tail_call(ins, t)]
            return [self.goto(t)]
        imp = self.mod.iat_import(ins)
        if imp is not None:
            info = self.ctx.import_info(self.mod, imp)
            if info.intrinsic:
                return [self.tail_intrinsic(ins, info.intrinsic)]
            return [self.tail_stmt(info, info.sym)]
        jt = self.mod.jump_tables.get(ins.addr)
        o = ins.ops[0]
        if jt:
            self.has_switch = True
            cases = " ".join(f"case {hx(x)}: {self.goto(x)}" for x in dict.fromkeys(jt.targets))
            return [f"{{ u32 t = {self.rd(o, 4)}; switch (t) {{ {cases} default: G_(SPILL();) RT_ICALL(c, t); return; }} }}"]
        return [f"{{ u32 t = {self.rd(o, 4)}; G_(SPILL();) RT_ICALL(c, t); return; }}"]

    def i_call(self, ins):
        info = self.ctx.call_info(self.mod, ins)
        ret = hx(ins.next)
        if info.intrinsic:
            return self.emit_intrinsic(ins, info.intrinsic)
        lines = []
        if info.setjmp:
            # хостовый setjmp в кадре этой функции; longjmp гостя возвращается сюда же (RELOAD — его регистры;
            # с закреплёнными регистрами после обычного возврата они в регистрах — сначала в CPU)
            n = self.setjmps
            self.setjmps += 1
            lines.append(f"esp -= 4; st32(m + esp, {ret}); SPILL(); rt_setjmp_register(c, &sj{n}); "
                         f"if (!RT_SETJMP(&sj{n})) {{ {info.sym}(SNX_C_ARG); P_(SPILL();) }} RELOAD();")
        elif info.sym is not None:
            lines.append(f"esp -= 4; st32(m + esp, {ret}); " + self.call_stmt(info, self.ctx.inline_sym(info.sym)))
        else:
            o = ins.ops[0]
            gn = getattr(self.ctx, "gnatives_base", None)
            if (gn is not None and o.kind == "m" and o.size == 4 and o.seg in (None, "ds")
                    and o.base is None and o.index is not None and o.scale == 4):
                disp = o.disp & 0xFFFFFFFF
                if gn <= disp < gn + 0x4000 and (disp - gn) % 4 == 0:
                    base_idx = (disp - gn) >> 2
                    idx_r = self.r32(o.index)
                    gi = idx_r if base_idx == 0 else f"(u32)({idx_r} + {base_idx}u)"
                    lines.append(f"{{ u32 gi_ = {gi}; u32 t = {self.rd(o, 4)}; esp -= 4; st32(m + esp, {ret}); "
                                 f"{self.g_(self.sp(self.dirty()))} RT_GNATIVE_CALL(c, gi_, t); G_(RELOAD();) }}")
                else:
                    lines.append(f"{{ u32 t = {self.rd(o, 4)}; esp -= 4; st32(m + esp, {ret}); {self.g_(self.sp(self.dirty()))} "
                                 "RT_ICALL(c, t); G_(RELOAD();) }")
            else:
                lines.append(f"{{ u32 t = {self.rd(o, 4)}; esp -= 4; st32(m + esp, {ret}); {self.g_(self.sp(self.dirty()))} "
                             "RT_ICALL(c, t); G_(RELOAD();) }")
        rd = self.call_depth.get(ins.addr, 0)
        if rd:
            lines.append(f"f{self.depth} = (float)c->st_ret;" if self.use_f32 else f"f{self.depth} = c->st_ret;")
            self.max_depth = max(self.max_depth, self.depth + 1)
        if ins.next not in self.succ[ins.addr]:
            lines.append(f'rt_trap(c, {hx(ins.addr)}, "noreturn call returned"); return;')
        return lines

    def i_ret(self, ins):
        n = ins.ops[0].imm if ins.ops else 0
        lines = [f"esp += {4 + n}u;"]
        if self.depth_in.get(ins.addr, 0) >= 1:
            lines.append(f"c->st_ret = (double){self.st(0)};" if self.use_f32 else f"c->st_ret = {self.st(0)};")
        lines.append(f"{self.g_(self.sp(self.dirty()))} return;")
        return lines

    def i_int3(self, ins):
        return [f'SPILL(); rt_trap(c, {hx(ins.addr)}, "int3"); return;']

    def i_hlt(self, ins):
        return [f'SPILL(); rt_trap(c, {hx(ins.addr)}, "hlt"); return;']

    def i_ud2(self, ins):
        return [f'SPILL(); rt_trap(c, {hx(ins.addr)}, "ud2"); return;']

    def i_int(self, ins):
        return [f'SPILL(); rt_trap(c, {hx(ins.addr)}, "{ins.text}"); return;']

    i_into = i_int
    i_int1 = i_int
    i_iret = i_int
    i_iretd = i_int
    i_retf = i_int

    # регистры гостя — значениями и результатом, не адресом: адрес локальной, переданный в функцию, держит регистр
    # в памяти во всей функции (rdtsc есть в каждом замере статистики UE2, rep movs — в каждой копии структуры)
    def i_cpuid(self, ins):
        return ["{ u32 r[4]; rt_cpuid(eax, r); eax = r[0]; ebx = r[1]; ecx = r[2]; edx = r[3]; }"]

    def i_rdtsc(self, ins):
        return ["{ u64 t = RT_RDTSC(); eax = (u32)t; edx = (u32)(t >> 32); }"]

    # ---- строковые операции
    def i_string(self, ins):
        op = ins.string_op
        n = ins.ops[0].size if ins.ops else 4
        if op in ("movs", "stos", "lods"):
            n = ins.ops[0].size if op != "lods" else ins.ops[1].size
        if op == "scas":
            n = ins.ops[1].size
        if op == "cmps":
            n = ins.ops[0].size
        step = f"(df ? -{n} : {n})"
        acc = {4: "eax", 2: "(eax & 0xFFFFu)", 1: "(eax & 0xFFu)"}[n]
        rdf = {1: "(u32)m[{}]", 2: "(u32)rd16(m, {})", 4: "rd32(m, {})"}[n]
        wrf = {1: "m[{}] = (u8)({});", 2: "wr16(m, {}, (u16)({}));", 4: "wr32(m, {}, {});"}[n]
        if op == "movs":
            if ins.rep:
                nc = self.const_regs.get("ecx")
                if nc == 0:
                    return ["ecx = 0;"]
                self.uses.add("df")
                if nc is not None and nc <= REP_INLINE_MAX:
                    nb = (nc * n) & 0xFFFFFFFF
                    d = f"(df ? 0u - {nb}u : {nb}u)"
                    return [f"REP_MOVS(m, esi, edi, {nc}u, {n}, df); esi += {d}; edi += {d}; ecx = 0;"]
                d = f"(df ? 0u - ecx * {n}u : ecx * {n}u)"
                return [f"REP_MOVS(m, esi, edi, ecx, {n}, df); esi += {d}; edi += {d}; ecx = 0;"]
            self.uses.add("df")
            return [wrf.format("edi", rdf.format("esi")) + f" esi += {step}; edi += {step};"]
        if op == "stos":
            if ins.rep:
                nc = self.const_regs.get("ecx")
                if nc == 0:
                    return ["ecx = 0;"]
                self.uses.add("df")
                if nc is not None and nc <= REP_INLINE_MAX:
                    nb = (nc * n) & 0xFFFFFFFF
                    return [f"REP_STOS(m, edi, {nc}u, {acc}, {n}, df); edi += (df ? 0u - {nb}u : {nb}u); ecx = 0;"]
                return [f"REP_STOS(m, edi, ecx, {acc}, {n}, df); edi += (df ? 0u - ecx * {n}u : ecx * {n}u); ecx = 0;"]
            self.uses.add("df")
            return [wrf.format("edi", acc) + f" edi += {step};"]
        self.uses.add("df")
        if op == "lods":
            if ins.rep:
                raise EmitError("rep lods")
            return ["{ u32 v = " + rdf.format("esi") + "; " + self.wr_acc(n, "v") + f" esi += {step}; }}"]
        # scas / cmps: флаги как у cmp
        mask = self.mat.get(ins.addr, 0)
        if op == "scas":
            one = f"fa = {acc}; fb = {rdf.format('edi')}; fr = (fa - fb) & {hx(MASK[n])}; edi += {step};"
        else:
            one = f"fa = {rdf.format('esi')}; fb = {rdf.format('edi')}; fr = (fa - fb) & {hx(MASK[n])}; esi += {step}; edi += {step};"
        self.uses |= {"fa", "fb", "fr"}
        ex = self.flag_exprs("sub", n)
        fl = []
        for bit in (C, P, A, Z, S, O):
            if mask & bit:
                fl.append(f"{NAMES[bit]} = {ex[bit]};")
                self.uses.add(NAMES[bit])
        if not ins.rep:
            return [one + " " + " ".join(fl)]
        stop = "fr != 0" if ins.rep == "rep" else "fr == 0"
        return [f"if (ecx) {{ do {{ {one} ecx--; }} while (ecx && !({stop})); {' '.join(fl)} }}"]

    # ================================================================ x87
    def st(self, i):
        d = self.depth
        slot = d - 1 - i
        if slot < 0 or slot > 7:
            raise EmitError(f"ST({i}) at depth {d}")
        return f"f{slot}"

    def push_slot(self):
        d = self.depth
        if d >= 8:
            raise EmitError("x87 overflow")
        self.max_depth = max(self.max_depth, d + 1)
        return f"f{d}"

    def fmem_read(self, fmt, hp):
        if self.use_f32:
            return {
                "f32": f"ldf32({hp})", "i16": f"(float)(s16)ld16({hp})", "i32": f"(float)(s32)ld32({hp})",
            }[fmt]
        return {
            "f32": f"(double)ldf32({hp})", "f64": f"ldf64({hp})", "f80": f"rt_f80_to_f64({hp})",
            "i16": f"(double)(s16)ld16({hp})", "i32": f"(double)(s32)ld32({hp})",
            "i64": f"(double)(s64)ld64({hp})",
        }[fmt]

    def emit_x87(self, ins):
        if self.x87_bad:
            return [f'SPILL(); rt_unimpl(c, {hx(ins.addr)}, "x87 (bad stack analysis): {ins.text}"); return;']
        op = self.x87op(ins)
        k = op.kind
        a = op.args
        ft = "float" if self.use_f32 else "double"
        if k == "ld_mem":
            fmt, mem = a
            return [f"{self.push_slot()} = {self.fmem_read(fmt, self.ha(mem))};"]
        if k == "ld_st":
            return [f"{self.push_slot()} = {self.st(a[0])};"]
        if k == "ld_const":
            if self.use_f32:
                v = {"1": "1.0f", "0": "0.0f"}[a[0]]
            else:
                v = {"1": "1.0", "0": "0.0", "pi": "3.14159265358979323846", "l2t": "3.32192809488736234787",
                     "l2e": "1.44269504088896340736", "lg2": "0.30102999566398119521",
                     "ln2": "0.69314718055994530942"}[a[0]]
            return [f"{self.push_slot()} = {v};"]
        if k == "st_mem":
            fmt, mem, pop = a
            hp = self.ha(mem)
            v = self.st(0)
            dv = f"(double){v}" if self.use_f32 else v
            if fmt == "f32":
                return [f"stf32({hp}, {v});" if self.use_f32 else f"stf32({hp}, (float){v});"]
            if fmt == "f64":
                return [f"stf64({hp}, {v});"]
            if fmt == "f80":
                return [f"rt_f64_to_f80({hp}, {v});"]
            if fmt in ("i16", "i32", "i64"):
                bits = int(fmt[1:])
                w = {16: f"st16({hp}, (u16)FIST(c, {dv}, 16));", 32: f"st32({hp}, (u32)FIST(c, {dv}, 32));",
                     64: f"st64({hp}, (u64)FIST(c, {dv}, 64));"}[bits]
                return [w]
            if fmt.endswith("t"):  # fisttp — усечение
                bits = int(fmt[1:-1])
                return [{16: f"st16({hp}, (u16)FTRUNC({dv}, 16));", 32: f"st32({hp}, (u32)FTRUNC({dv}, 32));",
                         64: f"st64({hp}, (u64)FTRUNC({dv}, 64));"}[bits]]
        if k == "st_st":
            i, pop = a
            if i == 0:
                return []
            return [f"{self.st(i)} = {self.st(0)};"]
        if k == "fxch":
            i = a[0]
            if i == 0:
                return []
            return [f"{{ {ft} t = {self.st(0)}; {self.st(0)} = {self.st(i)}; {self.st(i)} = t; }}"]
        if k == "arith_mem":
            opn, fmt, mem = a
            return [self.farith(opn, self.st(0), self.st(0), self.fmem_read(fmt, self.ha(mem)))]
        if k == "arith_st":
            opn, dst, src, pop = a
            return [self.farith(opn, self.st(dst), self.st(dst), self.st(src))]
        if k in ("cmp_mem", "cmp_st"):
            if k == "cmp_mem":
                fmt, mem, pop = a
                b = self.fmem_read(fmt, self.ha(mem))
            else:
                i, pops, unord = a
                b = self.st(i)
            fcom = "FCOMF" if self.use_f32 else "FCOM"
            if ins.addr in self.x87_fused_cmp:
                self.uses.add("fxy")
                need_fsw = self.x87_fused_cmp[ins.addr]
                if need_fsw:
                    self.uses.add("fsw")
                    return [f"fx = {self.st(0)}; fy = {b}; fsw = {fcom}(fx, fy);"]
                return [f"fx = {self.st(0)}; fy = {b};"]
            self.uses.add("fsw")
            return [f"{{ {ft} x = {self.st(0)}, y = {b}; fsw = {fcom}(x, y); }}"]
        if k == "ftst":
            zero = "0.0f" if self.use_f32 else "0.0"
            fcom = "FCOMF" if self.use_f32 else "FCOM"
            if ins.addr in self.x87_fused_cmp:
                self.uses.add("fxy")
                need_fsw = self.x87_fused_cmp[ins.addr]
                if need_fsw:
                    self.uses.add("fsw")
                    return [f"fx = {self.st(0)}; fy = {zero}; fsw = {fcom}(fx, fy);"]
                return [f"fx = {self.st(0)}; fy = {zero};"]
            self.uses.add("fsw")
            return [f"fsw = {fcom}({self.st(0)}, {zero});"]
        if k == "fxam":
            self.uses.add("fsw")
            return [f"fsw = rt_fxam({self.st(0)});"]
        if k == "comi":
            i, pop = a
            if ins.addr in self.x87_fused_cmp:
                self.uses.add("fxy")
                return [f"fx = {self.st(0)}; fy = {self.st(i)};"]
            mask = self.mat.get(ins.addr, 0)
            lines = [f"{ft} x = {self.st(0)}, y = {self.st(i)}; int un = (x != x) || (y != y);"]
            for bit, e in ((Z, "(un || x == y)"), (P, "un"), (C, "(un || x < y)"), (O, "0"), (S, "0"), (A, "0")):
                if mask & bit:
                    lines.append(f"{NAMES[bit]} = {e};")
                    self.uses.add(NAMES[bit])
            return ["{ " + " ".join(lines) + " }"]
        if k == "fnstsw_ax":
            self.uses.add("fsw")
            top = (8 - self.depth) & 7
            return [f"eax = (eax & 0xFFFF0000u) | fsw | {top << 11}u;"]
        if k == "fnstsw_mem":
            self.uses.add("fsw")
            top = (8 - self.depth) & 7
            return [f"st16({self.ha(a[0])}, (u16)(fsw | {top << 11}u));"]
        if k == "fldcw":
            return [f"c->fpucw = ld16({self.ha(a[0])});"]
        if k == "fnstcw":
            return [f"st16({self.ha(a[0])}, (u16)c->fpucw);"]
        if k == "unary":
            f = a[0]
            v = self.st(0)
            if self.use_f32:
                e = {"chs": f"-{v}", "abs": f"fabsf({v})", "sqrt": f"sqrtf({v})"}[f]
                return [f"{v} = {e};"]
            sq = f"(double)(float)sqrt({v})" if self.fpu_single else f"sqrt({v})"
            e = {"chs": f"-{v}", "abs": f"fabs({v})", "sqrt": sq, "sin": f"X87_TRIG({v}, sin({v}))", "cos": f"X87_TRIG({v}, cos({v}))",
                 "rndint": f"rt_frndint(c, {v})", "f2xm1": f"(exp2({v}) - 1.0)"}[f]
            return [f"{v} = {e};"]
        if k == "binpop":
            f = a[0]
            s0, s1 = self.st(0), self.st(1)
            e = {"fyl2x": f"{s1} * log2({s0})", "fpatan": f"atan2({s1}, {s0})",
                 "fyl2xp1": f"{s1} * log2({s0} + 1.0)"}[f]
            return [f"{s1} = crt_nan({e}, {s0}, {s1});"]  # log отрицательного: -NaN x87, а не +NaN ARM
        if k == "bin":
            f = a[0]
            s0, s1 = self.st(0), self.st(1)
            if f == "fprem" or f == "fprem1":
                self.uses.add("fsw")
                fn = "fmod" if f == "fprem" else "remainder"
                return [f"{s0} = {fn}({s0}, {s1}); fsw = 0;"]
            if f == "fscale":
                return [f"{s0} = ldexp({s0}, (int)trunc({s1}));"]
        if k == "fptan":
            v = self.st(0)
            return [f"{v} = tan({v}); {self.push_slot()} = 1.0;"]
        if k == "fsincos":
            v = self.st(0)
            return [f"{{ double x = {v}; {v} = sin(x); {self.push_slot()} = cos(x); }}"]
        if k == "fcmov":
            cc, i = a
            return [f"if ({self.cond(ins.addr, cc)}) {self.st(0)} = {self.st(i)};"]
        if k in ("nop", "ffree"):
            return []
        if k == "fninit":
            return ["c->fpucw = 0x37Fu;"]
        return [f'SPILL(); rt_unimpl(c, {hx(ins.addr)}, "{ins.text}"); return;']

    def farith(self, opn, dst, x, y):
        e = {"add": f"{x} + {y}", "mul": f"{x} * {y}", "sub": f"{x} - {y}", "subr": f"{y} - {x}",
             "div": f"{x} / {y}", "divr": f"{y} / {x}"}[opn]
        if self.fpu_single:
            return f"{dst} = (double)(float)({e});"
        return f"{dst} = {e};"

    def emit_intrinsic(self, ins, name):
        d = self.depth
        s0 = self.st(0)
        if name in ("_ftol", "_ftol2", "_ftol2_sse"):
            dv = f"(double){s0}" if self.use_f32 else s0
            return [f"{{ s64 v = FTRUNC({dv}, 64); eax = (u32)v; edx = (u32)((u64)v >> 32); }}"]
        s1 = self.st(1) if d >= 2 else None
        two = {"_CIpow": f"{s1} = crt_pow({s1}, {s0});", "_CIfmod": f"{s1} = crt_fmod({s1}, {s0});",
               "_CIatan2": f"{s1} = crt_atan2({s1}, {s0});"}
        if name in two:
            return [two[name]]
        if name == "_CIsqrt":
            if self.use_f32:
                return [f"{s0} = sqrtf({s0});"]
            if self.fpu_single:
                return [f"{s0} = (double)(float)sqrt({s0});"]
        fn = {"_CIsin": "sin", "_CIcos": "cos", "_CItan": "tan", "_CIasin": "asin", "_CIacos": "acos",
              "_CIatan": "atan", "_CIsinh": "sinh", "_CIcosh": "cosh", "_CItanh": "tanh", "_CIexp": "crt_exp",
              "_CIlog": "log", "_CIlog10": "log10", "_CIsqrt": "sqrt"}[name]
        return [f"{s0} = {fn}({s0});"]

