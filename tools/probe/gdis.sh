#!/bin/sh
# gdis.sh <module dir in build/gen> <addr hex> [n] — инструкции из комментариев сгенерированного C
mod=$1; a=$2; n=${3:-60}
f=$(grep -l "/\* $a: " build/${GEN:-gen}/$mod/*.c | head -1)
[ -z "$f" ] && { echo "not found"; exit 1; }
l=$(grep -n "/\* $a: " "$f" | head -1 | cut -d: -f1)
tail -n +$l "$f" | grep -o "^/\* [0-9a-f]*: .* \*/\|^void f_[0-9a-f]*\|^}" | head -$n | sed 's|^/\* ||; s| \*/$||'
