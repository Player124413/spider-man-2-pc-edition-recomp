// Меню игры (GUI UE2: GUIController.ActivePage) для ввода хоста: открыто ли меню, что в фокусе, и запросы
// навигации с геймпада — их исполняет поток гостя родными средствами GUI (SetFocus, курсор GUIController).
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Поток гостя, раз в кадр (d3d8_present): прочитать меню и исполнить запросы. bb_w x bb_h — задний буфер
// (координаты элементов меню — в его пикселях).
void game_menu_frame(unsigned bb_w, unsigned bb_h);
// Поток гостя (d3d8_present, после пост-прохода вывода кадра в окно): построить элементы оверлея меню
// (значки Switch на вкладке GAME, контекстная легенда внизу справа) в координатах окна win_w x win_h.
// dx, dy, dw, dh — прямоугольник, в который выведен задний буфер игры внутри окна.
void game_menu_draw_overlay(int win_w, int win_h, float dx, float dy, float dw, float dh);

typedef struct {
    float x0, y0, x1, y1;  // в пикселях заднего буфера
} game_rect;

enum { GM_NONE, GM_BUTTON, GM_SLIDER, GM_COMBO, GM_EDIT };  // вид элемента в фокусе

// До запуска гостя: язык игры (Language из game.ini: int, rus) — подписи, которые меняет порт.
void game_menu_set_language(const char *lang);
int game_menu_is_rus(void);  // 1 — русская локализация

// Любой поток: открыто ли меню; *focus (если не NULL) — вид элемента в фокусе (GM_*).
int game_menu_open(int *focus);
// Кнопка «назад» на открытой странице (подпись BACK, CANCEL или NO): 1 и её прямоугольник, 0 — нет.
int game_menu_back(game_rect *r);

// Запросы с геймпада (любой поток; исполняются в следующем кадре)
void game_menu_nav(int dir);    // фокус на соседний элемент: 0 вверх, 1 вниз, 2 влево, 3 вправо
void game_menu_tab(int d);      // вкладка настроек: -1 предыдущая, +1 следующая
void game_menu_edit_activate(void);  // открыть экранную клавиатуру (Switch swkbd) для GUIEditBox в фокусе
// Курсор GUI — в точку заднего буфера (касание, щелчок по «назад»); -> номер запроса для game_menu_point_done
unsigned game_menu_point(float x, float y);
int game_menu_point_done(unsigned ticket);  // курсор на месте, и GUI уже видел его там (можно нажимать)
void game_menu_cursor(int visible);          // показать курсор (стики) или спрятать (крестовина, касание)
// Элемент меню, который нажался бы кнопкой A (at_point = 0) или тапом в точку (at_point = 1): поток гостя кладёт
// в g_el_r Bounds элемента в пикселях заднего буфера, чтобы хост нажал ровно по нему. Исполняется в следующем кадре.
void game_menu_element(int at_point, float x, float y);
// Итог: 0 — ещё не исполнено, 1 — элемент найден (x, y — его центр), 2 — под точкой/в фокусе ничего нет.
int game_menu_element_result(float *x, float *y);

// Курсор, поставленный гостем (переход крестовиной): 1 и точка, если изменился с прошлого вызова.
int game_menu_cursor_moved(float *x, float *y);

#ifdef __cplusplus
}
#endif
