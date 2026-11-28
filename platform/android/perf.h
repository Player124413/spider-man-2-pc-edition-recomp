// Android: подсказки производительности (ADPF) для главного потока гостя и потока рендера (platform/android/thread.c).
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void android_perf_target(uint64_t ns);    // желаемая длительность кадра (1/60 с или по ограничению FPS)
void android_perf_frame(uint64_t work_ns);  // длительность работы прошедшего кадра (без ожиданий vsync/ограничителя)

#ifdef __cplusplus
}
#endif
