"""Автоматический прогон карт Shrek 2 через snx_boot и проверка логов/скриншотов.

Пример запуска (совместим с python3 -I):
    tools/.venv/bin/python -I tools/test_maps.py \\
        --boot build/mac/host/boot/snx_boot --game game --user build/test-maps \\
        --maps SH2_Preamble.unr Book_FrontEnd.unr 1_Shreks_Swamp.unr --seconds 20

Режим --headless готовит в пользовательском каталоге game.ini с Engine.NullRenderDevice.
Режим --shot-ref <dir> снимает кадр через SNX_SHOT и сравнивает с эталонными PNG из <dir>
по MSE/PSNR (требуется Pillow). Проверяет логи на TRAP, hle_todo, GL error и нарушения
SNX_CHECK_ABI, затем выводит сводную таблицу PASS/FAIL.
"""
import argparse
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time


DEFAULT_MAPS = [
    "SH2_Preamble.unr",
    "Book_FrontEnd.unr",
    "1_Shreks_Swamp.unr",
]

RE_TRAP = re.compile(r"\[rt\]\s+TRAP\b|\[boot\]\s+Critical Error:|\[boot\]\s+GErrorHist:")
RE_TODO = re.compile(r"\[hle\]\s+TODO:\s*(.*)")
RE_GL_ERR = re.compile(r"\[d3d8\]\s+GL error\b.*")
RE_ABI = re.compile(r"\[abi\]\s+.*|\[regchk\]\s+.*")


def map_stem(name):
    return Path(name).stem


def find_case_insensitive(directory, filename):
    target = filename.lower()
    if not directory.is_dir():
        return None
    for entry in directory.iterdir():
        if entry.name.lower() == target:
            return entry
    return None


def set_ini_value(text, section, key, value):
    """Заменить или добавить key=value в секции [section] (регистронезависимо)."""
    nl = "\r\n" if "\r\n" in text or not text else "\n"
    lines = text.splitlines()
    out = []
    in_sec = False
    sec_found = False
    replaced = False
    sec_low = section.lower()
    key_low = key.lower()

    for line in lines:
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            if in_sec and not replaced:
                out.append(f"{key}={value}")
                replaced = True
            in_sec = stripped[1:-1].strip().lower() == sec_low
            if in_sec:
                sec_found = True
            out.append(line)
            continue
        if in_sec and "=" in line:
            k, _ = line.split("=", 1)
            if k.strip().lower() == key_low:
                out.append(f"{k.strip()}={value}")
                replaced = True
                continue
        out.append(line)

    if in_sec and not replaced:
        out.append(f"{key}={value}")
        replaced = True
    if not sec_found:
        if out and out[-1] != "":
            out.append("")
        out.append(f"[{section}]")
        out.append(f"{key}={value}")
    return nl.join(out) + nl


def prepare_headless_ini(game_dir, user_dir):
    """Подготовить <user>/System/game.ini с RenderDevice=Engine.NullRenderDevice."""
    sys_user = user_dir / "System"
    sys_user.mkdir(parents=True, exist_ok=True)
    dst = sys_user / "game.ini"
    if dst.exists():
        text = dst.read_text(encoding="latin-1")
    else:
        sys_game = find_case_insensitive(game_dir, "System") or (game_dir / "System")
        src = find_case_insensitive(sys_game, "game.ini") or find_case_insensitive(sys_game, "Default.ini")
        text = src.read_text(encoding="latin-1") if src and src.is_file() else ""
    text = set_ini_value(text, "Engine.Engine", "RenderDevice", "Engine.NullRenderDevice")
    dst.write_text(text, encoding="latin-1")


def find_reference_shot(ref_dir, stem, shot_name):
    """Найти эталонный PNG для карты в каталоге --shot-ref."""
    candidates = [
        ref_dir / f"{stem}.png",
        ref_dir / stem / shot_name,
        ref_dir / f"{stem}_{shot_name}",
    ]
    for path in candidates:
        if path.is_file():
            return path
    sub = ref_dir / stem
    if sub.is_dir():
        pngs = sorted(sub.glob("*.png"))
        if pngs:
            return pngs[0]
    return None


def compare_images_psnr(actual_path, ref_path):
    """Вычислить (mse, psnr_db) между двумя PNG через Pillow."""
    try:
        from PIL import Image, ImageChops
    except ImportError as exc:
        raise RuntimeError(
            "Для --shot-ref требуется Pillow (запускайте через tools/.venv/bin/python -I)"
        ) from exc

    with Image.open(actual_path) as im_act, Image.open(ref_path) as im_ref:
        a = im_act.convert("RGB")
        r = im_ref.convert("RGB")
        if a.size != r.size:
            raise ValueError(f"разные размеры изображений: {a.size[0]}x{a.size[1]} vs {r.size[0]}x{r.size[1]}")
        hist = ImageChops.difference(a, r).histogram()
        total_channels = a.size[0] * a.size[1] * 3
        if total_channels == 0:
            return 0.0, float("inf")
        sq_err = sum((i % 256) ** 2 * cnt for i, cnt in enumerate(hist))
        mse = sq_err / total_channels
        psnr = float("inf") if mse == 0.0 else 10.0 * math.log10((255.0 ** 2) / mse)
        return mse, psnr


def analyze_log(text):
    """Извлечь из вывода snx_boot маркеры TRAP, hle_todo, GL error и SNX_CHECK_ABI."""
    traps = []
    todos = []
    gl_errors = []
    abi_errors = []
    for line in text.splitlines():
        if RE_TRAP.search(line):
            traps.append(line.strip())
        m_todo = RE_TODO.search(line)
        if m_todo:
            todos.append(m_todo.group(1).strip())
        if RE_GL_ERR.search(line):
            gl_errors.append(line.strip())
        if RE_ABI.search(line):
            abi_errors.append(line.strip())
    return {
        "traps": traps,
        "todos": todos,
        "gl_errors": gl_errors,
        "abi_errors": abi_errors,
    }


def run_map(args, map_name):
    stem = map_stem(map_name)
    map_user = Path(args.user) / stem
    if args.clean_user and map_user.exists():
        shutil.rmtree(map_user)
    map_user.mkdir(parents=True, exist_ok=True)

    shot_dir = None
    if args.shot_ref or args.shot_dir:
        shot_dir = Path(args.shot_dir) / stem if args.shot_dir else map_user / "shots"
        if shot_dir.exists():
            shutil.rmtree(shot_dir)
        shot_dir.mkdir(parents=True, exist_ok=True)

    if args.headless:
        prepare_headless_ini(Path(args.game), map_user)

    cmd = [
        str(args.boot),
        "--game", str(args.game),
        "--user", str(map_user),
        "--seconds", str(args.seconds),
    ]
    if args.headless:
        cmd.append("--headless")
    cmd.append("--")
    cmd.append(map_name)
    if args.nosound:
        cmd.append("-nosound")

    env = os.environ.copy()
    if args.check_abi:
        env["SNX_CHECK_ABI"] = "1"
    if shot_dir is not None and not args.headless:
        env["SNX_SHOT"] = str(args.shot_frame)
        env["SNX_SHOT_DIR"] = str(shot_dir)

    timeout_s = args.timeout if args.timeout is not None else float(args.seconds) + 60.0
    t0 = time.monotonic()
    timed_out = False
    try:
        proc = subprocess.run(
            cmd,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            errors="replace",
            timeout=timeout_s,
            check=False,
        )
        rc = proc.returncode
        output = proc.stdout
    except subprocess.TimeoutExpired as exc:
        timed_out = True
        rc = -1
        raw = exc.stdout or ""
        output = raw.decode("utf-8", errors="replace") if isinstance(raw, bytes) else raw
    elapsed = time.monotonic() - t0

    log_path = map_user / "test_maps.log"
    log_path.write_text(output, encoding="utf-8")

    issues = analyze_log(output)
    notes = []
    failed = False

    if timed_out:
        failed = True
        notes.append(f"таймаут {timeout_s:.0f}с")
    elif rc != 0:
        failed = True
        notes.append(f"код выхода {rc}")

    if issues["traps"]:
        failed = True
        notes.append(f"TRAP({len(issues['traps'])})")
    if issues["todos"]:
        if not args.allow_todo:
            failed = True
        notes.append(f"TODO({len(issues['todos'])})")
    if issues["gl_errors"]:
        failed = True
        notes.append(f"GL({len(issues['gl_errors'])})")
    if issues["abi_errors"]:
        failed = True
        notes.append(f"ABI({len(issues['abi_errors'])})")

    mse = None
    psnr = None
    if args.shot_ref and not args.headless:
        ref_dir = Path(args.shot_ref)
        shots = sorted(shot_dir.glob("shot_*.png")) if shot_dir and shot_dir.is_dir() else []
        actual = shots[-1] if shots else None
        shot_name = actual.name if actual else f"shot_{args.shot_frame}.png"
        ref = find_reference_shot(ref_dir, stem, shot_name)
        if ref is None:
            notes.append("нет эталона PNG")
            if args.require_ref:
                failed = True
        elif actual is None:
            failed = True
            notes.append("скриншот не создан")
        else:
            try:
                mse, psnr = compare_images_psnr(actual, ref)
                if psnr < args.min_psnr:
                    failed = True
                    notes.append(f"PSNR {psnr:.1f}<{args.min_psnr:.1f}")
            except Exception as exc:
                failed = True
                notes.append(f"ошибка PNG: {exc}")

    return {
        "map": map_name,
        "status": "FAIL" if failed else "PASS",
        "elapsed": round(elapsed, 2),
        "returncode": rc,
        "timed_out": timed_out,
        "traps": len(issues["traps"]),
        "todos": len(issues["todos"]),
        "gl_errors": len(issues["gl_errors"]),
        "abi_errors": len(issues["abi_errors"]),
        "mse": mse,
        "psnr": psnr,
        "notes": "; ".join(notes),
        "log": str(log_path),
        "details": issues,
    }


def print_table(results):
    headers = ["Карта", "Статус", "Время", "Код", "TRAP", "TODO", "GL", "ABI", "PSNR", "Примечание"]
    rows = []
    for r in results:
        if r["psnr"] is None:
            psnr_s = "-"
        elif math.isinf(r["psnr"]):
            psnr_s = "inf"
        else:
            psnr_s = f"{r['psnr']:.1f}дБ"
        rows.append([
            r["map"],
            r["status"],
            f"{r['elapsed']:.1f}с",
            "TO" if r["timed_out"] else str(r["returncode"]),
            str(r["traps"]),
            str(r["todos"]),
            str(r["gl_errors"]),
            str(r["abi_errors"]),
            psnr_s,
            r["notes"] or "-",
        ])

    widths = [max(len(headers[i]), max((len(row[i]) for row in rows), default=0)) for i in range(len(headers))]
    fmt = "  ".join(f"{{:<{w}}}" for w in widths)
    sep = "  ".join("-" * w for w in widths)
    print(fmt.format(*headers))
    print(sep)
    for row in rows:
        print(fmt.format(*row))

    passed = sum(1 for r in results if r["status"] == "PASS")
    failed = len(results) - passed
    print(f"\nИтого: {passed} PASS, {failed} FAIL из {len(results)} карт.")


def discover_maps(game_dir):
    maps_dir = find_case_insensitive(game_dir, "Maps") or (game_dir / "Maps")
    if not maps_dir.is_dir():
        return []
    return sorted(p.name for p in maps_dir.iterdir() if p.is_file() and p.suffix.lower() == ".unr")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--boot", default="build/mac/host/boot/snx_boot", help="путь к исполняемому файлу snx_boot")
    ap.add_argument("--game", default="game", help="каталог игры (только чтение)")
    ap.add_argument("--user", default="build/test-maps-user", help="базовый пользовательский каталог прогонов")
    ap.add_argument("--maps", nargs="+", help="список карт для запуска (например, 1_Shreks_Swamp.unr)")
    ap.add_argument("--all-maps", action="store_true", help="прогнать все карты *.unr из <game>/Maps")
    ap.add_argument("--seconds", type=float, default=20.0, help="длительность прогона каждой карты в секундах")
    ap.add_argument("--timeout", type=float, default=None, help="жёсткий таймаут процесса (по умолчанию seconds + 60)")
    ap.add_argument("--headless", action="store_true", help="запуск без SDL с Engine.NullRenderDevice")
    ap.add_argument("--nosound", action="store_true", help="передать игре флаг -nosound")
    ap.add_argument("--shot-ref", default=None, help="каталог эталонных PNG для сравнения по MSE/PSNR (Pillow)")
    ap.add_argument("--shot-dir", default=None, help="каталог для сохранения снятых скриншотов")
    ap.add_argument("--shot-frame", type=int, default=120, help="номер кадра для SNX_SHOT (по умолчанию 120)")
    ap.add_argument("--min-psnr", type=float, default=30.0, help="минимальный допустимый PSNR в дБ (по умолчанию 30.0)")
    ap.add_argument("--require-ref", action="store_true", help="считать отсутствие эталонного PNG ошибкой")
    ap.add_argument("--check-abi", action="store_true", help="включить SNX_CHECK_ABI=1")
    ap.add_argument("--allow-todo", action="store_true", help="не помечать [hle] TODO как FAIL (только предупреждение)")
    ap.add_argument("--clean-user", action="store_true", help="очищать подкаталог пользователя карты перед прогоном")
    ap.add_argument("--json", dest="json_out", default=None, help="сохранить подробный отчёт в JSON")
    args = ap.parse_args(argv)

    boot = Path(args.boot)
    game = Path(args.game)
    if not boot.is_file():
        print(f"Ошибка: не найден бинарник snx_boot: {boot}", file=sys.stderr)
        return 2
    if not game.is_dir():
        print(f"Ошибка: не найден каталог игры: {game}", file=sys.stderr)
        return 2

    if args.maps:
        maps = args.maps
    elif args.all_maps:
        maps = discover_maps(game)
        if not maps:
            print(f"Ошибка: карты *.unr не найдены в {game}/Maps", file=sys.stderr)
            return 2
    else:
        maps = list(DEFAULT_MAPS)

    results = []
    for map_name in maps:
        res = run_map(args, map_name)
        results.append(res)
        if res["status"] == "FAIL":
            for kind, key in (("TRAP", "traps"), ("TODO", "todos"), ("GL", "gl_errors"), ("ABI", "abi_errors")):
                for line in res["details"][key][:3]:
                    print(f"  [{map_name}] {kind}: {line}", file=sys.stderr)

    print_table(results)

    if args.json_out:
        out_path = Path(args.json_out)
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding="utf-8")

    return 1 if any(r["status"] != "PASS" for r in results) else 0


if __name__ == "__main__":
    sys.exit(main())
