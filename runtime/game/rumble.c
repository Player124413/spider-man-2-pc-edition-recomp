// Вибрация по событиям игры (см. rumble.h): здоровье пешки локального игрока -> импульс в хостовый хук.
#include "runtime/game/rumble.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime/game/uobj.h"
#include "runtime/rt.h"

extern void (*hle_rumble_hook)(u16 low, u16 high, u32 ms);  // runtime/hle/misc.c

int game_rumble_from_health(float prev, float cur, float max, game_rumble_pulse *out)
{
    if (!(max > 0.0f) || !isfinite(max))
        max = 100.0f;
    if (!isfinite(prev) || !isfinite(cur) || !(prev > 0.0f))
        return 0;  // мусор или пешка уже мертва
    if (cur <= 0.0f) {  // гибель — длинный сильный импульс
        *out = (game_rumble_pulse){0xFFFFu, 0xFFFFu, 600u};
        return 1;
    }
    float d = prev - cur;
    if (d < max * 0.01f)  // рост здоровья, шум float, медленное убывание
        return 0;
    float frac = d / max;
    if (frac > 1.0f)
        frac = 1.0f;
    float s = 0.45f + 2.2f * frac;  // лёгкий удар ощутим, четверть полоски и больше — во всю силу
    if (s > 1.0f)
        s = 1.0f;
    float ms = 140.0f + 500.0f * frac;
    if (ms > 400.0f)
        ms = 400.0f;
    out->high = (u16)(s * 65535.0f);
    out->low = (u16)(s * 0.75f * 65535.0f);
    out->ms = (u32)ms;
    return 1;
}

int game_rumble_from_hits(int pawns, int props, game_rumble_pulse *out)
{
    if (pawns > 0) {  // враг: короткий плотный толчок, по нескольким сразу — чуть сильнее
        float s = pawns > 1 ? 0.75f : 0.6f;
        *out = (game_rumble_pulse){(u16)(s * 0.8f * 65535.0f), (u16)(s * 65535.0f), pawns > 1 ? 110u : 90u};
        return 1;
    }
    if (props > 0) {  // ящик, бочка, физический предмет — лёгкий щелчок
        *out = (game_rumble_pulse){(u16)(0.3f * 65535.0f), (u16)(0.45f * 65535.0f), 60u};
        return 1;
    }
    return 0;
}

static u32 g_epoch, g_pc_cls, g_pc, g_frames;
static uo_prop g_p_player, g_p_pawn, g_p_kwpawn;  // KWPawn — у KWHeroController (Shrek 2)
static u32 g_kw_cls;                              // класс контроллера, для которого искался KWPawn
static u32 g_pawn_cls, g_pawn;  // класс со смещениями ниже; пешка прошлого кадра
static volatile int g_player_active;  // читается хостом в input_tick: главное меню или уже игра
static uo_prop g_p_health, g_p_max;
static int g_has_max;
static float g_health;
static int g_log = -1;
static double g_test_at = -1;  // SNX_RUMBLE_TEST=<с>: отнять четверть здоровья (проверка цепочки без урона в игре)
static u64 g_start_ns;         // отсчёт SNX_RUMBLE_TEST — от первого кадра

static int alive(u32 o) { return o && uo_at(rd32(g_mem, o + 4)) == o; }

// ---------------------------------------------------------------- попадания героя
// Скрипты SHGame: ShHeroPawn.StartRegularAttack в начале удара сбрасывает флаги «задет этим ударом» у целей, а
// HitSHPawn / HitKarmaActor / HitSHPropsStatic после TakeDamage взводят его у задетой. Порт следит за переходом
// флага 0 -> 1 у целей рядом с героем (враги ставят этот флаг только самому герою — он исключён, его урон
// обрабатывается по здоровью). Удары компаньона рядом с героем тоже ощутимы — отличить их по флагу нельзя.
enum { HK_PAWN, HK_KARMA, HK_PROP, HK_N };
static const char *const g_hk_cls_name[HK_N] = {"shpawn", "KWKarmaActor", "ShPropsStatic"};
static const char *const g_hk_flag_name[HK_N] = {"bWasHitInThisAttack", "bWasHitInThisAttack",
                                                 "bSHPropsStaticWasHitInThisAttack"};
#define HIT_RADIUS 600.0f  // UU: дальность удара героя с запасом (HitSomebody: радиус + AttackDist + 200)
#define MAX_TGT 1024
#define SCAN_STEP 512u     // объектов за один фоновый шаг по переиспользованным слотам GObjObjects
#define SCAN_PERIOD 8u     // фоновый обход переиспользованных слотов раз в 8 кадров (степень двойки)
#define KIND_CACHE_N 2048u // кеш «класс -> тип цели» (степень двойки, мультипликативный хеш)
typedef struct {
    u32 obj;
    u8 kind, last;
} hit_tgt;
static hit_tgt g_tgt[MAX_TGT];
static int g_ntgt, g_hk_ready;
static u32 g_hk_cls[HK_N], g_scan_i, g_last_count;
static uo_prop g_hk_flag[HK_N], g_p_loc;
static struct {
    u32 cls;
    int kind;  // -1 — не цель
} g_kind_cache[KIND_CACHE_N];
static double g_hit_test_at = -1;  // SNX_RUMBLE_HIT_TEST=<с>: взвести флаг ближайшей цели (проверка без драки)

static void hits_reset(void)
{
    g_ntgt = 0;
    g_scan_i = 0;
    g_last_count = 0;
    g_hk_ready = 0;
    memset(g_kind_cache, 0, sizeof g_kind_cache);
}

static int hits_init(void)
{
    u32 actor = uo_find_class("Actor");
    if (!actor || !uo_find_prop(actor, "Location", &g_p_loc))
        return 0;
    int any = 0;
    for (int k = 0; k < HK_N; k++) {
        g_hk_cls[k] = uo_find_class(g_hk_cls_name[k]);
        if (g_hk_cls[k] && !uo_find_prop(g_hk_cls[k], g_hk_flag_name[k], &g_hk_flag[k]))
            g_hk_cls[k] = 0;
        any |= g_hk_cls[k] != 0;
    }
    return any;
}

static int kind_of(u32 o)
{
    u32 cls = uo_class(o);
    unsigned h = (cls * 0x9E3779B1u) >> 21;  // 11 бит -> [0, KIND_CACHE_N)
    if (g_kind_cache[h].cls == cls)
        return g_kind_cache[h].kind;
    int kind = -1;
    for (int k = 0; k < HK_N && kind < 0; k++)
        if (g_hk_cls[k] && uo_is_a_class(o, g_hk_cls[k]))
            kind = k;
    g_kind_cache[h].cls = cls;
    g_kind_cache[h].kind = kind;
    return kind;
}

static int flag_of(u32 o, int kind)
{
    const uo_prop *f = &g_hk_flag[kind];
    return (rd32(g_mem, o + f->offset) & f->bitmask) != 0;
}

static float dist2(u32 a, u32 b)
{
    float d2 = 0.0f;
    for (int i = 0; i < 3; i++) {
        float d = rdf32(g_mem, a + g_p_loc.offset + 4u * i) - rdf32(g_mem, b + g_p_loc.offset + 4u * i);
        d2 += d * d;
    }
    return d2;
}

static void hits_try_add(u32 o)
{
    if (!o || g_ntgt == MAX_TGT)
        return;
    int kind = kind_of(o);
    if (kind < 0)
        return;
    for (int i = 0; i < g_ntgt; i++)
        if (g_tgt[i].obj == o)
            return;
    g_tgt[g_ntgt++] = (hit_tgt){o, (u8)kind, (u8)flag_of(o, kind)};
}

// Новые объекты в хвосте GObjObjects ([g_last_count, n)) подхватываются сразу (без задержки при спавне);
// фоновый круговой проход по переиспользованным слотам идёт раз в SCAN_PERIOD кадров порциями по SCAN_STEP
static void hits_scan(void)
{
    u32 n = uo_count();
    if (n < g_last_count)
        g_last_count = 0;
    if (g_last_count < n) {
        u32 limit = g_last_count + 2048u;
        if (limit > n)
            limit = n;
        for (; g_last_count < limit; g_last_count++)
            hits_try_add(uo_at(g_last_count));
    }
    if ((g_frames & (SCAN_PERIOD - 1u)) != 0)
        return;
    if (g_scan_i >= n)
        g_scan_i = 0;
    for (u32 end = g_scan_i + SCAN_STEP; g_scan_i < n && g_scan_i < end; g_scan_i++)
        hits_try_add(uo_at(g_scan_i));
}

static void hits_frame(u32 pawn)
{
    if (!g_hk_ready) {
        if (g_frames % 60 != 1)
            return;
        if (!(g_hk_ready = hits_init()))
            return;
    }
    hits_scan();
    if (g_hit_test_at >= 0 && (double)(rt_ticks_ns() - g_start_ns) / 1e9 >= g_hit_test_at) {
        int best = -1;
        float bd = HIT_RADIUS * HIT_RADIUS;
        for (int i = 0; i < g_ntgt; i++) {
            float d = alive(g_tgt[i].obj) && g_tgt[i].obj != pawn && !g_tgt[i].last ? dist2(g_tgt[i].obj, pawn) : bd;
            if (d < bd)
                bd = d, best = i;
        }
        if (best >= 0) {
            g_hit_test_at = -1;
            const uo_prop *f = &g_hk_flag[g_tgt[best].kind];
            u32 o = g_tgt[best].obj;
            wr32(g_mem, o + f->offset, rd32(g_mem, o + f->offset) | f->bitmask);
            fprintf(stderr, "[rumble] SNX_RUMBLE_HIT_TEST: %s hit flag set\n", uo_name(o));
        }
    }
    int hit[HK_N] = {0};
    for (int i = 0; i < g_ntgt;) {
        hit_tgt *t = &g_tgt[i];
        if (!alive(t->obj)) {  // уничтожен и собран: на его место — последний
            *t = g_tgt[--g_ntgt];
            continue;
        }
        int f = flag_of(t->obj, t->kind);
        if (f && !t->last && t->obj != pawn && dist2(t->obj, pawn) < HIT_RADIUS * HIT_RADIUS) {
            hit[t->kind]++;
            if (g_log)
                fprintf(stderr, "[rumble] hero hit %s\n", uo_name(t->obj));
        }
        t->last = (u8)f;
        i++;
    }
    game_rumble_pulse p;
    if (game_rumble_from_hits(hit[HK_PAWN], hit[HK_KARMA] + hit[HK_PROP], &p))
        hle_rumble_hook(p.low, p.high, p.ms);
}

static u32 find_local_pc(void)
{
    g_pc_cls = uo_find_class("PlayerController");
    if (!g_pc_cls || !uo_find_prop(g_pc_cls, "Player", &g_p_player) || !uo_find_prop(g_pc_cls, "Pawn", &g_p_pawn))
        return 0;
    for (u32 i = 0, n = uo_count(); i < n; i++) {
        u32 o = uo_at(i);
        if (o && uo_is_a_class(o, g_pc_cls) && rd32(g_mem, o + g_p_player.offset))
            return o;
    }
    return 0;
}

int game_player_active(void) { return g_player_active != 0; }

void game_rumble_frame(void)
{
    if (g_log < 0) {
        g_log = getenv("SNX_LOG_RUMBLE") != NULL;
        const char *t = getenv("SNX_RUMBLE_TEST");
        g_test_at = t ? atof(t) : -1;
        t = getenv("SNX_RUMBLE_HIT_TEST");
        g_hit_test_at = t ? atof(t) : -1;
        g_start_ns = rt_ticks_ns();
    }
    if (!hle_rumble_hook || !uo_init()) {
        g_player_active = 0;
        return;
    }
    g_frames++;
    u32 ep = __atomic_load_n(&rt_gc_epoch, __ATOMIC_RELAXED);
    if (ep != g_epoch) {  // смена карты: контроллер, пешка и классы могли смениться
        g_epoch = ep;
        g_pc = g_pawn = g_pawn_cls = g_kw_cls = 0;
        g_player_active = 0;
        hits_reset();
    }
    if (g_pc && (g_frames % 120 == 0) && (!alive(g_pc) || !rd32(g_mem, g_pc + g_p_player.offset)))
        g_pc = g_pawn = 0;
    if (!g_pc && g_frames % 60 == 1)
        g_pc = find_local_pc();
    if (!g_pc) {
        g_player_active = 0;
        return;
    }
    // Controller.Pawn на время роликов пуст, а KWHeroController.KWPawn по-прежнему указывает на героя
    u32 pawn = rd32(g_mem, g_pc + g_p_pawn.offset);
    if (!pawn) {
        u32 pc_cls = uo_class(g_pc);
        if (pc_cls != g_kw_cls) {
            g_kw_cls = pc_cls;
            if (!uo_find_prop(pc_cls, "KWPawn", &g_p_kwpawn))
                g_p_kwpawn.offset = 0;
        }
        if (g_p_kwpawn.offset)
            pawn = rd32(g_mem, g_pc + g_p_kwpawn.offset);
    }
    if (g_log && g_frames % 600 == 0) {
        fprintf(stderr, "[rumble] frame %u: controller %08x %s (player %08x)", g_frames, g_pc, uo_name(g_pc),
                rd32(g_mem, g_pc + g_p_player.offset));
        fprintf(stderr, ", pawn %08x %s, hit targets %d\n", pawn, uo_name(pawn), g_ntgt);
    }
    if (!alive(pawn)) {
        g_pawn = 0;
        g_player_active = 0;
        return;
    }
    g_player_active = 1;
    hits_frame(pawn);
    u32 cls = uo_class(pawn);
    if (cls != g_pawn_cls) {
        if (!uo_find_prop(cls, "Health", &g_p_health)) {
            g_pawn = 0;
            return;
        }
        g_has_max = uo_find_prop(cls, "MaxHealth", &g_p_max) || uo_find_prop(cls, "HealthMax", &g_p_max);
        g_pawn_cls = cls;
        g_pawn = 0;
    }
    float h = rdf32(g_mem, pawn + g_p_health.offset);
    float max = g_has_max ? rdf32(g_mem, pawn + g_p_max.offset) : 100.0f;
    if (pawn != g_pawn) {  // новая пешка (начало карты, смена героя): только запомнить
        if (g_log)
            fprintf(stderr, "[rumble] player pawn %s health %.1f/%.1f\n", uo_name(pawn), h, max);
        g_pawn = pawn;
        g_health = h;
        return;
    }
    if (g_test_at >= 0 && (double)(rt_ticks_ns() - g_start_ns) / 1e9 >= g_test_at && h > 0) {
        g_test_at = -1;
        h -= (max > 0 ? max : 100.0f) * 0.25f;
        wrf32(g_mem, pawn + g_p_health.offset, h);
        fprintf(stderr, "[rumble] SNX_RUMBLE_TEST: health -> %.1f\n", h);
    }
    game_rumble_pulse p;
    if (game_rumble_from_health(g_health, h, max, &p)) {
        if (g_log)
            fprintf(stderr, "[rumble] health %.1f -> %.1f of %.1f: %04x/%04x %u ms\n", g_health, h, max, p.low,
                    p.high, p.ms);
        hle_rumble_hook(p.low, p.high, p.ms);
    }
    g_health = h;
}
