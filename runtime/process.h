// Процесс гостя: модули, потоки (TEB/стек/CPU), PEB, вызовы гостевых функций из хоста.
#pragma once
#include "runtime/loader.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RT_PEB 0x00010000u
#define RT_TEB_BASE 0x00011000u
#define RT_MAX_THREADS 200u
#define RT_RET_SENTINEL 0x000FFFF0u  // адрес возврата в хост для rt_guest_call
#define RT_STACK_DEFAULT 0x100000u

// TEB (подмножество NT_TIB/TEB x86)
#define TEB_SEH 0x00u
#define TEB_STACK_BASE 0x04u
#define TEB_STACK_LIMIT 0x08u
#define TEB_SELF 0x18u
#define TEB_PID 0x20u
#define TEB_TID 0x24u
#define TEB_TLS_PTR 0x2Cu
#define TEB_PEB 0x30u
#define TEB_LAST_ERROR 0x34u
#define TEB_TLS_SLOTS 0xE10u
#define RT_TLS_SLOTS 64u

typedef struct GuestThread {
    CPU cpu;
    u32 teb, tid, stack_lo, stack_hi;
    u32 start, param;  // CreateThread
    u32 handle;        // описатель объекта-потока (kernel32)
    int finished;
    u32 exit_code;
    int suspended;
    void *exit_jmp;  // jmp_buf хостового потока для ExitThread
    void *sync;      // состояние ожидания (kernel32)
} GuestThread;

typedef struct {
    const rt_module_desc *desc;
    int attached;  // DllMain(DLL_PROCESS_ATTACH) вызван
    int refs;
} rt_module;

// Инициализация: память, образы всех модулей (без DllMain), PEB, главный поток, файловая система.
// game_root — каталог игры на хосте (с System/, только чтение), user_root — каталог для записи (оверлей),
// guest_root — путь игры для гостя (напр. "C:\\Shrek2").
int rt_process_init(const rt_module_desc *const *mods, const char *game_root, const char *user_root,
                    const char *guest_root, const char *cmdline);
// DllMain(DLL_PROCESS_ATTACH) модуля и (сначала) модулей, из которых он импортирует. 0 — успех.
int rt_module_attach(rt_module *m);
rt_module *rt_module_by_base(u32 base);
rt_module *rt_module_by_name(const char *name);  // без учёта регистра, ".dll" можно опустить
rt_module *rt_module_main(void);                // webhead.exe
u32 rt_module_export(const rt_module *m, const char *name, u32 ordinal);  // 0 — нет
void rt_symbolize(u32 addr, char *out, size_t cap);  // "addr Module.dll!Export+0x12"
void rt_backtrace(const CPU *c);                     // цепочка ebp и скан стека в stderr
int rt_looks_like_ret(u32 a);  // похоже на адрес возврата: внутри модуля, перед ним — call

GuestThread *rt_thread_current(void);
GuestThread *rt_thread_create(u32 stack_size);  // TEB и стек; хостовый поток запускает вызывающий
void rt_thread_bind(GuestThread *t);            // сделать текущим для этого хостового потока
void rt_thread_release(GuestThread *t);

// Вызов гостевой функции: аргументы кладутся справа налево, адрес возврата — RT_RET_SENTINEL;
// esp восстанавливается (подходит и для stdcall, и для cdecl). Возвращает eax.
u32 rt_guest_call(CPU *c, u32 fn, int nargs, const u32 *args);

// Строки окружения процесса (в гостевой куче)
u32 rt_cmdline_a(void);
u32 rt_cmdline_w(void);
const char *rt_game_root(void);
const char *rt_guest_root(void);

void rt_set_last_error(u32 e);
u32 rt_get_last_error(void);

#ifdef __cplusplus
}
#endif
