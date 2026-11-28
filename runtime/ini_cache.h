// Кеш ini движка (FConfigCacheIni, game.exe) в памяти гостя: раскладка для runtime/overrides_core.c и
// runtime/overrides_launch.c. TMap UE2 — массив пар {HashNext, ключ FString, значение}, затем Hash и HashCount;
// FString — TArray<TCHAR> {Data, Num, Max}, Num 0 — пустая строка.
// FConfigCacheIni: +0x04 пары файлов (по 0x30: +0x04 имя, +0x10 пары секций, +0x24 Dirty), +0x18 SystemIni,
// +0x24 UserIni. Секция — по 0x24 (+0x04 имя, +0x10 пары); ключ и значение — по 0x1C (+0x04 ключ, +0x10 значение).
#pragma once
#include <stdlib.h>

#include "runtime/abi.h"

#define TARRAY_NUM 4u
#define CFG_FILES 0x04u
#define CFG_FILE_SIZE 0x30u
#define CFG_FILE_SECS 0x10u
#define CFG_SEC_SIZE 0x24u
#define CFG_SEC_PAIRS 0x10u
#define CFG_KV_SIZE 0x1Cu
#define CFG_SYSTEM_INI 0x18u
#define CFG_USER_INI 0x24u

static inline u32 fstr_data(u32 fs) { return rd32(g_mem, fs + TARRAY_NUM) ? rd32(g_mem, fs) : 0; }

static inline int cfg_check(void)  // SNX_CFG_CHECK=1: сверка с оригиналом
{
    static int v = -1;
    if (v < 0)
        v = getenv("SNX_CFG_CHECK") != NULL;
    return v;
}
