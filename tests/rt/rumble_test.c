// Синтетическая проверка game_rumble_from_health (runtime/game/rumble.c): импульс при уроне и гибели, тишина
// при росте здоровья, шуме float, мёртвой пешке и мусоре в памяти гостя.
#include <math.h>
#include <stdio.h>

#include "runtime/game/rumble.h"

static int g_fail;
#define CHECK(x)                                                         \
    do {                                                                 \
        if (!(x)) {                                                      \
            fprintf(stderr, "%s:%d: FAIL %s\n", __FILE__, __LINE__, #x); \
            g_fail++;                                                    \
        }                                                                \
    } while (0)

int main(void)
{
    game_rumble_pulse p = {0};
    // рост, без изменений, шум float и медленное убывание (< 1% максимума за кадр) — тишина
    CHECK(!game_rumble_from_health(50.0f, 60.0f, 100.0f, &p));
    CHECK(!game_rumble_from_health(50.0f, 50.0f, 100.0f, &p));
    CHECK(!game_rumble_from_health(50.0f, 49.9999f, 100.0f, &p));
    CHECK(!game_rumble_from_health(50.0f, 49.5f, 100.0f, &p));
    // мёртвая пешка и мусор
    CHECK(!game_rumble_from_health(0.0f, -10.0f, 100.0f, &p));
    CHECK(!game_rumble_from_health(NAN, 10.0f, 100.0f, &p));
    CHECK(!game_rumble_from_health(10.0f, INFINITY, 100.0f, &p));
    // лёгкий удар: ощутимо, но слабее сильного
    CHECK(game_rumble_from_health(100.0f, 95.0f, 100.0f, &p));
    game_rumble_pulse light = p;
    CHECK(light.high >= 0x7000 && light.high < 0xFFFF && light.low < light.high && light.ms >= 140 && light.ms < 400);
    // четверть полоски — во всю силу, длительность ограничена
    CHECK(game_rumble_from_health(100.0f, 75.0f, 100.0f, &p));
    CHECK(p.high == 0xFFFF && p.ms <= 400 && p.ms > light.ms);
    // гибель
    CHECK(game_rumble_from_health(10.0f, 0.0f, 100.0f, &p));
    CHECK(p.low == 0xFFFF && p.high == 0xFFFF && p.ms == 600);
    // максимум неизвестен или мусор — считаем 100
    CHECK(game_rumble_from_health(100.0f, 95.0f, 0.0f, &p) && p.high == light.high);
    CHECK(game_rumble_from_health(100.0f, 95.0f, NAN, &p) && p.high == light.high);
    // масштаб по максимуму: 5 из 1000 — меньше 1%, тишина
    CHECK(!game_rumble_from_health(1000.0f, 995.0f, 1000.0f, &p));
    // попадания героя: нет — тишина; предмет — лёгкий короткий; враг — сильнее; несколько врагов — ещё сильнее
    CHECK(!game_rumble_from_hits(0, 0, &p));
    CHECK(game_rumble_from_hits(0, 1, &p));
    game_rumble_pulse prop = p;
    CHECK(game_rumble_from_hits(1, 0, &p));
    game_rumble_pulse one = p;
    CHECK(game_rumble_from_hits(3, 2, &p));
    CHECK(prop.high < one.high && prop.ms < one.ms && one.high < p.high && one.ms <= p.ms);
    CHECK(p.high < light.high || p.ms < light.ms);  // удар героя слабее, чем урон по нему
    CHECK(one.low < one.high && p.ms <= 150);
    if (g_fail)
        return 1;
    printf("rumble_test: OK\n");
    return 0;
}
