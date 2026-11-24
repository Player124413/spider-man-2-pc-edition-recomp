#!/bin/sh
# Иконка NRO 256x256 из локальных файлов игры (в git не идёт): лицевая сторона скана обложки, а без него —
# заставка игры Help/Logo.bmp. macOS (sips).  tools/make_icon.sh <out.jpg>
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
out=$1
cover="$ROOT/original/Shrek 2/Обложка/1.jpg"
logo="$ROOT/game/Help/Logo.bmp"
tmp="$out.tmp.jpg"
mkdir -p "$(dirname "$out")"
if [ -f "$cover" ] && [ "$(sips -g pixelWidth "$cover" | awk '/pixelWidth/{print $2}')" = 6438 ]; then
  # скан 6438x4314: справа от корешка — лицевая сторона; квадрат с логотипом и лицом, без плашки «PC CD-ROM»
  sips -s format jpeg -c 2990 2990 --cropOffset 330 3445 "$cover" --out "$tmp" >/dev/null
elif [ -f "$logo" ]; then
  sips -s format jpeg -c 480 480 --cropOffset 0 80 "$logo" --out "$tmp" >/dev/null
else
  echo "make_icon: no cover scan or Logo.bmp" >&2
  exit 1
fi
sips -z 256 256 -s formatOptions 90 "$tmp" --out "$out" >/dev/null
rm -f "$tmp"
