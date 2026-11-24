// Настройки порта поверх game.ini игры — до того, как его прочтёт игра.
#pragma once
#include <stddef.h>

// [section] key=value в ini — 8-битном или UTF-16LE с BOM, как его пишет игра (CRLF как в файле); нет ключа — добавить в секцию, нет секции — в конец.
// 1 — файл изменён, 0 — значение уже такое, -1 — ошибка ввода-вывода.
int boot_ini_set(const char *path, const char *section, const char *key, const char *value);

// Значение ключа (без перевода строки) в out; 1 — ключ есть, 0 — нет ключа или файла.
int boot_ini_get(const char *path, const char *section, const char *key, char *out, size_t cap);

// После vfs_init: <user>/System/game.ini (если его нет — копия game.ini/Default.ini игры, как сделала бы она
// сама) получает ReduceMouseLag=False, UseJoystick=True и, если res_w > 0, полноэкранное разрешение res_w x res_h;
// <user>/System/User.ini (нет — копия DefUser.ini) — привязки геймпада Joy* там, где они пусты.
void boot_config_apply(int res_w, int res_h);

// Размер кадра на телефоне: экран *w x *h -> разрешение рендера. aspect — предел пропорций (SNX_ASPECT: «16:9»,
// «4:3», «21:9» или число ширина/высота; NULL, пусто или «full» — во весь экран): экран шире предела — кадр
// сужается до него (по бокам поля). Затем масштаб scale (0,25–1), чётные размеры, высота не меньше 360.
// Возвращает предел (0 — нет).
float boot_render_size(int *w, int *h, float scale, const char *aspect);
