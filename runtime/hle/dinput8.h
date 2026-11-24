// Ввод хоста -> DirectInput 8 гостя (мышь: относительные смещения и кнопки 0-7; геймпад).
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void dinput8_mouse_move(int dx, int dy, int dz);
void dinput8_mouse_button(int i, int down);
// Геймпад: оси lX lY lZ lRx lRy lRz в единицах, которые увидит игра (WinDrv делит (v - 32768) на 65535 —
// здесь значение заранее умножено, так что «Axis aBaseY» в User.ini получит ровно axis[i]), кнопки 0-31 битами,
// POV — сотые доли градуса по часовой от «вверх» или -1.
void dinput8_joy(const float axis[6], unsigned buttons, int pov);
// кнопки, как их увидит очередной опрос игры (GetDeviceState): нажатые сейчас и нажатые после прошлого опроса —
// короткое нажатие между двумя опросами не теряется (двойной прыжок «через раз»). Сбрасывает запомненные.
unsigned dinput8_joy_poll_buttons(void);
// диагностика: [0] опросы мыши, [1] опросы джойстика, [2] из них без захвата (Acquire), [3] вызовы Acquire
void dinput8_stats(unsigned out[4]);

#ifdef __cplusplus
}
#endif
