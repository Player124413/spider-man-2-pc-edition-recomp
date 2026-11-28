"""Профиль PGO со Switch по FTP (ftpd на консоли): *.gcda из /switch/shrek2/pgo -> build/pgo (AGENTS.md, «PGO»).

    python3 -I tools/pgo_fetch.py <SWITCH_IP> [--port 5000] [--out build/pgo] [--remote /switch/shrek2/pgo]
    SNX_PGO=use tools/build.sh switch

Каталог out очищается от старых *.gcda; в out/stamp — время забора (build.sh по нему пересобирает с нуля).
"""
import argparse
import ftplib
import glob
import os
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def connect(host, port, remote):
    """Подключиться к ftpd на Switch и перейти в каталог с *.gcda."""
    ftp = ftplib.FTP()
    ftp.connect(host, port, timeout=20)
    ftp.login()
    ftp.cwd(remote)  # ftpd на консоли: NLST с путём-аргументом отдаёт пустой список
    return ftp


def close_quiet(ftp):
    if ftp is None:
        return
    try:
        ftp.close()
    except Exception:
        pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("--port", type=int, default=5000)
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "pgo"))
    ap.add_argument("--remote", default="/switch/shrek2/pgo")
    args = ap.parse_args()

    ftp = connect(args.host, args.port, args.remote)
    names = sorted(n.rsplit("/", 1)[-1] for n in ftp.nlst())
    names = [n for n in names if n.endswith(".gcda")]
    if not names:
        close_quiet(ftp)
        raise SystemExit(f"no *.gcda in {args.remote}")

    os.makedirs(args.out, exist_ok=True)
    for pat in ("*.gcda", "*.gcda.part"):
        for old in glob.glob(os.path.join(args.out, pat)):
            os.remove(old)

    total = 0
    retries = 0
    for idx, n in enumerate(names, 1):
        dst = os.path.join(args.out, n)
        part = dst + ".part"
        for attempt in range(1, 5):
            try:
                if ftp is None:
                    ftp = connect(args.host, args.port, args.remote)
                with open(part, "wb") as fp:
                    ftp.retrbinary(f"RETR {n}", fp.write)
                os.replace(part, dst)
                total += os.path.getsize(dst)
                break
            except (TimeoutError, OSError, ftplib.error_temp, EOFError) as err:
                if os.path.exists(part):
                    os.remove(part)
                close_quiet(ftp)
                ftp = None
                if attempt == 4:
                    raise SystemExit(f"failed to fetch {n} after 4 attempts: {err}") from err
                retries += 1
                print(f"  повтор {attempt}/3 для {n} ({idx}/{len(names)}): {err}", flush=True)
                time.sleep(1)
        if idx % 25 == 0 and idx < len(names):
            print(f"  {idx}/{len(names)} ({total >> 10} KB)...", flush=True)

    try:
        if ftp is not None:
            ftp.quit()
    except Exception:
        close_quiet(ftp)

    with open(os.path.join(args.out, "stamp"), "w") as fp:
        fp.write(time.strftime("%Y-%m-%d %H:%M:%S") + f" {len(names)} files\n")
    extra = f", повторов: {retries}" if retries else ""
    print(f"{len(names)} files, {total >> 10} KB -> {args.out}{extra}")


if __name__ == "__main__":
    main()
