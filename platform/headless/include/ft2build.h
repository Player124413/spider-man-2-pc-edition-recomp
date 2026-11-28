/* Заглушка FreeType для headless-сборки: реальный шрифт не нужен, GDI падает
 * назад на моноширинную аппроксимацию (text_extent в runtime/hle/gdi32.c). */
#pragma once
#define FT_FREETYPE_H "ft_stub.h"
#define FT_OUTLINE_H  "ft_stub.h"
#include "ft_stub.h"
