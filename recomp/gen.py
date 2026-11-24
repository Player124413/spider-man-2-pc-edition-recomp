"""Генерация C для набора модулей: контекст вызовов, сводки x87, файлы, таблицы."""
from concurrent.futures import ProcessPoolExecutor
import json
import multiprocessing
import os
import re
import zlib
from dataclasses import dataclass

import regs
from emit import FuncCompiler
from x87 import X87_INTRINSICS

FUNCS_PER_FILE = 300
# Листовые функции не длиннее (инструкций) встраиваются в места вызова: static inline копия fi_<адрес> в каждом
# файле, где её зовут. С restrict-указателем на CPU компилятор схлопывает SPILL/RELOAD вокруг вызова — у мелких
# функций (FName::operator==, FVector::SizeSquared, UObject::IsA) это и есть почти вся цена.
INLINE_MAX = int(os.environ.get("SNX_INLINE_MAX", "32"))  # 0 — без встраивания (для сравнения)

# Статически вложенные хелперы CRT без имени: распознаём по началу кода.
# setjmp гостя: адрес возврата — продолжение для longjmp (середина функции); вызывающий регистрирует
# хостовый jmp_buf, а rt_call, получив переход на это продолжение, делает хостовый longjmp в его кадр
SETJMP_EXPORTS = {"_setjmp3", "_setjmp"}

SIGNATURES = {
    bytes.fromhex("558bec83ec2083e4f0d9c0d9542418df7c2410df6c2410"): "_ftol2",
}

# Внешние (HLE) функции, возвращающие float/double в ST(0); остальные импорты x87 не трогают.
HLE_FLOAT_RET = {("vorbisfile.dll", "ov_time_total"), ("vorbisfile.dll", "ov_time_tell")}


INLINE_DECL = "static inline __attribute__((always_inline)) void"


@dataclass
class CallInfo:
    sym: str | None
    ret_depth: int | None
    intrinsic: str | None = None
    setjmp: bool = False  # msvcr71 _setjmp3/_setjmp: вызывающий делает хостовый setjmp (см. emit.i_call)
    addr: int | None = None  # гостевой адрес сгенерированной функции-цели (для сводок по регистрам)


def c_ident(s):
    return re.sub(r"[^A-Za-z0-9_]", "_", s)


def dll_stem(dll):
    d = dll.lower()
    return d[:-4] if d.endswith(".dll") else d


class Ctx:
    def __init__(self, prog, overrides=None, wraps=None):
        self.prog = prog
        self.summary = {}  # guest addr -> x87 exit depth
        self.overrides = overrides or {}  # guest addr -> (sym, ret_depth)
        self.wraps = wraps or {}  # guest addr -> sym обёртки (оригинал доступен ей как ovr_orig_<sym>)
        self.hle_used = {}  # sym -> (dll, name)
        self.reg_touched = {}  # guest addr -> регистры, которые функция (с вызываемыми) читает или пишет (regs.py)
        self.reg_out = {}  # guest addr -> регистры, которые к возврату могут измениться
        self._sig_cache = {}
        self._inline = {}  # guest addr -> тело fi_<адрес> или None
        self.mod_of = {}
        for img in prog.images.values():
            self.mod_of[img.name.lower()] = img
        # Базовый адрес таблицы диспетчера UnrealScript GNatives (4096 слотов): разный в EN и RUS Core.dll
        core_img = self.mod_of.get("core.dll")
        self.gnatives_base = (core_img.exports.get("?GNatives@@3PAP8UObject@@AEXAAUFFrame@@QAX@ZA")
                              if core_img is not None else None)

    def fname(self, addr):
        return f"f_{addr:08x}"

    def module_for(self, va):
        for name, mod in self.prog.modules.items():
            if mod.img.contains(va):
                return mod
        return None

    def import_sym(self, mod, imp):
        r = self.prog.resolve_import(imp)
        if r[0] == "guest":
            return self.target_info(self.module_for(r[1]), r[1]).sym or self.fname(r[1])
        dll, name = r[1], r[2]
        sym = f"hle_{c_ident(dll_stem(dll))}_{c_ident(name if not name.startswith('#') else 'ord' + name[1:])}"
        self.hle_used[sym] = (dll, name)
        return sym

    def import_info(self, mod, imp):
        r = self.prog.resolve_import(imp)
        if r[0] == "guest":
            tmod = self.module_for(r[1])
            return self.target_info(tmod, r[1])
        return CallInfo(self.import_sym(mod, imp), int((r[1].lower(), r[2]) in HLE_FLOAT_RET))

    def import_ret_depth(self, imp):
        r = self.prog.resolve_import(imp)
        if r[0] == "guest":
            info = self.target_info(self.module_for(r[1]), r[1])
            return info.ret_depth or 0
        return int((r[1].lower(), r[2]) in HLE_FLOAT_RET)

    def target_info(self, mod, t, depth=0):
        if t in self.overrides:
            sym, rd = self.overrides[t]
            return CallInfo(sym, rd)
        name = self.prog.export_name_at(t)
        if name in SETJMP_EXPORTS and mod is not None and t in mod.functions:
            return CallInfo(self.fname(t), 0, setjmp=True, addr=t)
        if name in X87_INTRINSICS:
            return CallInfo(None, None, intrinsic=name)
        if mod is not None:
            sig = self.signature_at(mod, t)
            if sig:
                return CallInfo(None, None, intrinsic=sig)
        if mod is not None:
            ins = mod.insn(t)
            if ins is not None and ins.mnem == "jmp":
                imp = mod.iat_import(ins)
                if imp is not None:
                    return self.import_info(mod, imp)
                # переходник инкрементальной компоновки MSVC (jmp rel32 на тело функции): вызов и запись таблицы —
                # сразу на тело (лишний переход, а у vtable через переходники — ещё и строка кеша кода на вызов)
                bt = ins.branch_target()
                tmod = self.module_for(bt) if bt is not None and depth < 4 else None
                if (tmod is not None and bt != t and t not in mod.eh_conts and t not in mod.eh_targets
                        and (bt in tmod.functions or bt in self.overrides)):
                    return self.target_info(tmod, bt, depth + 1)
        if mod is None or t not in mod.functions:
            return CallInfo(None, 0)  # не функция (мусорный код / вне образа) — rt_call по константе
        return CallInfo(self.fname(t), self.summary.get(t, 0), addr=t)

    def is_special(self, addr):
        """setjmp/longjmp гостя: сводки по регистрам для него консервативны (см. recomp/regs.py)."""
        return self.prog.export_name_at(addr) in SETJMP_EXPORTS

    def reg_masks(self, info):
        """-> (touched, out) цели вызова (recomp/regs.py): HLE, оверрайд, косвенный — (все, eax/ecx/edx/esp)."""
        if not regs.ENABLED or info.setjmp:
            return regs.ALL, regs.ALL
        if info.addr is not None:
            return self.reg_touched.get(info.addr, regs.ALL), self.reg_out.get(info.addr, regs.ALL)
        return regs.ALL, regs.ABI_OUT

    def inline_sym(self, sym):
        """f_<адрес> мелкой листовой функции -> fi_<адрес> (её копию допишет write_module), иначе sym."""
        m = re.fullmatch(r"f_([0-9a-f]{8})", sym or "")
        if not m:
            return sym
        t = int(m.group(1), 16)
        for _ in range(4):  # переходники инкрементальной компоновки MSVC (jmp на тело): экспорты Core — через них
            mod = self.module_for(t)
            ins = mod.insn(t) if mod is not None else None
            if ins is None or ins.mnem != "jmp" or ins.branch_target() is None:
                break
            t = ins.branch_target()
        return f"fi_{t:08x}" if self.inline_body(t) else sym

    def inline_body(self, t):
        if t in self._inline:
            return self._inline[t]
        self._inline[t] = None
        mod = self.module_for(t)
        f = mod.functions.get(t) if mod is not None else None
        if (f is None or f.calls or f.tail_targets or f.unknown_indirect or f.invalid or len(f.insns) > INLINE_MAX
                or t in mod.eh_conts or t in mod.eh_targets or t in self.overrides):
            return None
        if any(mod.insn(a).mnem in ("call", "int", "int3", "hlt", "ud2") for a in f.insns):
            return None
        fc = FuncCompiler(mod, f, self)
        code = fc.compile()
        head = f"void {self.fname(t)}(SNX_C_PARAM)\n"
        if (fc.errors or fc.x87_bad or fc.setjmps or not code.startswith(head)
                or re.search(r"\b(?:f|fc|fi|hle|ovr)_\w+\((?:c|SNX_C_ARG)\)|RT_ICALL|RT_GNATIVE_CALL|rt_call\(", code)):
            return None
        self._inline[t] = f"{INLINE_DECL} fi_{t:08x}(SNX_C_PARAM)\n" + code[len(head):]
        return self._inline[t]

    def signature_at(self, mod, t):
        if t in self._sig_cache:
            return self._sig_cache[t]
        name = None
        if mod.img.contains(t):
            head = mod.img.bytes_at(t, 32)
            for sig, n in SIGNATURES.items():
                if head.startswith(sig):
                    name = n
        self._sig_cache[t] = name
        return name

    def call_info(self, mod, ins):
        t = ins.branch_target()
        if t is not None:
            return self.target_info(mod, t)
        imp = mod.iat_import(ins)
        if imp is not None:
            return self.import_info(mod, imp)
        return CallInfo(None, None)

    def tail_ret_depth(self, mod, t):
        info = self.target_info(mod, t)
        return info.ret_depth or 0


def compute_summaries(ctx, max_iter=6, log=print):
    """Итерация сводок «глубина x87 на выходе» по всем функциям всех модулей."""
    for it in range(max_iter):
        changed = 0
        for mod in ctx.prog.modules.values():
            for e, f in mod.functions.items():
                if e in ctx.overrides:
                    continue
                fc = FuncCompiler(mod, f, ctx)
                fc.succ = {a: fc.succ_of(fc.ins[a]) for a in fc.addrs}
                fc.x87_analyze()
                d = min(fc.exit_depth, 1)
                if ctx.summary.get(e, 0) != d:
                    ctx.summary[e] = d
                    changed += 1
        log(f"x87 summaries: iteration {it} changed {changed}")
        if not changed:
            break


def module_crc(path):
    with open(path, "rb") as f:
        data = f.read()
    return zlib.crc32(data) & 0xFFFFFFFF, len(data)


def write_if_changed(path, text):
    """Файл пишется, только если содержимое другое: ninja пересобирает лишь изменённые (по времени файла)."""
    try:
        with open(path) as f:
            if f.read() == text:
                return
    except OSError:
        pass
    with open(path, "w") as f:
        f.write(text)


_POOL_STATE = None


def _module_stem(name):
    return c_ident(dll_stem(name.lower()).replace(".exe", "_exe"))


def _compile_chunk(ctx, mod, chunk, stem, chunk_num, mdir):
    """Скомпилировать один чанк функций модуля и записать его в <stem>_<NNNN>.c."""
    name = mod.img.name
    bodies = []
    errors = []
    err_funcs = 0
    x87_bad = 0
    unimpl = {}
    for e in chunk:
        intr = ctx.target_info(mod, e).intrinsic
        if intr:
            # x87-интринсик (_CI*, _ftol*, вложенный _ftol2): все вызовы исполняют его нативно,
            # тело берёт аргумент из ST(0) вызывающего — как отдельная функция непредставимо
            bodies.append(f"void {ctx.fname(e)}(SNX_C_PARAM)\n{{\n    P_(CPU *restrict c = g_pin_c;)\n"
                          f"    rt_trap(c, {e:#x}u, \"x87 intrinsic {intr} called indirectly\");\n}}")
            continue
        fc = FuncCompiler(mod, mod.functions[e], ctx)
        code = fc.compile()
        if fc.errors:
            err_funcs += 1
            errors.extend(f"{e:08x}: {x}" for x in fc.errors)
        if fc.x87_bad:
            x87_bad += 1
        for m in re.findall(r'rt_unimpl\(c, 0x[0-9a-f]+u, "([^" ]+)', code):
            unimpl[m] = unimpl.get(m, 0) + 1
        bodies.append(code)
    text = "\n\n".join(bodies)
    guest_syms = sorted(set(re.findall(r"\b((?:f|fc)_[0-9a-f]{8})\(SNX_C_ARG\)", text)))
    host_syms = sorted(set(re.findall(r"\b((?:hle|ovr)_[A-Za-z0-9_]+)\(c\)", text)))
    protos = "\n".join([f"void {s}(SNX_C_PARAM);" for s in guest_syms] + [f"void {s}(CPU *c);" for s in host_syms])
    inl = sorted({int(x, 16) for x in re.findall(r"\bfi_([0-9a-f]{8})\(SNX_C_ARG\)", text)})
    if inl:
        protos += ("\n" if protos else "") + "\n".join(f"{INLINE_DECL} fi_{t:08x}(SNX_C_PARAM);" for t in inl)
        text += "\n\n" + "\n\n".join(ctx.inline_body(t) for t in inl)
    fn = f"{stem}_{chunk_num:04d}.c"
    write_if_changed(os.path.join(mdir, fn), f"/* Сгенерировано recomp из {name}. Не редактировать, не коммитить. */\n"
                     '#include "runtime/gen_rt.h"\n\n' + protos + "\n\n" + text + "\n")
    return chunk_num, fn, errors, err_funcs, x87_bad, unimpl


def _chunk_worker(task):
    """Точка входа рабочего процесса (fork): компиляция одного чанка без сериализации ctx/prog."""
    mod_idx, chunk_num = task
    ctx, mod_infos = _POOL_STATE
    mod, stem, mdir, funcs = mod_infos[mod_idx]
    ctx.hle_used.clear()
    chunk = funcs[chunk_num * FUNCS_PER_FILE:(chunk_num + 1) * FUNCS_PER_FILE]
    res = _compile_chunk(ctx, mod, chunk, stem, chunk_num, mdir)
    return mod_idx, res, dict(ctx.hle_used)


def _write_module_tail(ctx, mod, stem, mdir, funcs, chunk_results, log=print):
    """Собрать статистику чанков, записать <stem>_table.c, errors.txt и stats.json."""
    name = mod.img.name
    stats = {"functions": len(funcs), "errors": 0, "x87_bad": 0, "unimpl": {}}
    errors = []
    files = []
    for _, fn, chunk_errors, err_funcs, x87_bad, chunk_unimpl in sorted(chunk_results, key=lambda r: r[0]):
        files.append(fn)
        errors.extend(chunk_errors)
        stats["errors"] += err_funcs
        stats["x87_bad"] += x87_bad
        for m, cnt in chunk_unimpl.items():
            stats["unimpl"][m] = stats["unimpl"].get(m, 0) + cnt

    # таблица функций + IAT + описание модуля
    crc, size = module_crc(mod.img.path)
    lines = [f"/* Сгенерировано recomp из {name}. */", '#include "runtime/gen_rt.h"', '#include "runtime/loader.h"', ""]
    for e in funcs:
        lines.append(f"void {ctx.fname(e)}(SNX_C_PARAM);")
    iat = []
    for imp in mod.img.imports:
        r = ctx.prog.resolve_import(imp)
        if r[0] == "guest":
            iat.append(f"    {{0x{imp.iat_va:08x}u, 0x{r[1]:08x}u, NULL, NULL}},")
        else:
            sym = ctx.import_sym(mod, imp)
            lines.append(f"void {sym}(CPU *c);")
            iat.append(f'    {{0x{imp.iat_va:08x}u, 0, "{r[1]}", {sym}}},')
    ovr_syms = sorted({s for s, _ in ctx.overrides.values()})
    for s in ovr_syms:
        lines.append(f"void {s}(CPU *c);")
    for e in funcs:
        if e in ctx.wraps:
            lines.append(f"void (*const ovr_orig_{ctx.wraps[e]})(CPU *c) = SNX_GUEST_FN({ctx.fname(e)});")
    # переходник (jmp на тело, Ctx.target_info) — в таблице сразу тело: косвенный вызов через vtable не прыгает дважды
    entry = {}
    for e in funcs:
        if e in ctx.overrides:
            continue
        info = ctx.target_info(mod, e)
        if info.sym and info.sym != ctx.fname(e) and not info.intrinsic and not info.setjmp:
            if info.sym in ovr_syms:
                entry[e] = (info.sym, 1)
            elif re.fullmatch(r"f_[0-9a-f]{8}", info.sym):
                entry[e] = (info.sym, 0)
    own = {ctx.fname(e) for e in funcs}
    for s in sorted({s for s, _ in entry.values()} - own - set(ovr_syms)):
        lines.append(f"void {s}(SNX_C_PARAM);")
    lines.append("")
    lines.append(f"static const rt_func_entry funcs[] = {{")
    for e in funcs:  # оверрайд — функция хоста (native = 1): регистры гостя ей — через CPU (runtime/abi.h)
        if e in ctx.overrides:
            lines.append(f"    {{0x{e:08x}u, {ctx.overrides[e][0]}, 1}},")
        elif e in entry:
            sym, native = entry[e]
            lines.append(f"    {{0x{e:08x}u, {sym}, 1}}," if native else f"    {{0x{e:08x}u, SNX_GUEST_FN({sym})}},")
        else:
            lines.append(f"    {{0x{e:08x}u, SNX_GUEST_FN({ctx.fname(e)})}},")
    lines.append("};")
    lines.append(f"static const rt_iat_entry iat[] = {{")
    lines.extend(iat or ["    {0, 0, NULL, NULL},"])
    lines.append("};")
    # экспорты для GetProcAddress (UE2 ищет нативные функции по именам), по возрастанию имени — для bsearch
    ords = {va: o for o, va in mod.img.export_ordinals.items()}
    named = sorted(mod.img.exports.items(), key=lambda kv: kv[0].encode())
    lines.append("static const rt_export_entry exports[] = {")
    for n, va in named:
        lines.append(f'    {{"{n}", 0x{va:08x}u, {ords.get(va, 0)}u}},')
    for o, va in sorted(mod.img.export_ordinals.items()):
        if va not in mod.img.exports.values():
            lines.append(f"    {{NULL, 0x{va:08x}u, {o}u}},")
    if not mod.img.exports and not mod.img.export_ordinals:
        lines.append("    {NULL, 0, 0},")
    lines.append("};")
    nexp = len(named) + sum(1 for va in mod.img.export_ordinals.values() if va not in mod.img.exports.values())
    lines.append(f"const rt_module_desc mod_{stem} = {{")
    lines.append(f'    "{name}", 0x{mod.img.base:08x}u, 0x{mod.img.preferred_base:08x}u, 0x{mod.img.size:x}u, '
                 f"0x{crc:08x}u, {size}u, 0x{mod.img.entry:08x}u, {1 if mod.img.is_dll else 0},")
    lines.append(f"    funcs, {len(funcs)}u, iat, {len(iat)}u, exports, {len(named)}u, {nexp}u,")
    lines.append("};")
    write_if_changed(os.path.join(mdir, f"{stem}_table.c"), "\n".join(lines) + "\n")
    files.append(f"{stem}_table.c")
    with open(os.path.join(mdir, "errors.txt"), "w") as f:
        f.write("\n".join(errors) + "\n")
    with open(os.path.join(mdir, "stats.json"), "w") as f:
        json.dump(stats, f, indent=1, sort_keys=True)
    log(f"{name}: {len(funcs)} functions, {len(files)} files, {stats['errors']} with errors, "
        f"{stats['x87_bad']} x87-bad, unimpl kinds {len(stats['unimpl'])}")
    return stem, files


def write_modules(ctx, mods, outdir, jobs=1, log=print):
    """Сгенерировать C-файлы для списка модулей; при jobs > 1 чанки компилируются параллельно (детерминированно)."""
    mod_infos = []
    tasks = []
    for mod_idx, mod in enumerate(mods):
        stem = _module_stem(mod.img.name)
        mdir = os.path.join(outdir, stem)
        os.makedirs(mdir, exist_ok=True)
        funcs = sorted(mod.functions)
        mod_infos.append((mod, stem, mdir, funcs))
        for chunk_num, chunk_i in enumerate(range(0, len(funcs), FUNCS_PER_FILE)):
            weight = sum(len(mod.functions[e].insns) for e in funcs[chunk_i:chunk_i + FUNCS_PER_FILE])
            tasks.append((weight, mod_idx, chunk_num))

    use_fork = jobs > 1 and len(tasks) > 1 and "fork" in multiprocessing.get_all_start_methods()
    by_mod = [[] for _ in mod_infos]
    hle_by_mod = [[] for _ in mod_infos]

    if use_fork:
        # Крупные чанки запускаем первыми — меньше «хвост» ожидания на последнем ядре;
        # результаты и hle_used собираются строго по (mod_idx, chunk_num), как при jobs == 1.
        ordered = [(mod_idx, chunk_num) for _, mod_idx, chunk_num in sorted(tasks, key=lambda t: (-t[0], t[1], t[2]))]
        global _POOL_STATE
        _POOL_STATE = (ctx, mod_infos)
        try:
            mp_ctx = multiprocessing.get_context("fork")
            with ProcessPoolExecutor(max_workers=min(jobs, len(ordered)), mp_context=mp_ctx) as pool:
                for mod_idx, res, local_hle in pool.map(_chunk_worker, ordered):
                    by_mod[mod_idx].append(res)
                    hle_by_mod[mod_idx].append((res[0], local_hle))
        finally:
            _POOL_STATE = None
    else:
        for _, mod_idx, chunk_num in tasks:
            mod, stem, mdir, funcs = mod_infos[mod_idx]
            chunk = funcs[chunk_num * FUNCS_PER_FILE:(chunk_num + 1) * FUNCS_PER_FILE]
            by_mod[mod_idx].append(_compile_chunk(ctx, mod, chunk, stem, chunk_num, mdir))

    out = []
    for mod_idx, (mod, stem, mdir, funcs) in enumerate(mod_infos):
        if use_fork:
            for _, local_hle in sorted(hle_by_mod[mod_idx], key=lambda item: item[0]):
                ctx.hle_used.update(local_hle)
        out.append(_write_module_tail(ctx, mod, stem, mdir, funcs, by_mod[mod_idx], log=log))
    return out


def write_module(ctx, mod, outdir, log=print, jobs=1):
    return write_modules(ctx, [mod], outdir, jobs=jobs, log=log)[0]


_STUB_RE = re.compile(r'^__attribute__\(\(weak\)\) void (\w+)\(CPU \*c\) \{ rt_hle_missing\(c, "([^"]*)", "([^"]*)"\); \}$')


def write_hle_stubs(ctx, outdir, partial=False):
    """partial — сгенерированы не все модули: заглушки остальных (из прежнего файла) остаются"""
    path = os.path.join(outdir, "hle_stubs.c")
    used = dict(ctx.hle_used)
    if partial and os.path.exists(path):
        for ln in open(path, encoding="utf-8"):
            m = _STUB_RE.match(ln.rstrip("\n"))
            if m:
                used.setdefault(m.group(1), (m.group(2), m.group(3)))
    lines = ["/* Слабые заглушки для HLE-импортов, у которых ещё нет реализации. */", '#include "runtime/gen_rt.h"', ""]
    for sym, (dll, name) in sorted(used.items()):
        lines.append(f'__attribute__((weak)) void {sym}(CPU *c) {{ rt_hle_missing(c, "{dll}", "{name}"); }}')
    write_if_changed(path, "\n".join(lines) + "\n")
