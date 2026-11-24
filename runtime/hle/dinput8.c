// HLE DirectInput 8 (WinDrv: мышь — относительные смещения и кнопки, джойстик — перечисление через EnumDevices;
// клавиатура идёт оконными сообщениями). Состояние мыши и джойстика наполняет хост (dinput8_mouse_*, dinput8_joy).
// Джойстик — один геймпад (DIJOYSTATE2): WinDrv при UseJoystick=True читает оси lX..lRz как JoyX/JoyY/JoyZ/JoyR/
// JoyU/JoyV (значение (v - 32768) / 65535 уходит в UInput::DirectAxis), кнопки 0-11 — Joy1..Joy12, крестовину
// (POV 0) — Joy13..Joy16 (вверх, вправо, вниз, влево).
#include <math.h>
#include <pthread.h>
#include <stdlib.h>

#include "runtime/hle/com.h"
#include "runtime/hle/dinput8.h"

#define DI_OK 0u
#define DIERR_INVALIDPARAM 0x80070057u
#define DIERR_DEVICENOTREG 0x80040154u
#define DIERR_NOTACQUIRED 0x8007000Cu

enum { DEV_MOUSE = 1, DEV_KEYBOARD = 2, DEV_JOY = 3 };

typedef struct {
    int kind, acquired;
    u32 data_size;  // из SetDataFormat
} didev;

// Состояние мыши: накопленные смещения (GetDeviceState забирает и обнуляет), кнопки, буфер событий
#define MQ 256
static pthread_mutex_t g_in_lock = PTHREAD_MUTEX_INITIALIZER;
static s32 g_dx, g_dy, g_dz;
static u8 g_btn[8];
static u32 g_ev[MQ][2];  // ofs, data
static int g_ev_head, g_ev_count;
static u32 g_seq;

static void event(u32 ofs, u32 data)
{
    if (g_ev_count == MQ) {  // переполнение: самое старое теряется (DI_BUFFEROVERFLOW)
        g_ev_head = (g_ev_head + 1) % MQ;
        g_ev_count--;
    }
    g_ev[(g_ev_head + g_ev_count) % MQ][0] = ofs;
    g_ev[(g_ev_head + g_ev_count) % MQ][1] = data;
    g_ev_count++;
}

void dinput8_mouse_move(int dx, int dy, int dz)
{
    pthread_mutex_lock(&g_in_lock);
    g_dx += dx;
    g_dy += dy;
    g_dz += dz;
    if (dx)
        event(0, (u32)dx);
    if (dy)
        event(4, (u32)dy);
    if (dz)
        event(8, (u32)dz);
    pthread_mutex_unlock(&g_in_lock);
}

void dinput8_mouse_button(int i, int down)
{
    if (i < 0 || i >= 8)
        return;
    pthread_mutex_lock(&g_in_lock);
    g_btn[i] = down ? 0x80 : 0;
    event(12u + (u32)i, down ? 0x80u : 0u);
    pthread_mutex_unlock(&g_in_lock);
}

// Состояние геймпада: оси в единицах игры (см. dinput8.h), кнопки, POV
static s32 g_joy_axis[6];
static u32 g_joy_buttons;
static u32 g_joy_sticky;  // нажатые после последнего GetDeviceState: игра увидит их хотя бы раз, даже если
                          // кнопку уже отпустили (хост обновляет состояние чаще, чем игра его читает)
static u32 g_joy_pov = 0xFFFFFFFFu;

void dinput8_joy(const float axis[6], u32 buttons, int pov)
{
    pthread_mutex_lock(&g_in_lock);
    for (int i = 0; i < 6; i++) {
        double v = 32768.0 + (double)axis[i] * 65535.0;
        g_joy_axis[i] = v > 2e9 ? 2000000000 : v < -2e9 ? -2000000000 : (s32)lrint(v);
    }
    g_joy_sticky |= buttons & ~g_joy_buttons;
    g_joy_buttons = buttons;
    g_joy_pov = pov < 0 ? 0xFFFFFFFFu : (u32)pov;
    pthread_mutex_unlock(&g_in_lock);
}

// кнопки для опроса игрой: нажатые сейчас и нажатые после прошлого опроса (вызывать под g_in_lock)
static u32 take_buttons(void)
{
    u32 b = g_joy_buttons | g_joy_sticky;
    g_joy_sticky = 0;
    return b;
}

u32 dinput8_joy_poll_buttons(void)
{
    pthread_mutex_lock(&g_in_lock);
    u32 b = take_buttons();
    pthread_mutex_unlock(&g_in_lock);
    return b;
}

// ================================================================ IDirectInputDevice8W
// Методы COM — stdcall с this в ARG(0): RET(n) считает и this.
static u32 g_vt_dev, g_vt_di;

#define DEV(c) ((didev *)com_host(ARG(0)))

static void dev_QueryInterface(CPU *c)
{
    wr32(g_mem, ARG(2), 0);
    RET(3, E_NOINTERFACE);
}

// GetCapabilities(DIDEVCAPS*): dwSize, dwFlags, dwDevType, dwAxes, dwButtons, dwPOVs, …
static void dev_GetCapabilities(CPU *c)
{
    didev *d = DEV(c);
    u32 p = ARG(1), size = rd32(g_mem, p);
    memset(g_mem + p + 4, 0, size > 4 && size < 64 ? size - 4 : 40);
    wr32(g_mem, p + 4, 1);  // DIDC_ATTACHED
    if (d && d->kind == DEV_MOUSE) {
        wr32(g_mem, p + 8, 0x12);  // DI8DEVTYPE_MOUSE
        wr32(g_mem, p + 12, 3);
        wr32(g_mem, p + 16, 3);
    } else if (d && d->kind == DEV_JOY) {
        wr32(g_mem, p + 8, 0x0115);  // DI8DEVTYPE_GAMEPAD, DI8DEVTYPEGAMEPAD_STANDARD
        wr32(g_mem, p + 12, 6);
        wr32(g_mem, p + 16, 12);
        wr32(g_mem, p + 20, 1);
    } else {
        wr32(g_mem, p + 8, 0x13);  // DI8DEVTYPE_KEYBOARD
        wr32(g_mem, p + 16, 128);
    }
    RET(2, DI_OK);
}

static void dev_GetProperty(CPU *c) { RET(3, DIERR_INVALIDPARAM); }
static void dev_SetProperty(CPU *c) { RET(3, DI_OK); }  // DIPROP_BUFFERSIZE, DIPROP_AXISMODE, …

// счётчики для диагностики ввода (dinput8_stats): опросы мыши и джойстика, из них без захвата; захваты
static _Atomic unsigned g_st_mouse, g_st_joy, g_st_unacq, g_st_acq;

void dinput8_stats(unsigned out[4])
{
    out[0] = g_st_mouse;
    out[1] = g_st_joy;
    out[2] = g_st_unacq;
    out[3] = g_st_acq;
}

static void dev_Acquire(CPU *c)
{
    didev *d = DEV(c);
    g_st_acq++;
    if (d)
        d->acquired = 1;
    RET(1, DI_OK);
}

static void dev_Unacquire(CPU *c)
{
    didev *d = DEV(c);
    if (d)
        d->acquired = 0;
    RET(1, DI_OK);
}

// GetDeviceState(cbData, lpvData): DIMOUSESTATE(2) = lX, lY, lZ, rgbButtons[4|8]
static void dev_GetDeviceState(CPU *c)
{
    didev *d = DEV(c);
    u32 cb = ARG(1), p = ARG(2);
    memset(g_mem + p, 0, cb);
    if (d && d->kind == DEV_MOUSE)
        g_st_mouse++;
    else if (d && d->kind == DEV_JOY)
        g_st_joy++;
    if (d && !d->acquired)
        g_st_unacq++;
    if (d && d->kind == DEV_MOUSE && cb >= 16) {
        pthread_mutex_lock(&g_in_lock);
        wr32(g_mem, p, (u32)g_dx);
        wr32(g_mem, p + 4, (u32)g_dy);
        wr32(g_mem, p + 8, (u32)g_dz);
        g_dx = g_dy = g_dz = 0;
        memcpy(g_mem + p + 12, g_btn, cb >= 20 ? 8 : 4);
        pthread_mutex_unlock(&g_in_lock);
    } else if (d && d->kind == DEV_JOY && cb >= 0x50) {
        // DIJOYSTATE(2): lX lY lZ lRx lRy lRz, rglSlider[2], rgdwPOV[4] (+0x20), rgbButtons[32|128] (+0x30)
        pthread_mutex_lock(&g_in_lock);
        for (u32 i = 0; i < 6; i++)
            wr32(g_mem, p + 4 * i, (u32)g_joy_axis[i]);
        wr32(g_mem, p + 0x18, 32768);
        wr32(g_mem, p + 0x1C, 32768);
        wr32(g_mem, p + 0x20, g_joy_pov);
        for (u32 i = 1; i < 4; i++)
            wr32(g_mem, p + 0x20 + 4 * i, 0xFFFFFFFFu);
        u32 b = take_buttons();
        for (u32 i = 0; i < 32; i++)
            g_mem[p + 0x30 + i] = b >> i & 1 ? 0x80 : 0;
        pthread_mutex_unlock(&g_in_lock);
    }
    RET(3, d && d->acquired ? DI_OK : DIERR_NOTACQUIRED);
}

// GetDeviceData(cbObjectData, rgdod, pdwInOut, dwFlags): DIDEVICEOBJECTDATA = ofs, data, time, seq[, appdata]
static void dev_GetDeviceData(CPU *c)
{
    didev *d = DEV(c);
    u32 cb = ARG(1), out = ARG(2), inout = ARG(3), peek = ARG(4) & 1;
    u32 want = rd32(g_mem, inout), n = 0;
    if (!d || !d->acquired) {
        wr32(g_mem, inout, 0);
        RET(5, DIERR_NOTACQUIRED);
    }
    if (d->kind == DEV_MOUSE) {
        pthread_mutex_lock(&g_in_lock);
        u32 now = (u32)(rt_ticks_ns() / 1000000u);
        for (; n < want && (int)n < g_ev_count; n++) {
            u32 *e = g_ev[(g_ev_head + (int)n) % MQ];
            if (out) {
                u32 r = out + n * cb;
                memset(g_mem + r, 0, cb);
                wr32(g_mem, r, e[0]);
                wr32(g_mem, r + 4, e[1]);
                wr32(g_mem, r + 8, now);
                wr32(g_mem, r + 12, g_seq + n);
            }
        }
        if (!peek) {
            g_ev_head = (g_ev_head + (int)n) % MQ;
            g_ev_count -= (int)n;
            g_seq += n;
            g_dx = g_dy = g_dz = 0;  // буферизованные смещения уже отданы
        }
        pthread_mutex_unlock(&g_in_lock);
    }
    wr32(g_mem, inout, n);
    RET(5, DI_OK);
}

// SetDataFormat(DIDATAFORMAT*): dwSize, dwObjSize, dwFlags, dwDataSize, dwNumObjs, rgodf
static void dev_SetDataFormat(CPU *c)
{
    didev *d = DEV(c);
    if (d)
        d->data_size = rd32(g_mem, ARG(1) + 12);
    RET(2, DI_OK);
}

static void dev_EnumObjects(CPU *c) { RET(4, DI_OK); }  // WinDrv задаёт так диапазоны осей — здесь они и так 0..65535
static void dev_SetEventNotification(CPU *c) { RET(2, DI_OK); }
static void dev_SetCooperativeLevel(CPU *c) { RET(3, DI_OK); }
static void dev_Poll(CPU *c) { RET(1, 1u); }  // DI_NOEFFECT: устройство не опрашивается

static void dev_destroy(void *p) { free(p); }

// ================================================================ IDirectInput8W
static int guid_sys(u32 g, u32 last)  // GUID_SysMouse 6F1D2B60-…, GUID_SysKeyboard 6F1D2B61-…
{
    return com_iid_eq(g, last, 0xD5A0, 0x11CF, 0xBFC7444553540000ull);
}

// Экземпляр (и продукт) нашего геймпада: {534E5850-4144-0000-0000-000000000001} («SNXPAD»)
static const u8 g_joy_guid[16] = {0x50, 0x58, 0x4E, 0x53, 0x44, 0x41, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
static int guid_joy(u32 g) { return !memcmp(g_mem + g, g_joy_guid, 16); }

// CreateDevice(REFGUID, IDirectInputDevice8W **, IUnknown *)
static void di_CreateDevice(CPU *c)
{
    int kind = guid_sys(ARG(1), 0x6F1D2B60) ? DEV_MOUSE : guid_sys(ARG(1), 0x6F1D2B61) ? DEV_KEYBOARD
               : guid_joy(ARG(1))           ? DEV_JOY
                                            : 0;
    if (!kind) {
        char s[64];
        com_iid_str(ARG(1), s, sizeof s);
        fprintf(stderr, "[dinput8] CreateDevice %s -> not registered\n", s);
        wr32(g_mem, ARG(2), 0);
        RET(4, DIERR_DEVICENOTREG);
    }
    didev *d = calloc(1, sizeof *d);
    d->kind = kind;
    wr32(g_mem, ARG(2), com_create(g_vt_dev, d, dev_destroy));
    RET(4, DI_OK);
}

// EnumDevices(dwDevType, callback, pvRef, dwFlags): для всех устройств или игровых (DI8DEVCLASS_ALL 0,
// DI8DEVCLASS_GAMECTRL 4, DI8DEVTYPE_JOYSTICK/GAMEPAD) — один геймпад. Обратный вызов (lpddi, pvRef) получает
// DIDEVICEINSTANCEW в стеке гостя: dwSize, guidInstance, guidProduct, dwDevType, tszInstanceName[260],
// tszProductName[260], guidFFDriver, wUsagePage, wUsage.
static void di_EnumDevices(CPU *c)
{
    u32 type = ARG(1) & 0xFF, cb = ARG(2), ref = ARG(3), esp = c->esp;
    if (cb && (type == 0 || type == 4 || type == 0x14 || type == 0x15)) {
        static const char name[] = "Switch Controller";
        u32 p = (c->esp -= 0x460);
        memset(g_mem + p, 0, 0x44C);
        wr32(g_mem, p, 0x44C);
        memcpy(g_mem + p + 4, g_joy_guid, 16);
        memcpy(g_mem + p + 0x14, g_joy_guid, 16);
        wr32(g_mem, p + 0x24, 0x0115);
        for (u32 i = 0; i < sizeof name; i++) {
            wr16(g_mem, p + 0x28 + 2 * i, (u8)name[i]);
            wr16(g_mem, p + 0x230 + 2 * i, (u8)name[i]);
        }
        wr16(g_mem, p + 0x448, 1);  // HID: Generic Desktop / Game Pad
        wr16(g_mem, p + 0x44A, 5);
        u32 args[2] = {p, ref};
        rt_guest_call(c, cb, 2, args);
        c->esp = esp;
    }
    RET(5, DI_OK);
}

static void di_GetDeviceStatus(CPU *c) { RET(2, guid_joy(ARG(1)) ? DI_OK : DIERR_DEVICENOTREG); }

static void di_QueryInterface(CPU *c)
{
    if (com_iid_eq(ARG(1), 0xBF798031, 0x483A, 0x4DA2, 0xAA995D64ED369700ull)) {  // IID_IDirectInput8W
        com_addref(ARG(0));
        wr32(g_mem, ARG(2), ARG(0));
        RET(3, S_OK);
    }
    wr32(g_mem, ARG(2), 0);
    RET(3, E_NOINTERFACE);
}

static void init_vtables(void)
{
    if (g_vt_di)
        return;
    guest_fn dev[32] = {
        dev_QueryInterface, com_AddRef, com_Release, dev_GetCapabilities, dev_EnumObjects, dev_GetProperty,
        dev_SetProperty, dev_Acquire, dev_Unacquire, dev_GetDeviceState, dev_GetDeviceData, dev_SetDataFormat,
        dev_SetEventNotification, dev_SetCooperativeLevel,
    };
    dev[25] = dev_Poll;
    g_vt_dev = com_vtable("IDirectInputDevice8W", dev, 32);
    guest_fn di[11] = {di_QueryInterface, com_AddRef, com_Release, di_CreateDevice, di_EnumDevices, di_GetDeviceStatus};
    g_vt_di = com_vtable("IDirectInput8W", di, 11);
}

// DirectInput8Create(hinst, version, riid, ppvOut, punkOuter)
void hle_dinput8_DirectInput8Create(CPU *c)
{
    HLE_ENTER("DirectInput8Create", 5);
    init_vtables();
    if (!com_iid_eq(ARG(2), 0xBF798031, 0x483A, 0x4DA2, 0xAA995D64ED369700ull))
        fprintf(stderr, "[dinput8] DirectInput8Create: interface is not IDirectInput8W\n");
    wr32(g_mem, ARG(3), com_create(g_vt_di, NULL, NULL));
    RET(5, DI_OK);
}
