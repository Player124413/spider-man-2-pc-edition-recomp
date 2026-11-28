// HLE COM-объекты (DirectDraw, Direct3D 8, DirectInput 8): объект живёт в гостевой памяти
// ([+0] vtable, [+4] номер в таблице хоста), vtable — трамплины на HLE-методы. Метод вызывается как stdcall:
// ARG(0) — this, дальше аргументы; снимает 1 + n аргументов (RET(1 + n, hr)).
#pragma once
#include "runtime/hle/hle.h"

#ifdef __cplusplus
extern "C" {
#endif

#define S_OK 0u
#define E_NOINTERFACE 0x80004002u
#define E_NOTIMPL 0x80004001u
#define E_OUTOFMEMORY 0x8007000Eu

// vtable из n методов (NULL — ловушка «метод не реализован»); name — для сообщений.
u32 com_vtable(const char *name, const guest_fn *methods, int n);
// Новый объект с refcount 1; destroy вызывается при последнем Release.
u32 com_create(u32 vtbl, void *host, void (*destroy)(void *host));
// Однопоточный COM-объект (только поток гостя, без атомиков в AddRef/Release — D3D8).
u32 com_create_st(u32 vtbl, void *host, void (*destroy)(void *host));
void *com_host(u32 obj);  // NULL — не наш объект
u32 com_addref(u32 obj);
u32 com_release(u32 obj);

// Методы IUnknown, общие для всех объектов
void com_AddRef(CPU *c);
void com_Release(CPU *c);

// GUID гостя равен {d1, d2, d3, d4[8]}
int com_iid_eq(u32 guid, u32 d1, u16 d2, u16 d3, u64 d4_be);
void com_iid_str(u32 guid, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
