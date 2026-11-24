#!/bin/sh
# Обёртка компилятора для SNX_PGO=use (cmake/gen.cmake): с выносом холодного (-freorder-blocks-and-partition).
# Функции со switch (t) помечаются SNX_NO_JT (optimize("no-jump-tables")) в recomp/emit.py, но обёртка остаётся
# страховкой на случай, если GCC aarch64 разнесёт метки таблицы переходов switch по разным секциям.
"$@" 2>/dev/null || exec "$@" -fno-reorder-blocks-and-partition
