// Описание рекомпилированного модуля (генерируется в <module>_table.c) и загрузчик образов.
#pragma once
#include "runtime/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    u32 iat_va;          // слот IAT
    u32 guest_target;    // != 0: адрес экспорта в рекомпилированном модуле
    const char *dll;     // для HLE-импорта
    guest_fn hle;        // HLE-функция (адрес трамплина выделяет рантайм)
} rt_iat_entry;

typedef struct {
    const char *name;  // NULL — экспорт только по ординалу
    u32 addr;
    u32 ordinal;
} rt_export_entry;

typedef struct {
    const char *name;
    u32 base, preferred_base, size;
    u32 crc32, file_size;
    u32 entry;
    int is_dll;
    const rt_func_entry *funcs;
    u32 nfuncs;
    const rt_iat_entry *iat;
    u32 niat;
    const rt_export_entry *exports;  // сначала именованные (по возрастанию имени), затем только-ординальные
    u32 nnamed, nexports;
} rt_module_desc;

// Загружает файл модуля из каталога игры в гостевую память по desc->base, применяет релокации,
// заполняет IAT и регистрирует функции. 0 — успех.
int rt_load_module(const rt_module_desc *desc, const char *game_system_dir);

#ifdef __cplusplus
}
#endif
