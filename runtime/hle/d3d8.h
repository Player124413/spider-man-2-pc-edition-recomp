// HLE Direct3D 8: общее для d3d8/ddraw (профиль видеокарты, который видит игра).
#pragma once
#include "runtime/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

u32 d3d8_vidmem_total(void);   // байт видеопамяти (DirectDraw GetAvailableVidMem, GetAvailableTextureMem)
void d3d8_fill_caps(u32 caps);  // D3DCAPS8 (0xD4 байт) по гостевому адресу
// Точка окна (доли его ширины/высоты) -> пиксель заднего буфера с учётом вписывания кадра. 0 — устройства нет.
int d3d8_window_to_target(float u, float v, int *x, int *y);
int d3d8_target_to_window(float x, float y, float *u, float *v);  // обратное
// Число draw-вызовов в последнем кадре: главное меню на Bink рисует 1–3, игра — десятки. Используется хостом,
// чтобы отличить меню от игры, когда GUIController и пешка игрока ещё/уже недоступны.
int d3d8_frame_draws(void);
// BinkCopyToBuffer положил кадр w x h в (x, y) от гостевого адреса dest с шагом pitch: если это залоченная цель
// рендера, при Unlock кадр растягивается на неё с сохранением пропорций (ролики 640x480/800x600 в 1080p).
void d3d8_movie_frame(u32 dest, u32 pitch, u32 x, u32 y, u32 w, u32 h);
// Счётчик кадров поверх картинки (Snx.ini [Overlay] ShowFPS, на ходу — кнопкой)
void d3d8_set_fps_overlay(int on);
int d3d8_fps_overlay(void);
// Диагностика кадра поверх картинки (только для отладки, SNX_DIAG=1): цвет заливки Clear, туман, TextureFactor и
// состояние первых отрисовок кадра — по скриншоту видно, откуда взялся цвет на экране. Пункта в интерфейсе нет.
int d3d8_diag(void);
// Режим 4:3-pillarbox для страниц меню на широком экране (выключен по умолчанию; согласован с 3D-камерой и вводом)
void d3d8_set_menu_aspect(int on);
int d3d8_menu_aspect(void);
// Заставка загрузки до создания устройства: картинка BGRX w x h, строки снизу вверх с шагом stride
void d3d8_splash(const u8 *bgrx, int w, int h, u32 stride);
// Поворот камеры вокруг вертикали в градусах, снятый с матрицы вида игры на последнем Present (хост держит камеру
// от автодоворота за персонажем и калибрует жест пальца). valid = 0 — матрица не похожа на видовую, значение
// брать нельзя (тогда хост работает по умолчанию).

#ifdef __cplusplus
}
#endif
