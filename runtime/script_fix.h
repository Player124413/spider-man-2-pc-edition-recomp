// Исправления байткода UnrealScript игры в памяти, сразу после загрузки функции (UStruct::Link,
// runtime/overrides_core.c; запасной путь — обход объектов при загрузке карты и в кадрах). Файлы игры не меняются.
#pragma once
#include "runtime/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

// Поток гостя, после UStruct::Link структуры st (функция, состояние или класс).
void script_fix_linked(u32 st);
// Поток гостя: обход всех объектов в поисках неисправленных функций. frame = 0 — загрузка карты (UEngine::Flush),
// каждый раз; 1 — кадр (d3d8_present): раз в 2 с, пока функция ни разу не найдена.
void script_fix_scan(int frame);

// Байткод Shrek.PostLoadGame (SHGame.u) в памяти: s — n байт Script. -> 1 исправлен, 2 уже исправлен, 0 — не
// узнан. Отдельно — для теста на синтетическом байткоде (tests/rt/script_fix_test.c).
int script_fix_post_load_game(u8 *s, u32 n);

#ifdef __cplusplus
}
#endif
