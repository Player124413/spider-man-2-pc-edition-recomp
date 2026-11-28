// Вибрация геймпада по событиям игры. На ПК Shrek 2 отдачи нет: IFC23 (Immersion) без устройства, скрипты SHGame
// ForceFeedback не вызывают. Порт следит за здоровьем пешки локального игрока (Engine.Pawn.Health — float) и
// толкает импульс в хостовый хук вибрации (hle_rumble_hook, host/boot/input.c) при уроне и при гибели; удары
// героя по врагам и предметам — по флагам «задет этим ударом» у целей (rumble.c).
#pragma once
#include "runtime/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    u16 low, high;  // сила низко- и высокочастотного мотора, 0..0xFFFF
    u32 ms;         // длительность
} game_rumble_pulse;

// Импульс по изменению здоровья prev -> cur при максимуме max (<= 0 — 100). 0 — вибрации нет (здоровье не
// уменьшилось, изменение — шум float или пешка уже была мертва).
int game_rumble_from_health(float prev, float cur, float max, game_rumble_pulse *out);

// Импульс за попадания героя в этом кадре: pawns — по врагам, props — по предметам. 0 — попаданий нет.
int game_rumble_from_hits(int pawns, int props, game_rumble_pulse *out);

// Раз в кадр в потоке гостя (d3d8_present): поиск PlayerController с Viewport, пешка, здоровье -> импульс.
void game_rumble_frame(void);
// Есть ли в мире живая пешка локального игрока: признак того, что началась игра (а не главное меню).
int game_player_active(void);

#ifdef __cplusplus
}
#endif
