"""Поиск функций и построение CFG.

Модель:
* «Сильные» точки входа (strong): экспорты, точка входа модуля, цели `call rel32`,
  цели релокаций из не-кодовых секций (vtable, указатели на функции, EH-таблицы),
  адреса функций, взятые операндом инструкции (`push offset f`). Прыжок `jmp` на сильную
  точку входа (кроме собственной) — хвостовой вызов.
* «Слабые» (weak): прочие найденные адреса кода. Получают свою C-функцию (для dispatch),
  но прыжки на них — внутренние goto.
* Функция = все инструкции, достижимые от входа по jcc/jmp/fallthrough без перехода через
  хвостовые вызовы. Код может дублироваться между функциями — это корректно.
"""
from dataclasses import dataclass, field

from x86 import decode

# Импорты, после вызова которых управление не возвращается.
NORETURN_IMPORTS = {
    "_CxxThrowException", "ExitProcess", "exit", "_exit", "abort", "longjmp", "RaiseException",
    "ExitThread", "_endthread", "_endthreadex", "FatalAppExitA", "FatalAppExitW", "TerminateProcess",
    "_amsg_exit", "_invalid_parameter",
}

TERMINATORS = {"ret", "retf", "iret", "iretd", "hlt", "ud2", "int3", "into", "int1", "int"}

# Инструкции, которых нет в коде MSVC для user-mode: признак того, что декодируются данные.
JUNK = {"in", "out", "insb", "insd", "insw", "outsb", "outsd", "outsw", "hlt", "cli", "sti", "lcall", "ljmp",
        "les", "lds", "lss", "lfs", "lgs", "bound", "arpl", "aaa", "aas", "daa", "das", "aam", "aad", "salc",
        "into", "iret", "iretd", "retf", "xlatb", "sldt", "lsl", "lar", "int1", "icebp", "pushal", "popal",
        "wait", "fwait", "clts", "invd", "wbinvd", "ud2", "lahf", "cmc", "bndldx", "bndstx"}
JUNK_SEG_REGS = {"es", "ss", "cs", "ds"}


# Исключения C++ MSVC: функция с кадром EH начинается с push ebp; mov ebp, esp; push -1; push <заглушка>, где
# заглушка — mov eax, <FuncInfo>; jmp __CxxFrameHandler. Фанклеты catch (из FuncInfo) возвращают в eax адрес
# продолжения; __CxxFrameHandler переходит туда (_JumpToContinuation) с EBP кадра функции. Такие функции получают
# обёртку с хостовым setjmp и вход «с продолжения» (emit.FuncCompiler.wrap, rt_eh_* в runtime). Продолжение вне
# разобранного тела (туда ведёт только catch: хвост функции с эпилогом) — отдельная функция, её зовут из кадра.
EH_MAGICS = {0x19930520, 0x19930521, 0x19930522}


@dataclass
class JumpTable:
    jmp_addr: int
    table: int
    targets: list


def is_nop_like(ins):
    """Многобайтовые заполнители MSVC: lea r, [r(+0)], mov r, r, nop."""
    if ins is None:
        return False
    if ins.mnem == "nop":
        return True
    if ins.mnem in ("lea", "mov") and len(ins.ops) == 2 and ins.ops[0].kind == "r":
        r, o = ins.ops[0].reg, ins.ops[1]
        if ins.mnem == "mov":
            return o.kind == "r" and o.reg == r
        return o.kind == "m" and o.base == r and not o.index and not o.disp and not o.seg
    return False


@dataclass
class Function:
    entry: int
    insns: list = field(default_factory=list)  # отсортированные адреса инструкций
    labels: set = field(default_factory=set)
    tail_targets: set = field(default_factory=set)
    calls: set = field(default_factory=set)
    unknown_indirect: int = 0
    invalid: int = 0


class Module:
    def __init__(self, img, program):
        self.img = img
        self.program = program
        self.insns = {}  # addr -> Insn | None
        self.strong = set()
        self.weak = set()
        self.jump_tables = {}  # jmp_addr -> JumpTable
        self.jt_slots = set()  # адреса 4-байтовых слотов таблиц
        self.byte_tables = set()  # (адрес, длина) байтовых таблиц индексов switch в коде
        self.functions = {}
        self.eh_conts = {}  # вход функции с catch -> адреса продолжений catch
        self.eh_targets = set()  # продолжения, у которых есть своя функция
        self.code_ranges = [(s.va, s.va + s.vsize) for s in img.code_sections()]

    # ---------------------------------------------------------------- decode
    def in_code(self, va):
        for a, b in self.code_ranges:
            if a <= va < b:
                return True
        return False

    def insn(self, va):
        if va in self.insns:
            return self.insns[va]
        ins = None
        if self.in_code(va):
            ins = decode(self.img.data, va, va - self.img.base)
        self.insns[va] = ins
        return ins

    # ------------------------------------------------------------- classify
    def iat_import(self, ins):
        """Если операнд — слот IAT ([disp] без base/index), вернуть Import."""
        if not ins.ops:
            return None
        o = ins.ops[0]
        if o.kind == "m" and o.base is None and o.index is None and o.seg in (None, "ds"):
            return self.img.iat_by_va.get(o.disp)
        return None

    def is_noreturn_call(self, ins):
        imp = self.iat_import(ins)
        if imp is not None and imp.name in NORETURN_IMPORTS:
            return True
        tgt = ins.branch_target()
        if tgt is not None:
            t = self.insn(tgt)
            if t is not None and t.mnem == "jmp":
                imp = self.iat_import(t)
                if imp is not None and imp.name in NORETURN_IMPORTS:
                    return True
            name = self.program.export_name_at(tgt)
            if name in NORETURN_IMPORTS:
                return True
        return False

    def jump_table(self, ins):
        """`jmp [idx*4 + table]` → JumpTable или None."""
        if ins.mnem != "jmp" or not ins.ops or ins.ops[0].kind != "m":
            return None
        o = ins.ops[0]
        if o.base is not None or o.index is None or o.scale != 4 or not self.img.contains(o.disp):
            return None
        if ins.addr in self.jump_tables:
            return self.jump_tables[ins.addr]
        table = o.disp
        bound = self._jt_bound(ins, o.index)
        bt = self._byte_index_table(ins, o.index)
        if bt is not None:
            # двухуровневый switch: cmp ограничивает байтовую таблицу индексов, размер таблицы адресов —
            # максимальный индекс в ней + 1; сама байтовая таблица — данные в коде
            btab, n = bt
            bound = max(self.img.bytes_at(btab, n)) + 1
            self.byte_tables.add((btab, n))
        targets = []
        slot = table
        limit = bound if bound is not None else 4096
        if self.img.has_relocs and slot not in self.img.relocs and bound is None:
            # индексы с 1..3: слот 0 перекрыт кодом (хвост memmove: `jmp [edx*4 + tbl]`, edx = 1..3)
            for k in (1, 2, 3):
                if table + 4 * k in self.img.relocs and self.in_code(self.img.u32(table + 4 * k)):
                    slot = table + 4 * k
                    break
        while len(targets) < limit and self.img.contains(slot + 3):
            if self.img.has_relocs and slot not in self.img.relocs:
                break
            t = self.img.u32(slot)
            if not self.in_code(t):
                break
            if bound is None and not self.img.has_relocs and abs(t - ins.addr) > 0x10000:
                break
            targets.append(t)
            self.jt_slots.add(slot)
            slot += 4
        if self.img.has_relocs and (bound is None or not targets):
            # Отрицательные индексы (memcpy/memmove в CRT): релоцированные слоты перед базой таблицы.
            # Пример: `jmp [ecx*4 + tbl+0x10]` с ecx = -3 (хвост memmove при n < 4 и невыровненном dst) —
            # диспетчер стоит на 4 байта ПОСЛЕ настоящей таблицы, и без этого разбора таблица выходила пустой:
            # тогда сгенерированный switch не имел ни одного case, и любой переход уходил в RT_ICALL
            # («call to unknown guest address» на 0x13002af3 в msvcr71!memmove — вылет после роликов).
            # bound тут ни при чём: `cmp idx,N` перед таким jmp относится к предыдущему диспетчеру.
            slot = table - 4
            back = []
            while len(back) < 16 and slot in self.img.relocs and self.in_code(self.img.u32(slot)):
                back.append(self.img.u32(slot))
                self.jt_slots.add(slot)
                slot -= 4
            targets = back[::-1] + targets
        if not targets:
            # Таблицы нет (индекс отрицательный от чужой базы либо это не диспетчер): пустой JumpTable
            # нельзя кешировать — emit сгенерировал бы switch без case'ов, то есть RT_ICALL на каждый переход.
            return None
        jt = JumpTable(ins.addr, table, targets)
        self.jump_tables[ins.addr] = jt
        return jt

    def _byte_index_table(self, jmp, idx_reg):
        """`movzx idx, byte ptr [r + tab]` перед jmp и `cmp r, N` перед ним → (tab, N + 1)."""
        for back in range(2, 24):
            a = jmp.addr - back
            ins = self.insns.get(a) or (self.insn(a) if self.in_code(a) else None)
            if ins is None or ins.next > jmp.addr:
                continue
            if ins.mnem == "movzx" and len(ins.ops) == 2 and ins.ops[0].kind == "r" and ins.ops[0].reg == idx_reg:
                m = ins.ops[1]
                if m.kind != "m" or m.size != 1 or m.index or not m.base or not self.img.contains(m.disp):
                    return None
                n = self._jt_bound(ins, m.base)
                return (m.disp, n) if n is not None else None
        return None

    def _jt_bound(self, jmp, idx_reg):
        """Ищет `cmp idx, N` + `ja` непосредственно перед jmp (в пределах 12 инструкций назад по линейке)."""
        # Линейный проход назад по уже декодированным соседям: смотрим предыдущие ≤40 байт.
        best = None
        for back in range(2, 48):
            a = jmp.addr - back
            ins = self.insns.get(a) or (self.insn(a) if self.in_code(a) else None)
            if ins is None or ins.next > jmp.addr:
                continue
            if ins.mnem == "cmp" and len(ins.ops) == 2 and ins.ops[0].kind == "r" and ins.ops[1].kind == "i":
                r = ins.ops[0].reg
                if r == idx_reg or (idx_reg and r[-2:] == idx_reg[-2:]):
                    best = (ins.ops[1].imm & 0xFFFFFFFF) + 1
                    break
        if best is not None and best > 4096:
            best = None
        return best

    def plausible(self, va, limit=48):
        """Похоже ли место на начало функции: линейно декодируем до ret/jmp без мусора."""
        if not self.in_code(va):
            return False
        if self.in_code(self.img.u32(va)) and self.in_code(self.img.u32(va + 4)):
            return False  # нераспознанная таблица адресов в коде (jump-таблица), а не функция
        n = 0
        while n < limit:
            ins = self.insn(va)
            if ins is None or ins.mnem in JUNK:
                return False
            if ins.raw[:2] == b"\0\0" or (ins.mnem in ("push", "pop") and ins.ops and ins.ops[0].kind == "r"
                                             and ins.ops[0].reg in JUNK_SEG_REGS):
                return False
            if ins.mnem in TERMINATORS:
                return ins.mnem in ("ret", "int3") and n > 0 or ins.mnem == "ret"
            if ins.mnem == "jmp":
                return True
            n += 1
            va = ins.next
        return True

    # ---------------------------------------------------- global exploration
    def explore(self, seeds, weak_seeds=()):
        """Глобальный обход достижимого кода; цели call из сильного кода становятся strong,
        из кода, найденного только эвристикой (weak), — weak."""
        work = [(s, False) for s in seeds if self.in_code(s)] + [(s, True) for s in weak_seeds if self.in_code(s)]
        seen = set()
        new_strong = set()
        new_weak = set()
        entries = {s for s, _ in work}
        while work:
            va, wk = work.pop()
            while va not in seen:
                ins = self.insn(va)
                if ins is None:
                    break
                seen.add(va)
                m = ins.mnem
                if m == "call":
                    t = ins.branch_target()
                    if t is not None and self.in_code(t):
                        if t not in self.strong:
                            (new_weak if wk else new_strong).add(t)
                        work.append((t, wk))
                    if self.is_noreturn_call(ins):
                        break
                elif m == "jmp":
                    t = ins.branch_target()
                    if t is not None:
                        # функция из одного jmp — переходник инкрементальной линковки (ILT, вся таблица
                        # в начале Engine): цель — отдельная функция, а не продолжение тела переходника
                        if self.in_code(t) and t not in self.strong and (
                                va in self.strong or va in new_strong or va in self.weak or va in new_weak
                                or va in entries):
                            (new_weak if wk else new_strong).add(t)
                        work.append((t, wk))
                    else:
                        jt = self.jump_table(ins)
                        if jt:
                            work.extend((x, wk) for x in jt.targets)
                    break
                elif ins.is_jcc() or ins.is_loop():
                    t = ins.branch_target()
                    if t is not None:
                        work.append((t, wk))
                elif m in TERMINATORS:
                    break
                va = ins.next
        self.strong |= new_strong
        self.weak |= new_weak - self.strong
        return seen

    def collect_seeds(self):
        img = self.img
        strong = set(img.exports.values()) | set(img.export_ordinals.values())
        if img.entry:
            strong.add(img.entry)
        code_relocs = []
        for r in img.relocs:
            t = img.u32(r)
            if not self.in_code(t):
                continue
            if self.in_code(r):
                code_relocs.append(r)
            else:
                strong.add(t)
        trusted = set(img.exports.values()) | set(img.export_ordinals.values()) | ({img.entry} if img.entry else set())
        if not img.has_relocs:
            strong |= self._scan_data_pointers()
        self.strong |= {s for s in strong if self.in_code(s) and (s in trusted or self.plausible(s))}
        return code_relocs

    def _scan_data_pointers(self):
        """Для образов без релокаций (game.exe): выровненные dword в данных, указывающие в код."""
        out = set()
        for s in self.img.sections:
            if s.executable:
                continue
            for va in range(s.va, s.va + s.vsize - 3, 4):
                t = self.img.u32(va)
                if self.in_code(t):
                    out.add(t)
        return out

    def run(self):
        code_relocs = self.collect_seeds()
        seen = set()
        frontier = set(self.strong)
        weak_frontier = set()
        rounds = 0
        while frontier or weak_frontier:
            rounds += 1
            seen |= self.explore(frontier - seen, weak_frontier - seen)
            frontier = set()
            weak_frontier = set()
            # Операнды инструкций, указывающие на код: релокации внутри инструкций / immediates.
            # Релокация без найденной инструкции-владельца — слабый вход, но только когда сильный обход
            # исчерпан: иначе владелец (movzx с таблицей индексов switch) находится позже, а вход остаётся.
            orphans = set()
            for r in code_relocs:
                if r in self.jt_slots:
                    continue
                t = self.img.u32(r)
                owner = self._owner_insn(r, seen)
                if owner is not None:
                    # Сильный вход, только если адрес — непосредственный операнд (push/mov offset f).
                    # Смещение в памяти ([idx*4+table], [idx+bytetable]) — это данные в коде.
                    if any(o.kind == "i" and (o.imm & 0xFFFFFFFF) == t for o in owner.ops) \
                            and t not in self.strong and self.plausible(t):
                        self.strong.add(t)
                        frontier.add(t)
                elif t not in seen and t not in self.weak and self.plausible(t):
                    orphans.add(t)
            if not self.img.has_relocs:
                for va in list(seen):
                    ins = self.insns[va]
                    for o in ins.ops:
                        v = o.imm & 0xFFFFFFFF if o.kind == "i" else (o.disp if o.kind == "m" else None)
                        if v is None or ins.mnem in ("call", "jmp") or ins.is_jcc() or ins.is_loop():
                            continue
                        if self.in_code(v) and v not in self.strong and self.plausible(v):
                            self.strong.add(v)
                            frontier.add(v)
            frontier -= seen
            if not frontier:
                weak_frontier = orphans - seen
                self.weak |= weak_frontier
            if not frontier and not weak_frontier:
                weak_frontier = self._gap_seeds(seen) - seen
                self.weak |= weak_frontier
        self.reachable = seen
        # слабые входы, оказавшиеся внутри таблиц switch (найдены раньше, чем сам switch), — данные
        in_tables = set()
        for t, n in self.byte_tables:
            in_tables.update(range(t, t + n))
        for slot in self.jt_slots:
            in_tables.update(range(slot, slot + 4))
        self.weak -= in_tables
        self.rounds = rounds
        self.weak -= self.strong
        for e in sorted(self.strong | self.weak):
            self.functions[e] = self.build_function(e)
        self.eh_conts = {}
        extra = set()
        for e in sorted(self.functions):
            conts = self._eh_continuations(e)
            funclets = []
            if not conts:
                conts, funclets = self._seh_scopes(e)
            if conts:
                self.eh_conts[e] = conts
                extra |= {v for v in conts if v not in self.functions[e].insns and v not in self.functions}
            extra |= {v for v in funclets if v not in self.functions}
        for v in sorted(extra):
            self.weak.add(v)
            self.functions[v] = self.build_function(v)
            self.reachable |= set(self.functions[v].insns)
        self.eh_targets = {v for conts in self.eh_conts.values() for v in conts if v in self.functions}

    def _eh_continuations(self, entry):
        """Адреса продолжений catch функции entry (пусто — нет кадра EH C++ или catch не возвращаются)"""
        seq = [self.insn(entry)]
        while len(seq) < 4 and seq[-1] is not None:
            seq.append(self.insn(seq[-1].next))
        if None in seq:
            return []
        a, b, c, d = seq
        if not (a.mnem == "push" and a.ops[0].kind == "r" and a.ops[0].reg == "ebp"
                and b.mnem == "mov" and b.ops[0].kind == "r" and b.ops[0].reg == "ebp"
                and b.ops[1].kind == "r" and b.ops[1].reg == "esp"
                and c.mnem == "push" and c.ops[0].kind == "i" and (c.ops[0].imm & 0xFFFFFFFF) == 0xFFFFFFFF
                and d.mnem == "push" and d.ops[0].kind == "i"):
            return []
        stub = self.insn(d.ops[0].imm & 0xFFFFFFFF)
        if not (stub is not None and stub.mnem == "mov" and stub.ops[0].kind == "r" and stub.ops[0].reg == "eax"
                and stub.ops[1].kind == "i"):
            return []
        img = self.img
        fi = stub.ops[1].imm & 0xFFFFFFFF
        if not (img.contains(fi) and img.contains(fi + 20) and img.u32(fi) in EH_MAGICS):
            return []
        f = self.functions[entry]
        lo, hi = entry, max(f.insns) + 0x1000
        conts = set()
        ntry, ptry = img.u32(fi + 12), img.u32(fi + 16)
        for t in range(min(ntry, 256)):
            ncatch, parr = img.u32(ptry + 20 * t + 12), img.u32(ptry + 20 * t + 16)
            for k in range(min(ncatch, 64)):
                hf = self.functions.get(img.u32(parr + 16 * k + 12))
                for va in hf.insns if hf else ():
                    i = self.insns[va]
                    if i.mnem == "mov" and i.ops[0].kind == "r" and i.ops[0].reg == "eax" and i.ops[1].kind == "i":
                        v = i.ops[1].imm & 0xFFFFFFFF
                        if lo <= v < hi and self.in_code(v) and self.insn(v) is not None:
                            conts.add(v)
        return sorted(conts)

    def _is_except_handler3(self, va):
        """va — _except_handler3 msvcr70 (сам экспорт или переходник jmp [IAT] на него)"""
        if self.img.exports.get("_except_handler3") == va:
            return True
        i = self.insn(va) if self.in_code(va) else None
        if i is None or i.mnem != "jmp" or i.ops[0].kind != "m" or i.ops[0].base or i.ops[0].index:
            return False
        imp = self.img.iat_by_va.get(i.ops[0].disp & 0xFFFFFFFF)
        return imp is not None and imp.name == "_except_handler3"

    def _seh_scopes(self, entry):
        """SEH (__try) функции entry: (продолжения __except, фанклеты — фильтры и __finally).

        Кадр _except_handler3: push ebp; mov ebp, esp; push -1; push <таблица>; push _except_handler3 — или
        push <размер>; push <таблица>; call __SEH_prolog (тот кладёт _except_handler3 и ставит ebp так же: вход
        минус 4). Таблица — записи {внешний уровень, фильтр, обработчик}; фильтр 0 — __finally (фанклет, его зовут
        и он возвращается), иначе обработчик — блок __except: на него _except_handler3 переходит call-ом с EBP кадра
        и не возвращается — как на продолжение catch (longjmp в кадр функции, rt_eh_continue)."""
        seq = [self.insn(entry)]
        while len(seq) < 5 and seq[-1] is not None:
            seq.append(self.insn(seq[-1].next))
        if len(seq) < 3 or None in seq[:3]:
            return [], []

        def push_imm(i):
            return i is not None and i.mnem == "push" and i.ops[0].kind == "i"

        table = None
        a, b, c = seq[:3]
        if (len(seq) == 5 and None not in seq and a.mnem == "push" and a.ops[0].kind == "r" and a.ops[0].reg == "ebp"
                and b.mnem == "mov" and b.ops[0].kind == "r" and b.ops[0].reg == "ebp"
                and push_imm(c) and (c.ops[0].imm & 0xFFFFFFFF) == 0xFFFFFFFF
                and push_imm(seq[3]) and push_imm(seq[4]) and self._is_except_handler3(seq[4].ops[0].imm & 0xFFFFFFFF)):
            table = seq[3].ops[0].imm & 0xFFFFFFFF
        elif push_imm(a) and push_imm(b) and c.mnem == "call" and c.branch_target() is not None:
            h = self.insn(c.branch_target())
            if push_imm(h) and self._is_except_handler3(h.ops[0].imm & 0xFFFFFFFF):
                table = b.ops[0].imm & 0xFFFFFFFF
        if table is None:
            return [], []
        img = self.img
        f = self.functions[entry]
        lo, hi = entry, max(f.insns) + 0x1000
        conts, funclets = set(), set()
        for k in range(64):
            e = table + 12 * k
            if not img.contains(e + 11):
                break
            prev, filt, hnd = img.u32(e), img.u32(e + 4), img.u32(e + 8)
            ok = ((prev == 0xFFFFFFFF or prev < k) and lo <= hnd < hi and self.in_code(hnd) and self.insn(hnd)
                  and (filt == 0 or (self.in_code(filt) and self.insn(filt) is not None)))
            if not ok:
                break
            if filt:
                conts.add(hnd)
                funclets.add(filt)
            else:
                funclets.add(hnd)
        return sorted(conts), sorted(funclets)

    def _gap_seeds(self, seen):
        """Начала кода в непокрытых промежутках после выравнивания (int3/nop) — мёртвые/неучтённые функции."""
        img = self.img
        cov = bytearray(img.size)
        for va in seen:
            o = va - img.base
            cov[o:o + self.insns[va].size] = b"\1" * self.insns[va].size
        for s in self.jt_slots:
            cov[s - img.base:s - img.base + 4] = b"\1\1\1\1"
        for t, n in self.byte_tables:
            cov[t - img.base:t - img.base + n] = b"\1" * n
        out = set()
        for a, b in self.code_ranges:
            i = a - img.base
            end = b - img.base
            while i < end:
                if cov[i]:
                    i += 1
                    continue
                while i < end and not cov[i]:
                    if img.data[i] in (0xCC, 0x90):
                        i += 1
                    elif is_nop_like(ins := self.insn(img.base + i)):
                        i += ins.size  # выравнивание головы цикла внутри функции (lea esp, [esp])
                    else:
                        break
                if i < end and not cov[i]:
                    # пропуск начинается с не-padding байта: если перед ним padding или покрытый код — кандидат
                    if self.plausible(img.base + i):
                        out.add(img.base + i)
                    # до int3 по границам инструкций: байт 0xCC бывает внутри инструкции (fmul st(4) = d8 cc),
                    # и побайтовый пропуск заводил ложный вход посреди функции
                    while i < end and not cov[i] and img.data[i] != 0xCC:
                        ins = self.insn(img.base + i)
                        i += ins.size if ins is not None else 1
        return out

    def _owner_insn(self, va, seen):
        for k in range(1, 16):
            a = va - k
            if a in seen and self.insns[a].next > va:
                return self.insns[a]
        return None

    # ------------------------------------------------------------ functions
    def is_tail(self, entry, ins, t):
        """Переход на чужой сильный вход — хвостовой вызов; обратный переход внутрь [entry, ins] — цикл."""
        if t == entry or t not in self.strong:
            return False
        return not (entry < t <= ins.addr)

    def build_function(self, entry):
        f = Function(entry)
        work = [entry]
        body = set()
        while work:
            va = work.pop()
            while va not in body:
                ins = self.insn(va)
                if ins is None:
                    f.invalid += 1
                    break
                body.add(va)
                m = ins.mnem
                if m == "call":
                    t = ins.branch_target()
                    if t is not None:
                        f.calls.add(t)
                    if self.is_noreturn_call(ins):
                        break
                elif m == "jmp":
                    t = ins.branch_target()
                    if t is not None:
                        if self.is_tail(entry, ins, t):
                            f.tail_targets.add(t)
                        else:
                            f.labels.add(t)
                            work.append(t)
                    else:
                        jt = self.jump_tables.get(ins.addr)
                        if jt:
                            for t in jt.targets:
                                f.labels.add(t)
                                work.append(t)
                        else:
                            f.unknown_indirect += 1
                    break
                elif ins.is_jcc() or ins.is_loop():
                    t = ins.branch_target()
                    if t is not None:
                        if self.is_tail(entry, ins, t):
                            f.tail_targets.add(t)
                        else:
                            f.labels.add(t)
                            work.append(t)
                elif m in TERMINATORS:
                    break
                va = ins.next
        f.insns = sorted(body)
        return f


class Program:
    def __init__(self, images):
        self.images = images  # name(lower) -> Image
        self._export_at = {}
        for img in images.values():
            for n, va in img.exports.items():
                self._export_at.setdefault(va, n)
        self.modules = {}

    def export_name_at(self, va):
        return self._export_at.get(va)

    def resolve_import(self, imp):
        """Import -> ('guest', va) для рекомпилируемых модулей, иначе ('hle', dll, name)."""
        img = self.images.get(imp.dll)
        if img is None:
            return ("hle", imp.dll, imp.key)
        if imp.name is not None:
            if imp.name in img.exports:
                return ("guest", img.exports[imp.name])
            if imp.name in img.forwarders:
                fdll, fname = img.forwarders[imp.name].split(".", 1)
                return self.resolve_import(type(imp)(imp.iat_va, fdll.lower() + ".dll", fname, None))
            raise KeyError(f"{imp.dll}!{imp.name} not exported")
        return ("guest", img.export_ordinals[imp.ordinal])
