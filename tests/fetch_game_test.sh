#!/bin/sh
# Проверка tools/fetch_game.sh без игры: синтетические архивы (zip и 7z, вложенные папки, имена в другом регистре)
# раздаются локальным HTTP-сервером; ошибки — отсутствующий модуль, веб-страница вместо архива, не архив.
# Нужны python3 и 7z. Запуск: sh tests/fetch_game_test.sh (в CI — .github/workflows/android.yml).
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
PORT=${PORT:-18765}
trap 'kill $SRV 2>/dev/null || true; rm -rf "$T"' EXIT

mods=$(python3 -c "import json; print(' '.join(json.load(open('$ROOT/recomp/addrmap.json'))['modules']))")
mkdir -p "$T/src/Spider-Man 2/SYSTEM" "$T/srv"
for m in $mods; do
  lower=$(printf %s "$m" | tr 'A-Z' 'a-z')
  # В архиве исполняемый файл может называться Webhead.exe (с заглавной), хотя в addrmap.json он webhead.exe
  if [ "$lower" = webhead.exe ]; then lower=Webhead.exe; fi
  echo "$m" > "$T/src/Spider-Man 2/SYSTEM/$lower"  # регистр не как в addrmap.json
done
# Заглушка pefile: проверяет, что PE открывается по имени из addrmap (строчное webhead.exe), а не Webhead.exe
mkdir -p "$T/stub"
cat > "$T/stub/pefile.py" <<'PYEOF'
import os
class PEFormatError(Exception):
    pass
class PE:
    def __init__(self, path, fast_load=False):
        if os.path.basename(path) != "webhead.exe":
            raise AssertionError("fetch_game: PE открыт не по имени из addrmap: %s" % path)
        self.sections = []
PYEOF
echo ini > "$T/src/Spider-Man 2/SYSTEM/Default.ini"
mkdir -p "$T/src/Spider-Man 2/Maps" && echo map > "$T/src/Spider-Man 2/Maps/a.unr"
(cd "$T/src" && 7z a -tzip "$T/srv/game.zip" "Spider-Man 2" >/dev/null && 7z a -t7z "$T/srv/game.7z" "Spider-Man 2" >/dev/null)
rm "$T/src/Spider-Man 2/SYSTEM/gui.dll"
(cd "$T/src" && 7z a -tzip "$T/srv/nogui.zip" "Spider-Man 2" >/dev/null)
echo '<!DOCTYPE html><html>login</html>' > "$T/srv/page.zip"
head -c 4096 /dev/urandom > "$T/srv/junk.zip"

(cd "$T/srv" && exec python3 -m http.server "$PORT" --bind 127.0.0.1 >/dev/null 2>&1) &
SRV=$!
for _ in 1 2 3 4 5 6 7 8 9 10; do
  curl -fs "http://127.0.0.1:$PORT/" >/dev/null 2>&1 && break
  sleep 0.5
done

fail=0
ok() {  # ok <архив>: успех, 12 модулей с именами из addrmap.json
  out="$T/out-$1"
  if PYTHONPATH="$T/stub${PYTHONPATH:+:$PYTHONPATH}" sh "$ROOT/tools/fetch_game.sh" "http://127.0.0.1:$PORT/$1" "$out" >"$T/log" 2>&1; then
    for m in $mods; do
      [ -f "$out/$m" ] || { echo "FAIL $1: нет $m"; fail=1; }
    done
    n=$(ls "$out" | wc -l)
    [ "$n" -eq 12 ] || { echo "FAIL $1: $n файлов вместо 12"; fail=1; }
    echo "ok   $1"
  else
    echo "FAIL $1:"; cat "$T/log"; fail=1
  fi
}
bad() {  # bad <архив> <ожидаемый текст ошибки>
  if sh "$ROOT/tools/fetch_game.sh" "http://127.0.0.1:$PORT/$1" "$T/out-$1" >"$T/log" 2>&1; then
    echo "FAIL $1: ожидалась ошибка"; fail=1
  elif grep -q "$2" "$T/log"; then
    echo "ok   $1 -> $(grep "$2" "$T/log" | head -1)"
  else
    echo "FAIL $1: нет «$2» в выводе:"; cat "$T/log"; fail=1
  fi
}
ok game.zip
ok game.7z
bad nogui.zip "нет GUI.dll"
bad page.zip "веб-страница"
bad junk.zip "не удалось распаковать"
bad missing.zip "404"
exit $fail
