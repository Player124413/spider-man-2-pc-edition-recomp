// Кнопки геймпада в HLE DirectInput (runtime/hle/dinput8.c): короткое нажатие, которое хост успел и нажать, и
// отпустить между двумя опросами игры, игра всё равно видит — один опрос нажатой, следующий отпущенной. Прежде
// такое нажатие терялось: хост обновляет состояние каждые ~5 мс, игра читает раз в кадр (жалоба: двойной прыжок
// с геймпада — через раз). Удержание — нажата во всех опросах; отпускание — со следующего опроса.
#include <stdio.h>

#include "runtime/hle/dinput8.h"

static int g_fail;

#define CHECK(cond, ...)                                       \
    do {                                                       \
        if (!(cond)) {                                         \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                      \
            fputc('\n', stderr);                               \
            g_fail = 1;                                        \
        }                                                      \
    } while (0)

int main(void)
{
    static const float ax[6];
    unsigned b;
    // короткое нажатие B (бит 1) между опросами
    dinput8_joy(ax, 0, -1);
    (void)dinput8_joy_poll_buttons();
    dinput8_joy(ax, 2, -1);
    dinput8_joy(ax, 0, -1);
    b = dinput8_joy_poll_buttons();
    CHECK(b == 2, "short press lost: %x", b);
    b = dinput8_joy_poll_buttons();
    CHECK(b == 0, "short press stuck: %x", b);
    // удержание A (бит 0): нажата во всех опросах, пока держат
    dinput8_joy(ax, 1, -1);
    for (int i = 0; i < 3; i++) {
        b = dinput8_joy_poll_buttons();
        CHECK(b == 1, "held button, poll %d: %x", i, b);
    }
    dinput8_joy(ax, 0, -1);
    b = dinput8_joy_poll_buttons();
    CHECK(b == 0, "released button still pressed: %x", b);
    // два коротких нажатия подряд (двойной прыжок), опрос после каждого
    for (int i = 0; i < 2; i++) {
        dinput8_joy(ax, 2, -1);
        dinput8_joy(ax, 0, -1);
        b = dinput8_joy_poll_buttons();
        CHECK(b == 2, "tap %d lost: %x", i, b);
        b = dinput8_joy_poll_buttons();
        CHECK(b == 0, "tap %d stuck: %x", i, b);
    }
    if (!g_fail)
        printf("dinput_test: ok\n");
    return g_fail;
}
