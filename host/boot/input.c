// Ввод хоста (события SDL, главный поток) -> гость: сообщения Win32 окну с фокусом (WM_KEY*, WM_CHAR,
// WM_MOUSE*), состояние клавиш для GetKeyState, относительные смещения и кнопки мыши для DirectInput.
// Геймпад (Switch: Joy-Con/Pro): в игре — джойстик DirectInput, в меню — мышь (см. раздел «геймпад» ниже и
// раскладку <user>/System/Snx.ini, g_snx_default).
#include <SDL.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#ifdef __SWITCH__
#include <switch.h>
#endif

#include "host/boot/config.h"
#include "host/boot/input.h"
#include "host/boot/touch.h"
#include "runtime/hle/bink.h"
#include "runtime/hle/d3d8.h"
#include "runtime/hle/dinput8.h"
#include "runtime/hle/overlay.h"
#include "runtime/hle/user32.h"
#include "runtime/game/menu.h"
#include "runtime/rt.h"

extern void (*hle_rumble_hook)(u16 low, u16 high, u32 ms);  // runtime/hle/misc.c

enum {
    WM_CLOSE = 0x10, WM_KEYDOWN = 0x100, WM_KEYUP = 0x101, WM_CHAR = 0x102, WM_SYSKEYDOWN = 0x104,
    WM_SYSKEYUP = 0x105, WM_MOUSEMOVE = 0x200, WM_LBUTTONDOWN = 0x201, WM_LBUTTONUP = 0x202,
    WM_RBUTTONDOWN = 0x204, WM_RBUTTONUP = 0x205, WM_MBUTTONDOWN = 0x207, WM_MBUTTONUP = 0x208,
    WM_MOUSEWHEEL = 0x20A,
};

static u32 vk_of(SDL_Scancode sc)
{
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z)
        return 'A' + (u32)(sc - SDL_SCANCODE_A);
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9)
        return '1' + (u32)(sc - SDL_SCANCODE_1);
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12)
        return 0x70 + (u32)(sc - SDL_SCANCODE_F1);
    if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9)
        return 0x61 + (u32)(sc - SDL_SCANCODE_KP_1);
    switch (sc) {
    case SDL_SCANCODE_0: return '0';
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return 0x0D;
    case SDL_SCANCODE_ESCAPE: return 0x1B;
    case SDL_SCANCODE_BACKSPACE: return 0x08;
    case SDL_SCANCODE_TAB: return 0x09;
    case SDL_SCANCODE_SPACE: return 0x20;
    case SDL_SCANCODE_MINUS: return 0xBD;
    case SDL_SCANCODE_EQUALS: return 0xBB;
    case SDL_SCANCODE_LEFTBRACKET: return 0xDB;
    case SDL_SCANCODE_RIGHTBRACKET: return 0xDD;
    case SDL_SCANCODE_BACKSLASH: return 0xDC;
    case SDL_SCANCODE_SEMICOLON: return 0xBA;
    case SDL_SCANCODE_APOSTROPHE: return 0xDE;
    case SDL_SCANCODE_GRAVE: return 0xC0;
    case SDL_SCANCODE_COMMA: return 0xBC;
    case SDL_SCANCODE_PERIOD: return 0xBE;
    case SDL_SCANCODE_SLASH: return 0xBF;
    case SDL_SCANCODE_CAPSLOCK: return 0x14;
    case SDL_SCANCODE_PAUSE: return 0x13;
    case SDL_SCANCODE_INSERT: return 0x2D;
    case SDL_SCANCODE_HOME: return 0x24;
    case SDL_SCANCODE_PAGEUP: return 0x21;
    case SDL_SCANCODE_DELETE: return 0x2E;
    case SDL_SCANCODE_END: return 0x23;
    case SDL_SCANCODE_PAGEDOWN: return 0x22;
    case SDL_SCANCODE_RIGHT: return 0x27;
    case SDL_SCANCODE_LEFT: return 0x25;
    case SDL_SCANCODE_DOWN: return 0x28;
    case SDL_SCANCODE_UP: return 0x26;
    case SDL_SCANCODE_KP_0: return 0x60;
    case SDL_SCANCODE_KP_DIVIDE: return 0x6F;
    case SDL_SCANCODE_KP_MULTIPLY: return 0x6A;
    case SDL_SCANCODE_KP_MINUS: return 0x6D;
    case SDL_SCANCODE_KP_PLUS: return 0x6B;
    case SDL_SCANCODE_KP_PERIOD: return 0x6E;
    case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return 0x11;
    case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return 0x10;
    case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: return 0x12;
    default: return 0;
    }
}

static u32 g_buttons;              // MK_LBUTTON 1, MK_RBUTTON 2, MK_MBUTTON 0x10
static float g_cu = 0.5f, g_cv = 0.5f;  // курсор в долях окна (мышь хоста или правый стик)

// курсор (доли окна) -> координаты заднего буфера гостя
static u32 cursor_lparam(void)
{
    int x, y;
    if (!d3d8_window_to_target(g_cu, g_cv, &x, &y)) {
        x = (int)(g_cu * 800.0f);
        y = (int)(g_cv * 600.0f);
    }
    user32_set_cursor_pos(x, y);
    return (u32)(u16)x | (u32)(u16)y << 16;
}

static u32 mouse_lparam(int wx, int wy)
{
    SDL_Window *w = SDL_GetMouseFocus();
    int ww = 1, wh = 1;
    if (w)
        SDL_GetWindowSize(w, &ww, &wh);
    g_cu = (float)wx / (float)ww;
    g_cv = (float)wy / (float)wh;
    return cursor_lparam();
}

static void key(u32 vk, int down)
{
    u32 hwnd = user32_focus();
    if (!hwnd)
        return;
    user32_set_key(vk, down);
    user32_post(hwnd, down ? WM_KEYDOWN : WM_KEYUP, vk, 1u | (down ? 0 : 3u << 30));
}

// кнопка мыши: 0 левая, 1 правая, 2 средняя
static void mouse_button(int idx, int down, u32 lparam)
{
    static const u32 mk[3] = {1, 2, 0x10}, vk[3] = {1, 2, 4};
    static const u32 msg[3][2] = {{WM_LBUTTONUP, WM_LBUTTONDOWN}, {WM_RBUTTONUP, WM_RBUTTONDOWN},
                                  {WM_MBUTTONUP, WM_MBUTTONDOWN}};
    u32 hwnd = user32_focus();
    g_buttons = down ? g_buttons | mk[idx] : g_buttons & ~mk[idx];
    dinput8_mouse_button(idx, down);
    user32_set_key(vk[idx], down);
    if (hwnd)
        user32_post(hwnd, msg[idx][down], g_buttons, lparam);
}

static void wheel(int clicks)
{
    u32 hwnd = user32_focus();
    dinput8_mouse_move(0, 0, clicks * 120);
    if (hwnd)
        user32_post(hwnd, WM_MOUSEWHEEL, g_buttons | (u32)(u16)(s16)(clicks * 120) << 16, 0);
}

// ---------------------------------------------------------------- геймпад
// Два режима. В игре геймпад виден игре как джойстик DirectInput (runtime/hle/dinput8.c): левый стик — оси
// JoyX/JoyY, правый — JoyU/JoyV, кнопки — Joy1..Joy12, крестовина — Joy13..Joy16; их действия — в User.ini
// (умолчания пишет host/boot/config.c). Пока зажата кнопка слоя (Layer=, по умолчанию ZL), кнопки шлют
// клавиши второго слоя из Snx.ini («ZL+X=5» — зелья 1–9); «Кнопка=Клавиша» без слоя — клавиша вместо джойстика.
// В меню игры (GUIController.ActivePage, runtime/game/menu.c) джойстик молчит, а геймпад ведёт GUI игры:
// крестовина — фокус на соседний элемент (у ползунка и выпадающего списка влево/вправо — стрелки: значение),
// A — щелчок по элементу в фокусе, B — по кнопке «назад» страницы (BACK, CANCEL, NO), если она есть, иначе
// Escape; L/R — вкладки настроек; Plus — Escape, Minus — пробел; стики — курсор. Касание — щелчок в точке.
static float g_axis[SDL_CONTROLLER_AXIS_MAX];  // оси без устройства: SNX_KEYS (~RX=...)
static int g_trig[2];
// Оси — по каждому открытому геймпаду отдельно (SDL_JoystickID), в игру — сильнее отклонённая. Прежде одна общая
// таблица: второе устройство (встроенный геймпад и «виртуальный», геймпад-переходник и т. п.) событием с нулём
// затирало правый стик настоящего — камера «пропадала» (жалоба: Retroid Pocket, GameSir X5 Lite).
#define MAX_PADS 8
static struct {
    SDL_JoystickID id;  // -1 — свободно
    SDL_GameController *gc;
    float axis[SDL_CONTROLLER_AXIS_MAX];
    unsigned events;    // событий осей (диагностика)
} g_pads[MAX_PADS] = {[0 ... MAX_PADS - 1] = {.id = -1}};
enum { PB_ZL = SDL_CONTROLLER_BUTTON_MAX, PB_ZR, PB_N };  // курки — тоже кнопки
enum {  // цели, кроме клавиш
    T_LMB = 0x1000, T_RMB, T_MMB, T_WHEEL_UP, T_WHEEL_DOWN, T_FPS, T_JOY, T_NAV, T_SELECT, T_BACK, T_TAB_PREV,
    T_TAB_NEXT, T_QUICKMENU
};
static const char *const g_pb_name[PB_N] = {
    [SDL_CONTROLLER_BUTTON_A] = "A", [SDL_CONTROLLER_BUTTON_B] = "B", [SDL_CONTROLLER_BUTTON_X] = "X",
    [SDL_CONTROLLER_BUTTON_Y] = "Y", [SDL_CONTROLLER_BUTTON_BACK] = "Minus", [SDL_CONTROLLER_BUTTON_START] = "Plus",
    [SDL_CONTROLLER_BUTTON_LEFTSTICK] = "LStick", [SDL_CONTROLLER_BUTTON_RIGHTSTICK] = "RStick",
    [SDL_CONTROLLER_BUTTON_LEFTSHOULDER] = "L", [SDL_CONTROLLER_BUTTON_RIGHTSHOULDER] = "R",
    [SDL_CONTROLLER_BUTTON_DPAD_UP] = "DPadUp", [SDL_CONTROLLER_BUTTON_DPAD_DOWN] = "DPadDown",
    [SDL_CONTROLLER_BUTTON_DPAD_LEFT] = "DPadLeft", [SDL_CONTROLLER_BUTTON_DPAD_RIGHT] = "DPadRight",
    [PB_ZL] = "ZL", [PB_ZR] = "ZR",
};
// кнопка -> номер кнопки джойстика + 1 (Joy1 = A ... Joy12 = RStick); крестовина — POV, 0
static const u8 g_pb_joy[PB_N] = {
    [SDL_CONTROLLER_BUTTON_A] = 1, [SDL_CONTROLLER_BUTTON_B] = 2, [SDL_CONTROLLER_BUTTON_X] = 3,
    [SDL_CONTROLLER_BUTTON_Y] = 4, [SDL_CONTROLLER_BUTTON_LEFTSHOULDER] = 5, [SDL_CONTROLLER_BUTTON_RIGHTSHOULDER] = 6,
    [PB_ZL] = 7, [PB_ZR] = 8, [SDL_CONTROLLER_BUTTON_BACK] = 9, [SDL_CONTROLLER_BUTTON_START] = 10,
    [SDL_CONTROLLER_BUTTON_LEFTSTICK] = 11, [SDL_CONTROLLER_BUTTON_RIGHTSTICK] = 12,
};
static const struct {
    const char *name;
    u32 t;
} g_key_names[] = {
    {"LeftMouse", T_LMB}, {"RightMouse", T_RMB}, {"MiddleMouse", T_MMB}, {"MouseWheelUp", T_WHEEL_UP},
    {"MouseWheelDown", T_WHEEL_DOWN}, {"Enter", 0x0D}, {"Escape", 0x1B}, {"Space", 0x20}, {"Tab", 0x09},
    {"Backspace", 0x08}, {"Shift", 0x10}, {"Ctrl", 0x11}, {"Alt", 0x12}, {"Pause", 0x13}, {"CapsLock", 0x14},
    {"PageUp", 0x21}, {"PageDown", 0x22}, {"End", 0x23}, {"Home", 0x24}, {"Left", 0x25}, {"Up", 0x26},
    {"Right", 0x27}, {"Down", 0x28}, {"Insert", 0x2D}, {"Delete", 0x2E}, {"Semicolon", 0xBA}, {"Equals", 0xBB},
    {"Comma", 0xBC}, {"Minus", 0xBD}, {"Period", 0xBE}, {"Slash", 0xBF}, {"Tilde", 0xC0}, {"LeftBracket", 0xDB},
    {"Backslash", 0xDC}, {"RightBracket", 0xDD}, {"SingleQuote", 0xDE}, {"ToggleFPS", T_FPS},
    {"QuickMenu", T_QUICKMENU},
};

#define SNX_INI_VERSION 3
static const char g_snx_default[] =
    "; Настройки порта webheadNX. Меняются здесь или прямо в игре через Quick Menu (ZL+Plus).\n"
    "[Port]\n"
    "Version=3          ; формат файла (файл старого формата переименовывается в Snx.ini.old)\n"
    "\n"
    "[Overlay]\n"
    "ShowFPS=False       ; счётчик кадров в углу (переключается и на ходу: ZL+Minus)\n"
    "MenuAspect4x3=False ; страницы меню в 4:3 на широком экране (переключается в Quick Menu)\n"
    "\n"
    "; Геймпад. В игре он виден ей как джойстик: кнопки A B X Y L R ZL ZR Minus Plus LStick RStick - Joy1..Joy12,\n"
    "; крестовина - Joy13..Joy16 (вверх, вправо, вниз, влево), левый стик - JoyX/JoyY, правый - JoyU/JoyV.\n"
    "; Что они делают - в User.ini, секция [Engine.Input] (Joy1=Fire и т. д.).\n"
    "; Здесь - второй слой: ZL+Кнопка=Клавиша, пока зажат ZL; Кнопка=Клавиша - клавиша вместо джойстика.\n"
    "; Кнопки: A B X Y L R ZL ZR Plus Minus LStick RStick DPadUp DPadDown DPadLeft DPadRight.\n"
    "; Клавиши - имена из User.ini игры: A..Z 0..9 F1..F12 NumPad0..NumPad9 Enter Escape Space Tab Up Down Left\n"
    ";   Right Home End PageUp PageDown LeftBracket RightBracket ... и LeftMouse RightMouse MiddleMouse\n"
    ";   MouseWheelUp MouseWheelDown, ToggleFPS, QuickMenu.\n"
    "; В меню игры: стики - курсор, крестовина - к соседней кнопке меню, A - выбрать, B и Plus - назад.\n"
    "[Pad]\n"
    "Layer=ZL           ; кнопка второго слоя (None - без слоя)\n"
    "Plus=Escape        ; меню - клавишей: открытое с джойстика меню игра рисует без книги\n"
    "B=Ctrl             ; прыжок - клавишей Ctrl, как на ПК: двойной прыжок и лианы (кнопка джойстика Joy2\n"
    ";                    срабатывала через раз). B= (пусто) - снова Joy2 из User.ini\n"
    "CameraMode=Mouse   ; камера правым стиком: Mouse - как мышь/палец, Joystick - ось JoyU/JoyV\n"
    "MoveSpeed=1200     ; бег при полном наклоне левого стика (как клавиша W)\n"
    "CameraSpeed=72     ; поворот камеры правым стиком (умножается ещё на скорость мыши из меню игры)\n"
    "InvertCameraY=False\n"
    "DeadZone=0.15      ; мёртвая зона стиков (доля хода)\n"
    "Rumble=True        ; вибрация геймпада\n"
    "; зелья 1-9 (как цифры на клавиатуре): с ZL крестовина по часовой стрелке от верха - 1..4,\n"
    "; кнопки по часовой стрелке от X - 5..8, R - 9\n"
    "ZL+DPadUp=1\n"
    "ZL+DPadRight=2\n"
    "ZL+DPadDown=3\n"
    "ZL+DPadLeft=4\n"
    "ZL+X=5\n"
    "ZL+A=6\n"
    "ZL+B=7\n"
    "ZL+Y=8\n"
    "ZL+R=9\n"
    "ZL+Minus=ToggleFPS\n"
    "ZL+Plus=QuickMenu\n";

static char g_snx_path[1100];
static u32 g_map[2][PB_N];  // [слой][кнопка] -> виртуальная клавиша или T_*; 0 — джойстик (в слое — ничего)
static u32 g_held[PB_N];    // что нажала кнопка: отпускается то же, даже если сменились слой или режим
static int g_layer_btn = -1, g_layer, g_menu;
static float g_move_speed = 1200.0f, g_cam_speed = 72.0f, g_dead = 0.15f;
static int g_invert_y;
// Камера правым стиком: 1 — смещениями мыши DirectInput (как камера пальцем), 0 — осью джойстика JoyU/JoyV.
// Мышь — основной ввод игры на ПК: у оси джойстика камера на Android «пропадала» (после прыжков по кувшинкам и
// просто через минуту-две, до захода в меню), у пальца — нет. Скорость: ось V даёт aMouseX = 20·V, мышь —
// 1,2 на отсчёт в секунду (MouseX: Axis aMouseX Speed=6, ×0,01; UInput::ReadInput ×20/Δt), то есть
// CameraSpeed 72 ≈ 1200 отсчётов/с при полном наклоне; ещё × «Чувствительность камеры» лаунчера (SNX_LOOK_SENS).
static int g_cam_mouse = 1;
static float g_look_sens = 1.0f;
static int g_rumble = 1;

// ---------------------------------------------------------------- геймпады и вибрация (SDL_GameControllerRumble)
static pthread_mutex_t g_pad_lock = PTHREAD_MUTEX_INITIALIZER;
static u16 g_rmb_low, g_rmb_high;
static u32 g_rmb_ms, g_rmb_seq, g_rmb_done;

void input_rumble(unsigned short low_freq, unsigned short high_freq, unsigned int duration_ms)
{
    pthread_mutex_lock(&g_pad_lock);
    if (!g_rumble && (low_freq || high_freq)) {
        pthread_mutex_unlock(&g_pad_lock);
        return;
    }
    g_rmb_low = (u16)low_freq;
    g_rmb_high = (u16)high_freq;
    g_rmb_ms = (u32)duration_ms;
    g_rmb_seq++;
    pthread_mutex_unlock(&g_pad_lock);
}

int input_rumble_enabled(void)
{
    pthread_mutex_lock(&g_pad_lock);
    int on = g_rumble;
    pthread_mutex_unlock(&g_pad_lock);
    return on;
}

void input_set_rumble_enabled(int on)
{
    pthread_mutex_lock(&g_pad_lock);
    g_rumble = on != 0;
    if (!g_rumble) {
        g_rmb_low = g_rmb_high = 0;
        g_rmb_ms = 0;
        g_rmb_seq++;
    }
    pthread_mutex_unlock(&g_pad_lock);
}

#ifdef __SWITCH__
// Вибрация Switch напрямую через hid libnx. SDL 2.28 devkitPro на консоли сообщает «rumble no» для всех слотов, а
// SDL_GameControllerRumble отвечает успехом без вибрации (проверено на Switch в портативе). Устройства вибрации
// берутся у портатива (две половинки Joy-Con) и у игрока 1 (Pro Controller, пара или одиночный Joy-Con) и
// перезапрашиваются при смене стиля контроллера; длительность отсчитывает rumble_tick.
typedef struct {
    u32 style;
    int n;
    HidVibrationDeviceHandle h[2];
} nx_vib;
static nx_vib g_vib[2];  // [0] портатив, [1] игрок 1
static u64 g_vib_stop_ns;
static int g_vib_on;

static int nx_vib_prepare(nx_vib *v, HidNpadIdType id)
{
    u32 st = hidGetNpadStyleSet(id);
    if (st == v->style)
        return v->n;
    v->style = st;
    v->n = 0;
    HidNpadStyleTag tag;
    int n;
    if (st & HidNpadStyleTag_NpadHandheld)
        tag = HidNpadStyleTag_NpadHandheld, n = 2;
    else if (st & HidNpadStyleTag_NpadFullKey)
        tag = HidNpadStyleTag_NpadFullKey, n = 1;
    else if (st & HidNpadStyleTag_NpadJoyDual)
        tag = HidNpadStyleTag_NpadJoyDual, n = 2;
    else if (st & HidNpadStyleTag_NpadJoyLeft)
        tag = HidNpadStyleTag_NpadJoyLeft, n = 1;
    else if (st & HidNpadStyleTag_NpadJoyRight)
        tag = HidNpadStyleTag_NpadJoyRight, n = 1;
    else
        return 0;
    if (R_SUCCEEDED(hidInitializeVibrationDevices(v->h, n, id, tag)))
        v->n = n;
    return v->n;
}

// Сила моторов 0..1 -> всем подключённым; возвращает число моторов, принявших значение
static int nx_vib_send(float amp_low, float amp_high)
{
    // частоты — штатные для «удара» Joy-Con (160 и 320 Гц)
    HidVibrationValue v = {.amp_low = amp_low, .freq_low = 160.0f, .amp_high = amp_high, .freq_high = 320.0f};
    HidVibrationValue vals[2] = {v, v};
    static const HidNpadIdType ids[2] = {HidNpadIdType_Handheld, HidNpadIdType_No1};
    int sent = 0;
    for (int i = 0; i < 2; i++) {
        int n = nx_vib_prepare(&g_vib[i], ids[i]);
        if (n && R_SUCCEEDED(hidSendVibrationValues(g_vib[i].h, vals, n)))
            sent += n;
    }
    return sent;
}
#endif

// Вызывается в главном потоке из input_tick(). Мак, Linux и Android — SDL_GameControllerRumble (исполняется в потоке
// событий SDL), Switch — hid libnx.
static void rumble_tick(void)
{
    u16 low = 0, high = 0;
    u32 ms = 0;
    int pending = 0;
    pthread_mutex_lock(&g_pad_lock);
    if (g_rmb_seq != g_rmb_done) {
        low = g_rmb_low;
        high = g_rmb_high;
        ms = g_rmb_ms;
        g_rmb_done = g_rmb_seq;
        pending = 1;
    }
#ifdef __SWITCH__
    pthread_mutex_unlock(&g_pad_lock);
    u64 now = rt_ticks_ns();
    if (pending) {
        static int logged;  // первые импульсы — в лог: видно, приняли ли их моторы
        int ok = nx_vib_send(low / 65535.0f, high / 65535.0f);
        g_vib_on = (low || high) && ms;
        g_vib_stop_ns = now + (u64)ms * 1000000u;
        if ((low || high) && logged < 3) {
            logged++;
            fprintf(stderr, "[pad] rumble %04x/%04x %u ms: %d motors (hid)\n", low, high, ms, ok);
        }
    } else if (g_vib_on && now >= g_vib_stop_ns) {
        g_vib_on = 0;
        nx_vib_send(0.0f, 0.0f);
    }
    return;
#endif
    if (pending) {
        static int logged;  // первые импульсы — в лог: видно, дошёл ли запрос до SDL и принял ли его драйвер
        int n = 0, ok = 0;
        for (int i = 0; i < MAX_PADS; i++)
            if (g_pads[i].gc) {
                n++;
                ok += SDL_GameControllerRumble(g_pads[i].gc, low, high, ms) == 0;
            }
        if ((low || high) && logged < 3) {
            logged++;
            fprintf(stderr, "[pad] rumble %04x/%04x %u ms: %d of %d pads%s%s\n", low, high, ms, ok, n,
                    ok < n ? ", " : "", ok < n ? SDL_GetError() : "");
        }
    }
    pthread_mutex_unlock(&g_pad_lock);
}

static int pad_button_of(const char *n)
{
    for (int b = 0; b < PB_N; b++)
        if (g_pb_name[b] && !strcasecmp(g_pb_name[b], n))
            return b;
    return -1;
}

// имя клавиши -> цель; 0 — нет такой
static u32 key_of(const char *n)
{
    size_t len = strlen(n);
    if (len == 1 && isalnum((unsigned char)n[0]))
        return (u32)toupper((unsigned char)n[0]);
    if ((n[0] == 'F' || n[0] == 'f') && len <= 3 && atoi(n + 1) >= 1 && atoi(n + 1) <= 12)
        return 0x6F + (u32)atoi(n + 1);
    if (!strncasecmp(n, "NumPad", 6) && len == 7 && isdigit((unsigned char)n[6]))
        return 0x60 + (u32)(n[6] - '0');
    for (size_t i = 0; i < sizeof g_key_names / sizeof g_key_names[0]; i++)
        if (!strcasecmp(g_key_names[i].name, n))
            return g_key_names[i].t;
    return 0;
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    for (char *t = s + strlen(s); t > s && isspace((unsigned char)t[-1]);)
        *--t = 0;
    return s;
}

static int is_true(const char *v) { return !strcasecmp(v, "True") || !strcmp(v, "1"); }

// Атомарная запись текста по умолчанию в Snx.ini (<path>.tmp + rename)
static int write_snx_default(void)
{
    if (!g_snx_path[0])
        return -1;
    char tmp[1200];
    int n = snprintf(tmp, sizeof tmp, "%s.tmp", g_snx_path);
    if (n < 0 || (size_t)n >= sizeof tmp)
        return -1;
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return -1;
    size_t len = sizeof g_snx_default - 1;
    int ok = fwrite(g_snx_default, 1, len, f) == len && fflush(f) == 0;
#ifndef __SWITCH__
    if (ok)
        (void)fsync(fileno(f));
#endif
    if (fclose(f) != 0 || !ok) {
        remove(tmp);
        return -1;
    }
    if (rename(tmp, g_snx_path) != 0) {
#ifdef __SWITCH__
        if (errno == EEXIST && remove(g_snx_path) == 0 && rename(tmp, g_snx_path) == 0)
            return 0;
#endif
        remove(tmp);
        return -1;
    }
    return 0;
}

static void qm_draw(int w, int h);
static void qm_sync_snapshot(void);

// 1 — разобран; 0 — файл старого формата (нет Version=SNX_INI_VERSION)
static int load_settings(const char *text, const char *src)
{
    memset(g_map, 0, sizeof g_map);
    g_layer_btn = -1;
    g_rumble = 1;
    d3d8_set_menu_aspect(0);  // умолчание для Snx.ini v2 без ключа MenuAspect4x3
    // Умолчание для существующих Snx.ini версии 2 без строки ZL+Plus=QuickMenu (перекрывается строкой из файла)
    g_map[1][SDL_CONTROLLER_BUTTON_START] = T_QUICKMENU;
    int pad = 0, version = 0;  // pad: секция [Pad]
    for (const char *p = text; *p;) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        char ln[256];
        snprintf(ln, sizeof ln, "%.*s", (int)(n < sizeof ln ? n : sizeof ln - 1), p);
        p += n + (e ? 1 : 0);
        char *c = strchr(ln, ';'), *eq = strchr(ln, '=');
        if (c)
            *c = 0;
        char *sec = trim(ln);
        if (*sec == '[') {
            pad = !strncasecmp(sec, "[Pad]", 5);
            continue;
        }
        if (!eq || (c && c < eq))
            continue;
        *eq = 0;
        char *k = trim(ln), *v = trim(eq + 1);
        if (!pad) {
            if (!strcasecmp(k, "ShowFPS"))
                d3d8_set_fps_overlay(is_true(v));
            else if (!strcasecmp(k, "MenuAspect4x3"))
                d3d8_set_menu_aspect(is_true(v));
            else if (!strcasecmp(k, "Version"))
                version = atoi(v);
            continue;
        }
        if (!strcasecmp(k, "Layer")) {
            g_layer_btn = strcasecmp(v, "None") ? pad_button_of(v) : -1;
            continue;
        }
        if (!strcasecmp(k, "MoveSpeed") || !strcasecmp(k, "CameraSpeed") || !strcasecmp(k, "DeadZone")) {
            float f = (float)atof(v);
            *(!strcasecmp(k, "MoveSpeed") ? &g_move_speed : !strcasecmp(k, "CameraSpeed") ? &g_cam_speed : &g_dead) = f;
            continue;
        }
        if (!strcasecmp(k, "InvertCameraY")) {
            g_invert_y = is_true(v);
            continue;
        }
        if (!strcasecmp(k, "CameraMode")) {
            g_cam_mouse = strcasecmp(v, "Joystick") != 0;
            continue;
        }
        if (!strcasecmp(k, "Rumble")) {
            g_rumble = is_true(v);
            continue;
        }
        char *plus = strchr(k, '+');
        int b = pad_button_of(plus ? plus + 1 : k);
        u32 t = *v ? key_of(v) : 0;
        if (b < 0 || (*v && !t))
            fprintf(stderr, "[input] %s: cannot parse '%s=%s'\n", src, k, v);
        else
            g_map[plus != NULL][b] = t;
    }
    if (g_dead < 0.0f || g_dead > 0.9f)
        g_dead = 0.15f;
    return version == SNX_INI_VERSION;
}

static void input_load(const char *user_dir)
{
    char old[1100];
    snprintf(g_snx_path, sizeof g_snx_path, "%s/System/Snx.ini", user_dir);
    ov_set_draw_hook(qm_draw);
    hle_rumble_hook = input_rumble;
    FILE *f = fopen(g_snx_path, "rb");
    if (f) {
        char *text = calloc(1, 64 << 10);
        int ok = 0;
        if (text) {
            text[fread(text, 1, (64 << 10) - 1, f)] = 0;
            ok = load_settings(text, g_snx_path);
        }
        free(text);
        fclose(f);
        if (ok) {
            qm_sync_snapshot();
            return;
        }
        snprintf(old, sizeof old, "%s.old", g_snx_path);  // старый формат (клавиши вместо джойстика) — в сторону
        remove(old);
        rename(g_snx_path, old);
        fprintf(stderr, "[input] %s: old format, saved as %s\n", g_snx_path, old);
    }
    load_settings(g_snx_default, "default");
    qm_sync_snapshot();
    write_snx_default();
}

void input_init(const char *user_dir)
{
    input_load(user_dir);
    const char *cm = getenv("SNX_PAD_CAMERA");  // поверх Snx.ini: лаунчер («Камера правым стиком»)
    if (cm && *cm)
        g_cam_mouse = strcasecmp(cm, "joystick") != 0;
    const char *ls = getenv("SNX_LOOK_SENS");
    if (ls && atof(ls) > 0.0)
        g_look_sens = (float)atof(ls);
    fprintf(stderr, "[input] right stick camera: %s, speed %.0f, sensitivity %.2f\n", g_cam_mouse ? "mouse" : "joystick",
            (double)g_cam_speed, (double)g_look_sens);
    const char *e = getenv("SNX_SHOW_FPS");  // поверх Snx.ini: лаунчер Android («Счётчик FPS»)
    if (e)
        d3d8_set_fps_overlay(*e == '1');
}

static u32 g_joy_buttons;  // кнопки джойстика (биты Joy1..Joy12) и крестовины (биты 16..19: вверх, вниз, влево, вправо)
// Нажатия с последней передачи в dinput8_joy: короткое нажатие (нажал и отпустил между двумя тактами ввода) не
// теряется — игра увидит кнопку нажатой хотя бы в одном опросе (dinput8 держит нажатие до чтения игрой).
static u32 g_joy_latch;

// Щелчок в точке заднего буфера (B — по кнопке «назад», касание экрана): курсор GUI ставит поток гостя
// (game_menu_point), нажатие — когда GUI уже увидел курсор там (элемент под курсором выбирается заранее),
// отпускание — с кнопкой/пальцем или сразу после нажатия, если их уже отпустили.
static int g_click_state;  // 0 — нет, 1 — курсор подведён, ждём нажатия, 2 — нажата
static int g_click_up;     // кнопка/палец уже отпущены
static unsigned g_click_ticket;

static void sync_cursor(float x, float y)  // курсор хоста (для WM_* и GetCursorPos) — туда же, где курсор GUI
{
    float u, v;
    if (d3d8_target_to_window(x, y, &u, &v)) {
        g_cu = u < 0.0f ? 0.0f : u > 1.0f ? 1.0f : u;
        g_cv = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
    }
}

static void click_at(float x, float y)
{
    if (g_click_state == 2)
        mouse_button(0, 0, cursor_lparam());
    sync_cursor(x, y);
    g_click_ticket = game_menu_point(x, y);
    g_click_state = 1;
    g_click_up = 0;
}

static void click_release(void)
{
    if (g_click_state == 2) {
        mouse_button(0, 0, cursor_lparam());
        g_click_state = 0;
    } else if (g_click_state == 1)
        g_click_up = 1;
}

static void click_tick(void)
{
    if (g_click_state == 1 && game_menu_point_done(g_click_ticket)) {
        mouse_button(0, 1, cursor_lparam());
        g_click_state = 2;
    } else if (g_click_state == 2 && g_click_up) {
        mouse_button(0, 0, cursor_lparam());
        g_click_state = 0;
    }
}

static void touch(float u, float v)  // касание: доли окна -> точка заднего буфера
{
    int x, y;
    if (d3d8_window_to_target(u, v, &x, &y))
        click_at((float)x, (float)y);
}

static void back(int down)
{
    game_rect r;
    if (!down) {
        click_release();
        return;
    }
    if (game_menu_back(&r)) {
        if (getenv("SNX_LOG_MENU"))
            fprintf(stderr, "[menu] back: click (%.0f,%.0f)\n", (r.x0 + r.x1) / 2, (r.y0 + r.y1) / 2);
        click_at((r.x0 + r.x1) / 2, (r.y0 + r.y1) / 2);
        return;
    }
    key(0x1B, 1);
    key(0x1B, 0);
}

// крестовина в меню: у ползунка и выпадающего списка в фокусе влево/вправо — стрелки (значение меняет сам
// элемент), иначе — фокус на соседний элемент
static u32 g_nav_key;            // нажатая крестовиной стрелка
static int g_nav_dir = -1;       // зажатое направление (повтор, как у клавиатуры)
static u32 g_nav_next;           // время следующего повтора, мс

static void nav(int dir, int down)
{
    static const u32 arrows[4] = {0x26, 0x28, 0x25, 0x27};
    int kind = GM_NONE;
    if (!down) {
        if (g_nav_key)
            key(g_nav_key, 0);
        g_nav_key = 0;
        if (dir == g_nav_dir)
            g_nav_dir = -1;
        return;
    }
    if (dir != g_nav_dir) {
        g_nav_dir = dir;
        g_nav_next = SDL_GetTicks() + 400;
    }
    game_menu_open(&kind);
    if (dir >= 2 && (kind == GM_SLIDER || kind == GM_COMBO)) {
        key(g_nav_key = arrows[dir], 1);
        return;
    }
    game_menu_nav(dir);
}

// A: щелчок по элементу в фокусе (курсор GUI уже на нём); ползунок щелчком не трогаем — щелчок ставит
// значение под курсором (середину); для текстового поля открываем экранную клавиатуру на Switch.
static void select_(int down)
{
    int kind = GM_NONE;
    static int pressed;
    game_menu_open(&kind);
    if (down && kind == GM_SLIDER)
        return;
    if (down && kind == GM_EDIT)
        game_menu_edit_activate();
    if (down || pressed)
        mouse_button(0, down, cursor_lparam());
    pressed = down;
}

static void qm_toggle(void);
static void qm_adjust(int dir);
static int qm_is_open(void);

static void send_target(int b, u32 t, int down)
{
    switch (t) {
    case 0: break;
    case T_LMB: case T_RMB: case T_MMB: mouse_button((int)(t - T_LMB), down, cursor_lparam()); break;
    case T_WHEEL_UP: if (down) wheel(1); break;
    case T_WHEEL_DOWN: if (down) wheel(-1); break;
    case T_FPS: if (down) d3d8_set_fps_overlay(!d3d8_fps_overlay()); break;
    case T_QUICKMENU: if (down) qm_toggle(); break;
    case T_NAV: nav(b - SDL_CONTROLLER_BUTTON_DPAD_UP, down); break;
    case T_SELECT: select_(down); break;
    case T_BACK: back(down); break;
    case T_TAB_PREV: case T_TAB_NEXT: if (down) game_menu_tab(t == T_TAB_NEXT ? 1 : -1); break;
    case T_JOY: {
        u32 bit = g_pb_joy[b] ? 1u << (g_pb_joy[b] - 1) : 1u << (16 + b - SDL_CONTROLLER_BUTTON_DPAD_UP);
        g_joy_buttons = down ? g_joy_buttons | bit : g_joy_buttons & ~bit;
        if (down)
            g_joy_latch |= bit & 0xFFF;
        break;
    }
    default: key(t, down);
    }
}

// SDL на Switch раздаёт кнопки по положению, как у геймпада Xbox (нижняя — A): нижняя кнопка Switch (B)
// приходит как SDL_CONTROLLER_BUTTON_A. Здесь — по надписям на кнопках.
static int by_label(int b)
{
#ifdef __SWITCH__
    switch (b) {
    case SDL_CONTROLLER_BUTTON_A: return SDL_CONTROLLER_BUTTON_B;
    case SDL_CONTROLLER_BUTTON_B: return SDL_CONTROLLER_BUTTON_A;
    case SDL_CONTROLLER_BUTTON_X: return SDL_CONTROLLER_BUTTON_Y;
    case SDL_CONTROLLER_BUTTON_Y: return SDL_CONTROLLER_BUTTON_X;
    default: break;
    }
#endif
    return b;
}

static int is_dpad(int b) { return b >= SDL_CONTROLLER_BUTTON_DPAD_UP && b <= SDL_CONTROLLER_BUTTON_DPAD_RIGHT; }

static u32 target_of(int b)
{
    // Вызов Quick Menu (например, ZL+Plus) работает и в игре, и поверх открытого меню игры
    if (g_layer && g_map[1][b] == T_QUICKMENU)
        return T_QUICKMENU;
    if (!g_layer && g_map[0][b] == T_QUICKMENU)
        return T_QUICKMENU;
    if (g_menu) {
        switch (b) {
        case SDL_CONTROLLER_BUTTON_A: return T_SELECT;
        case SDL_CONTROLLER_BUTTON_B: return T_BACK;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return T_TAB_PREV;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return T_TAB_NEXT;
        case SDL_CONTROLLER_BUTTON_START: return 0x1B;
        case SDL_CONTROLLER_BUTTON_BACK: return 0x20;
        default: return is_dpad(b) ? T_NAV : 0;
        }
    }
    if (b == g_layer_btn)
        return 0;
    if (g_layer)
        return g_map[1][b];
    return g_map[0][b] ? g_map[0][b] : g_pb_joy[b] || is_dpad(b) ? T_JOY : 0;
}

static int g_qm_dir = -1;  // повтор крестовины в Quick Menu
static u32 g_qm_next;

static void pad_button(int b, int down)
{
    if (b < 0 || b >= PB_N)
        return;
    if (b == g_layer_btn)
        g_layer = down;
    if (qm_is_open()) {
        // Пока открыто Quick Menu, ввод геймпада перехватывается и не уходит в игру/меню игры
        if (down) {
            if (b == SDL_CONTROLLER_BUTTON_B || b == SDL_CONTROLLER_BUTTON_START ||
                (g_layer && g_map[1][b] == T_QUICKMENU) || (!g_layer && g_map[0][b] == T_QUICKMENU))
                qm_toggle();
            else if (b == SDL_CONTROLLER_BUTTON_A)
                qm_adjust(4);
            else if (is_dpad(b)) {
                int dir = b - SDL_CONTROLLER_BUTTON_DPAD_UP;
                g_qm_dir = dir;
                g_qm_next = SDL_GetTicks() + 350;
                qm_adjust(dir);
            }
        } else if (is_dpad(b) && (b - SDL_CONTROLLER_BUTTON_DPAD_UP) == g_qm_dir) {
            g_qm_dir = -1;
        }
        return;
    }
    if (down) {
        if (g_held[b])
            return;
        g_held[b] = target_of(b);
        send_target(b, g_held[b], 1);
    } else {
        send_target(b, g_held[b], 0);
        g_held[b] = 0;
    }
}

// смена режима (меню <-> игра): отпустить всё, что держат кнопки; нажатое заново сработает уже по-новому.
// Кнопки джойстика при входе в меню не отпускаются: событие Joy* в открытом меню переводит GUI игры (в нём
// остался код для Xbox) в режим фокуса с затемнёнными кнопками. Джойстик в меню замирает в прежнем состоянии
// (g_joy_frozen) и отпускается уже по возвращении в игру.
static u32 g_joy_frozen;

static void release_all(void)
{
    u32 joy = g_joy_buttons;
    for (int b = 0; b < PB_N; b++)
        if (g_held[b]) {
            send_target(b, g_held[b], 0);
            g_held[b] = 0;
        }
    g_joy_buttons = 0;
    g_joy_latch = 0;
    g_joy_frozen = joy;
}

static int pad_slot(SDL_JoystickID id, int alloc)
{
    int free_ = -1;
    for (int i = 0; i < MAX_PADS; i++) {
        if (g_pads[i].id == id)
            return i;
        if (g_pads[i].id < 0 && free_ < 0)
            free_ = i;
    }
    if (alloc && free_ >= 0) {
        memset(&g_pads[free_], 0, sizeof g_pads[free_]);
        g_pads[free_].id = id;
    }
    return alloc ? free_ : -1;
}

// ось a: самая отклонённая среди геймпадов (и SNX_KEYS)
static float pad_axis(int a)
{
    float best = g_axis[a];
    for (int i = 0; i < MAX_PADS; i++)
        if (g_pads[i].id >= 0 && fabsf(g_pads[i].axis[a]) > fabsf(best))
            best = g_pads[i].axis[a];
    return best;
}

// ---------------------------------------------------------------- Quick Menu порта (поверх игры через overlay.c)
enum { QM_FPS, QM_MENU_ASPECT, QM_INVERT_Y, QM_CAM_SPEED, QM_MOVE_SPEED, QM_DEADZONE, QM_RUMBLE, QM_COUNT };

static pthread_mutex_t g_qm_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    int open, sel;
    int invert_y, rumble;
    float cam_speed, move_speed, dead;
} g_qm;

static int qm_is_open(void)
{
    pthread_mutex_lock(&g_qm_lock);
    int open = g_qm.open;
    pthread_mutex_unlock(&g_qm_lock);
    return open;
}

static void qm_sync_snapshot(void)
{
    int rmb = input_rumble_enabled();
    pthread_mutex_lock(&g_qm_lock);
    g_qm.invert_y = g_invert_y;
    g_qm.cam_speed = g_cam_speed;
    g_qm.move_speed = g_move_speed;
    g_qm.dead = g_dead;
    g_qm.rumble = rmb;
    pthread_mutex_unlock(&g_qm_lock);
}

static void qm_save_key(const char *sec, const char *k, const char *val)
{
    if (!g_snx_path[0])
        return;
    if (boot_ini_set(g_snx_path, sec, k, val) < 0) {
        // При первом запуске каталога <user>/System могло не быть в момент input_init — создаём файл сейчас
        if (write_snx_default() == 0)
            boot_ini_set(g_snx_path, sec, k, val);
    }
}

static void qm_save_one(int item)
{
    char buf[32];
    switch (item) {
    case QM_FPS:
        qm_save_key("Overlay", "ShowFPS", d3d8_fps_overlay() ? "True" : "False");
        break;
    case QM_MENU_ASPECT:
        qm_save_key("Overlay", "MenuAspect4x3", d3d8_menu_aspect() ? "True" : "False");
        break;
    case QM_INVERT_Y:
        qm_save_key("Pad", "InvertCameraY", g_invert_y ? "True" : "False");
        break;
    case QM_CAM_SPEED:
        snprintf(buf, sizeof buf, "%.0f", g_cam_speed);
        qm_save_key("Pad", "CameraSpeed", buf);
        break;
    case QM_MOVE_SPEED:
        snprintf(buf, sizeof buf, "%.0f", g_move_speed);
        qm_save_key("Pad", "MoveSpeed", buf);
        break;
    case QM_DEADZONE:
        snprintf(buf, sizeof buf, "%.2f", g_dead);
        qm_save_key("Pad", "DeadZone", buf);
        break;
    case QM_RUMBLE:
        qm_save_key("Pad", "Rumble", input_rumble_enabled() ? "True" : "False");
        break;
    default: break;
    }
}

static void qm_toggle(void)
{
    int now_open;
    pthread_mutex_lock(&g_qm_lock);
    now_open = !g_qm.open;
    g_qm.open = now_open;
    pthread_mutex_unlock(&g_qm_lock);
    g_qm_dir = -1;
    if (now_open) {
        release_all();
        g_nav_dir = -1;
        qm_sync_snapshot();
    } else if (!g_menu) {
        g_joy_frozen = 0;
    }
}

// dir: 0 вверх, 1 вниз, 2 влево, 3 вправо, 4 A (переключить)
static void qm_adjust(int dir)
{
    int sel, changed = 0;
    pthread_mutex_lock(&g_qm_lock);
    if (dir == 0)
        g_qm.sel = (g_qm.sel + QM_COUNT - 1) % QM_COUNT;
    else if (dir == 1)
        g_qm.sel = (g_qm.sel + 1) % QM_COUNT;
    sel = g_qm.sel;
    pthread_mutex_unlock(&g_qm_lock);
    if (dir < 2)
        return;
    switch (sel) {
    case QM_FPS:
        d3d8_set_fps_overlay(!d3d8_fps_overlay());
        changed = 1;
        break;
    case QM_MENU_ASPECT:
        d3d8_set_menu_aspect(!d3d8_menu_aspect());
        changed = 1;
        break;
    case QM_INVERT_Y:
        g_invert_y = !g_invert_y;
        changed = 1;
        break;
    case QM_CAM_SPEED:
        if (dir == 2 || dir == 3) {
            float v = g_cam_speed + (dir == 3 ? 4.0f : -4.0f);
            v = v < 20.0f ? 20.0f : v > 160.0f ? 160.0f : v;
            if (v != g_cam_speed) {
                g_cam_speed = v;
                changed = 1;
            }
        }
        break;
    case QM_MOVE_SPEED:
        if (dir == 2 || dir == 3) {
            float v = g_move_speed + (dir == 3 ? 50.0f : -50.0f);
            v = v < 600.0f ? 600.0f : v > 1800.0f ? 1800.0f : v;
            if (v != g_move_speed) {
                g_move_speed = v;
                changed = 1;
            }
        }
        break;
    case QM_DEADZONE:
        if (dir == 2 || dir == 3) {
            float v = roundf((g_dead + (dir == 3 ? 0.05f : -0.05f)) * 20.0f) / 20.0f;
            v = v < 0.05f ? 0.05f : v > 0.50f ? 0.50f : v;
            if (fabsf(v - g_dead) > 1e-4f) {
                g_dead = v;
                changed = 1;
            }
        }
        break;
    case QM_RUMBLE: {
        int on = !input_rumble_enabled();
        input_set_rumble_enabled(on);
        if (on)
            input_rumble(0x5000u, 0x8000u, 120u);
        changed = 1;
        break;
    }
    default: break;
    }
    if (changed) {
        qm_sync_snapshot();
        qm_save_one(sel);
    }
}

static float qm_clamp(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

// Рисуется в потоке гостя из ov_flush поверх кадра (и поверх меню игры, если оно открыто)
static void qm_draw(int w, int h)
{
    if (w <= 0 || h <= 0)
        return;
    int open, sel, invert_y, rumble;
    float cam_speed, move_speed, dead;
    pthread_mutex_lock(&g_qm_lock);
    open = g_qm.open;
    sel = g_qm.sel;
    invert_y = g_qm.invert_y;
    cam_speed = g_qm.cam_speed;
    move_speed = g_qm.move_speed;
    dead = g_qm.dead;
    rumble = g_qm.rumble;
    pthread_mutex_unlock(&g_qm_lock);
    if (!open)
        return;

    int fps = d3d8_fps_overlay();
    int menu_aspect = d3d8_menu_aspect();
    int rus = game_menu_is_rus();
    static const char *const labels[QM_COUNT][2] = {
        [QM_FPS] = {"Show FPS", "Счётчик кадров"},
        [QM_MENU_ASPECT] = {"Menu Aspect 4:3", "Меню 4:3"},
        [QM_INVERT_Y] = {"Invert Camera Y", "Инверсия камеры Y"},
        [QM_CAM_SPEED] = {"Camera Speed", "Скорость камеры"},
        [QM_MOVE_SPEED] = {"Move Speed", "Скорость бега"},
        [QM_DEADZONE] = {"Stick Deadzone", "Мёртвая зона"},
        [QM_RUMBLE] = {"Rumble", "Вибрация"},
    };

    float sc = qm_clamp((float)h / 720.0f, 0.65f, 1.55f);
    float pad_x = 24.0f * sc, pad_y = 18.0f * sc;
    float hdr_h = 44.0f * sc, row_h = 38.0f * sc, ftr_h = 38.0f * sc;
    float title_h = 24.0f * sc, text_h = 20.0f * sc, icon_h = 19.0f * sc;
    float cw = qm_clamp((float)w * 0.48f, 360.0f * sc, 620.0f * sc);
    if (cw > (float)w - 28.0f)
        cw = (float)w - 28.0f;
    float ch = pad_y * 2.0f + hdr_h + QM_COUNT * row_h + ftr_h;
    if (ch > (float)h - 24.0f) {
        float k = ((float)h - 24.0f) / ch;
        pad_x *= k;
        pad_y *= k;
        hdr_h *= k;
        row_h *= k;
        ftr_h *= k;
        title_h *= k;
        text_h *= k;
        icon_h *= k;
        ch = (float)h - 24.0f;
    }
    float cx = ((float)w - cw) * 0.5f, cy = ((float)h - ch) * 0.5f;

    ov_rect(0.0f, 0.0f, (float)w, (float)h, OV_RGBA(0, 0, 0, 120));
    ov_rect(cx - 2.0f, cy - 2.0f, cw + 4.0f, ch + 4.0f, OV_RGBA(130, 195, 85, 220));
    ov_rect(cx, cy, cw, ch, OV_RGBA(18, 22, 28, 238));

    const char *title = rus ? "webheadNX - Быстрое меню" : "webheadNX - Quick Menu";
    ov_text(cx + pad_x, cy + pad_y + (hdr_h - title_h) * 0.35f, title_h, OV_RGBA(180, 235, 130, 255), title);
    ov_rect(cx + pad_x, cy + pad_y + hdr_h - 4.0f * sc, cw - 2.0f * pad_x, 1.5f, OV_RGBA(130, 195, 85, 120));

    float y0 = cy + pad_y + hdr_h;
    for (int i = 0; i < QM_COUNT; i++) {
        float ry = y0 + (float)i * row_h;
        if (i == sel)
            ov_rect(cx + pad_x * 0.5f, ry + 2.0f, cw - pad_x, row_h - 4.0f, OV_RGBA(95, 160, 60, 135));
        u32 col_lbl = i == sel ? OV_RGBA(255, 255, 255, 255) : OV_RGBA(215, 220, 228, 255);
        ov_text(cx + pad_x, ry + (row_h - text_h) * 0.5f, text_h, col_lbl, labels[i][rus]);

        char val[32];
        u32 col_val = i == sel ? OV_RGBA(255, 240, 150, 255) : OV_RGBA(185, 200, 215, 255);
        int is_bool = (i == QM_FPS || i == QM_MENU_ASPECT || i == QM_INVERT_Y || i == QM_RUMBLE);
        if (is_bool) {
            int on = i == QM_FPS ? fps : i == QM_MENU_ASPECT ? menu_aspect : i == QM_INVERT_Y ? invert_y : rumble;
            snprintf(val, sizeof val, "%s", on ? (rus ? "Вкл" : "On") : (rus ? "Выкл" : "Off"));
            col_val = on ? OV_RGBA(150, 235, 110, 255) : OV_RGBA(175, 180, 190, 255);
        } else if (i == QM_CAM_SPEED) {
            snprintf(val, sizeof val, "< %.0f >", cam_speed);
        } else if (i == QM_MOVE_SPEED) {
            snprintf(val, sizeof val, "< %.0f >", move_speed);
        } else {
            snprintf(val, sizeof val, "< %.0f%% >", dead * 100.0f);
        }
        float vw = ov_text_width(text_h, val);
        ov_text(cx + cw - pad_x - vw, ry + (row_h - text_h) * 0.5f, text_h, col_val, val);
    }

    float fy = y0 + QM_COUNT * row_h;
    ov_rect(cx + pad_x, fy + 2.0f, cw - 2.0f * pad_x, 1.0f, OV_RGBA(255, 255, 255, 45));
    float fcy = fy + ftr_h * 0.55f, fx = cx + pad_x;
    float fth = text_h * 0.80f, fgap = icon_h * 0.30f, fitem = icon_h * 0.90f;
    const char *h_dpad = rus ? "Изменить" : "Adjust";
    const char *h_a = rus ? "Перекл." : "Toggle";
    const char *h_b = rus ? "Закрыть" : "Close";
    float fw = ov_icon_width(icon_h, OV_DPAD) + fgap + ov_text_width(fth, h_dpad) + fitem +
               ov_icon_width(icon_h, OV_A) + fgap + ov_text_width(fth, h_a) + fitem +
               ov_icon_width(icon_h, OV_B) + fgap + ov_text_width(fth, h_b);
    if (fw > cw - 2.0f * pad_x && fw > 1.0f) {
        float k = (cw - 2.0f * pad_x) / fw;
        icon_h *= k;
        fth *= k;
        fgap *= k;
        fitem *= k;
    }
    u32 col_ftr = OV_RGBA(210, 215, 225, 255);
    fx += ov_icon(fx, fcy - icon_h * 0.5f, icon_h, OV_DPAD) + fgap;
    fx += ov_text(fx, fcy - fth * 0.54f, fth, col_ftr, h_dpad) + fitem;
    fx += ov_icon(fx, fcy - icon_h * 0.5f, icon_h, OV_A) + fgap;
    fx += ov_text(fx, fcy - fth * 0.54f, fth, col_ftr, h_a) + fitem;
    fx += ov_icon(fx, fcy - icon_h * 0.5f, icon_h, OV_B) + fgap;
    ov_text(fx, fcy - fth * 0.54f, fth, col_ftr, h_b);
}

// стик с круглой мёртвой зоной: (x, y) -> длина 0..1 с тем же направлением; curve — квадратичный отклик
static void stick(float x, float y, int curve, float *ox, float *oy)
{
    float m = sqrtf(x * x + y * y);
    if (m <= g_dead) {
        *ox = *oy = 0.0f;
        return;
    }
    float k = (m - g_dead) / (1.0f - g_dead);
    k = k > 1.0f ? 1.0f : k;
    if (curve)
        k *= k;
    *ox = x / m * k;
    *oy = y / m * k;
}

// SNX_KEYS=<секунда>:<VK hex>,... — нажать и отпустить клавишу в заданное время от старта (проверки без рук:
// например, F12 с привязкой «setres 1024x768» в User.ini); <секунда>:@<u>/<v> — касание экрана в точке окна
// (доли ширины и высоты; в меню); <секунда>:%<кнопка>+ / %<кнопка>- — нажать / отпустить кнопку геймпада
// (имена как в Snx.ini); <секунда>:~<ось>=<-1..1> — стик (LX LY RX RY).
static void scripted_keys(void)
{
    static const char *p;
    static int init;
    static u32 t0, held, clicked;
    if (!init) {
        init = 1;
        t0 = SDL_GetTicks();
        p = getenv("SNX_KEYS");
    }
    if (held) {
        key(held, 0);
        held = 0;
    }
    if (clicked && SDL_GetTicks() - clicked >= 300) {  // палец убран
        click_release();
        clicked = 0;
    }
    double at;
    unsigned vk;
    float u, v;
    int n;
    if (!p || sscanf(p, "%lf:%n", &at, &n) != 1 || (double)(SDL_GetTicks() - t0) < at * 1000)
        return;
    p += n;
    if (sscanf(p, "@%f/%f%n", &u, &v, &n) == 2) {
        fprintf(stderr, "[input] scripted touch %.3f %.3f\n", u, v);
        touch(u, v);
        clicked = SDL_GetTicks();
    } else if (*p == '%' || *p == '~') {  // геймпад
        char name[16], op = 0;
        float val = 0;
        if (*p == '%' && sscanf(p + 1, "%15[A-Za-z]%c%n", name, &op, &n) == 2) {
            fprintf(stderr, "[input] scripted pad %s%c\n", name, op);
            pad_button(pad_button_of(name), op == '+');
        } else if (*p == '~' && sscanf(p + 1, "%2s=%f%n", name, &val, &n) == 2) {
            static const char *const ax[4] = {"LX", "LY", "RX", "RY"};
            for (int i = 0; i < 4; i++)
                if (!strcasecmp(name, ax[i]))
                    g_axis[i] = val;
            fprintf(stderr, "[input] scripted stick %s=%.2f\n", name, val);
        } else {
            p = NULL;
            return;
        }
        n++;
    } else if (sscanf(p, "%x%n", &vk, &n) == 1) {
        fprintf(stderr, "[input] scripted key %02x\n", vk);
        key(vk, 1);
        held = vk;
    } else {
        p = NULL;
        return;
    }
    p += n;
    p = *p == ',' ? p + 1 : NULL;
}

// ---------------------------------------------------------------- сенсорный экран (host/boot/touch.h)
// Пишут потоки лаунчера (JNI), читает input_tick: состояние под замком, применение — только в главном потоке.
static pthread_mutex_t g_touch_lock = PTHREAD_MUTEX_INITIALIZER;
static u32 g_touch_buttons, g_touch_applied;
static float g_touch_axis[4];
static float g_look_x, g_look_y;
static struct {
    short vk;
    char down;
} g_touch_keys[32];
static int g_ntouch_keys;
static volatile int g_touch_menu, g_touch_ready;
// главное меню Spider-Man 2 рисуется поверх Bink-ролика и не поднимает GUIController ActivePage, поэтому
// game_menu_open() в нём ложно; лаунчер включает этот флаг кнопкой «Mеню», пока игрок в главном меню.
static volatile int g_menu_override;

void touch_pad(uint32_t buttons, float lx, float ly, float rx, float ry)
{
    pthread_mutex_lock(&g_touch_lock);
    g_touch_buttons = buttons & ((1u << PB_N) - 1);
    g_touch_axis[0] = lx;
    g_touch_axis[1] = ly;
    g_touch_axis[2] = rx;
    g_touch_axis[3] = ry;
    pthread_mutex_unlock(&g_touch_lock);
}

void touch_look(float dx, float dy)
{
    pthread_mutex_lock(&g_touch_lock);
    g_look_x += dx;
    g_look_y += dy;
    pthread_mutex_unlock(&g_touch_lock);
}

void touch_key(int vk, int down)
{
    pthread_mutex_lock(&g_touch_lock);
    if (g_ntouch_keys < 32 && vk > 0 && vk < 256) {
        g_touch_keys[g_ntouch_keys].vk = (short)vk;
        g_touch_keys[g_ntouch_keys].down = (char)(down != 0);
        g_ntouch_keys++;
    }
    pthread_mutex_unlock(&g_touch_lock);
}

int touch_state(void) { return (g_touch_menu ? 1 : 0) | (g_touch_ready ? 2 : 0); }

void touch_menu_override(int on) { g_menu_override = on != 0; }

// оси экранных стиков: поверх физического геймпада — тот, что отклонён сильнее
static float g_touch_ax[4];

// Диагностика ввода в лог (Android — всегда, иначе SNX_LOG_TOUCH=1): первые изменения экранного геймпада и касания
// меню, раз в 10 с — сводка: идёт ли цикл ввода, окно с фокусом, меню, опросы DirectInput игрой. По ней видно,
// где теряется ввод (жалоба «при втором запуске не работает управление»).
static int log_touch(void)
{
    static int on = -1;
    if (on < 0) {
#ifdef __ANDROID__
        on = 1;
#else
        on = getenv("SNX_LOG_TOUCH") != NULL;
#endif
    }
    return on;
}

static unsigned g_diag_pad, g_diag_taps, g_diag_ticks;

static void diag_tick(void)
{
    static u32 next;
    g_diag_ticks++;
    u32 now = SDL_GetTicks();
    if (!log_touch() || (s32)(now - next) < 0)
        return;
    next = now + 10000;
    unsigned st[4];
    dinput8_stats(st);
    int x = 0, y = 0;
    int ready = d3d8_window_to_target(0.5f, 0.5f, &x, &y);
    fprintf(stderr, "[touch] t=%us ticks=%u focus=%x menu=%d ready=%d center=%d,%d pad=%u taps=%u buttons=%x "
                    "lx=%.2f ly=%.2f | dinput: mouse %u joy %u unacquired %u acquire %u\n",
            now / 1000, g_diag_ticks, user32_focus(), g_menu, ready, x, y, g_diag_pad, g_diag_taps, g_touch_applied,
            (double)g_touch_ax[0], (double)g_touch_ax[1], st[0], st[1], st[2], st[3]);
    for (int i = 0; i < MAX_PADS; i++)  // геймпады: правый стик сейчас и число событий осей (камера «пропадает»)
        if (g_pads[i].id >= 0 && g_pads[i].events)
            fprintf(stderr, "[pad] #%d axis events %u rx=%.2f ry=%.2f lx=%.2f ly=%.2f\n", (int)g_pads[i].id,
                    g_pads[i].events, (double)g_pads[i].axis[SDL_CONTROLLER_AXIS_RIGHTX],
                    (double)g_pads[i].axis[SDL_CONTROLLER_AXIS_RIGHTY], (double)g_pads[i].axis[0],
                    (double)g_pads[i].axis[1]);
}


static void touch_tick(void)
{
    u32 buttons;
    float look_x, look_y;
    int nkeys;
    short vks[32];
    char downs[32];
    static float prev_ax[4];
    pthread_mutex_lock(&g_touch_lock);
    buttons = g_touch_buttons;
    memcpy(g_touch_ax, g_touch_axis, sizeof g_touch_ax);
    look_x = g_look_x;
    look_y = g_look_y;
    g_look_x = g_look_y = 0;
    nkeys = g_ntouch_keys;
    for (int i = 0; i < nkeys; i++) {
        vks[i] = g_touch_keys[i].vk;
        downs[i] = g_touch_keys[i].down;
    }
    g_ntouch_keys = 0;
    pthread_mutex_unlock(&g_touch_lock);
    int x, y;
    g_touch_ready = d3d8_window_to_target(0.5f, 0.5f, &x, &y);
    u32 diff = buttons ^ g_touch_applied;
    if ((diff || memcmp(g_touch_ax, prev_ax, sizeof prev_ax)) && log_touch() && g_diag_pad++ < 12)
        fprintf(stderr, "[touch] pad buttons=%x lx=%.2f ly=%.2f focus=%x menu=%d\n", buttons, (double)g_touch_ax[0],
                (double)g_touch_ax[1], user32_focus(), g_menu);
    memcpy(prev_ax, g_touch_ax, sizeof prev_ax);
    for (int b = 0; diff && b < PB_N; b++)
        if (diff & (1u << b))
            pad_button(b, (buttons >> b) & 1);
    g_touch_applied = buttons;
    for (int i = 0; i < nkeys; i++)
        key((u32)vks[i], downs[i]);
    // камера пальцем: смещение мыши DirectInput (в меню — не нужно, там касание — щелчок)
    static float rx_, ry_;
    if (!g_menu && (look_x != 0.0f || look_y != 0.0f)) {
        rx_ += look_x;
        ry_ += (g_invert_y ? -look_y : look_y);
        int dx = (int)rx_, dy = (int)ry_;
        rx_ -= (float)dx;
        ry_ -= (float)dy;
        if (dx || dy)
            dinput8_mouse_move(dx, dy, 0);
    }
}

static float axis_of(int a)
{
    float p = pad_axis(a), t = a < 4 ? g_touch_ax[a] : 0.0f;
    return fabsf(t) > fabsf(p) ? t : p;
}

void input_tick(void)
{
    scripted_keys();
    touch_tick();
    diag_tick();
    rumble_tick();
    static u64 last;
    static float fx, fy;  // дробные остатки смещения мыши
    u64 now = SDL_GetPerformanceCounter();
    float dt = last ? (float)(now - last) / (float)SDL_GetPerformanceFrequency() : 0.0f;
    last = now;
    if (dt > 0.1f)
        dt = 0.1f;
    click_tick();
    // главное меню Spider-Man 2 — Bink-заставка без GUIController: без этого признака касания
    // перехватываются экранным геймпадом и пункты «Начать новую игру» не нажимаются
    int menu = game_menu_open(NULL) || g_menu_override || bink_menu_movie_active();
    g_touch_menu = menu;
    if (menu != g_menu) {
        release_all();
        g_menu = menu;
        g_nav_dir = -1;
        if (!menu)
            g_joy_frozen = 0;
    }
    // курки -> кнопки ZL/ZR
    for (int i = 0; i < 2; i++) {
        int on = pad_axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT + i) > (g_trig[i] ? 0.3f : 0.5f);
        if (on != g_trig[i])
            pad_button(PB_ZL + i, g_trig[i] = on);
    }
    if (qm_is_open()) {
        static const float zero[6];
        dinput8_joy(zero, g_menu ? (g_joy_frozen & 0xFFF) : 0, -1);
        if (g_qm_dir >= 0 && (s32)(SDL_GetTicks() - g_qm_next) >= 0) {
            g_qm_next = SDL_GetTicks() + 120;
            qm_adjust(g_qm_dir);
        }
        return;
    }
    float lx, ly, rx, ry;
    stick(axis_of(SDL_CONTROLLER_AXIS_LEFTX), axis_of(SDL_CONTROLLER_AXIS_LEFTY), 0, &lx, &ly);
    stick(axis_of(SDL_CONTROLLER_AXIS_RIGHTX), axis_of(SDL_CONTROLLER_AXIS_RIGHTY), 1, &rx, &ry);
    if (!g_menu) {
        // джойстик: lX lY — левый стик, lRy lRz (JoyU, JoyV) — правый; крестовина — POV
        static const int pov_of[16] = {-1, 0, 18000, -1, 27000, 31500, 22500, 27000,
                                       9000, 4500, 13500, 9000, -1, 0, 18000, -1};
        float ax[6] = {lx * g_move_speed, ly * g_move_speed, 0, 0, rx * g_cam_speed,
                       (g_invert_y ? -ry : ry) * g_cam_speed};
        if (g_cam_mouse) {  // камера — смещениями мыши, как пальцем (см. g_cam_mouse)
            static float cfx, cfy;
            float k = g_cam_speed * (1200.0f / 72.0f) * g_look_sens * dt;
            ax[4] = ax[5] = 0.0f;
            cfx += rx * k;
            cfy += (g_invert_y ? -ry : ry) * k;
            if (rx == 0.0f && ry == 0.0f)
                cfx = cfy = 0.0f;
            int dx = (int)cfx, dy = (int)cfy;
            cfx -= (float)dx;
            cfy -= (float)dy;
            if (dx || dy)
                dinput8_mouse_move(dx, dy, 0);
        }
        dinput8_joy(ax, (g_joy_buttons | g_joy_latch) & 0xFFF, pov_of[g_joy_buttons >> 16 & 15]);
        g_joy_latch = 0;
        return;
    }
    static const float zero[6];
    dinput8_joy(zero, g_joy_frozen & 0xFFF, -1);
    float gx, gy;
    if (game_menu_cursor_moved(&gx, &gy))  // курсор GUI переставлен на элемент в фокусе
        sync_cursor(gx, gy);
    if (g_nav_dir >= 0 && (s32)(SDL_GetTicks() - g_nav_next) >= 0) {
        g_nav_next = SDL_GetTicks() + 120;
        nav(g_nav_dir, 1);
    }
    // меню: оба стика -> курсор (смещения мыши DirectInput); курсор виден, когда им водят
    float cx = fabsf(lx) > fabsf(rx) ? lx : rx, cy = fabsf(ly) > fabsf(ry) ? ly : ry;
    if (cx == 0.0f && cy == 0.0f)
        return;
    game_menu_cursor(1);
    fx += cx * 1200.0f * dt;
    fy += cy * 1200.0f * dt;
    int dx = (int)fx, dy = (int)fy;
    fx -= (float)dx;
    fy -= (float)dy;
    g_cu += cx * 0.7f * dt;
    g_cv += cy * 0.9f * dt;
    g_cu = g_cu < 0.0f ? 0.0f : g_cu > 1.0f ? 1.0f : g_cu;
    g_cv = g_cv < 0.0f ? 0.0f : g_cv > 1.0f ? 1.0f : g_cv;
    if (dx || dy)
        dinput8_mouse_move(dx, dy, 0);
    u32 hwnd = user32_focus();
    if (hwnd)
        user32_post(hwnd, WM_MOUSEMOVE, g_buttons, cursor_lparam());
}

void input_event(const void *p)
{
    const SDL_Event *e = p;
    u32 hwnd = user32_focus();
    switch (e->type) {
    case SDL_QUIT:
        if (hwnd)
            user32_post(hwnd, WM_CLOSE, 0, 0);
        else
            exit(0);
        break;
    case SDL_KEYDOWN:
    case SDL_KEYUP: {
        u32 vk = vk_of(e->key.keysym.scancode);
        if (!vk || !hwnd)
            break;
        int down = e->type == SDL_KEYDOWN, alt = (e->key.keysym.mod & KMOD_ALT) != 0;
        user32_set_key(vk, down);
        u32 lp = 1u | (e->key.repeat ? 1u << 30 : 0) | (down ? 0 : 3u << 30);
        user32_post(hwnd, alt ? (down ? WM_SYSKEYDOWN : WM_SYSKEYUP) : (down ? WM_KEYDOWN : WM_KEYUP), vk, lp);
        break;
    }
    case SDL_TEXTINPUT:
        if (!hwnd)
            break;
        // Декодирование UTF-8 -> UTF-16 (BMP) для WM_CHAR (работает кириллица с клавиатуры на macOS)
        for (const u8 *s = (const u8 *)e->text.text; *s;) {
            u32 cp = 0;
            if (*s < 0x80u) {
                cp = *s++;
            } else if ((*s & 0xE0u) == 0xC0u && (s[1] & 0xC0u) == 0x80u) {
                cp = ((u32)(s[0] & 0x1Fu) << 6) | (u32)(s[1] & 0x3Fu);
                s += 2;
                if (cp < 0x80u)
                    cp = 0;
            } else if ((*s & 0xF0u) == 0xE0u && (s[1] & 0xC0u) == 0x80u && (s[2] & 0xC0u) == 0x80u) {
                cp = ((u32)(s[0] & 0x0Fu) << 12) | ((u32)(s[1] & 0x3Fu) << 6) | (u32)(s[2] & 0x3Fu);
                s += 3;
                if (cp < 0x800u || (cp >= 0xD800u && cp <= 0xDFFFu))
                    cp = 0;
            } else if ((*s & 0xF8u) == 0xF0u && (s[1] & 0xC0u) == 0x80u && (s[2] & 0xC0u) == 0x80u &&
                       (s[3] & 0xC0u) == 0x80u) {
                s += 4;  // вне BMP: 16-битный WM_CHAR пропускает суррогатные пары
            } else {
                s++;
            }
            if (cp > 0 && cp <= 0xFFFFu)
                user32_post(hwnd, WM_CHAR, cp, 1);
        }
        break;
    case SDL_FINGERDOWN:  // сенсорный экран (Switch в руках): в меню — курсор и щелчок, в игре — ничего
    case SDL_FINGERUP:
    case SDL_FINGERMOTION:
        if (e->type == SDL_FINGERDOWN && log_touch() && g_diag_taps++ < 12) {
            int x = 0, y = 0;
            int ok = d3d8_window_to_target(e->tfinger.x, e->tfinger.y, &x, &y);
            fprintf(stderr, "[touch] finger %.2f,%.2f -> %d,%d (%d) menu=%d focus=%x\n", (double)e->tfinger.x,
                    (double)e->tfinger.y, x, y, ok, g_menu, user32_focus());
        }
        if (!g_menu || qm_is_open())
            break;
        if (e->type == SDL_FINGERDOWN)
            touch(e->tfinger.x, e->tfinger.y);
        else if (e->type == SDL_FINGERUP)
            click_release();
        else {  // палец ведут (ползунок): курсор GUI за ним
            int x, y;
            if (d3d8_window_to_target(e->tfinger.x, e->tfinger.y, &x, &y)) {
                sync_cursor((float)x, (float)y);
                game_menu_point((float)x, (float)y);
            }
        }
        break;
    case SDL_MOUSEMOTION:
        if (e->motion.which == SDL_TOUCH_MOUSEID)  // мышь, которую SDL изображает из касаний
            break;
        dinput8_mouse_move(e->motion.xrel, e->motion.yrel, 0);
        if (hwnd)
            user32_post(hwnd, WM_MOUSEMOVE, g_buttons, mouse_lparam(e->motion.x, e->motion.y));
        break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP: {
        if (e->button.which == SDL_TOUCH_MOUSEID)
            break;
        int b = e->button.button;
        int idx = b == SDL_BUTTON_LEFT ? 0 : b == SDL_BUTTON_RIGHT ? 1 : b == SDL_BUTTON_MIDDLE ? 2 : -1;
        if (idx >= 0)
            mouse_button(idx, e->type == SDL_MOUSEBUTTONDOWN, mouse_lparam(e->button.x, e->button.y));
        break;
    }
    case SDL_MOUSEWHEEL:
        wheel(e->wheel.y);
        break;
    case SDL_CONTROLLERDEVICEADDED: {
        SDL_GameController *gc = SDL_GameControllerOpen(e->cdevice.which);
        SDL_JoystickID id = gc ? SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gc)) : -1;
        int slot = id >= 0 ? pad_slot(id, 1) : -1;
        if (slot >= 0 && !g_pads[slot].gc)
            g_pads[slot].gc = gc;
        else if (gc && slot >= 0)
            SDL_GameControllerClose(gc);  // уже открыт (SDL считает ссылки): лишнюю ссылку — обратно
        char *map = gc ? SDL_GameControllerMapping(gc) : NULL;
        fprintf(stderr, "[input] pad added: #%d \"%s\" slot %d mapping %s\n", (int)id,
                gc ? SDL_GameControllerName(gc) : "?", slot, map ? map : "-");
        SDL_free(map);
        break;
    }
    case SDL_CONTROLLERDEVICEREMOVED: {  // отключили: оси — в ноль, нажатое — отпустить
        int slot = pad_slot(e->cdevice.which, 0);
        fprintf(stderr, "[input] pad removed: #%d slot %d\n", (int)e->cdevice.which, slot);
        if (slot < 0)
            break;
        if (g_pads[slot].gc)
            SDL_GameControllerClose(g_pads[slot].gc);
        memset(&g_pads[slot], 0, sizeof g_pads[slot]);
        g_pads[slot].id = -1;
        for (int b = 0; b < PB_N; b++)
            if (g_held[b])
                pad_button(b, 0);
        g_trig[0] = g_trig[1] = 0;
        g_touch_applied = 0;  // кнопки, которые держит палец, нажмутся заново в touch_tick
        break;
    }
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP:
        pad_button(by_label(e->cbutton.button), e->type == SDL_CONTROLLERBUTTONDOWN);
        break;
    case SDL_CONTROLLERAXISMOTION: {
        int slot = pad_slot(e->caxis.which, 1);
        if (slot >= 0 && e->caxis.axis < SDL_CONTROLLER_AXIS_MAX) {
            g_pads[slot].axis[e->caxis.axis] = (float)e->caxis.value / 32767.0f;
            g_pads[slot].events++;
        }
        break;
    }
    default:
        break;
    }
}
