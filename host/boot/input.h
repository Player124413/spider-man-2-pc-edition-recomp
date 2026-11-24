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
