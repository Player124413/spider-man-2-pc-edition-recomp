#!/usr/bin/env bash
# Headless-сборка: рекомпилированный код + рантайм + HLE (D3D8/GL/Bink/Win32) без SDL/OpenGL-библиотек.
# Требует уже выполненных `tools/recomp.sh gen` и `tools/recomp.sh build`.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GEN="${GEN:-$ROOT/build/gen}"
LIB="$ROOT/build/lib"
OUT="$ROOT/build/rt-hl"
HLE="$ROOT/build/hle-hl"
BIN="$ROOT/build/hl-bin"
mkdir -p "$OUT" "$HLE" "$BIN"
rm -f "$HLE/headless_run.o"

CFLAGS="-O1 -fPIC -fno-stack-protector -fno-common -DSNX_HEADLESS -I$ROOT/platform/headless/include -I$ROOT"

for f in runtime/rt.c runtime/loader.c runtime/overrides_crt.c runtime/overrides_core.c \
         runtime/overrides_engine.c runtime/overrides_launch.c runtime/ovr_orig_weak.c \
         runtime/script_fix.c runtime/vmem.c runtime/heap.c runtime/process.c runtime/hle/vfs.c; do
  o="$OUT/$(basename "$f" .c).o"
  gcc $CFLAGS -c "$ROOT/$f" -o "$o"
done

# bink_video.c (libavcodec), openal.c, overlay.c, vorbisfile.c —非空 headless-заглушки,
# vfs.c уже собран выше вместе с рантаймом.
for f in runtime/hle/advapi32.c runtime/hle/bink.c runtime/hle/bink_audio.c runtime/hle/com.c \
         runtime/hle/d3d8.c runtime/hle/d3d8_dev.c runtime/hle/d3d8_ff.c runtime/hle/d3d8_ps.c \
         runtime/hle/d3d8_res.c runtime/hle/ddraw.c runtime/hle/dinput8.c runtime/hle/gdi32.c \
         runtime/hle/gl_cs.c runtime/hle/hle.c runtime/hle/kernel32.c runtime/hle/kernel32_file.c \
         runtime/hle/misc.c runtime/hle/shell32.c runtime/hle/user32.c runtime/hle/wsock32.c \
         platform/headless/gl_stubs.c platform/headless/stubs.c; do
  o="$HLE/$(basename "$f" .c).o"
  gcc $CFLAGS -c "$ROOT/$f" -o "$o"
done

gcc $CFLAGS -c "$ROOT/tests/headless/headless_run.c" -o "$BIN/headless_run.o"
gcc -o "$ROOT/build/headless_run" "$BIN/headless_run.o" "$OUT"/*.o "$HLE"/*.o \
  -Wl,--start-group "$LIB"/lib*.a -Wl,--end-group -lm -lpthread
echo "Готово: $ROOT/build/headless_run"
