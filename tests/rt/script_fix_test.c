// Правка байткода Shrek.PostLoadGame (runtime/script_fix.c) на синтетическом байткоде той же формы, что в памяти
// игры: ссылки на объекты и FName — по 4 байта, переходы — смещения в Script.
#include <stdio.h>
#include <string.h>

#include "runtime/script_fix.h"

static int fails;
#define CHECK(x)                                                         \
    do {                                                                 \
        if (!(x)) {                                                      \
            fprintf(stderr, "%s:%d: FAIL %s\n", __FILE__, __LINE__, #x); \
            fails++;                                                     \
        }                                                                \
    } while (0)

static u8 buf[512];
static u32 len;

static void b(u8 v) { buf[len++] = v; }
static void w16(u32 v) { b((u8)v), b((u8)(v >> 8)); }
static void w32(u32 v) { w16(v & 0xFFFF), w16(v >> 16); }
static const u32 LABEL = 0x12345678;  // FuturePlayerLabel (StrProperty)
static const u32 SAVED = 0x0BADF00D;  // SavedTag

// тело функции; mod — с веткой «if (!IsInBonusLevel()) { FuturePlayerLabel = ""; return; }» (как в SHGame.u:
// выход на обычных уровнях, её правка не трогает). -> смещения: *chk — if (FuturePlayerLabel == ""), *ret_mod —
// return этой ветки, *saved — SavedTag = ...
static void build(int mod, u32 *chk, u32 *ret_mod, u32 *saved)
{
    len = 0;
    // foreach ... (упрощённо: что-то перед проверкой)
    b(0x0F), b(0x01), w32(SAVED), b(0x21), w32(77);  // SavedTag = 'X'
    *ret_mod = 0;
    if (mod) {
        u32 j = len;
        b(0x07), w16(0), b(0x81), b(0x1B), w32(500), b(0x16), b(0x16);  // if (!IsInBonusLevel()) goto
        b(0x0F), b(0x01), w32(LABEL), b(0x1F), b(0x00);                 // FuturePlayerLabel = ""
        *ret_mod = len;
        b(0x04), b(0x0B);  // return
        buf[j + 1] = (u8)len, buf[j + 2] = (u8)(len >> 8);
    }
    *chk = len;
    b(0x07), w16(*chk + 14), b(0x7A), b(0x01), w32(LABEL), b(0x1F), b(0x00), b(0x16);  // if (!(L == "")) goto A
    b(0x04), b(0x0B);                                                                  // return
    b(0x1B), w32(600), b(0x01), w32(LABEL), b(0x16);                                   // A: SwitchControlToPawn(L)
    b(0x0F), b(0x01), w32(LABEL), b(0x1F), b(0x00);                                    // L = ""
    *saved = len;
    b(0x0F), b(0x01), w32(SAVED), b(0x19), b(0x01), w32(1), w16(5), b(0x01), w32(2);  // SavedTag = ...Tag
    b(0x0F), b(0x19), b(0x01), w32(1), w16(5), b(0x01), w32(2), b(0x21), w32(88);     // ...Tag = 'MainPlayer'
    b(0x04), b(0x0B);  // как в SHGame.u: без EX_EndOfScript
}

int main(void)
{
    u32 chk, rm, saved;
    for (int mod = 0; mod < 2; mod++) {
        build(mod, &chk, &rm, &saved);
        u8 orig[sizeof buf];
        memcpy(orig, buf, len);
        CHECK(script_fix_post_load_game(buf, len) == 1);
        CHECK((u32)(buf[chk + 1] | buf[chk + 2] << 8) == saved);  // пустая метка — сразу к SavedTag
        CHECK(buf[chk + 3] == 0x7B);                          // == -> !=
        CHECK(buf[chk + 12] == 0x0B && buf[chk + 13] == 0x0B);  // return -> nothing
        if (mod)
            CHECK(buf[rm] == 0x04 && buf[rm + 1] == 0x0B);  // обычные уровни — как в игре
        u32 diff = 0;
        for (u32 i = 0; i < len; i++)
            diff += buf[i] != orig[i];
        CHECK(diff == 3u);  // младший байт перехода, условие, return
        CHECK(script_fix_post_load_game(buf, len) == 2);  // повторно — уже исправлено
    }
    // чужой байткод: переход не за return, другое условие — не трогать
    build(0, &chk, &rm, &saved);
    buf[chk + 1]++;
    u8 orig[sizeof buf];
    memcpy(orig, buf, len);
    CHECK(script_fix_post_load_game(buf, len) == 0);
    CHECK(!memcmp(orig, buf, len));
    build(0, &chk, &rm, &saved);
    buf[chk + 3] = 0x7C;
    CHECK(script_fix_post_load_game(buf, len) == 0);
    CHECK(script_fix_post_load_game(NULL, 0) == 0);
    if (!fails)
        printf("script_fix_test: ok\n");
    return fails != 0;
}
