"""CLI рекомпилятора: python3 -I recomp/main.py <команда> --game <game/System> [...]"""
import argparse
from concurrent.futures import ProcessPoolExecutor, as_completed
import datetime
import multiprocessing
import os
import sys
import time

import pefile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from discover import Module, Program  # noqa: E402
from emit import fpu_single_mode  # noqa: E402
from gen import Ctx, compute_summaries, module_crc, write_hle_stubs, write_modules  # noqa: E402
from overrides import resolve as resolve_overrides, verify as verify_overrides, wrapped as resolve_wraps  # noqa: E402
from regs import compute as compute_regs  # noqa: E402
from image import IMAGE_SCN_CNT_CODE, IMAGE_SCN_MEM_EXECUTE, load_addrmap, load_image  # noqa: E402

ASSET_DIRS = ("Maps", "Textures", "Sounds", "Music", "Movies")
SAFEDISC_ENTROPY = 7.0
_DISC_PROG = None


def _find_entry_ci(directory, name):
    """Найти файл или каталог внутри directory без учёта регистра (сначала точное совпадение)."""
    exact = os.path.join(directory, name)
    if os.path.exists(exact):
        return exact
    want = name.lower()
    try:
        for item in sorted(os.listdir(directory)):
            if item.lower() == want:
                return os.path.join(directory, item)
    except OSError:
        return None
    return None


def load_program(game_dir, only=None):
    amap = load_addrmap()
    images = {}
    for name, base in amap.items():
        path = _find_entry_ci(game_dir, name) or os.path.join(game_dir, name)
        images[name.lower()] = load_image(path, base)
    prog = Program(images)
    for name in images:
        if only and name not in only:
            continue
        prog.modules[name] = Module(images[name], prog)
    return prog


def _discover_worker(name):
    """Разбор одного модуля в рабочем процессе (fork)."""
    t = time.time()
    mod = _DISC_PROG.modules[name]
    mod.run()
    return (name, mod.insns, mod.strong, mod.weak, mod.jump_tables, mod.jt_slots,
            mod.byte_tables, mod.functions, mod.eh_conts, mod.eh_targets, mod.reachable,
            mod.rounds, time.time() - t)


def run_discovery(prog, jobs=1):
    """Разобрать все модули prog; при jobs > 1 модули разбираются параллельно (fork)."""
    names = list(prog.modules.keys())
    times = {}
    use_fork = jobs > 1 and len(names) > 1 and "fork" in multiprocessing.get_all_start_methods()
    if not use_fork:
        for name in names:
            t = time.time()
            prog.modules[name].run()
            times[name] = time.time() - t
        return times

    # Крупнейший модуль (Engine.dll) разбираем прямо в родительском процессе (без pickle 800k инструкций),
    # остальные модули параллельно в рабочих процессах (суммарно не больше jobs процессов).
    ordered = sorted(names, key=lambda n: -sum(s.vsize for s in prog.modules[n].img.code_sections()))
    main_name, rest = ordered[0], ordered[1:]
    global _DISC_PROG
    _DISC_PROG = prog
    try:
        mp_ctx = multiprocessing.get_context("fork")
        with ProcessPoolExecutor(max_workers=min(max(1, jobs - 1), len(rest)), mp_context=mp_ctx) as pool:
            futs = [pool.submit(_discover_worker, n) for n in rest]
            t0 = time.time()
            prog.modules[main_name].run()
            times[main_name] = time.time() - t0
            for fut in as_completed(futs):
                (name, insns, strong, weak, jump_tables, jt_slots, byte_tables,
                 functions, eh_conts, eh_targets, reachable, rounds, dt) = fut.result()
                mod = prog.modules[name]
                mod.insns = insns
                mod.strong = strong
                mod.weak = weak
                mod.jump_tables = jump_tables
                mod.jt_slots = jt_slots
                mod.byte_tables = byte_tables
                mod.functions = functions
                mod.eh_conts = eh_conts
                mod.eh_targets = eh_targets
                mod.reachable = reachable
                mod.rounds = rounds
                times[name] = dt
    finally:
        _DISC_PROG = None
    return times


def _detect_safedisc(pe):
    """Распознать признаки защиты SafeDisc в PE: секции .icd/stxt*, высокая энтропия кода, точка входа в stxt*."""
    reasons = []
    safedisc_secs = []
    max_code_ent = 0.0
    ep = pe.OPTIONAL_HEADER.AddressOfEntryPoint
    for s in pe.sections:
        sname = s.Name.rstrip(b"\0").decode("latin-1")
        norm = sname.lower().lstrip(".")
        ent = s.get_entropy()
        is_code = bool(s.Characteristics & (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_CNT_CODE))
        if is_code and ent > max_code_ent:
            max_code_ent = ent
        if norm == "icd" or norm.startswith("stxt") or norm == "secdrv":
            safedisc_secs.append(sname)
            if s.VirtualAddress <= ep < s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData):
                reasons.append(f"точка входа ({ep:#x}) в секции {sname}")
    if safedisc_secs:
        reasons.insert(0, f"секции SafeDisc: {', '.join(safedisc_secs)}")
    if max_code_ent >= SAFEDISC_ENTROPY:
        reasons.append(f"высокая энтропия кода ({max_code_ent:.2f} >= {SAFEDISC_ENTROPY:.1f})")
    return bool(reasons), reasons, max_code_ent


def check_game(game_dir, addrmap_path=None, stream=None):
    """Проверить каталог System игры: 12 модулей, PE/SafeDisc, оверрайды и соседние каталоги ассетов.

    -> (errors: list[str], warnings: list[str]).
    """
    out = stream or sys.stdout

    def say(msg=""):
        print(msg, file=out, flush=True)

    errors = []
    warnings = []
    amap = load_addrmap(addrmap_path)
    game_abs = os.path.abspath(game_dir)

    say(f"Проверка каталога игры: {game_abs}")
    if not os.path.isdir(game_abs):
        msg = f"каталог не найден: {game_abs}"
        errors.append(msg)
        say(f"ОШИБКА: {msg}")
        return errors, warnings

    say()
    say("Модули (addrmap.json):")
    images = {}
    safedisc_game_exe = False
    safedisc_reasons = []
    ok_mods = 0

    for name, base in amap.items():
        path = _find_entry_ci(game_abs, name)
        if path is None or not os.path.isfile(path):
            errors.append(f"отсутствует модуль {name}")
            say(f"  {name:12s}  ОТСУТСТВУЕТ")
            continue
        crc, size = module_crc(path)
        try:
            pe = pefile.PE(path, fast_load=True)
        except (pefile.PEFormatError, OSError, ValueError) as exc:
            errors.append(f"{name}: битый PE ({exc})")
            say(f"  {name:12s}  {size:9d}  {crc:08x}  ----------  ОШИБКА: битый PE ({exc})")
            continue

        try:
            ts = pe.FILE_HEADER.TimeDateStamp
            dt = (datetime.datetime.fromtimestamp(ts, datetime.timezone.utc).strftime("%Y-%m-%d")
                  if ts else "0000-00-00")
            machine = pe.FILE_HEADER.Machine
            is_sd, sd_reasons, max_ent = _detect_safedisc(pe)
        finally:
            pe.close()

        mod_errs = []
        if machine != 0x14C:
            mod_errs.append(f"архитектура PE {machine:#x} != x86-32 (0x14c)")
        if is_sd:
            if name.lower() == "webhead.exe":
                safedisc_game_exe = True
                safedisc_reasons.extend(sd_reasons)
            mod_errs.append("SafeDisc/шифрование (" + "; ".join(sd_reasons) + ")")

        if machine == 0x14C:
            try:
                images[name.lower()] = load_image(path, base)
            except Exception as exc:
                mod_errs.append(f"ошибка загрузки PE ({exc})")

        if mod_errs:
            for e in mod_errs:
                errors.append(f"{name}: {e}")
            say(f"  {name:12s}  {size:9d}  {crc:08x}  {dt}  ОШИБКА: {'; '.join(mod_errs)}")
        else:
            ok_mods += 1
            say(f"  {name:12s}  {size:9d}  {crc:08x}  {dt}  OK (энтропия кода {max_ent:.2f})")

    # Проверка всех сигнатур и экспортов из overrides.py
    say()
    say("Сигнатуры и экспорты overrides.py:")
    prog = Program(images)
    ovr_results = verify_overrides(prog)
    ovr_ok = 0
    ovr_checked = 0
    game_exe_sigs_total = 0
    game_exe_sigs_missing = 0
    for key, sym, status, detail in ovr_results:
        mod_l = key.split("!", 1)[0].lower()
        is_sig = "!=" in key
        if mod_l == "webhead.exe" and is_sig and status != "missing_module":
            game_exe_sigs_total += 1
        if status == "ok":
            ovr_ok += 1
            ovr_checked += 1
        elif status == "missing_module":
            continue  # отсутствие/битый модуль уже учтены выше
        elif status == "missing_sig":
            ovr_checked += 1
            if mod_l == "webhead.exe":
                game_exe_sigs_missing += 1
            errors.append(f"оверрайд {key} ({sym}): сигнатура не найдена (0 совпадений)")
            say(f"  ОШИБКА: {key} ({sym}) — сигнатура не найдена")
        elif status == "dup_sig":
            ovr_checked += 1
            addrs = ", ".join(f"{a:#x}" for a in detail)
            errors.append(f"оверрайд {key} ({sym}): неоднозначная сигнатура ({len(detail)} совпадений: {addrs})")
            say(f"  ОШИБКА: {key} ({sym}) — {len(detail)} совпадений ({addrs})")
        elif status == "missing_export":
            ovr_checked += 1
            errors.append(f"оверрайд {key} ({sym}): экспорт {detail} не найден")
            say(f"  ОШИБКА: {key} ({sym}) — экспорт не найден")
        else:
            ovr_checked += 1
            errors.append(f"оверрайд {key} ({sym}): неизвестная сигнатура {detail}")
            say(f"  ОШИБКА: {key} ({sym}) — неизвестная сигнатура {detail}")
    say(f"  Проверено: {ovr_ok}/{ovr_checked} OK (всего записей в overrides.py: {len(ovr_results)})")

    if game_exe_sigs_total > 0 and game_exe_sigs_missing == game_exe_sigs_total and not safedisc_game_exe:
        safedisc_game_exe = True
        safedisc_reasons.append(f"не найдена ни одна из {game_exe_sigs_total} ожидаемых сигнатур webhead.exe")
        errors.append("webhead.exe: не найдены ожидаемые сигнатуры (возможен зашифрованный SafeDisc или другая сборка)")

    if safedisc_game_exe:
        say()
        say("!!! ОБНАРУЖЕН ЗАЩИЩЁННЫЙ (SafeDisc) ИЛИ ЗАШИФРОВАННЫЙ webhead.exe !!!")
        say(f"    Признаки: {'; '.join(safedisc_reasons)}.")
        say("    Рекомпилятору нужен распакованный webhead.exe без SafeDisc (см. README.md и docs/BUILDING.md;")
        say("    для русской версии подмените Game.exe скриптом tools/make_rus.sh).")

    # Проверка структуры соседних каталогов ассетов
    root_dir = os.path.dirname(game_abs)
    say()
    say(f"Соседние каталоги ассетов ({root_dir}):")
    assets_ok = 0
    for dname in ASSET_DIRS:
        dpath = _find_entry_ci(root_dir, dname)
        if dpath and os.path.isdir(dpath):
            count = 0
            for _, _, fnames in os.walk(dpath):
                count += sum(1 for f in fnames if not f.startswith("."))
            assets_ok += 1
            say(f"  {dname + '/':10s} OK ({count} файлов)")
        else:
            warnings.append(f"отсутствует каталог ассетов {dname}/")
            say(f"  {dname + '/':10s} ОТСУТСТВУЕТ (предупреждение)")

    say()
    status_str = "OK" if not errors else "ЕСТЬ ОШИБКИ"
    say(f"Итог: {status_str} — модули {ok_mods}/{len(amap)}, оверрайды {ovr_ok}/{len(ovr_results)}, "
        f"ассеты {assets_ok}/{len(ASSET_DIRS)}, ошибок: {len(errors)}, предупреждений: {len(warnings)}")
    return errors, warnings


def cmd_check(args):
    errors, _ = check_game(args.game)
    return 1 if errors else 0


def cmd_discover(args):
    jobs = max(1, args.jobs)
    prog = load_program(args.game, [m.lower() for m in args.modules] if args.modules else None)
    times = run_discovery(prog, jobs=jobs)
    for name, mod in prog.modules.items():
        fs = mod.functions.values()
        code = sum(s.vsize for s in mod.img.code_sections())
        covered = len(mod.reachable)
        print(f"{name:12s} strong={len(mod.strong):6d} weak={len(mod.weak):5d} funcs={len(mod.functions):6d} "
              f"insns={covered:8d} jt={len(mod.jump_tables):4d} unk_ind={sum(f.unknown_indirect for f in fs):4d} "
              f"invalid={sum(f.invalid for f in fs):4d} dup={sum(len(f.insns) for f in fs) - covered:7d} "
              f"rounds={mod.rounds} code={code} {times.get(name, 0.0):.1f}s", flush=True)
    return 0


def cmd_gen(args):
    jobs = max(1, args.jobs)
    prog = load_program(args.game)  # все модули нужны для межмодульных сводок
    if fpu_single_mode():
        print("x87: SNX_FPU_SINGLE=1 (округление арифметики к 24-битной мантиссе float)", flush=True)
    t = time.time()
    run_discovery(prog, jobs=jobs)
    print(f"discovery (jobs={jobs}): {time.time() - t:.1f}s", flush=True)
    ctx = Ctx(prog, resolve_overrides(prog), resolve_wraps(prog))
    t = time.time()
    compute_summaries(ctx)
    print(f"summaries: {time.time() - t:.1f}s", flush=True)
    t = time.time()
    compute_regs(prog, ctx)
    print(f"regs: {time.time() - t:.1f}s", flush=True)
    os.makedirs(args.out, exist_ok=True)
    only = {m.lower() for m in args.modules} if args.modules else None
    mods = [mod for name, mod in prog.modules.items() if not only or name in only]
    t = time.time()
    write_modules(ctx, mods, args.out, jobs=jobs)
    print(f"emit (jobs={jobs}): {time.time() - t:.1f}s", flush=True)
    write_hle_stubs(ctx, args.out, partial=only is not None)
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["discover", "gen", "check"])
    ap.add_argument("--game", required=True)
    ap.add_argument("--out", default="build/gen")
    ap.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 1,
                    help="число параллельных процессов (1 — последовательно)")
    ap.add_argument("modules", nargs="*")
    args = ap.parse_args()
    rc = {"discover": cmd_discover, "gen": cmd_gen, "check": cmd_check}[args.cmd](args)
    if rc:
        sys.exit(rc)


if __name__ == "__main__":
    main()
