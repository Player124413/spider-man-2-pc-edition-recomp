#!/bin/sh
# Каталог русской версии для порта: build/game-rus — копия установленной русской игры (клоны APFS, места не
# занимают), в которой защищённый SafeDisc System/Game.exe заменён совместимым распакованным английским
# game.exe из локальной копии (у русских DLL те же экспорты; .text защищённого Game.exe зашифрован).
#   tools/make_rus.sh <каталог русской игры, напр. Shrek2RUS> <путь к распакованному game.exe>
# Дальше: recomp gen --game build/game-rus/System --out build/gen-rus; tools/build.sh mac-rus|switch-rus.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
src=${1:?usage: $0 <russian game dir> <unpacked game.exe>}
exe=${2:?usage: $0 <russian game dir> <unpacked game.exe>}
out="$ROOT/build/game-rus"
[ -f "$src/System/Core.dll" ] || { echo "make_rus: $src/System/Core.dll not found" >&2; exit 1; }
[ -f "$exe" ] || { echo "make_rus: $exe not found" >&2; exit 1; }
rm -rf "$out"
mkdir -p "$ROOT/build"
cp -Rc "$src" "$out" 2>/dev/null || cp -R "$src" "$out"  # -c: клоны APFS
find "$out/System" -maxdepth 1 -iname game.exe -exec rm -f {} +
cp "$exe" "$out/System/game.exe"
echo "make_rus: $out ready"
