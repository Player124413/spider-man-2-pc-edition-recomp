// Управление состоянием HLE OpenAL из хоста (пауза при потере фокуса/сне и завершение работы).
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Приостановить (paused != 0) или возобновить (paused == 0) все активные контексты/устройства OpenAL. Потокобезопасно.
void openal_pause(int paused);
// Освободить все открытые контексты и устройства OpenAL при завершении работы. Потокобезопасно и идемпотентно.
void openal_shutdown(void);

#ifdef __cplusplus
}
#endif
