// Android: JNI для лаунчера (android/app/src/main/java/com/webheadnx/game/SnxNative.kt) — экранное управление
// (host/boot/touch.h) и состояние игры. Методы зовёт поток UI Android; touch_* — потокобезопасны.
#include <jni.h>
#include <stdio.h>
#include <unistd.h>

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

JNIEXPORT void JNICALL JNI_FN(look)(JNIEnv *env, jclass cls, jfloat dx, jfloat dy)
{
    (void)env;
    (void)cls;
    touch_look(dx, dy);
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

// Диагностика кадра поверх картинки: цвет заливки, туман, TextureFactor и первые отрисовки кадра. Пишет поток
// рендера, показывает поток гостя (d3d8_present) — вызов безопасен в любой момент.
JNIEXPORT void JNICALL JNI_FN(diag)(JNIEnv *env, jclass cls, jboolean on)
{
    (void)env;
    (void)cls;
    d3d8_set_diag(on ? 1 : 0);
}

// «Проверка неба»: показ вариантов отрисовки (туман, texgen, TextureFactor, гамма) с именем в углу экрана и
// замерами в лог — в журнале видно, на каком варианте цвет неба меняется
JNIEXPORT void JNICALL JNI_FN(skyTest)(JNIEnv *env, jclass cls)
{
    (void)env;
    (void)cls;
    d3d8_sky_test(3);
}

// Туман игры (D3DRS_FOGENABLE): по умолчанию выключен — он красит небо ровным цветом тумана
JNIEXPORT void JNICALL JNI_FN(fog)(JNIEnv *env, jclass cls, jboolean on)
{
    (void)env;
    (void)cls;
    d3d8_set_fog(on ? 1 : 0);
}

// Частота экрана (режим, выбранный GameActivity под ограничение FPS, или смена частоты системой) и ограничение
JNIEXPORT void JNICALL JNI_FN(display)(JNIEnv *env, jclass cls, jfloat hz, jint fps_cap)
{
    (void)env;
    (void)cls;
    plat_set_display(hz, fps_cap);
}
