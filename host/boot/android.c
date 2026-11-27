// Android: JNI для лаунчера (android/app/src/main/java/com/webheadnx/game/SnxNative.kt) — экранное управление
// (host/boot/touch.h) и состояние игры. Методы зовёт поток UI Android; touch_* — потокобезопасны.
#include <jni.h>
#include <stdio.h>
#include <unistd.h>

#include "host/boot/input.h"
#include "host/boot/touch.h"
#include "platform/window.h"
#include "runtime/hle/d3d8.h"
#include "runtime/hle/hle.h"

#define JNI_FN(name) Java_com_webheadnx_game_SnxNative_##name

JNIEXPORT void JNICALL JNI_FN(pad)(JNIEnv *env, jclass cls, jint buttons, jfloat lx, jfloat ly, jfloat rx, jfloat ry)
{
    (void)env;
    (void)cls;
    touch_pad((uint32_t)buttons, lx, ly, rx, ry);
}

JNIEXPORT void JNICALL JNI_FN(key)(JNIEnv *env, jclass cls, jint vk, jboolean down)
{
    (void)env;
    (void)cls;
    touch_key(vk, down ? 1 : 0);
}

// Выход из игры кнопкой «Назад» (лаунчер спросил подтверждение): записанное гостем — на диск, процесс — сразу.
// Без этого SDLActivity.onDestroy ждал бы конца потока SDL (игра на SDL_QUIT может не выйти — зависание UI).
JNIEXPORT void JNICALL JNI_FN(exitNow)(JNIEnv *env, jclass cls)
{
    (void)env;
    (void)cls;
    hle_file_flush_all();
    fflush(NULL);
    _exit(0);
}

JNIEXPORT jint JNICALL JNI_FN(state)(JNIEnv *env, jclass cls)
{
    (void)env;
    (void)cls;
    return touch_state();
}

// Главное меню на Bink-заставке: движок не считает его GUI-меню, поэтому лаунчер включает принудительный
// режим касаний-мыши. В нём экранные кнопки прячутся, а тап по пункту меню уходит в click_at().
JNIEXPORT void JNICALL JNI_FN(menuMode)(JNIEnv *env, jclass cls, jboolean on)
{
    (void)env;
    (void)cls;
    touch_menu_override(on ? 1 : 0);
}

// Тап по пункту главного меню из overlay-View: экранное управление перехватывает MotionEvent,
// поэтому SDL-поверхность его не получает — передаём координаты прямо в обработчик кликов.
JNIEXPORT void JNICALL JNI_FN(mouse)(JNIEnv *env, jclass cls, jfloat u, jfloat v, jint down)
{
    (void)env;
    (void)cls;
    touch_mouse(u, v, (int)down);
}

JNIEXPORT void JNICALL JNI_FN(menuTap)(JNIEnv *env, jclass cls, jfloat u, jfloat v, jboolean down)
{
    (void)env;
    (void)cls;
    touch_menu_tap(u, v, down ? 1 : 0);
}

// Камера пальцем: смещение пальца в долях высоты экрана — двигает камеру так же, как мышь в оригинале;
// в отсчёты мыши его переводит cam_tick по измеренному отклику камеры игры
JNIEXPORT void JNICALL JNI_FN(look)(JNIEnv *env, jclass cls, jfloat dx, jfloat dy)
{
    (void)env;
    (void)cls;
    touch_look(dx, dy);
}

// Камера пальцем как виртуальный правый стик: значение оси (-1..1) прямо с отклонения пальца (без накоплений)
JNIEXPORT void JNICALL JNI_FN(camAxis)(JNIEnv *env, jclass cls, jfloat vx, jfloat vy)
{
    (void)env;
    (void)cls;
    touch_cam_axis(vx, vy);
}

// Режим камеры пальцем: 0 — виртуальный правый стик под пальцем, 1 — жест «градусы», 2 — отсчёты мыши
JNIEXPORT void JNICALL JNI_FN(camTouch)(JNIEnv *env, jclass cls, jint mode)
{
    (void)env;
    (void)cls;
    input_set_cam_touch((int)mode);
}

// «Умная камера»: градусов поворота на полную высоту экрана (ползунок в шестерёнке)
JNIEXPORT void JNICALL JNI_FN(camSens)(JNIEnv *env, jclass cls, jfloat v)
{
    (void)env;
    (void)cls;
    input_set_cam_sens(v);
}

// Чувствительность камеры правого стика: ползунок в шестерёнке действует сразу, без перезапуска игры
JNIEXPORT void JNICALL JNI_FN(lookSens)(JNIEnv *env, jclass cls, jfloat v)
{
    (void)env;
    (void)cls;
    input_set_look_sens(v);
}

// Камера живьём: мёртвая зона, скорость поворота, зеркало осей (правый стик и палец-камера)
JNIEXPORT void JNICALL JNI_FN(camSet)(JNIEnv *env, jclass cls, jfloat dead, jfloat speed, jboolean inv_x,
                                     jboolean inv_y)
{
    (void)env;
    (void)cls;
    input_set_camera(0.0f, dead, speed, inv_x ? 1 : 0, inv_y ? 1 : 0);
}

// Частота экрана (режим, выбранный GameActivity под ограничение FPS, или смена частоты системой) и ограничение
JNIEXPORT void JNICALL JNI_FN(display)(JNIEnv *env, jclass cls, jfloat hz, jint fps_cap)
{
    (void)env;
    (void)cls;
    plat_set_display(hz, fps_cap);
}
