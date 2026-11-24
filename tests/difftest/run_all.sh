#!/bin/sh
# Дифф-тесты всех сгенерированных модулей против unicorn: tests/difftest/run_all.sh [funcs] [seeds]
# Логи — build/difftest_<stem>.log (прогресс раз в 10 с, смотреть tail -f), сводка — в stdout.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
FUNCS=${1:-1500}
SEEDS=${2:-4}
export UC_IGNORE_REG_BREAK=1
for stem in msvcr70 core engine window windrv d3ddrv fire ipdrv gui kwgame alaudio game_exe; do
  h="$ROOT/build/mac/tests/difftest/difftest_$stem"
  [ -x "$h" ] || continue
  case $stem in
    game_exe) mod=game.exe ;;
    msvcr70) mod=msvcr70.dll ;;
    *) mod=$(ls "$ROOT/game/System" | grep -i "^$stem.dll$") ;;
  esac
  log="$ROOT/build/difftest_$stem.log"
  "$ROOT/tools/.venv/bin/python" -I "$ROOT/tests/difftest/difftest.py" --harness "$h" --game "$ROOT/game/System" \
      --module "$mod" --funcs "$FUNCS" --seeds "$SEEDS" > "$log" 2>&1
  echo "== $mod: $(grep 'tests in' "$log" || tail -1 "$log")"
  grep '^FAIL' "$log"
done
