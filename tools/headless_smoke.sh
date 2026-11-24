#!/usr/bin/env bash
# Проверка headless-запуска: игра должна стартовать, войти в кадровый цикл D3D8 и не дать ни одного trap.
# Игра сама не выходит — поэтому运行 ограничено таймаутом (код 124 считается успехом).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GAME="${GAME:-$ROOT/references/coolgame}"
SECONDS="${SNX_SMOKE_SECONDS:-8}"
LOG="$ROOT/build/headless_smoke.log"

mkdir -p "$ROOT/build/userdata-smoke/System"
cp "$ROOT/tests/headless/userdata/System/Webhead.ini" "$ROOT/build/userdata-smoke/System/Webhead.ini"
"$ROOT/tools/build_headless.sh"

set +e
SNX_GAME="$GAME" SNX_USERDATA="$ROOT/build/userdata-smoke" \
  timeout "$SECONDS" "$ROOT/build/headless_run" >"$LOG" 2>&1
rc=$?
set -e

if [[ $rc -ne 0 && $rc -ne 124 ]]; then
  echo "HEADLESS SMOKE FAIL: rc=$rc" >&2
  tail -40 "$LOG" >&2
  exit 1
fi
if grep -q 'HEADLESS FAIL\|\[rt\] TRAP\|shader compile failed' "$LOG"; then
  echo "HEADLESS SMOKE FAIL: trap/ошибка в логе" >&2
  grep -m5 'HEADLESS FAIL\|\[rt\] TRAP\|shader compile failed' "$LOG" >&2
  exit 1
fi
grep -q 'HEADLESS OK: modules attached' "$LOG" || { echo "HEADLESS SMOKE FAIL: нет старта" >&2; exit 1; }
grep -q '\[d3d8\] frame ' "$LOG" || { echo "HEADLESS SMOKE FAIL: нет кадров" >&2; exit 1; }
frames=$(grep -c '\[d3d8\] frame ' "$LOG")
echo "HEADLESS SMOKE OK: startup без trap, кадров: $frames (лог: $LOG)"
