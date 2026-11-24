#!/bin/sh
# Сборка: tools/build.sh mac|switch|mac-rus|switch-rus|switch-pgo|android|android-rus|android-uni [цели cmake...]
# -rus — русская версия (SNX_VARIANT=rus: код из build/gen-rus, каталог сборки build/<платформа>-rus).
# switch-pgo — Switch со счётчиками профиля (build/switch-pgo, SNX_PGO=gen). Switch собирается по профилю из
# build/pgo, если он есть (AGENTS.md, «PGO»; SNX_PGO= — без него): новый профиль (tools/pgo_fetch.py) — с нуля.
# android — APK через Gradle (docs/ANDROID.md): нужны ANDROID_HOME (SDK с NDK 27 и CMake 3.31.6), JDK 17 и
# библиотеки из tools/android_deps.sh; результат — build/webheadNX-<eng|rus>.apk.
# android-uni — универсальный APK с обеими версиями игры (нужны build/gen и build/gen-rus): сначала APK rus, из него
# libmain.so -> build/uni-jni/arm64-v8a/libmain_rus.so, CRC32 Core.dll обеих копий -> build/uni-assets/variants.txt
# (по ним лаунчер узнаёт версию игры в папке), затем flavor uni с кодом английской; результат — build/webheadNX-uni.apk.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PLAT=${1:-mac}
[ $# -gt 0 ] && shift
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
if [ "$PLAT" = android-uni ]; then
  sh "$0" android-rus "$@"
  rm -rf "$ROOT/build/uni-jni" "$ROOT/build/uni-assets"
  mkdir -p "$ROOT/build/uni-jni/arm64-v8a" "$ROOT/build/uni-assets"
  unzip -p "$ROOT/build/webheadNX-rus.apk" lib/arm64-v8a/libmain.so > "$ROOT/build/uni-jni/arm64-v8a/libmain_rus.so"
  [ -s "$ROOT/build/uni-jni/arm64-v8a/libmain_rus.so" ] || { echo "android-uni: в APK rus нет libmain.so" >&2; exit 1; }
  for v in eng:game/System rus:build/game-rus/System; do
    core=$(find "$ROOT/${v#*:}" -maxdepth 1 -type f -iname core.dll 2>/dev/null | head -1)
    [ -n "$core" ] || continue
    python3 -c 'import sys, zlib; print("%s %08x" % (sys.argv[1], zlib.crc32(open(sys.argv[2], "rb").read()) & 0xffffffff))' \
      "${v%%:*}" "$core" >> "$ROOT/build/uni-assets/variants.txt"
  done
  cat "$ROOT/build/uni-assets/variants.txt" 2>/dev/null || echo "android-uni: нет Core.dll — версия игры только по дате PE"
  PLAT=android
  VARIANT=uni
fi
if [ "$PLAT" = android ] || [ "$PLAT" = android-rus ]; then
  FLAVOR=Eng
  [ "$VARIANT" = rus ] && FLAVOR=Rus
  [ "$VARIANT" = uni ] && FLAVOR=Uni
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
  "$GRADLE" -p "$ROOT/android" "assemble${FLAVOR}Release" "$@"
  LOWER=$(echo "$FLAVOR" | tr 'A-Z' 'a-z')
  OUT=$(ls "$ROOT/android/app/build/outputs/apk/$LOWER/release/"*.apk | head -1)
  cp "$OUT" "$ROOT/build/webheadNX-$LOWER.apk"
  echo "APK: $ROOT/build/webheadNX-$LOWER.apk"
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
  *) echo "usage: $0 mac|switch|mac-rus|switch-rus|switch-pgo|android|android-rus|android-uni [targets]" >&2; exit 2 ;;
esac
if [ $# -gt 0 ]; then
  cmake --build "$ROOT/build/$PLAT" $CLEAN --target "$@"
else
  cmake --build "$ROOT/build/$PLAT" $CLEAN
fi
case "$PLAT" in
  switch*) printf '%s\n' "$WANT" > "$STAMP" ;;
esac
