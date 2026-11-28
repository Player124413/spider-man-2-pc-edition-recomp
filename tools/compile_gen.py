#!/usr/bin/env python3
"""Сборка сгенерированного рекомпилятором C в объектные файлы и статические библиотеки.

Использование: python3 tools/compile_gen.py [--gen build/gen] [--jobs N] [--cc gcc]

Сгенерированный код зависит только от runtime/gen_rt.h и libm, поэтому для этой
стадии не нужны SDL, OpenGL, OpenAL и прочие библиотеки хостового порта.
"""
import argparse
import concurrent.futures
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent.parent
CFLAGS = ["-O1", "-fPIC", "-fno-stack-protector", "-fno-common", "-I", str(ROOT)]


def compile_one(args):
    src, dst, cc = args
    dst.parent.mkdir(parents=True, exist_ok=True)
    cmd = [cc, "-c", *CFLAGS, str(src), "-o", str(dst)]
    r = subprocess.run(cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    return src, dst, r.returncode, r.stderr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gen", type=Path, default=ROOT / "build/gen")
    ap.add_argument("--obj", type=Path, default=ROOT / "build/obj")
    ap.add_argument("--lib", type=Path, default=ROOT / "build/lib")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    ap.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    args = ap.parse_args()

    gen = args.gen.resolve()
    if not gen.is_dir():
        sys.exit(f"нет каталога {gen}: сначала run tools/recomp.sh gen")
    if shutil.which(args.cc) is None:
        sys.exit(f"компилятор {args.cc} не найден")

    sources = sorted(gen.glob("*/*.c")) + ([gen / "hle_stubs.c"] if (gen / "hle_stubs.c").exists() else [])
    jobs = []
    for src in sources:
        dst = args.obj.resolve() / src.relative_to(gen).with_suffix(".o")
        jobs.append((src, dst, args.cc))

    t0 = time.time()
    failed = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        for i, (src, dst, rc, err) in enumerate(pool.map(compile_one, jobs), 1):
            if rc:
                failed.append((src, err))
            if i % 25 == 0 or i == len(jobs):
                print(f"  C -> obj: {i}/{len(jobs)}", flush=True)
    if failed:
        for src, err in failed[:10]:
            print(f"ОШИБКА {src}:\n{err}", file=sys.stderr)
        sys.exit(f"не собрано {len(failed)} файлов")

    # Одна статическая библиотека на модуль (как в cmake/gen.cmake): линковщик
    # подтянет из неё только реально используемые функции.
    args.lib.mkdir(parents=True, exist_ok=True)
    libs = []
    for mod in sorted(p for p in args.obj.iterdir() if p.is_dir()):
        objs = sorted(mod.glob("*.o"))
        if not objs:
            continue
        out = args.lib / f"lib{mod.name}.a"
        if out.exists():
            out.unlink()
        subprocess.run(["ar", "rcs", str(out), *map(str, objs)], check=True)
        libs.append(out)
    stubs = sorted(args.obj.glob("hle_stubs*.o"))
    if stubs:
        out = args.lib / "libhle_stubs.a"
        if out.exists():
            out.unlink()
        subprocess.run(["ar", "rcs", str(out), *map(str, stubs)], check=True)
        libs.append(out)

    report = ["# Результат рекомпиляции", "", f"Модулей: {len([p for p in args.obj.iterdir() if p.is_dir()])}",
              f"Файлов C: {len(sources)}", f"Ошибок компиляции: 0", f"Время: {time.time() - t0:.1f} c", ""]
    for mod in sorted(p for p in args.obj.iterdir() if p.is_dir()):
        objs = sorted(mod.glob("*.o"))
        report.append(f"- `{mod.name}`: {len(objs)} .o, "
                      f"{sum(p.stat().st_size for p in objs) / 1048576:.1f} MiB объекта")
    (args.obj.parent / "REPORT.md").write_text("\n".join(report) + "\n", encoding="utf-8")
    print(f"Готово: {len(sources)} файлов, {len(libs)} библиотек, {time.time() - t0:.1f} c")
    print(f"Отчёт: {args.obj.parent / 'REPORT.md'}")


if __name__ == "__main__":
    main()
