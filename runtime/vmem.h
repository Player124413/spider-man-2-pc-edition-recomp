// Виртуальная память гостя: VirtualAlloc/VirtualFree/VirtualQuery поверх окна g_mem.
// Резервирование гранулами 64 КБ, фиксация страницами 4 КБ; зафиксированная память при выдаче нулевая.
// Защита страниц не соблюдается (всё окно — чтение/запись).
#pragma once
#include "runtime/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VM_MEM_COMMIT 0x1000u
#define VM_MEM_RESERVE 0x2000u
#define VM_MEM_DECOMMIT 0x4000u
#define VM_MEM_RELEASE 0x8000u
#define VM_MEM_FREE 0x10000u
#define VM_MEM_PRIVATE 0x20000u
#define VM_MEM_IMAGE 0x1000000u
#define VM_MEM_TOP_DOWN 0x100000u

typedef struct {
    u32 base, alloc_base, alloc_protect, size, state, protect, type;
} vm_info;  // раскладка MEMORY_BASIC_INFORMATION (28 байт)

void vm_init(void);
// Пометить область как занятую образом модуля (VirtualQuery вернёт MEM_IMAGE).
void vm_mark_image(u32 base, u32 size);
// VirtualAlloc: 0 при ошибке.
u32 vm_alloc(u32 addr, u32 size, u32 type, u32 protect);
// VirtualFree: 0 при ошибке.
int vm_free(u32 addr, u32 size, u32 type);
// VirtualQuery: 0, если адрес вне окна.
int vm_query(u32 addr, vm_info *out);
// Обнулить гостевую память и вернуть физические страницы хосту, где это возможно.
void vm_zero(u32 addr, u32 size);

#ifdef __cplusplus
}
#endif
