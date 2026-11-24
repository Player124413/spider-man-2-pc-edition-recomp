// Окно игры и GL-контекст поверх SDL2 (mac, Switch, Android).
// SDL требует создавать окна и разбирать события в главном потоке (macOS), а гость (и его D3D-устройство)
// исполняется в своём потоке. Поэтому главный поток крутит plat_run(): события SDL и запросы гостя;
// поток гостя просит окно через plat_window_open() и получает GL-контекст текущим у себя.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

int plat_init(void);       // главный поток: SDL (видео, контроллеры); 0 — успех
void plat_run(void);       // главный поток: события и запросы, пока не plat_quit()
void plat_quit(void);      // любой поток

// Любой поток: создать (или изменить) окно w x h с GL-контекстом. 0 — успех.
int plat_window_open(int w, int h, int fullscreen, const char *title);
// Сделать контекст текущим у вызывающего потока (тот, кто рисует), при первом вызове — загрузить функции GL.
int plat_gl_bind(void);
// Отвязать GL-контекст от вызывающего потока (при остановке потока рендера).
void plat_gl_unbind(void);
// Освободить окно/контекст SDL и снять системные хуки (главный поток, после остановки потока рендера).
void plat_shutdown(void);
int plat_window_is_open(void);
void plat_screen_size(int *w, int *h);    // после plat_init: режим экрана (Switch: 1280x720 / 1920x1080)
void plat_drawable_size(int *w, int *h);  // размер кадра окна в пикселях
void plat_swap(void);                     // показать кадр (поток с текущим контекстом)

// Частота экрана и ограничение FPS (Android: GameActivity выбирает режим экрана под ограничение и сообщает частоту;
// любой поток). Частота равна ограничению (60 FPS при 60 Гц) — кадры отмеряет vsync: ровно, без сна ограничителя
// между обновлениями экрана (рывки 8/17/25 мс на 90/120 Гц). Интервал обмена 2 и больше Android не умеет
// (ANativeWindow: 0 или 1), поэтому при 120 Гц — прежний ограничитель.
void plat_set_display(float hz, int fps_cap);
int plat_vsync_paced(void);  // 1 — ограничение держит vsync, сон ограничителя не нужен

// Событие SDL (SDL_Event *) — в главном потоке, для перевода в ввод гостя.
typedef void (*plat_event_fn)(const void *sdl_event);
extern plat_event_fn plat_on_event;
// Каждый проход цикла главного потока (не реже ~200 раз в секунду): непрерывный ввод (стики).
extern void (*plat_on_tick)(void);

// Приложение уходит в фон / возвращается (Android: сворачивание, блокировка экрана). Обработчики зовутся в главном
// потоке; до 8 штук (звук, ввод). Поток рендера на это время отпускает контекст GL и ждёт в plat_swap.
typedef void (*plat_pause_fn)(int paused);
void plat_add_pause_hook(plat_pause_fn fn);
int plat_paused(void);

// События жизненного цикла ОС (главный поток: appletHook на Switch, SDL_WINDOWEVENT на ПК):
// смена фокуса приложения (1 — в фокусе, 0 — в фоне/HOME);
extern void (*plat_on_focus)(int focused);
// пробуждение после сна (Switch: AppletHookType_OnResume);
extern void (*plat_on_resume)(void);
// смена режима экрана (Switch: док <-> портатив, 1920x1080 <-> 1280x720);
extern void (*plat_on_screen_mode)(int w, int h);
// системный запрос выхода (Switch: AppletHookType_OnExitRequest / завершение appletMainLoop).
extern void (*plat_on_exit_request)(void);

// Проверить и сбросить флаг смены режима экрана (OnOperationMode): 1 — режим изменился, новый размер в *w/*h.
int plat_take_screen_mode_changed(int *w, int *h);

#ifdef __cplusplus
}
#endif
