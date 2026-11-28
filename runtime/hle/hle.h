// Общее для HLE-реализаций Win32: аргументы stdcall, трассировка, строки гостя, объекты ядра.
// Функция hle_<dll>_<Name>(CPU *c) вызывается вместо импорта: [esp] — адрес возврата, аргументы с esp+4;
// stdcall-функция сама снимает адрес возврата и аргументы (RET).
#pragma once
#include <stdio.h>

#include "runtime/process.h"
#include "runtime/rt.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ARG(i) rd32(g_mem, c->esp + 4u + 4u * (u32)(i))
#define RET(n, v)                       \
    do {                                \
        c->eax = (u32)(v);              \
        c->esp += 4u + 4u * (u32)(n);   \
        return;                         \
    } while (0)

// Трассировка вызовов HLE: SNX_HLE_TRACE=1 в окружении.
extern int g_hle_trace;
void hle_trace(CPU *c, const char *name, int nargs);
#define HLE_ENTER(name, n)              \
    do {                                \
        if (g_hle_trace)                \
            hle_trace(c, name, n);      \
    } while (0)
// Неполная реализация: однократное сообщение в лог.
void hle_todo(const char *what);

// Отложенная запись файлов гостя (kernel32_file.c) — на диск; при выходе — сама (atexit), при ловушке — вызвать
void hle_file_flush_all(void);
// размер файла по имени (UTF-16 гостя) без открытия, если он известен (kernel32_file.c): 1 — *size (-1 — нет файла)
int hle_file_size_known(u32 wname, s64 *size);

// Долгие кадры (строка [hitch] в d3d8_present): счётчики с начала работы, кадр берёт разность
typedef struct {
    _Atomic unsigned long long reads, read_bytes, read_ns;  // чтения файлов гостя с диска (kernel32_file.c)
    _Atomic unsigned long long links, link_ns;              // сборка программ GL (d3d8_ff.c, поток рендера)
    _Atomic unsigned long long uploads, upload_bytes;       // заливка уровней текстур (d3d8_res.c)
} hle_hitch_counters;
extern hle_hitch_counters g_hitch;
extern void (*hle_frame_hook)(int frame);  // d3d8_present: кадр показан (отметки профайлера)

// Строки гостя
const char *hle_cstr(u32 a);                         // хостовый указатель на ANSI-строку (или "")
size_t hle_wlen(u32 a);                              // длина UTF-16 строки в символах
void hle_w2utf8(u32 w, char *out, size_t cap);       // UTF-16 гостя -> UTF-8 хоста
u32 hle_put_a(u32 dst, u32 cap, const char *s);      // копия с усечением; длина без нуля
u32 hle_put_w(u32 dst, u32 cap, const char *utf8);   // UTF-8 -> UTF-16 гостя; длина без нуля

// Объекты ядра: описатели 0x100, 0x104, ... (как у Windows — кратны 4)
enum { OBJ_NONE, OBJ_THREAD, OBJ_EVENT, OBJ_MUTEX, OBJ_SEMAPHORE, OBJ_FILE, OBJ_FIND, OBJ_MAPPING, OBJ_PROCESS };
typedef struct hle_obj hle_obj;
u32 hle_obj_new(int type, void *data);
void *hle_obj_get(u32 h, int type);  // NULL при неверном описателе/типе
int hle_obj_type(u32 h);
int hle_obj_close(u32 h);
void hle_obj_set_closer(u32 h, void (*closer)(void *));

// Функции HLE-DLL, которые гость находит через GetProcAddress (DLL загружается динамически); таблица
// завершается {NULL}, трамплин создаётся при первом запросе.
typedef struct {
    const char *name;
    guest_fn fn;
    u32 thunk;
} hle_dyn_fn;
u32 hle_dyn_thunk(hle_dyn_fn *table, const char *dll, const char *name);  // 0 — нет такой функции
extern hle_dyn_fn hle_openal_fns[];  // openal.c: OpenAL32.dll / DefOpenAL32.dll

// Ресурс PE: адрес IMAGE_RESOURCE_DATA_ENTRY (HRSRC) или 0; name/type — строка гостя или MAKEINTRESOURCE.
u32 hle_find_resource(u32 hmod, u32 name, u32 type, int wide);
// Данные ресурса (гостевой адрес) по HRSRC
static inline u32 hle_resource_data(u32 hmod, u32 hrsrc) { return hrsrc ? hmod + rd32(g_mem, hrsrc) : 0; }

#define ERROR_SUCCESS 0u
#define ERROR_FILE_NOT_FOUND 2u
#define ERROR_PATH_NOT_FOUND 3u
#define ERROR_ACCESS_DENIED 5u
#define ERROR_INVALID_HANDLE 6u
#define ERROR_NOT_ENOUGH_MEMORY 8u
#define ERROR_NO_MORE_FILES 18u
#define ERROR_INVALID_PARAMETER 87u
#define ERROR_INSUFFICIENT_BUFFER 122u
#define ERROR_MOD_NOT_FOUND 126u
#define ERROR_PROC_NOT_FOUND 127u
#define ERROR_ALREADY_EXISTS 183u
#define ERROR_ENVVAR_NOT_FOUND 203u

#ifdef __cplusplus
}
#endif
