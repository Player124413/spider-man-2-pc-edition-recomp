#!/bin/sh
# Сборка: tools/build.sh mac|switch|mac-rus|switch-rus|switch-pgo|android [цели cmake...]
# -rus — только для mac/switch: отдельная генерация из русской копии игры. Для Android вариантов нет:
# один APK поддерживает английскую и русскую локализации (язык определяется лаунчером по System/*.int).
# switch-pgo — Switch со счётчиками профиля (build/switch-pgo, SNX_PGO=gen). Switch собирается по профилю из
# build/pgo, если он есть (AGENTS.md, «PGO»; SNX_PGO= — без него): новый профиль (tools/pgo_fetch.py) — с нуля.
# android — APK через Gradle (docs/ANDROID.md): нужны ANDROID_HOME (SDK с NDK 27 и CMake 3.31.6), JDK 17 и
# библиотеки из tools/android_deps.sh; результат — build/webheadNX.apk.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PLAT=${1:-mac}
[ $# -gt 0 ] && shift
case "$PLAT" in
  android-eng|android-rus|android-uni)
    echo "$PLAT: отдельных сборок eng/rus/uni больше нет — собираю один APK" >&2
    PLAT=android ;;
esac
case "$PLAT" in
  *-rus) VARIANT=rus ;;
  *)     VARIANT= ;;
esac
PGO=${SNX_PGO-auto}
if [ "$PLAT" = switch-pgo ]; then
  PGO=gen
elif [ "$PGO" = auto ]; then
  PGO=
  case "$PLAT" in switch*) [ -f "$ROOT/build/pgo/stamp" ] && PGO=use ;; esac
fi
if [ "$PLAT" = android ]; then
  [ -d "$ROOT/build/android-deps/SDL" ] || sh "$ROOT/tools/android_deps.sh"
  GRADLE=${SNX_GRADLE:-}  # свой gradle; по умолчанию — версия из gradle-wrapper.properties (с AGP 8.7 проверена она)
  if [ -z "$GRADLE" ]; then  # gradle-wrapper.jar в репозитории нет: дистрибутив скачивается в build/gradle
    URL=$(sed -n 's/^distributionUrl=//p' "$ROOT/android/gradle/wrapper/gradle-wrapper.properties" | sed 's/\\:/:/')
    NAME=$(basename "$URL" -bin.zip)
    if [ ! -x "$ROOT/build/gradle/$NAME/bin/gradle" ]; then
      mkdir -p "$ROOT/build/gradle"
      curl -fL "$URL" -o "$ROOT/build/gradle/gradle.zip"
      (cd "$ROOT/build/gradle" && unzip -q -o gradle.zip && rm gradle.zip)
    fi
    GRADLE="$ROOT/build/gradle/$NAME/bin/gradle"
  fi
  "$GRADLE" -p "$ROOT/android" assembleRelease "$@"
  OUT=$(ls "$ROOT/android/app/build/outputs/apk/release/"*.apk | head -1)
  cp "$OUT" "$ROOT/build/webheadNX.apk"
  echo "APK: $ROOT/build/webheadNX.apk"
  exit 0
fi
CLEAN=
case "$PLAT" in
  mac|mac-rus)       cmake -S "$ROOT" -B "$ROOT/build/$PLAT" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
                           -DSNX_VARIANT="$VARIANT" >/dev/null ;;
  switch|switch-rus|switch-pgo)
                     : "${DEVKITPRO:=/opt/devkitpro}"; export DEVKITPRO; PATH="$DEVKITPRO/tools/bin:$PATH"; export PATH
                     cmake -S "$ROOT" -B "$ROOT/build/$PLAT" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
                           -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" -DSNX_VARIANT="$VARIANT" \
                           -DSNX_PGO="$PGO" >/dev/null
                     # профиль не зависимость объектников: другой профиль (или его смена с выключенным) — с нуля
                     STAMP="$ROOT/build/$PLAT/pgo.stamp"
                     WANT="$PGO"
                     [ "$PGO" = use ] && WANT="use $(cat "$ROOT/build/pgo/stamp" 2>/dev/null || echo none)"
                     if [ "$(cat "$STAMP" 2>/dev/null || true)" != "$WANT" ]; then
                       CLEAN=--clean-first
                     fi ;;
  *) echo "usage: $0 mac|switch|mac-rus|switch-rus|switch-pgo|android [targets]" >&2; exit 2 ;;
esac
if [ $# -gt 0 ]; then
  cmake --build "$ROOT/build/$PLAT" $CLEAN --target "$@"
else
  cmake --build "$ROOT/build/$PLAT" $CLEAN
fi
case "$PLAT" in
  switch*) printf '%s\n' "$WANT" > "$STAMP" ;;
esac
