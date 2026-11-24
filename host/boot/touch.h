// Виртуальный геймпад сенсорного экрана (Android: экранные кнопки и стики рисует и разбирает лаунчер,
// android/…/TouchControlsView.kt, состояние приходит через JNI, host/boot/android.c). Вызывается из любого потока;
// применяется в input_tick (главный поток) — так же, как физический геймпад: кнопки по раскладке Snx.ini и
// User.ini, в меню игры — навигация.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Кнопки — биты номеров кнопок SDL_GameControllerButton (A=0 … DPadRight=14) и 15 — ZL, 16 — ZR.
// Оси — -1..1 (вниз и вправо — плюс), без мёртвой зоны (её учитывает input.c).
void touch_pad(uint32_t buttons, float lx, float ly, float rx, float ry);
// Смещение камеры пальцем по свободной части экрана — в пикселях экрана (переводится в мышь DirectInput).
void touch_look(float dx, float dy);
// Клавиша Windows (VK_*) с экранной кнопки, назначенной на клавишу (зелья 1–9 и т. п.).
void touch_key(int vk, int down);
// Состояние для лаунчера: бит 0 — открыто меню игры (экранное управление прячется, касания — меню),
// бит 1 — игра показала первый кадр.
int touch_state(void);
// Принудительный «режим меню» для главного меню на Bink-заставке: движок его не считает GUI-меню
// (GUIController ActivePage пуст), поэтому касания надо передавать в обработчик кликов мыши принудительно.
void touch_menu_override(int on);

#ifdef __cplusplus
}
#endif
