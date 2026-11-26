package com.webheadnx.game

// JNI к libmain.so (host/boot/android.c -> host/boot/touch.h). Только в процессе игры (:game), после загрузки
// библиотек SDLActivity.
object SnxNative {
    // биты кнопок — номера SDL_GameControllerButton; 15 — ZL, 16 — ZR
    const val A = 0
    const val B = 1
    const val X = 2
    const val Y = 3
    const val MINUS = 4      // BACK
    const val PLUS = 6       // START
    const val LSTICK = 7
    const val RSTICK = 8
    const val L = 9
    const val R = 10
    const val DPAD_UP = 11
    const val DPAD_DOWN = 12
    const val DPAD_LEFT = 13
    const val DPAD_RIGHT = 14
    const val ZL = 15
    const val ZR = 16

    const val STATE_MENU = 1
    const val STATE_READY = 2
    const val STATE_MENU_PASS = 4  // главное меню на Bink-заставке: касания идут в SDL, кнопки остаются
    const val STATE_IN_GAME = 8    // в мире есть живой герой: палец по пустому месту — камера, вне игры — мышь

    @JvmStatic external fun pad(buttons: Int, lx: Float, ly: Float, rx: Float, ry: Float)
    @JvmStatic external fun key(vk: Int, down: Boolean)
    @JvmStatic external fun state(): Int
    // главное меню на Bink-заставке: касания идут в обработчик кликов, экранные кнопки прячутся
    @JvmStatic external fun menuMode(on: Boolean)
    // тап по пустому месту в главном меню: координаты в долях экрана (SDL-поверхность не видит касание
    // под overlay-_view, поэтому клик передаётся напрямую в гостевой обработчик мыши)
    @JvmStatic external fun menuTap(x: Float, y: Float, down: Boolean)
    // виртуальная мышь: -1 — только перемещение курсора, 2 — щелчок по точке, 1/0 — зажать/отпустить ЛКМ;
    // координаты в долях окна
    @JvmStatic external fun mouse(x: Float, y: Float, down: Int)
    // камера пальцем: смещение пальца по экрану в пикселях (палец ведёт камеру, как мышь в ПК-версии)
    @JvmStatic external fun look(dx: Float, dy: Float)
    // чувствительность камеры правого стика: действует сразу, без перезапуска
    @JvmStatic external fun lookSens(v: Float)
    // камера живьём: мёртвая зона, скорость поворота, зеркало осей (правый стик и палец-камера)
    @JvmStatic external fun camSet(dead: Float, speed: Float, invX: Boolean, invY: Boolean)
    // частота экрана и ограничение FPS: равны — кадры отмеряет vsync (platform/window.c plat_set_display)
    @JvmStatic external fun display(hz: Float, fpsCap: Int)
    // сбросить файлы гостя и завершить процесс :game (выход кнопкой «Назад»)
    @JvmStatic external fun exitNow()
}
