#!/bin/sh
# Скачать архив своей копии игры по ссылке и достать из него папку System (DLL и Webhead.exe — вход рекомпилятора).
#   tools/fetch_game.sh <ссылка> <куда положить System>
# Ссылка: прямая на файл или публичная Яндекс Диска (disk.yandex.ru, yadi.sk), Google Диска, Dropbox.
# Архив: zip, 7z или rar (распаковывает 7z); внутри — папка игры целиком или только System, на любой глубине.
# Для русской версии в System должен лежать распакованный английский Webhead.exe (как делает tools/make_rus.sh).
# Используется workflow .github/workflows/android.yml (поле «Ссылка на архив игры»).
set -eu
url=${1:?usage: $0 <url> <dest System dir>}
dest=${2:?usage: $0 <url> <dest System dir>}
tmp=${TMPDIR:-/tmp}/snx-fetch.$$
mkdir -p "$tmp"
trap 'rm -rf "$tmp"' EXIT

UA="Mozilla/5.0 (X11; Linux x86_64) webheadNX-fetch"
case "$url" in
  *disk.yandex.*|*yadi.sk*)
    # публичная ссылка -> временная прямая через API Диска
    api="https://cloud-api.yandex.net/v1/disk/public/resources/download?public_key=$(printf %s "$url" | python3 -c 'import sys, urllib.parse; print(urllib.parse.quote(sys.stdin.read().strip(), safe=""))')"
    url=$(curl -fsSL "$api" | python3 -c 'import json, sys; print(json.load(sys.stdin)["href"])') ;;
  *drive.google.com*|*docs.google.com*)
    id=$(printf %s "$url" | sed -n 's|.*/d/\([A-Za-z0-9_-]*\).*|\1|p')
    [ -n "$id" ] || id=$(printf %s "$url" | sed -n 's|.*[?&]id=\([A-Za-z0-9_-]*\).*|\1|p')
    [ -n "$id" ] || { echo "fetch_game: не понял ссылку Google Диска" >&2; exit 1; }
    url="https://drive.usercontent.google.com/download?id=$id&export=download&confirm=t" ;;
  *dropbox.com*)
    url=$(printf %s "$url" | sed -e 's/[?&]dl=0//' -e 's/[?&]raw=1//')
    case "$url" in *\?*) url="$url&dl=1" ;; *) url="$url?dl=1" ;; esac ;;
esac

echo "fetch_game: скачивание…"
curl -fsSL --retry 3 --retry-delay 5 -A "$UA" -o "$tmp/archive" "$url"
size=$(wc -c < "$tmp/archive")
echo "fetch_game: $size байт"
if head -c 512 "$tmp/archive" | grep -qi "<html"; then
  echo "fetch_game: вместо архива пришла веб-страница — ссылка не прямая или файл закрыт" >&2
  exit 1
fi

mkdir -p "$tmp/x"
7z x -y -bd -o"$tmp/x" "$tmp/archive" >/dev/null || { echo "fetch_game: не удалось распаковать (нужен zip/7z/rar)" >&2; exit 1; }
rm -f "$tmp/archive"

# Частая беда с архивами из GitHub: .bik (и другие ассеты) лежат как указатели Git LFS по 130 байт.
# Такие «файлы» рекомпилятор примет за битые ролики — игра запустится, но все видео будут отсутствовать.
lfs=$(find "$tmp/x" -type f -size -4096c -exec grep -l -F 'version https://git-lfs.github.com/spec/v1' {} + 2>/dev/null | head -1 || true)
if [ -n "$lfs" ]; then
  echo "fetch_game: $(basename "$lfs") — указатель Git LFS, а не файл игры. Нужны реальные файлы:" >&2
  echo "  git lfs install && git lfs pull   (в клоне исходной игры, затем заново упаковать архив)" >&2
  echo "  Настоящий .bik начинается с «BIK» и весит мегабайты, а не 132 байта." >&2
  exit 1
fi

core=$(find "$tmp/x" -type f -iname core.dll | head -1)
[ -n "$core" ] || { echo "fetch_game: в архиве нет System/Core.dll" >&2; exit 1; }
sys=$(dirname "$core")
exe=$(find "$sys" -maxdepth 1 -type f -iname Webhead.exe | head -1)
[ -n "$exe" ] || { echo "fetch_game: в $(basename "$sys") нет Webhead.exe" >&2; exit 1; }

# модули из recomp/addrmap.json — с именами в его регистре (на Linux регистр важен); защищённый SafeDisc
# Webhead.exe (секции stxt*) рекомпилировать нельзя: нужен распакованный (README, «Таблетка»)
PY=python3
[ -x "$(dirname "$0")/.venv/bin/python" ] && PY="$(dirname "$0")/.venv/bin/python"
"$PY" - "$sys" "$dest" "$(dirname "$0")/../recomp/addrmap.json" <<'PYEOF'
import json, os, shutil, sys
src, dst, amap = sys.argv[1:4]
names = json.load(open(amap))["modules"]
have = {f.lower(): f for f in os.listdir(src)}
missing = [n for n in names if n.lower() not in have]
if missing:
    sys.exit("fetch_game: в System нет " + ", ".join(missing))
# В addrmap исполняемый модуль записан как webhead.exe (строчная w), а в архиве он может быть Webhead.exe.
# Копируем под именем из addrmap и проверяем PE по тому же имени: на Linux регистр важен.
exe_dst = next(n for n in names if n.lower() == "webhead.exe")
shutil.rmtree(dst, ignore_errors=True)
os.makedirs(dst)
for n in names:
    shutil.copyfile(os.path.join(src, have[n.lower()]), os.path.join(dst, n))
try:
    import pefile
    try:
        pe = pefile.PE(os.path.join(dst, exe_dst), fast_load=True)
    except pefile.PEFormatError as e:
        sys.exit("fetch_game: %s — не исполняемый файл Windows (%s)" % (exe_dst, e))
    secs = [s.Name.rstrip(b"\0").decode("latin-1") for s in pe.sections]
    if any(x.lower().startswith("stxt") for x in secs):
        sys.exit("fetch_game: %s защищён SafeDisc (секции %s) — положите в System распакованный Webhead.exe"
                 % (exe_dst, ", ".join(secs)))
except ImportError:
    pass
print("fetch_game: %d модулей -> %s" % (len(names), dst))
PYEOF
