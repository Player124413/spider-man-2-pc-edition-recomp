#!/bin/sh
# Исходники библиотек для Android-сборки (cmake/android.cmake) — в build/android-deps:
#   SDL 2.32.10, openal-soft 1.25.2, libogg 1.3.6, libvorbis 1.3.7, FreeType 2.14.3.
# tools/android_deps.sh --ffmpeg — ещё и FFmpeg 7.1.5 (статически, только декодер видео Bink) для arm64-v8a: нужен
# ANDROID_NDK_HOME (или ANDROID_HOME с ndk/27.*). Без FFmpeg ролики идут чёрным кадром со звуком.
# Повторный запуск ничего не качает заново (удали каталог библиотеки, чтобы обновить).
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEPS=${SNX_ANDROID_DEPS:-$ROOT/build/android-deps}
mkdir -p "$DEPS"

fetch() {  # fetch <каталог> <github org/repo> <тег>
  [ -d "$DEPS/$1" ] && return 0
  echo "== $1 ($3)"
  tmp="$DEPS/.tmp-$1"
  rm -rf "$tmp" && mkdir -p "$tmp"
  curl -fsSL "https://codeload.github.com/$2/tar.gz/refs/tags/$3" | tar xz -C "$tmp"
  mv "$tmp"/* "$DEPS/$1"
  rm -rf "$tmp"
}

fetch SDL libsdl-org/SDL release-2.32.10
fetch openal-soft kcat/openal-soft 1.25.2
fetch ogg xiph/ogg v1.3.6
fetch vorbis xiph/vorbis v1.3.7
fetch freetype freetype/freetype VER-2-14-3

if [ "${1:-}" = --ffmpeg ] && [ ! -f "$DEPS/ffmpeg/arm64-v8a/lib/libavcodec.a" ]; then
  fetch ffmpeg-src FFmpeg/FFmpeg n7.1.5
  NDK=${ANDROID_NDK_HOME:-}
  if [ -z "$NDK" ] && [ -n "${ANDROID_HOME:-}" ]; then
    NDK=$(ls -d "$ANDROID_HOME"/ndk/27.* 2>/dev/null | sort | tail -1)
  fi
  [ -n "$NDK" ] && [ -d "$NDK" ] || { echo "нужен ANDROID_NDK_HOME (NDK 27)" >&2; exit 1; }
  case "$(uname -s)" in Darwin) HOST=darwin-x86_64 ;; *) HOST=linux-x86_64 ;; esac
  TC="$NDK/toolchains/llvm/prebuilt/$HOST/bin"
  API=24
  OUT="$DEPS/ffmpeg/arm64-v8a"
  B="$DEPS/ffmpeg-build"
  rm -rf "$B" && mkdir -p "$B"
  echo "== ffmpeg (bink) -> $OUT"
  (cd "$B" && "$DEPS/ffmpeg-src/configure" --prefix="$OUT" --target-os=android --arch=aarch64 --enable-cross-compile \
      --cc="$TC/aarch64-linux-android$API-clang" --cxx="$TC/aarch64-linux-android$API-clang++" \
      --ar="$TC/llvm-ar" --ranlib="$TC/llvm-ranlib" --strip="$TC/llvm-strip" --nm="$TC/llvm-nm" \
      --enable-static --disable-shared --disable-programs --disable-doc --disable-everything \
      --disable-avdevice --disable-avformat --disable-swscale --disable-swresample --disable-avfilter \
      --disable-network --disable-iconv --disable-zlib --disable-bzlib --disable-lzma --disable-vulkan \
      --enable-decoder=bink --enable-pic --enable-neon --extra-cflags="-O3 -fPIC" >/dev/null
   make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" >/dev/null && make install >/dev/null)
  rm -rf "$B"
fi
echo "библиотеки Android: $DEPS"
