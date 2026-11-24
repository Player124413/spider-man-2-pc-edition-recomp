#!/usr/bin/env bash
# Полный цикл рекомпиляции Spider-Man 2 (PC): проверка игры -> x86 -> C -> объекты и .a.
# Файлы игры НЕ копируются в репозиторий: нужна только ваша собственная копия.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GAME="${GAME:-$ROOT/references/coolgame}"    # каталог с System/, Maps/, ...
GEN="${GEN:-$ROOT/build/gen}"
JOBS="${JOBS:-$(nproc)}"
PY="$ROOT/.venv/bin/python"
CMD="${1:-all}"

need_python() {
  if [[ ! -x "$PY" ]]; then
    python3 -m venv "$ROOT/.venv"
    "$PY" -m pip install -q --upgrade pip
    "$PY" -m pip install -q -r "$ROOT/tools/requirements.txt"
  fi
  "$PY" -c 'import pefile, capstone' >/dev/null 2>&1 || \
    "$PY" -m pip install -q -r "$ROOT/tools/requirements.txt"
}

case "$CMD" in
  check)
    need_python
    "$PY" -I "$ROOT/recomp/main.py" check --game "$GAME/System"
    ;;
  gen)
    need_python
    "$PY" -I "$ROOT/recomp/main.py" gen --game "$GAME/System" --out "$GEN" -j "$JOBS"
    ;;
  build)
    python3 "$ROOT/tools/compile_gen.py" --gen "$GEN" --jobs "$JOBS"
    ;;
  smoke)
    "$ROOT/tools/build_smoke.sh"
    "$ROOT/build/smoke/spider_smoke" "$GAME"
    ;;
  all)
    "$0" check
    "$0" gen
    "$0" build
    "$0" smoke
    ;;
  *)
    echo "Использование: $0 {check|gen|build|smoke|all}" >&2
    exit 2
    ;;
esac
