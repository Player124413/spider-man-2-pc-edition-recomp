"""Проверить индекс git или историю одной публикуемой ветки; файлы игры не нужны."""
import argparse
from pathlib import PurePosixPath
import subprocess
import sys


LOCAL_ROOTS = {"game", "shrek2rus", "original", "build", "shots", "shot-ref", "userdata", "gen", "generated"}
LOCAL_SUFFIXES = {
    ".exe", ".dll", ".nro", ".nso", ".elf", ".nacp", ".o", ".a", ".dylib",
    ".zip", ".iso", ".img", ".ccd", ".cue", ".sub", ".7z", ".rar",
    ".u", ".unr", ".utx", ".usx", ".ukx", ".uax", ".rus_uax", ".bik", ".ogg", ".usa", ".sav",
    ".gcda", ".gcno", ".log", ".dmp", ".pem", ".key",
    ".png", ".bmp", ".jpg", ".jpeg",
}


def git(*args):
    return subprocess.check_output(["git", *args])


def path_problem(name):
    p = PurePosixPath(name.lower())
    if (p.parts[0] in LOCAL_ROOTS or ".venv" in p.parts or "__pycache__" in p.parts
            or p.parts[:2] in (("tools", "llvm"), ("tools", "ghidra"))):
        return "локальный каталог"
    if (p.suffix in LOCAL_SUFFIXES or p.name == ".ds_store" or p.name.startswith(".env")
            or p.name.startswith("snx.log")):
        return "локальный артефакт или настройки"
    if (p.parts[:2] == ("docs", "recon") and p.suffix == ".txt") or (
            len(p.parts) == 1 and p.suffix == ".txt" and p.name.startswith(("nxlink", "prof", "crash"))):
        return "локальный отчёт или трасса"
    return None


def blob_problem(data):
    if len(data) > 1024 * 1024:
        return "файл больше 1 МиБ: проверить состав вручную"
    if b"\0" in data:
        return "бинарное содержимое"
    try:
        source = data.decode("utf-8")
    except UnicodeDecodeError:
        return "содержимое не UTF-8"
    if source.startswith("/* Сгенерировано recomp из ") or source.startswith("/* Слабые заглушки для HLE-импортов"):
        return "вывод рекомпилятора"
    return None


def check(revisions):
    problems, seen = set(), set()
    for rev in revisions:
        listing = git("ls-tree", "-r", "-z", rev) if rev else git("ls-files", "--stage", "-z")
        for entry in listing.split(b"\0"):
            if not entry:
                continue
            meta, raw_name = entry.split(b"\t", 1)
            name = raw_name.decode("utf-8", errors="replace")
            fields = meta.decode("ascii").split()
            mode, oid = fields[0], fields[2] if rev else fields[1]
            if mode not in ("100644", "100755") or (not rev and fields[2] != "0"):
                problems.add((name, "необычный тип записи или неразрешённый конфликт"))
                continue
            reason = path_problem(name)
            if reason:
                problems.add((name, reason))
            if oid not in seen:
                seen.add(oid)
                reason = blob_problem(git("cat-file", "blob", oid))
                if reason:
                    problems.add((name, reason))
    for name, reason in sorted(problems):
        print(f"BLOCK: {name}: {reason}")
    print(f"Checked {len(seen)} unique blobs; {len(problems)} problems.")
    return bool(problems)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("revision", nargs="?", help="ветка/коммит; без аргумента — индекс git")
    ap.add_argument("--history", action="store_true", help="все предки revision (по умолчанию HEAD)")
    args = ap.parse_args()
    if args.history:
        if git("rev-parse", "--is-shallow-repository").strip() == b"true":
            ap.error("нужна полная история: git fetch --unshallow")
        rev = git("rev-parse", "--verify", "--end-of-options", (args.revision or "HEAD") + "^{commit}").decode().strip()
        revisions = git("rev-list", rev).decode().splitlines()
    elif args.revision:
        revisions = [git("rev-parse", "--verify", "--end-of-options", args.revision + "^{tree}").decode().strip()]
    else:
        revisions = [None]
    return check(revisions)


if __name__ == "__main__":
    sys.exit(main())
