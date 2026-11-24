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
check "fetch_game: PE по имени из addrmap" "grep -q 'exe_dst = next' '$ROOT/tools/fetch_game.sh' && ! grep -q 'dst, \"Webhead.exe\"' '$ROOT/tools/fetch_game.sh'"
check "диагностика Git LFS в роликах" "grep -q 'git-lfs.github.com/spec/v1' '$ROOT/tools/fetch_game.sh' && grep -q 'movieProblem' '$ROOT/android/app/src/main/java/com/shreknx/game/GameFolder.kt'"

# Один APK на обе локализации: никаких eng/rus/both/uni во workflow, Gradle и скрипте сборки
check "одна ссылка на игру в workflow" "grep -q 'game_url:' '$ROOT/.github/workflows/android.yml' && ! grep -q '^ *variant:' '$ROOT/.github/workflows/android.yml'"
check "нет вариантов eng/rus/uni в workflow" "! grep -Eq '\[eng, rus\]|game_url_rus|both|android-uni' '$ROOT/.github/workflows/android.yml'"
check "нет flavor'ов в Gradle" "! grep -q 'productFlavors\|flavorDimensions' '$ROOT/android/app/build.gradle'"
check "имя приложения в основных ресурсах" "grep -q 'name=\"app_name\"' '$ROOT/android/app/src/main/res/values/strings.xml'"
check "один APK в tools/build.sh" "grep -q 'assembleRelease' '$ROOT/tools/build.sh' && grep -q 'webheadNX.apk' '$ROOT/tools/build.sh'"
check "язык определяется лаунчером" "grep -q 'detectLanguage' '$ROOT/android/app/src/main/java/com/shreknx/game/GameVariant.kt' && ! grep -rq 'BuildConfig.VARIANT' '$ROOT/android/app/src/main/java'"
check "обновление без вариантов" "grep -q 'webheadNX-nogame-' '$ROOT/android/app/src/main/java/com/shreknx/game/Updater.kt' && ! grep -q 'variants()' '$ROOT/android/app/src/main/java/com/shreknx/game/Updater.kt'"

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
