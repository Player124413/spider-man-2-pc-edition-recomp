#!/usr/bin/env bash
# Проверка Android-проекта без Android SDK: состав файлов, соответствие карте модулей и отсутствие
# Shrek-специфичных путей. Полная сборка APK требует JDK 17 + SDK/NDK (docs/ANDROID.md).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
fail=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fail=1; fi; }

check "Gradle-проект" "test -f '$ROOT/android/app/build.gradle' && test -f '$ROOT/android/settings.gradle'"
check "Kotlin-лаунчер и игра" "test -f '$ROOT/android/app/src/main/java/com/shreknx/game/LauncherActivity.kt' && test -f '$ROOT/android/app/src/main/java/com/shreknx/game/GameActivity.kt'"
check "JNI-мост" "test -f '$ROOT/host/boot/android.c' && grep -q 'Java_com_shreknx_game_SnxNative' '$ROOT/host/boot/android.c'"
check "CMake Android" "test -f '$ROOT/cmake/android.cmake' && grep -q 'SNX_ANDROID_DEPS' '$ROOT/cmake/android.cmake'"
check "зависимости" "test -x '$ROOT/tools/android_deps.sh' && grep -q 'ffmpeg-src' '$ROOT/tools/android_deps.sh'"
check "Workflow" "test -f '$ROOT/.github/workflows/android.yml' && grep -q 'SNX_ANDROID_VIDEO' '$ROOT/.github/workflows/android.yml'"
check "имя exe в лаунчере" "grep -q '\"Webhead.exe\"' '$ROOT/android/app/src/main/java/com/shreknx/game/GameFolder.kt'"
check "нет game.exe в исходниках порта" "! grep -rq 'game.exe' '$ROOT/android/app/src/main/java' '$ROOT/tools/fetch_game.sh'"

n=$("$ROOT/.venv/bin/python" -c 'import json; print(len(json.load(open("recomp/addrmap.json"))["modules"]))' 2>/dev/null || echo 0)
check "12 модулей в addrmap.json" "test \"$n\" -eq 12"

if [ -x "$ROOT/.venv/bin/python" ]; then
  "$ROOT/.venv/bin/python" - <<'PY' || fail=1
try:
    import yaml
except ImportError:
    print("skip workflow YAML (нет PyYAML)")
else:
    yaml.safe_load(open(".github/workflows/android.yml"))
    print("ok   workflow YAML")
PY
fi

if [ "$fail" -ne 0 ]; then echo "ANDROID CHECK FAIL" >&2; exit 1; fi
echo "ANDROID CHECK OK"
