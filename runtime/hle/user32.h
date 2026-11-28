// Связь user32/gdi32 с хостом (платформенный слой SDL) и между собой.
#pragma once
#include "runtime/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

// Размер «экрана» гостя (GetSystemMetrics, GetDeviceCaps, рабочая область); по умолчанию 1280x720.
void user32_set_screen(int w, int h);
void user32_screen(int *w, int *h);

// Поставить сообщение в очередь потока-владельца окна (события хоста -> гость).
void user32_post(u32 hwnd, u32 msg, u32 wp, u32 lp);
// Синхронный вызов оконной процедуры (SendMessage) на контексте c.
u32 user32_send(CPU *c, u32 hwnd, u32 msg, u32 wp, u32 lp);

int user32_client_size(u32 hwnd, int *w, int *h);  // клиентская область окна; 0 — нет такого окна
u32 user32_focus(void);                            // окно с фокусом ввода (или активное)

// Состояние ввода, которое видит гость: курсор (экранные координаты) и клавиши (GetKeyState).
void user32_set_cursor_pos(int x, int y);
void user32_set_key(u32 vk, int down);
u32 user32_vk_to_char(u32 vk, int shift);  // символ WM_CHAR для раскладки US; 0 — нет символа

// Вызывается перед выборкой сообщений (PeekMessage/GetMessage): хост перекладывает свои события в очереди.
extern void (*user32_pump_hook)(void);
// Модальный диалог (окна на экране нет): заголовок и тексты элементов через перевод строки (после
// WM_INITDIALOG). Возвращает id кнопки, которую «нажать» (WM_COMMAND; диалог не закрылся сам — EndDialog с этим
// id), или 0 — ждать EndDialog из процедуры диалога. Поток гостя.
extern u32 (*user32_modal_hook)(const char *title, const char *texts);

// gdi32
u32 gdi32_window_dc(u32 hwnd);  // DC окна (0 — экран); один на окно, ReleaseDC его не удаляет
u32 gdi32_stock(int i);         // GetStockObject
// битмап: биты в памяти гостя (h > 0 — строки снизу вверх); 0 — не битмап
int gdi32_bitmap(u32 hbm, u32 *bits, s32 *w, s32 *h, u32 *bpp, u32 *stride);

#ifdef __cplusplus
}
#endif
