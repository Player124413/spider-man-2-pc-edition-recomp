// Ввод хоста (SDL) -> гость; вызывается в главном потоке для каждого события (plat_on_event).
#pragma once

void input_event(const void *sdl_event);
// главный поток, до цикла событий: настройки порта и раскладка геймпада <user_dir>/System/Snx.ini (нет файла —
// пишется по умолчанию)
void input_init(const char *user_dir);
// главный поток, каждый проход цикла: стики геймпада -> клавиши и мышь
void input_tick(void);
// Вибрация геймпада (SDL_GameControllerRumble; безопасно из любого потока, учитывает Rumble в Snx.ini)
void input_rumble(unsigned short low_freq, unsigned short high_freq, unsigned int duration_ms);
int input_rumble_enabled(void);
void input_set_rumble_enabled(int on);
// Чувствительность камеры правого стика (ползунок в игре действует сразу, без перезапуска)
void input_set_look_sens(float v);
// камера живьём (шестерёнка в игре): чувствительность, мёртвая зона, скорость поворота, зеркало осей (0 — как есть)
// камера: зеркало осей (чувствительность — input_set_look_sens)
void input_set_cam_mirror(int inv_x, int inv_y);
// «держать угол»: камера не возвращается сама (игра любит доворачивать её за спину герою)
void input_set_cam_hold(int on);
// Режим камеры пальцем (шестерёнка): 0 — виртуальный правый стик под пальцем, 1 — жест «градусы»,
// 2 — отсчёты мыши. Действует сразу, без перезапуска.
