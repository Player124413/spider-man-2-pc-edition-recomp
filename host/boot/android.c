// Android: JNI для лаунчера (android/app/src/main/java/com/shreknx/game/SnxNative.kt) — экранное управление
// (host/boot/touch.h) и состояние игры. Методы зовёт поток UI Android; touch_* — потокобезопасны.
#include <jni.h>
#include <stdio.h>
#include <unistd.h>

#include "host/boot/touch.h"
#include "platform/window.h"
#include "runtime/hle/hle.h"

#define JNI_FN(name) Java_com_shreknx_game_SnxNative_##name

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

// Частота экрана (режим, выбранный GameActivity под ограничение FPS, или смена частоты системой) и ограничение
JNIEXPORT void JNICALL JNI_FN(display)(JNIEnv *env, jclass cls, jfloat hz, jint fps_cap)
{
    (void)env;
    (void)cls;
    plat_set_display(hz, fps_cap);
}
