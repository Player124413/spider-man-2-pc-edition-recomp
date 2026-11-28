// Платформенные хуки (Switch / хост): режим ускорения CPU FastLoad, понижение приоритета фоновых потоков.
// На хосте (macOS/Linux) — пустые inline-заглушки, чтобы цели без platform/window.c (тесты snx_runtime)
// линковались без дополнительных зависимостей.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __SWITCH__
// Включить/выключить ускорение CPU (ApmCpuBoostMode_FastLoad: 1785 МГц на время загрузок/сохранений).
// Поддерживает вложенные вызовы по счётчику ссылок; безопасно вызывать из любого потока.
void rt_platform_cpu_boost(int on);
// Принудительно вернуть обычный режим CPU (выход, ловушка, аварийное завершение).
void rt_platform_cpu_boost_reset(void);
// Привязать текущий фоновый поток (ввод-вывод, подкачка) к ядру 0 с пониженным приоритетом.
void rt_platform_thread_background(void);
#else
static inline void rt_platform_cpu_boost(int on) { (void)on; }
static inline void rt_platform_cpu_boost_reset(void) {}
static inline void rt_platform_thread_background(void) {}
#endif

#ifdef __cplusplus
}
#endif
