"""Залить каталог на Switch по FTP (ftpd / sys-ftpd), одним соединением.

switch_upload.py --host <SWITCH_IP> [--port 5000] <локальный каталог> <путь на карте> [--only System,Maps]
Файлы, которые на карте уже есть с тем же размером, пропускаются (повторная заливка — только изменённое;
размеры — из LIST, а если сервер его не отдаёт, как FTP у DBI, — командой SIZE на каждый файл).
DBI держит одно управляющее соединение: пока идёт заливка, второй клиент не подключится.
Пример: tools/.venv/bin/python -I tools/switch_upload.py --host IP game /switch/shrek2/game
"""
import argparse
import ftplib
import os
import sys
import time


def remote_sizes(ftp, path):
    """Размеры файлов в каталоге на карте (имя в нижнем регистре -> размер); None — каталога нет."""
    try:
        ftp.cwd(path)
    except ftplib.error_perm:
        return None
    sizes = {}
    lines = []
    ftp.retrlines("LIST", lines.append)
    for ln in lines:
        parts = ln.split(None, 8)
        if len(parts) == 9 and not ln.startswith("d"):
            try:
                sizes[parts[8].lower()] = int(parts[4])
            except ValueError:
                pass
    return sizes


def ensure_dir(ftp, path):
    cur = ""
    for part in path.strip("/").split("/"):
        cur += "/" + part
        try:
            ftp.cwd(cur)
        except ftplib.error_perm:
            ftp.mkd(cur)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True)
    ap.add_argument("--port", type=int, default=5000)
    ap.add_argument("--only", default="", help="через запятую: верхние подкаталоги, которые заливать")
    ap.add_argument("src")
    ap.add_argument("dst")
    args = ap.parse_args()
    only = {x.lower() for x in args.only.split(",") if x}
    def connect():
        f = ftplib.FTP()
        f.connect(args.host, args.port, timeout=120)
        f.login()
        f.set_pasv(True)
        return f

    ftp = connect()
    total = sent = skipped = 0
    t0 = time.time()
    for root, dirs, files in os.walk(args.src):
        dirs.sort()
        rel = os.path.relpath(root, args.src)
        top = rel.split(os.sep)[0].lower()
        if only and rel != "." and top not in only:
            continue
        rdir = args.dst.rstrip("/") + ("" if rel == "." else "/" + rel.replace(os.sep, "/"))
        ensure_dir(ftp, rdir)
        have = remote_sizes(ftp, rdir) or {}
        for name in sorted(files):
            if only and rel == ".":
                continue
            local = os.path.join(root, name)
            size = os.path.getsize(local)
            total += size
            remote = have.get(name.lower())
            if remote is None and not have:
                try:
                    remote = ftp.size(rdir + "/" + name)
                except (ftplib.Error, OSError):
                    remote = None
            if remote == size:
                skipped += 1
                continue
            for attempt in range(5):  # sys-ftpd/ftpd иногда не отвечают после передачи — переподключиться
                try:
                    with open(local, "rb") as f:
                        ftp.storbinary("STOR " + rdir + "/" + name, f, blocksize=1 << 16)
                    break
                except (TimeoutError, OSError, ftplib.Error) as e:
                    print(f"  retry {name}: {e}", flush=True)
                    try:
                        ftp.close()
                    except OSError:
                        pass
                    time.sleep(2)
                    ftp = connect()
            else:
                sys.exit(f"failed: {local}")
            sent += size
            el = time.time() - t0
            print(f"{rdir}/{name} {size >> 10} KB  [{sent >> 20} MB, {sent / max(el, 1e-3) / 1e6:.1f} MB/s]",
                  flush=True)
    ftp.quit()
    print(f"done: sent {sent >> 20} MB, skipped {skipped} files (already there), total {total >> 20} MB")


if __name__ == "__main__":
    sys.exit(main())
