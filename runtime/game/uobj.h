// Отражение объектов UnrealScript гостя по его памяти (только чтение и запись полей, без вызовов гостя):
// имена (FName::Names), список объектов (UObject::GObjObjects), классы и свойства по имени.
#pragma once
#include "runtime/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

// Раскладка UObject этой сборки UE2: Outer +0x18, Name (индекс FName) +0x20, Class +0x24.
#define UO_OUTER 0x18u
#define UO_NAME 0x20u
#define UO_CLASS 0x24u

int uo_init(void);                         // адреса таблиц Core; 0 — Core ещё не загружен
const char *uo_name(u32 obj);              // имя объекта (статический буфер на поток), "" для 0
int uo_name_is(u32 obj, const char *name); // без учёта регистра
u32 uo_class(u32 obj);
u32 uo_outer(u32 obj);
int uo_is_a(u32 obj, const char *class_name);  // класс объекта или его предок
u32 uo_find_class(const char *name);       // объект-класс по имени; 0 — не загружен
int uo_is_a_class(u32 obj, u32 cls);       // то же, что uo_is_a, по указателю на класс
u32 uo_count(void);                        // размер GObjObjects (с пустыми ячейками)
u32 uo_at(u32 i);                          // объект или 0

// Свойство класса (ищется и в предках): смещение в объекте и маска для bool; 0 — нет такого.
typedef struct {
    u32 prop, offset, bitmask;
} uo_prop;
int uo_find_prop(u32 cls, const char *name, uo_prop *out);
u32 uo_get_u32(u32 obj, const char *name);       // 0, если свойства нет
int uo_get_bool(u32 obj, const char *name);      // -1, если свойства нет
int uo_set_bool(u32 obj, const char *name, int v);
// Вызовы гостя — только в потоке гостя изнутри вызова HLE (d3d8_present): регистры гостя сохраняются.
// Функция UnrealScript объекта по имени (FindFunction + ProcessEvent): parms — параметры по 4 байта в порядке
// объявления (объект, int, bool, float), *ret — слово после них (результат). 0 — нет такой функции.
int uo_call(u32 obj, const char *func, const u32 *parms, int n, u32 *ret);
// Строковое свойство (FString) — через FString::operator= игры (память — её распределителем); s — UTF-8
int uo_set_str(u32 obj, const char *name, const char *s);
void uo_dump(u32 obj);                     // свойства объекта в stderr (отладка)
// SNX_UO_DUMP=<с> (host/boot/main.c): GUIController, стек и дерево открытой страницы меню (SNX_UO_ALL=1 — со
// всеми свойствами); SNX_UO_CLASS=<класс> — свойства всех объектов класса. Поток гостя.
void uo_explore(void);

#ifdef __cplusplus
}
#endif
