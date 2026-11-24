// Исправления байткода UnrealScript игры в памяти (см. script_fix.h).
//
// Бонусные уровни (Beanstalk_bonus, _dawn, _knight), запущенные напрямую (выбор уровня лаунчера): камера стоит
// у ворот фермы, героя не видно, вступление рассказчика не идёт, таймер стоит. Причина — в скрипте игры
// (SHGame.Shrek.PostLoadGame, зовётся после загрузки карты):
//
//     foreach DynamicActors(class'ShHeroPawn', shhp)
//         if (shhp != None && shhp.Tag == 'MainPlayer') shhp.Tag = SavedTag;   // снять метку MainPlayer
//     if (FuturePlayerLabel == "") return;                                     // <- выход до возврата метки
//     SwitchControlToPawn(FuturePlayerLabel);
//     FuturePlayerLabel = "";
//     SavedTag = Level.PlayerHeroActor.Tag;
//     Level.PlayerHeroActor.Tag = 'MainPlayer';                                // вернуть метку герою
//
// Метку MainPlayer ищут камера и вступительная сценка. Если герой не пришёл с другого уровня (плакат
// «Разыскивается» переносит его через ServerTravel), FuturePlayerLabel пуст, и метка снимается без возврата.
// То же исправление — в Shrek 2 Enhanced Edition (SquareRoundOne/shrek2-enhanced-edition, tools/menupatch.py:
// «bonus level started directly ... lost the player»): при пустом FuturePlayerLabel пропускается только смена
// героя, метка возвращается.
//
// Байткод в памяти (ссылки на объекты и FName — по 4 байта, переходы — смещения в Script):
//     07 <u16 A> 7A 01 <L> 1F 00 16      if (!(FuturePlayerLabel == "")) goto A       ; A = это + 14
//     04 0B                              return
//  A: 1B <FName> 01 <L> 16                SwitchControlToPawn(FuturePlayerLabel)
//     0F 01 <L> 1F 00                    FuturePlayerLabel = ""
//  B: 0F ...                             SavedTag = Level.PlayerHeroActor.Tag
// Правка той же длины: 7A (==) -> 7B (!=), переход -> B, 04 0B (return) -> 0B 0B (nothing, nothing):
// пустая метка — переход к B, непустая — как раньше (смена героя, затем B). Как в Shrek 2 Enhanced Edition, больше
// ничего не меняется: ветка перед этим «if (!IsInBonusLevel()) { FuturePlayerLabel = ""; return; }» (81 — «!»)
// выходит только на обычных уровнях, и они остаются как в игре.
//
// Когда править: после UStruct::Link функции (байткод загружен) или класса Shrek (его функции), и при каждой новой
// загрузке: при смене карты сборка мусора может выгрузить SHGame.u, и он грузится заново (прямой запуск уровня —
// сначала карта Entry, затем сам уровень), а с ним и неисправленный байткод. Поэтому исправленная функция
// запоминается (объект и адрес байткода), а новая — правится снова. Дополнительно (если Link не застал байткод) —
// обход всех объектов (UObject::GObjObjects) при UEngine::Flush (загрузка карты) и раз в 2 с кадров, пока функция
// не найдена (script_fix_scan). SNX_SCRIPT_FIX=0 — не исправлять. Результат — строки [script] в snx.log.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "runtime/process.h"
#include "runtime/rt.h"
#include "runtime/script_fix.h"
#include "runtime/vmem.h"

static u32 le32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }

// if-переход с условием «FuturePlayerLabel <cond> ""» в p; -> смещение B (присваивание SavedTag) или 0
static u32 match_label_check(const u8 *s, u32 n, u32 p, u8 cond, u8 ret, u32 *label)
{
    if (p + 15 > n || s[p] != 0x07 || s[p + 3] != cond || s[p + 4] != 0x01 || s[p + 9] != 0x1F || s[p + 10] ||
        s[p + 11] != 0x16 || s[p + 12] != ret || s[p + 13] != 0x0B || s[p + 14] != 0x1B)
        return 0;
    u32 l = le32(s + p + 5), target = s[p + 1] | (u32)s[p + 2] << 8;
    for (u32 nm = 4; nm <= 8; nm += 4) {  // FName в байткоде — 4 байта (8 — на случай иной сборки)
        u32 a = p + 15 + nm, q = a + 6, b = q + 8;  // a: 01 <L> 16; q: 0F 01 <L> 1F 00; b: 0F
        if (b >= n)
            break;
        if (s[a] != 0x01 || le32(s + a + 1) != l || s[a + 5] != 0x16)
            continue;
        if (s[q] != 0x0F || s[q + 1] != 0x01 || le32(s + q + 2) != l || s[q + 6] != 0x1F || s[q + 7] || s[b] != 0x0F)
            continue;
        if (target != (cond == 0x7A ? p + 14 : b))
            continue;
        *label = l;
        return b;
    }
    return 0;
}

int script_fix_post_load_game(u8 *s, u32 n)
{
    if (!s || n < 40 || n > 0xFFFF)
        return 0;
    for (u32 p = 0; p + 40 <= n; p++) {
        u32 label;
        if (match_label_check(s, n, p, 0x7B, 0x0B, &label))
            return 2;
        u32 b = match_label_check(s, n, p, 0x7A, 0x04, &label);
        if (!b)
            continue;
        s[p + 1] = (u8)b;
        s[p + 2] = (u8)(b >> 8);
        s[p + 3] = 0x7B;
        s[p + 12] = 0x0B;
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------- объекты гостя
// Раскладка этой сборки UE2 (как runtime/game/uobj.c): UObject — Outer +0x18, Name +0x20, Class +0x24;
// UField — Next +0x30; UStruct — Children +0x40, Script (TArray<BYTE>: данные, число, ёмкость) +0x4C.
#define OBJ_OUTER 0x18u
#define OBJ_NAME 0x20u
#define OBJ_CLASS 0x24u
#define FIELD_NEXT 0x30u
#define STRUCT_CHILDREN 0x40u
#define STRUCT_SCRIPT 0x4Cu

static u32 g_names;         // FName::Names (TArray<FNameEntry*>)
static int g_off;           // SNX_SCRIPT_FIX=0
static u32 g_pending;       // Shrek.PostLoadGame без загруженного байткода: проверяется на каждом Link
static u32 g_done_fn, g_done_data;  // последняя исправленная (или проверенная) функция и её байткод
static int g_seen;          // функция хоть раз найдена (для обхода кадров)

static int committed(u32 a, u32 size)
{
    vm_info mi;
    return a >= 0x10000 && vm_query(a, &mi) && mi.state == VM_MEM_COMMIT && a + size <= mi.base + mi.size;
}

static int name_is(u32 obj, const char *name)
{
    if (!obj || !committed(obj, 0x28))
        return 0;
    u32 idx = rd32(g_mem, obj + OBJ_NAME);
    if (idx >= rd32(g_mem, g_names + 4))
        return 0;
    u32 e = rd32(g_mem, rd32(g_mem, g_names) + 4 * idx);
    if (!e || !committed(e, 0xC))
        return 0;
    for (u32 i = 0;; i++) {
        u16 ch = rd16(g_mem, e + 0xC + 2 * i);
        unsigned a = (unsigned char)name[i];
        if ((ch | 0x20u) != (a | 0x20u) || (!ch) != (!a))
            return 0;
        if (!ch)
            return 1;
    }
}

// Script функции: TArray (данные, число, ёмкость) на STRUCT_SCRIPT (*exact = 1) — данные в окне гостя, число
// правдоподобно; иначе (другая раскладка) первый TArray в полях UStruct, чей байткод кончается на 0x53
// (EX_EndOfScript) или на 04 0B (return; — так кончается Script функций в SHGame.u: без 0x53). На STRUCT_SCRIPT конец
// не проверяется: на устройстве (0.1.0-35) там был байткод в 201 байт (…04 0B), но первая версия требовала 0x53
// последним байтом, ёмкость >= числа и память по VirtualQuery и ждала «загрузки» вечно. Узнаёт ли байткод правка —
// решает сопоставление по образцу (script_fix_post_load_game), чужой байткод остаётся как есть.
static int script_of(u32 fn, u32 *data, u32 *num, int *exact)
{
    u32 d = rd32(g_mem, fn + STRUCT_SCRIPT), cnt = rd32(g_mem, fn + STRUCT_SCRIPT + 4);
    // окно гостя целиком доступно на чтение и запись (runtime/vmem.h), а память распределителя игры VirtualQuery
    // может и не описывать — хватает границ окна
    if (cnt >= 16 && cnt <= 0xFFFF && d >= 0x10000 && d < RT_WINDOW && cnt <= RT_WINDOW - d) {
        *data = d;
        *num = cnt;
        *exact = 1;
        return 1;
    }
    for (u32 k = 1; k <= 16; k++) {
        u32 off = 0x40 + 4 * k;
        if (off == STRUCT_SCRIPT)
            continue;
        d = rd32(g_mem, fn + off), cnt = rd32(g_mem, fn + off + 4);
        u32 cap = rd32(g_mem, fn + off + 8);
        if (cnt < 16 || cnt > 0xFFFF || cap < cnt || !committed(d, cnt) ||
            (g_mem[d + cnt - 1] != 0x53 && (g_mem[d + cnt - 2] != 0x04 || g_mem[d + cnt - 1] != 0x0B)))
            continue;
        *data = d;
        *num = cnt;
        *exact = 0;
        return 1;
    }
    return 0;
}

static void fix_post_load_game(u32 fn, const char *via)
{
    u32 data, num;
    int exact;
    g_seen = 1;
    if (!script_of(fn, &data, &num, &exact)) {
        if (g_pending != fn) {
            u32 d = rd32(g_mem, fn + STRUCT_SCRIPT), cnt = rd32(g_mem, fn + STRUCT_SCRIPT + 4);
            vm_info mi;
            int q = vm_query(d, &mi);
            fprintf(stderr, "[script] Shrek.PostLoadGame %08x (%s): no bytecode yet (Script %08x, %u bytes, max %u; "
                            "memory %s %08x+%x)\n", fn, via, d, cnt, rd32(g_mem, fn + STRUCT_SCRIPT + 8),
                    !q ? "outside" : mi.state == VM_MEM_COMMIT ? "committed" : mi.state == VM_MEM_RESERVE ? "reserved"
                                                                                                        : "free",
                    mi.base, mi.size);
            g_pending = fn;
        }
        return;  // байткод ещё не загружен — следующий Link
    }
    if (fn == g_done_fn && data == g_done_data) {
        if (g_pending == fn)
            g_pending = 0;
        return;
    }
    int r = script_fix_post_load_game(g_mem + data, num);
    if (r == 1)
        fprintf(stderr, "[script] Shrek.PostLoadGame %08x (%s, %u bytes%s): fixed, the hero keeps the MainPlayer tag "
                        "without a travelled hero (bonus levels started directly)\n",
                fn, via, num, exact ? "" : ", Script found off its usual place");
    else if (r == 2)
        fprintf(stderr, "[script] Shrek.PostLoadGame %08x (%s): already fixed\n", fn, via);
    else if (exact) {
        fprintf(stderr, "[script] Shrek.PostLoadGame %08x (%s): bytecode not recognised (%u bytes), left as is:", fn,
                via, num);
        for (u32 i = 0; i < num && i < 512; i++)
            fprintf(stderr, " %02x", g_mem[data + i]);
        fputc('\n', stderr);
    } else
        return;  // не тот TArray — ждать следующего Link
    g_done_fn = fn;
    g_done_data = data;
    if (g_pending == fn)
        g_pending = 0;
}

static int init(void)
{
    if (g_off)
        return 0;
    if (!g_names) {
        const char *env = getenv("SNX_SCRIPT_FIX");
        if (env && !strcmp(env, "0")) {
            g_off = 1;
            fprintf(stderr, "[script] SNX_SCRIPT_FIX=0: script fixes are off\n");
            return 0;
        }
        rt_module *core = rt_module_by_name("Core");
        g_names = core ? rt_module_export(core, "?Names@FName@@0V?$TArray@PAUFNameEntry@@@@A", 0) : 0;
        if (!g_names)
            return 0;
    }
    return 1;
}

static int is_post_load_game(u32 f)
{
    return name_is(f, "PostLoadGame") && name_is(rd32(g_mem, f + OBJ_CLASS), "Function") &&
           name_is(rd32(g_mem, f + OBJ_OUTER), "Shrek");
}

void script_fix_linked(u32 st)
{
    static int logged;
    if (!init())
        return;
    if (!logged++)
        fprintf(stderr, "[script] UStruct::Link hook active\n");
    if (g_pending && committed(g_pending, 0x58) && is_post_load_game(g_pending))
        fix_post_load_game(g_pending, "pending");
    if (!st || !committed(st, 0x58))
        return;
    u32 cls = rd32(g_mem, st + OBJ_CLASS);
    if (is_post_load_game(st)) {
        fix_post_load_game(st, "Link");
        return;
    }
    if (name_is(st, "Shrek") && name_is(cls, "Class"))  // функции класса могли загрузиться раньше него
        for (u32 f = rd32(g_mem, st + STRUCT_CHILDREN), j = 0; f && j < 4096; j++) {
            if (!committed(f, 0x58))
                break;
            if (is_post_load_game(f))
                fix_post_load_game(f, "class Link");
            f = rd32(g_mem, f + FIELD_NEXT);
        }
}

void script_fix_scan(int frame)
{
    static u32 objs, idx_plg = ~0u, last_ms;
    static int logged;
    if (!init())
        return;
    if (frame) {  // кадры: пока функция не найдена ни разу, раз в 2 с
        u32 now = (u32)(rt_ticks_ns() / 1000000u);
        if (g_seen || now - last_ms < 2000u)
            return;
        last_ms = now;
    }
    if (!objs) {
        rt_module *core = rt_module_by_name("Core");
        objs = core ? rt_module_export(core, "?GObjObjects@UObject@@0V?$TArray@PAVUObject@@@@A", 0) : 0;
        if (!objs)
            return;
    }
    u32 arr = rd32(g_mem, objs), n = rd32(g_mem, objs + 4), found = 0;
    if (!n || n > (1u << 24) || !committed(arr, 4 * n))
        return;
    for (u32 i = 0; i < n; i++) {
        u32 o = rd32(g_mem, arr + 4 * i);
        if (o < 0x10000)
            continue;
        u32 ni = rd32(g_mem, o + OBJ_NAME);
        if (idx_plg != ~0u ? ni != idx_plg : !name_is(o, "PostLoadGame"))
            continue;
        idx_plg = ni;
        if (is_post_load_game(o)) {
            found++;
            fix_post_load_game(o, frame ? "frame scan" : "load scan");
        }
    }
    if (!logged && (found || !frame)) {
        logged = 1;
        fprintf(stderr, "[script] object scan (%s): %u objects, Shrek.PostLoadGame %s\n", frame ? "frame" : "load", n,
                found ? "found" : "not loaded yet");
    }
}
