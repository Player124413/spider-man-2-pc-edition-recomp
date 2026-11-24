"""Сверка декодера звука Bink с ffmpeg.

compare.py --dec build/mac/tests/bink/bink_dec --tmp <каталог> <файл.bik|файл.uax> [...]
Для .bik сверяется дорожка 0; в .uax ищутся все вложенные потоки BIKi (звуки пакета), сверяются первые --limit.
Печатает на поток: длины, максимум |разности|, долю сэмплов с |разностью| > 1, SNR (дБ).
"""
import argparse
import array
import math
import os
import subprocess
import sys


def s16(path):
    a = array.array("h")
    with open(path, "rb") as f:
        a.frombytes(f.read())
    return a


def compare(dec, tmp, path, off, name):
    ours = os.path.join(tmp, "ours.raw")
    ref = os.path.join(tmp, "ref.raw")
    src = path
    if off:
        with open(path, "rb") as f:
            f.seek(off)
            head = f.read(8)
            data = head + f.read(int.from_bytes(head[4:8], "little"))
        src = os.path.join(tmp, "one.bik")
        with open(src, "wb") as f:
            f.write(data)
    with open(ours, "wb") as out:
        r = subprocess.run([dec, src], stdout=out, stderr=subprocess.PIPE, text=True)
    info = r.stderr.strip().replace("\n", "; ")
    if r.returncode:
        print(f"{name}: decoder failed: {info}")
        return False
    r = subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", src, "-map", "0:a:0",
                        "-f", "s16le", "-acodec", "pcm_s16le", ref], capture_output=True, text=True)
    if r.returncode:
        print(f"{name}: ffmpeg failed: {r.stderr.strip()}")
        return False
    a, b = s16(ours), s16(ref)
    n = min(len(a), len(b))
    maxd = big = 0
    sig = err = 0.0
    for i in range(n):
        d = abs(a[i] - b[i])
        maxd = max(maxd, d)
        big += d > 1
        sig += b[i] * b[i]
        err += d * d
    snr = 10 * math.log10(sig / err) if err else float("inf")
    ok = len(a) == len(b) and maxd <= 2
    print(f"{name}: {info}; ours {len(a)} ref {len(b)} max|d| {maxd} >1: {big / max(n, 1):.4%} "
          f"SNR {snr:.1f} dB {'OK' if ok else 'MISMATCH'}")
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dec", required=True)
    ap.add_argument("--tmp", required=True)
    ap.add_argument("--limit", type=int, default=20)
    ap.add_argument("files", nargs="+")
    args = ap.parse_args()
    bad = 0
    for path in args.files:
        if path.lower().endswith(".bik"):
            bad += not compare(args.dec, args.tmp, path, 0, os.path.basename(path))
            continue
        with open(path, "rb") as f:
            blob = f.read()
        pos = blob.find(b"BIKi")
        k = 0
        while pos >= 0 and k < args.limit:
            bad += not compare(args.dec, args.tmp, path, pos, f"{os.path.basename(path)}@{pos:#x}")
            k += 1
            pos = blob.find(b"BIKi", pos + 8)
    print(f"mismatches: {bad}")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
