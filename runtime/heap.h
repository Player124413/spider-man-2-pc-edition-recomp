// Кучи гостя (HeapCreate/HeapAlloc/...): блоки в гостевой памяти поверх vmem.
#pragma once
#include "runtime/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HEAP_ZERO_MEMORY 0x08u
#define HEAP_REALLOC_IN_PLACE_ONLY 0x10u

void heap_init(void);
u32 heap_process(void);  // GetProcessHeap
u32 heap_create(u32 flags);
int heap_destroy(u32 heap);
u32 heap_alloc(u32 heap, u32 flags, u32 size);  // 0 при нехватке памяти
u32 heap_realloc(u32 heap, u32 flags, u32 p, u32 size);
int heap_free(u32 heap, u32 p);
u32 heap_size(u32 heap, u32 p);  // запрошенный размер; 0xFFFFFFFF — не блок кучи

#ifdef __cplusplus
}
#endif
