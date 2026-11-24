// Внутренний интерфейс рантайма (для HLE, загрузчика, хоста).
#pragma once
#include "runtime/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RT_WINDOW 0x40000000u          // рабочее окно гостевой памяти (см. recomp/addrmap.json)
#define RT_THUNK_BASE 0x00100000u      // адреса трамплинов HLE-импортов
#define RT_THUNK_SIZE 16u

// Стеки хостовых потоков гостя (гостевая рекурсия = рекурсия хоста; загрузка уровня и катсцена — < 1 МБ).
// На mac резерв ленивый и ничего не стоит, на Switch стек сразу занимает физическую память.
#ifdef __SWITCH__
#define RT_HOST_STACK_MAIN (32u << 20)
#define RT_HOST_STACK_THREAD (8u << 20)
#else
#define RT_HOST_STACK_MAIN (512u << 20)
#define RT_HOST_STACK_THREAD (64u << 20)
#endif

// Switch: перенести текущий поток на ядро (0 — события SDL, 1 — главный поток гостя, 2 — прочие потоки гостя;
// иначе libnx создаёт все потоки на одном ядре). На маке ядра выбирает ОС: главному потоку гостя и потоку
// рендера — класс QoS «interactive» (иначе их то и дело переносит на энергоэффективные ядра — кадры в разы дольше).
// Android (platform/android/thread.c): главный поток гостя и поток рендера — на «больших» ядрах (по максимальной
// частоте из sysfs) с повышенным приоритетом и в сессии подсказок производительности (ADPF, Android 12+).
#if defined(__SWITCH__) || defined(__ANDROID__)
void rt_platform_thread_core(int core);
#endif
#if defined(__SWITCH__)
// До SDL и потоков: в applet-режиме (памяти меньше окна гостя) — текстовый экран libnx с подсказкой и 0 после
// нажатия кнопки (вызывающий выходит обычным возвратом из main); иначе 1 — можно запускаться.
int rt_platform_preflight(void);
#elif defined(__ANDROID__)
#elif defined(__APPLE__)
#include <pthread/qos.h>
static inline void rt_platform_thread_core(int core)
{
    if (core != 0)
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
}
#else
static inline void rt_platform_thread_core(int core) { (void)core; }
#endif

int rt_mem_init(void);
int rt_platform_mem_init(void);
void rt_register_function(u32 addr, guest_fn fn);
void rt_dump_cpu(const CPU *c);
void rt_dump_trace(int n);
u64 rt_ticks_ns(void);
// Паузируемые монотонные часы для гостя (GetTickCount, QueryPerformanceCounter, timeGetTime):
// время в фоне/сне вычитается из смещения, а одиночный скачок между соседними чтениями ограничен.
u64 rt_guest_ticks_ns(void);
void rt_clock_pause(int on);
void rt_clock_sync(void);  // поглотить скачок после пробуждения ОС (OnResume) без сдвига часов гостя
// время в открытии, размере и закрытии файлов (runtime/hle/kernel32_file.c; доля в строке [core] SaveConfig)
extern u64 rt_file_ns;
// счётчик сборок мусора UObject::CollectGarbage (runtime/overrides_core.c): сброс кешей UClass в menu.c и core
extern u32 rt_gc_epoch;
// активен ли режим 4:3 для страниц меню на широком заднем буфере (runtime/hle/d3d8_ff.c -> runtime/overrides_engine.c)
extern int rt_menu_aspect_4x3;
// размер файла по имени без открытия (FFileManagerWindows::FileSize, overrides_launch.c): 1 — ответ в *size
// (-1 — нет файла), 0 — открыть по-настоящему. Ставит host/boot/main.c (hle_file_size_known).
extern int (*rt_file_size_hook)(u32 wname, s64 *size);

// Перехват ловушек (тесты): если обработчик не возвращается (longjmp), abort не вызывается.
typedef void (*rt_trap_hook_fn)(CPU *c, u32 addr, const char *what);
extern rt_trap_hook_fn rt_trap_hook;

// Подмена счётчика rdtsc (дифф-тесты: детерминированная последовательность вместо времени хоста).
extern u64 (*rt_rdtsc_hook)(void);

// Отладка ABI (SNX_CHECK_ABI=1): после каждого косвенного вызова проверять, что вызванный сохранил
// esi/edi/ebx/ebp (так требует соглашение MSVC); нарушения печатаются (первые 64).
extern int rt_check_abi;

// Выделить адрес трамплина для HLE-функции и зарегистрировать его.
u32 rt_hle_thunk(const char *dll, guest_fn fn);

#ifdef __cplusplus
}
#endif
