#!/usr/bin/env bash
# Сборка smoke-теста: реальный сгенерированный код + рантайм, без SDL/OpenGL/DLL игры.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OBJ="$ROOT/build/obj"
LIB="$ROOT/build/lib"
RT="$ROOT/build/rt"
OUT="$ROOT/build/smoke"
mkdir -p "$RT" "$OUT"

CFLAGS="-O1 -fPIC -fno-stack-protector -fno-common -I$ROOT"
for f in runtime/rt.c runtime/loader.c runtime/overrides_crt.c runtime/overrides_core.c \
         runtime/overrides_engine.c runtime/overrides_launch.c runtime/ovr_orig_weak.c \
         runtime/script_fix.c runtime/vmem.c runtime/heap.c runtime/process.c runtime/hle/vfs.c; do
  o="$RT/$(basename "$f" .c).o"
  [[ "$o" -nt "$f" ]] || gcc $CFLAGS -c "$ROOT/$f" -o "$o"
done

gcc $CFLAGS -c "$ROOT/tests/smoke/spider_smoke.c" -o "$OUT/spider_smoke.o"
gcc -o "$OUT/spider_smoke" "$OUT/spider_smoke.o" "$RT"/*.o \
  -Wl,--start-group "$LIB"/lib*.a -Wl,--end-group -lm -lpthread

# Каталог пользователя должен существовать: игра пишет туда настройки и сохранения.
mkdir -p "$ROOT/build/userdata-smoke/System"
echo "Готово: $OUT/spider_smoke"
