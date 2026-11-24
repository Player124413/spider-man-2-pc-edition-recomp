"""Дифф-тесты рекомпилированных функций против unicorn.

python3 -I tests/difftest/difftest.py --harness build/mac/tests/difftest/difftest_core \
    --game game/System --module Core.dll [--funcs 500] [--seeds 4] [--only 0x10101234]

Одинаковые случайные состояния (регистры, стек, scratch-память) подаются в харнесс
(рекомпилированный C) и в unicorn (эталонный x86). Сравниваются регистры и память.
"""
import argparse
import os
import random
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "recomp"))

from unicorn.unicorn_py3.arch.intel import HOOK_INSN_CPUID_CFUNC  # noqa: E402
from unicorn.unicorn_py3.unicorn import uccallback  # noqa: E402
from unicorn import (UC_ARCH_X86, UC_HOOK_INSN, UC_PROT_EXEC, UC_PROT_READ, UC_HOOK_BLOCK, UC_HOOK_CODE, UC_HOOK_INTR, UC_HOOK_MEM_INVALID, UC_HOOK_MEM_WRITE, UC_MODE_32,  # noqa: E402
                     Uc, UcError)
from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_EBP, UC_X86_REG_EBX, UC_X86_REG_ECX,  # noqa: E402
                               UC_X86_REG_EDI, UC_X86_REG_EDX, UC_X86_REG_EFLAGS, UC_X86_REG_ESI,
                               UC_X86_REG_EIP, UC_X86_REG_ESP, UC_X86_REG_FPCW, UC_X86_REG_FPSW,
                               UC_X86_REG_FPTAG, UC_X86_REG_FS, UC_X86_INS_RDTSC,
                               UC_X86_REG_GDTR, UC_X86_REG_SS, UC_X86_REG_DS, UC_X86_REG_ES)

from image import load_addrmap, load_image  # noqa: E402
from gen import Ctx  # noqa: E402
from main import load_program  # noqa: E402
from overrides import resolve as resolve_overrides  # noqa: E402

REGS = [UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX, UC_X86_REG_ESP, UC_X86_REG_EBP,
        UC_X86_REG_ESI, UC_X86_REG_EDI]
RNAMES = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]

THUNK_BASE, THUNK_SIZE = 0x00100000, 0x10000
GDT_ADDR = 0x00D00000
TEB = 0x00E00000
STACK, STACK_SIZE = 0x00F00000, 0x10000
SCRATCH, SCRATCH_SIZE = 0x20000000, 0x10000
SENTINEL = 0x00DEAD00  # адрес возврата теста; в unicorn там заглушка «fstp qword [ST0_SLOT]»
TSC0, TSC_STEP = 0x0000000100000000, 0x1000  # rdtsc, как det_rdtsc в harness.c
ST0_SLOT = (SENTINEL & ~0xFFF) + 0x1000  # отдельная страница: страница заглушки только для чтения
FPUCW = 0x27F


class Harness:
    def __init__(self, path, game):
        self.p = subprocess.Popen([path, game], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
        ok = self.p.stdout.read(4)
        if ok != b"OKOK":
            raise RuntimeError(f"harness failed to start: {ok!r}")

    def protect(self, regions, readonly):
        """Оставить доступными только эти области окна гостя (остальное — ловушка, как в unicorn),
        readonly — только для чтения."""
        self.p.stdin.write(b"P" + struct.pack("<I", len(regions)) + b"".join(struct.pack("<II", a, n) for a, n in regions)
                           + struct.pack("<I", len(readonly)) + b"".join(struct.pack("<II", a, n) for a, n in readonly))

    def write(self, addr, data):
        self.p.stdin.write(b"W" + struct.pack("<II", addr, len(data)) + data)

    def read(self, addr, n):
        self.p.stdin.write(b"R" + struct.pack("<II", addr, n))
        self.p.stdin.flush()
        return self.p.stdout.read(n)

    def run(self, regs, fs_base, fn):
        self.p.stdin.write(b"X" + struct.pack("<8I", *regs) + struct.pack("<III", FPUCW, fs_base, fn))
        self.p.stdin.flush()
        st, = struct.unpack("<I", self.p.stdout.read(4))
        out = list(struct.unpack("<8I", self.p.stdout.read(32)))
        st_ret, = struct.unpack("<d", self.p.stdout.read(8))
        taddr, n = struct.unpack("<II", self.p.stdout.read(8))
        msg = self.p.stdout.read(n).decode("utf-8", "replace")
        return st, out, st_ret, taddr, msg


def gdt_entry(base, limit, access, flags):
    v = limit & 0xFFFF
    v |= (base & 0xFFFFFF) << 16
    v |= (access & 0xFF) << 40
    v |= ((limit >> 16) & 0xF) << 48
    v |= (flags & 0xF) << 52
    v |= ((base >> 24) & 0xFF) << 56
    return struct.pack("<Q", v)


class Emu:
    def __init__(self, regions):
        self.uc = Uc(UC_ARCH_X86, UC_MODE_32)
        for a, n in regions:
            self.uc.mem_map(a, n)
        self.uc.mem_map(GDT_ADDR, 0x1000)
        gdt = b"\0" * 8
        gdt += gdt_entry(0, 0xFFFFF, 0x9A, 0xC)  # 1: code
        gdt += gdt_entry(0, 0xFFFFF, 0x92, 0xC)  # 2: data
        gdt += gdt_entry(TEB, 0xFFF, 0x92, 0x4)  # 3: fs -> TEB
        self.uc.mem_write(GDT_ADDR, gdt)
        self.uc.reg_write(UC_X86_REG_GDTR, (0, GDT_ADDR, len(gdt) - 1, 0))
        self.uc.reg_write(UC_X86_REG_FS, 3 << 3)
        self.uc.reg_write(UC_X86_REG_SS, 2 << 3)
        self.uc.reg_write(UC_X86_REG_DS, 2 << 3)
        self.uc.reg_write(UC_X86_REG_ES, 2 << 3)
        self.written = set()
        self.stop_reason = None
        self.uc.hook_add(UC_HOOK_MEM_WRITE, self._on_write)
        self.uc.hook_add(UC_HOOK_INTR, self._on_intr)
        self.uc.hook_add(UC_HOOK_MEM_INVALID, self._on_invalid)
        # rdtsc — детерминированная последовательность, как в харнессе (det_rdtsc): иначе функции со временем
        # (appSeconds) расходятся. Привязка Python не принимает RDTSC в hook_add, сигнатура — как у CPUID.
        self.tsc = TSC0
        self._rdtsc_cb = uccallback(self.uc, HOOK_INSN_CPUID_CFUNC)(self._on_rdtsc)
        self.uc._Uc__do_hook_add(UC_HOOK_INSN, self._rdtsc_cb, 1, 0, UC_X86_INS_RDTSC)

    def _on_rdtsc(self, uc, key):
        self.tsc += TSC_STEP
        uc.reg_write(UC_X86_REG_EAX, self.tsc & 0xFFFFFFFF)
        uc.reg_write(UC_X86_REG_EDX, self.tsc >> 32)
        return 1

    def _on_write(self, uc, access, addr, size, value, data):
        self.written.add(addr >> 12)
        if (addr + size - 1) >> 12 != addr >> 12:
            self.written.add((addr + size - 1) >> 12)

    def _on_intr(self, uc, intno, data):
        self.stop_reason = f"interrupt {intno}"
        uc.emu_stop()

    def _on_invalid(self, uc, access, addr, size, value, data):
        self.stop_reason = f"fault {addr:08x}"
        return False

    def trace_blocks(self, on):
        if on and not hasattr(self, "_th"):
            self.trace = []
            self.watch = None
            self.blocklog = None
            self._th = self.uc.hook_add(UC_HOOK_BLOCK, self._on_block)
            self._tc = self.uc.hook_add(UC_HOOK_CODE, self._on_code)

    def _on_code(self, uc, a, sz, d):
        if self.blocklog:
            regs = [uc.reg_read(r) for r in REGS]
            self.blocklog.write(f"{a:08x} " + " ".join(f"{v:08x}" for v in regs) + "\n")

    def _on_block(self, uc, a, sz, d):
        self.trace.append(a)
        if False:
            regs = [uc.reg_read(r) for r in REGS]
            self.blocklog.write(f"{a:08x} " + " ".join(f"{v:08x}" for v in regs) + "\n")
        if a == self.watch:
            regs = [uc.reg_read(r) for r in REGS]
            st = struct.unpack("<12I", uc.mem_read(regs[4], 48))
            print(f"[uc watch] {a:08x}: " + " ".join(f"{n}={v:08x}" for n, v in zip(RNAMES, regs)))
            print("  stack: " + " ".join(f"{v:08x}" for v in st))

    def run(self, regs, fn, max_insns=3_000_000):
        uc = self.uc
        for r, v in zip(REGS, regs):
            uc.reg_write(r, v)
        uc.reg_write(UC_X86_REG_EFLAGS, 0x202)
        uc.reg_write(UC_X86_REG_FPCW, FPUCW)
        uc.reg_write(UC_X86_REG_FPSW, 0)
        uc.reg_write(UC_X86_REG_FPTAG, 0xFFFF)
        self.stop_reason = None
        self.written.clear()
        self.tsc = TSC0
        try:
            uc.emu_start(fn, SENTINEL, timeout=2_000_000, count=max_insns)
        except UcError as e:
            if self.stop_reason is None:
                self.stop_reason = f"uc error {e}"
        out = [uc.reg_read(r) for r in REGS]
        eip = uc.reg_read(UC_X86_REG_EIP)
        if self.stop_reason is None and eip != SENTINEL:
            self.stop_reason = f"did not finish (eip {eip:08x})"
        # функция вернула значение в ST0 (TOP сдвинут) — достать его заглушкой
        self.st0 = None
        if self.stop_reason is None and (uc.reg_read(UC_X86_REG_FPSW) >> 11) & 7:
            uc.emu_start(SENTINEL, SENTINEL + 6, count=1)
            self.st0, = struct.unpack("<d", uc.mem_read(ST0_SLOT, 8))
        return out


def rand_val(rng, ptrs):
    r = rng.random()
    if r < 0.4:
        return rng.choice(ptrs)
    if r < 0.6:
        return rng.randrange(256)
    if r < 0.7:
        return 0
    if r < 0.8:
        return rng.choice([1, 2, 4, 0xFFFFFFFF, 0x80000000, 0x7FFFFFFF])
    return rng.getrandbits(32)


def volatile_free(prog, mods, ovr, ctx):
    """Функции, после которых ecx/edx (и eax при float-результате) не сравниваются: они (транзитивно
    и между модулями) зовут замены CRT или x87-интринсики _CI*. Оригинал оставляет там мусор (например,
    сохранённое слово FPU), замена — нет; по любому соглашению вызова x86-32 ecx после возврата
    не значим, edx — только для 64-битных результатов, eax — не значим при результате в ST0."""
    calls = {}
    hit = set()
    funcs = set()
    for mod in mods:
        funcs.update(mod.functions)
    for mod in mods:
        for e, f in mod.functions.items():
            calls[e] = set()
            for a in f.insns:
                ins = mod.insns.get(a)
                if ins is None or ins.mnem not in ("call", "jmp"):
                    continue
                t = ins.branch_target()
                if t is None and (imp := mod.iat_import(ins)) is not None:
                    r = prog.resolve_import(imp)
                    t = r[1] if r[0] == "guest" else None
                if t in ovr or (t is not None and ctx.target_info(ctx.module_for(t), t).intrinsic):
                    hit.add(e)
                elif t in funcs:
                    calls[e].add(t)
    changed = True
    while changed:
        changed = False
        for e, cs in calls.items():
            if e not in hit and cs & hit:
                hit.add(e)
                changed = True
    return hit


def module_closure(game, name, extra):
    """Модуль и всё, из чего он импортирует (транзитивно), среди модулей карты адресов."""
    amap = {n.lower(): (n, b) for n, b in load_addrmap().items()}
    names, todo = [], [name] + extra
    while todo:
        n = todo.pop(0)
        if n in names or n not in amap:
            continue
        names.append(n)
        img = load_image(os.path.join(game, amap[n][0]), amap[n][1])
        todo += sorted({i.dll for i in img.imports})
    return names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--harness", required=True)
    ap.add_argument("--game", required=True)
    ap.add_argument("--module", required=True)
    ap.add_argument("--with", dest="extra", action="append", default=["msvcr70.dll"])
    ap.add_argument("--funcs", type=int, default=300)
    ap.add_argument("--seeds", type=int, default=4)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--only", action="append", default=[])
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--check-image", action="store_true",
                    help="после каждого теста сверять весь образ (включая .text/.rdata) и остановиться на первом расхождении")
    ap.add_argument("--full-every", type=int, default=25,
                    help="полная сверка записываемых секций раз в N тестов (1 — ловить расхождения состояния сразу)")
    ap.add_argument("--blocklog", help="лог блоков unicorn (с --only, --seeds 1)")
    ap.add_argument("--debug", help="в обычном прогоне печатать входы/выходы этой функции")
    ap.add_argument("--watch", help="печатать регистры/стек unicorn при входе в адрес (с --only)")
    args = ap.parse_args()

    names = module_closure(args.game, args.module.lower(), [m.lower() for m in args.extra])
    prog = load_program(args.game, names)
    target = prog.modules[args.module.lower()]
    target.run()
    errors_file = os.path.join(ROOT, "build", "gen", target.img.name.lower().replace(".dll", "").replace(".exe", "_exe"),
                               "errors.txt")
    bad_funcs = set()
    if os.path.exists(errors_file):
        for ln in open(errors_file):
            if ln.strip():
                bad_funcs.add(int(ln.split(":")[0], 16))

    ovr = set(resolve_overrides(prog))
    for n in names:
        if n != target.img.name.lower():
            prog.modules[n].run()
    ctx = Ctx(prog, resolve_overrides(prog))
    loose = volatile_free(prog, [prog.modules[n] for n in names], ovr, ctx)
    h = Harness(args.harness, args.game)
    regions = []
    data_pages = []
    for n in names:
        img = prog.images[n]
        regions.append((img.base, img.size))
        for s in img.sections:
            if s.chars & 0x80000000:  # writable
                data_pages.append((s.va, (s.vsize + 0xFFF) & ~0xFFF))
    regions += [(THUNK_BASE, THUNK_SIZE), (TEB, 0x1000), (STACK, STACK_SIZE), (SCRATCH, SCRATCH_SIZE)]
    emu = Emu(regions + [(SENTINEL & ~0xFFF, 0x2000)])
    emu.uc.mem_write(SENTINEL, b"\xdd\x1d" + struct.pack("<I", ST0_SLOT))
    # запись тестируемой функции на страницу заглушки портила её (в харнессе эта страница не отображена)
    emu.uc.mem_protect(SENTINEL & ~0xFFF, 0x1000, UC_PROT_READ | UC_PROT_EXEC)
    for n in names:
        img = prog.images[n]
        emu.uc.mem_write(img.base, h.read(img.base, img.size))
    emu.uc.mem_write(THUNK_BASE, h.read(THUNK_BASE, THUNK_SIZE))
    # как в Windows: всё в образах, кроме записываемых секций, — только чтение (иначе бегущая запись на
    # случайных данных портит .text, и следующие тесты исполняют испорченный код с обеих сторон)
    ro = readonly_ranges(prog, names)
    for a, n in ro:
        emu.uc.mem_protect(a, n, UC_PROT_READ | UC_PROT_EXEC)
    h.protect(regions, ro)

    rng = random.Random(args.seed)
    if args.only:
        funcs = [int(x, 16) for x in args.only]
    else:
        intr = {e for e in target.functions if ctx.target_info(target, e).intrinsic}  # тела — ловушки
        skip = bad_funcs | ovr | intr  # замены CRT проверяет crtmath.py
        cands = sorted(e for e in target.strong if e not in skip and e in target.functions)
        rng.shuffle(cands)
        funcs = cands[:args.funcs]

    if args.only:
        emu.trace_blocks(True)
    ptrs = [SCRATCH + 4 * rng.randrange(SCRATCH_SIZE // 4 - 64) for _ in range(64)]
    stats = {"pass": 0, "fail": 0, "skip_uc": 0, "skip_both": 0, "harness_trap": 0}
    fails = []
    t0 = last = time.time()
    tests = 0
    total = len(funcs) * args.seeds
    for fn in funcs:
        for k in range(args.seeds):
            tests += 1
            if time.time() - last >= 10:
                last = time.time()
                print(f"[{last - t0:.0f}s] {tests}/{total} {stats}", file=sys.stderr, flush=True)
            # общее начальное состояние
            scratch = bytearray()
            for i in range(SCRATCH_SIZE // 4):
                scratch += struct.pack("<I", rand_val(rng, ptrs))
            teb = bytearray(0x1000)
            struct.pack_into("<III", teb, 0, 0xFFFFFFFF, STACK + STACK_SIZE, STACK)
            struct.pack_into("<I", teb, 0x18, TEB)
            stack = bytearray(rng.getrandbits(8) for _ in range(256)) * (STACK_SIZE // 256)
            esp = STACK + STACK_SIZE - 0x400
            struct.pack_into("<I", stack, esp - STACK, SENTINEL)
            for i in range(1, 12):
                struct.pack_into("<I", stack, esp - STACK + 4 * i, rand_val(rng, ptrs))
            regs = [rand_val(rng, ptrs) for _ in range(8)]
            regs[4] = esp
            regs[5] = rand_val(rng, ptrs)
            for a, data in ((SCRATCH, scratch), (TEB, teb), (STACK, stack)):
                h.write(a, bytes(data))
                emu.uc.mem_write(a, bytes(data))

            if args.only and args.watch:
                emu.watch = int(args.watch, 16)
            if args.only and args.blocklog:
                emu.blocklog = open(args.blocklog, "w")
            if args.only:
                emu.trace = []
            uout = emu.run(regs, fn)
            if emu_blocklog := getattr(emu, "blocklog", None):
                emu_blocklog.flush()
            if args.only:
                print(f"seed {k}: regs " + " ".join(f"{n}={v:08x}" for n, v in zip(RNAMES, regs)))
                print("  uc blocks: " + " ".join(f"{a:08x}" for a in emu.trace[:80]))
            st, hout, st_ret, taddr, msg = h.run(regs, TEB, fn)
            if args.verbose:
                print(f"{fn:08x}#{k}: uc={emu.stop_reason} rc=st{st} {taddr:08x} {msg}")
            if args.check_image and (bad := readonly_diff(h, emu, prog, names)):
                print(f"read-only sections diverged after {fn:08x}#{k} (uc={emu.stop_reason} rc=st{st} {msg}): {bad}")
                return 1
            if emu.stop_reason is not None:
                if st != 0:
                    stats["skip_both"] += 1
                else:
                    stats["skip_uc"] += 1
                resync(h, emu, data_pages)
                continue
            if st != 0:
                stats["harness_trap"] += 1
                fails.append((fn, k, f"harness trap at {taddr:08x}: {msg} (unicorn finished)"))
                resync(h, emu, data_pages)
                continue
            diffs = []
            for i in range(8):
                if fn in loose and (i in (1, 2) or i == 0 and emu.st0 is not None):
                    continue
                if uout[i] != hout[i]:
                    if is_nan32(uout[i]) and is_nan32(hout[i]):
                        NAN_DIFFS[0] += 1  # float, переложенный в регистр (знак NaN x87/ARM)
                        continue
                    diffs.append(f"{RNAMES[i]}: uc={uout[i]:08x} rc={hout[i]:08x}")
            if args.debug and fn == int(args.debug, 16):
                print(f"debug {fn:08x}#{k}: in esp={regs[4]:08x} args={[hex(x) for x in struct.unpack('<4I', bytes(stack[esp - STACK + 4:esp - STACK + 20]))]} "
                      f"uc out={[hex(x) for x in uout]} fpsw={emu.uc.reg_read(UC_X86_REG_FPSW):04x} st0={emu.st0!r} "
                      f"rc out={[hex(x) for x in hout]} st_ret={st_ret!r}")
            if emu.st0 is not None and not same_float(emu.st0, st_ret):
                diffs.append(f"st0: uc={emu.st0!r} rc={st_ret!r}")
            pages = set(emu.written)
            # стек ниже конечного esp — мёртвый (временные значения callee), не сравниваем
            live = max(STACK, min(uout[4], STACK + STACK_SIZE) & ~3)
            check = [(live, STACK + STACK_SIZE - live), (SCRATCH, SCRATCH_SIZE), (TEB, 0x1000)]
            full = tests % args.full_every == 0
            for a, n in data_pages:
                for p in range(a >> 12, (a + n) >> 12):
                    if full or p in pages:
                        check.append((p << 12, 0x1000))
            for a, n in check:
                ub = emu.uc.mem_read(a, n)
                hb = h.read(a, n)
                if ub != hb:
                    diffs.extend(mem_diff(a, ub, hb))
            if diffs:
                stats["fail"] += 1
                fails.append((fn, k, "; ".join(diffs[:6])))
                resync(h, emu, data_pages)
            else:
                stats["pass"] += 1
    dt = time.time() - t0
    print(f"{tests} tests in {dt:.1f}s: {stats}" + (f", NaN sign/payload diffs ignored: {NAN_DIFFS[0]}" if NAN_DIFFS[0] else ""))
    byfn = {}
    for fn, k, d in fails:
        byfn.setdefault(fn, []).append((k, d))
    names_of = export_names(os.path.join(ROOT, "build", "kb", "exports"))
    for fn, lst in sorted(byfn.items()):
        nm = names_of.get(fn) or prog.export_name_at(fn)
        print(f"FAIL {fn:08x}{' ' + nm if nm else ''} ({len(lst)}/{args.seeds}): {lst[0][1]}")
    h.p.stdin.write(b"Q")
    h.p.stdin.close()
    return 1 if fails else 0


def export_names(kb_dir):
    """Адрес -> демэнгл из каталога tools/recon/exports.py (если он собран)."""
    out = {}
    if os.path.isdir(kb_dir):
        for fn in os.listdir(kb_dir):
            with open(os.path.join(kb_dir, fn)) as f:
                next(f, None)
                for ln in f:
                    addr, _, name, dem = ln.rstrip("\n").split("\t")
                    out.setdefault(int(addr, 16), dem or name)
    return out


def same_float(a, b):
    """ST0: x87 с PC=53 и double хоста совпадают побитно, кроме пограничных случаев (денормалы, NaN)."""
    if struct.pack("<d", a) == struct.pack("<d", b) or (a != a and b != b):
        return True
    return a == a and b == b and abs(a - b) <= 1e-12 * max(abs(a), abs(b))


NAN_DIFFS = [0]


def is_nan32(v):
    return (v & 0x7F800000) == 0x7F800000 and (v & 0x7FFFFF) != 0


def mem_diff(base, ub, hb):
    out = []
    for i in range(0, len(ub), 4):
        u, r = ub[i:i + 4], hb[i:i + 4]
        if u != r:
            uf, = struct.unpack("<f", bytes(u).ljust(4, b"\0"))
            rf, = struct.unpack("<f", bytes(r).ljust(4, b"\0"))
            if uf == uf and rf == rf and abs(uf) > 1e-30 and abs(uf - rf) <= 1e-6 * abs(uf):
                continue  # погрешность x87 (80 бит) vs double
            if uf != uf and rf != rf:
                # NaN с другим знаком/мантиссой (float или старшее слово double): недопустимая операция
                # над числами даёт на x87 -NaN, на ARM +NaN; побитно не повторяем
                NAN_DIFFS[0] += 1
                continue
            out.append(f"mem {base + i:08x}: uc={bytes(u).hex()} rc={bytes(r).hex()}")
    return out


def readonly_diff(h, emu, prog, names):
    """Расхождения в незаписываемых секциях образов (там обе стороны должны совпадать всегда)."""
    for n in names:
        for s in prog.images[n].sections:
            if s.chars & 0x80000000 or not s.vsize:
                continue
            ub, hb = bytes(emu.uc.mem_read(s.va, s.vsize)), h.read(s.va, s.vsize)
            if ub != hb:
                bad = [i for i in range(0, s.vsize - 3, 4) if ub[i:i + 4] != hb[i:i + 4]]
                return (f"{n} {s.name}: " + ", ".join(f"{s.va + i:08x} uc={ub[i:i + 4].hex()} rc={hb[i:i + 4].hex()}"
                                                     for i in bad[:4]) + f" ... {len(bad)} words")
    return None


def readonly_ranges(prog, names):
    """Страницы образов вне записываемых секций."""
    out = []
    for n in names:
        img = prog.images[n]
        rw = set()
        for sec in img.sections:
            if sec.chars & 0x80000000:
                rw.update(range(sec.va >> 12, (sec.va + max(sec.vsize, 1) + 0xFFF) >> 12))
        start = None
        for p in range((img.base >> 12), (img.base + img.size + 0xFFF) >> 12):
            if p not in rw and start is None:
                start = p
            elif p in rw and start is not None:
                out.append((start << 12, (p - start) << 12))
                start = None
        if start is not None:
            out.append((start << 12, ((img.base + img.size + 0xFFF) >> 12 << 12) - (start << 12)))
    return out


def resync(h, emu, data_pages):
    for a, n in data_pages:
        emu.uc.mem_write(a, h.read(a, n))


if __name__ == "__main__":
    sys.exit(main())
