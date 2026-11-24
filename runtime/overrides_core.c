// Нативные замены функций Core.dll и копий memcpy в модулях (таблица — recomp/overrides.py; game.exe —
// overrides_launch.c).
#include <stdio.h>
#include <stdlib.h>

#include "runtime/abi.h"
#include "runtime/ini_cache.h"
#include "runtime/process.h"
#include "runtime/rt.h"
#include "runtime/script_fix.h"

// ---------------------------------------------------------------- обход свойств класса
// TFieldIterator<UProperty> (Core.dll, не экспорт): от текущего поля по цепочке Children/Next, затем в
// GetInheritanceSuper, пропускает всё, что не свойство. В цепочке класса после его свойств — его функции, состояния
// и перечисления (у Actor — сотни), и каждое из них — промах кеша: обход свойств объекта (сохранение, сборка мусора,
// копирование значений по умолчанию) у SerializeTaggedProperties при сохранении — четверть кадра. Цепочки полей
// меняются только при загрузке (пакет .u дописывает свойства в уже зарегистрированные нативные классы, UStruct::Link)
// и сборке мусора, и пропуск от первого не-свойства до следующего свойства запоминается: ключ — (структура, поле),
// значение — новые (структура, поле). Link и CollectGarbage сменяют эпоху — кеш забывается; до главного цикла
// (GIsRunning) и пока идёт загрузка (GObjBeginLoadCount) он не используется. Таблица своя у потока.
// SNX_FIELD_CHECK=1 — каждое попадание сверяется с обходом.
#define OBJ_CLASS 0x24u
#define FIELD_NEXT 0x30u
#define STRUCT_CHILDREN 0x40u
#define CLASS_CAST_FLAGS 0x49Cu
#define CAST_PROPERTY 0x8000u
#define VT_INHERITANCE_SUPER 0x80u

#define FN_BITS 14u
#define FN_SIZE (1u << FN_BITS)

typedef struct {
    u32 st, f, st2, f2;
} fn_entry;

typedef struct {
    u32 epoch, used;
    fn_entry e[FN_SIZE];
} fn_cache;

static u32 g_fn_epoch = 1;
static int g_fn_check = -1;
static u32 g_load_count_va, g_running_va;  // Core: UObject::GObjBeginLoadCount, GIsRunning
static __thread fn_cache *t_fn;

static inline int is_property(const u8 *m, u32 f)
{
    return (rd32(m, rd32(m, f + OBJ_CLASS) + CLASS_CAST_FLAGS) & CAST_PROPERTY) != 0;
}

// пропуск оригинала: от (st, f) до свойства или до конца иерархии (0, 0)
static void field_walk(CPU *c, u32 *pst, u32 *pf)
{
    const u8 *m = g_mem;
    u32 st = *pst, f = *pf;
    for (;;) {
        for (; f; f = rd32(m, f + FIELD_NEXT))
            if (is_property(m, f))
                goto out;
        c->ecx = st;
        st = rt_guest_call(c, rd32(m, rd32(m, st) + VT_INHERITANCE_SUPER), 0, NULL);
        if (!st)
            break;
        f = rd32(m, st + STRUCT_CHILDREN);
    }
out:
    *pst = st;
    *pf = f;
}

void ovr_core_field_next(CPU *c)
{
    u8 *m = g_mem;
    u32 it = c->ecx, st = rd32(m, it), f = rd32(m, it + 4);
    c->esp += 4;
    if (!st || (f && is_property(m, f)))
        return;
    if (!g_running_va) {
        rt_module *core = rt_module_by_name("Core");
        g_load_count_va = rt_module_export(core, "?GObjBeginLoadCount@UObject@@0HA", 0);
        g_running_va = rt_module_export(core, "?GIsRunning@@3HA", 0);
        if (!g_load_count_va || !g_running_va)
            g_running_va = 1;  // нет экспортов — кеш выключен (адрес 1 — не ноль)
    }
    fn_cache *t = t_fn;
    if (!g_load_count_va || !rd32(m, g_running_va) || rd32(m, g_load_count_va) ||
        (!t && !(t = t_fn = calloc(1, sizeof *t)))) {
        field_walk(c, &st, &f);
        goto store;
    }
    u32 ep = __atomic_load_n(&g_fn_epoch, __ATOMIC_RELAXED);
    if (t->epoch != ep || t->used > FN_SIZE / 2) {
        memset(t->e, 0, sizeof t->e);
        t->epoch = ep;
        t->used = 0;
    }
    u32 h = ((st * 0x9E3779B1u) ^ (f * 0x85EBCA6Bu)) >> (32 - FN_BITS);
    fn_entry *e;
    for (;; h = (h + 1) & (FN_SIZE - 1)) {
        e = &t->e[h];
        if (e->st == st && e->f == f) {
            if (g_fn_check < 0)
                g_fn_check = getenv("SNX_FIELD_CHECK") != NULL;
            if (g_fn_check) {
                u32 st2 = st, f2 = f;
                field_walk(c, &st2, &f2);
                if (st2 != e->st2 || f2 != e->f2) {
                    fprintf(stderr, "[core] field cache: (%08x, %08x) -> (%08x, %08x), walk (%08x, %08x)\n", st, f,
                            e->st2, e->f2, st2, f2);
                    abort();
                }
            }
            st = e->st2;
            f = e->f2;
            goto store;
        }
        if (!e->st)
            break;
    }
    u32 st0 = st, f0 = f;
    field_walk(c, &st, &f);
    *e = (fn_entry){st0, f0, st, f};
    t->used++;
store:
    wr32(m, it, st);
    wr32(m, it + 4, f);
}

// UObject::CollectGarbage (освобождённые классы) и UStruct::Link (новые поля): кеш обхода свойств устарел
extern void (*const ovr_orig_ovr_core_collect_garbage)(CPU *c);
extern void (*const ovr_orig_ovr_core_struct_link)(CPU *c);

void ovr_core_collect_garbage(CPU *c)
{
    __atomic_add_fetch(&rt_gc_epoch, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_fn_epoch, 1, __ATOMIC_RELAXED);
    RT_ENTER_GEN(c, ovr_orig_ovr_core_collect_garbage);  // оригинал — код гостя
    __atomic_add_fetch(&rt_gc_epoch, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_fn_epoch, 1, __ATOMIC_RELAXED);
}

// После Link байткод функции уже в памяти: исправления скриптов игры (runtime/script_fix.c)
void ovr_core_struct_link(CPU *c)
{
    u32 self = c->ecx;
    __atomic_add_fetch(&g_fn_epoch, 1, __ATOMIC_RELAXED);
    RT_ENTER_GEN(c, ovr_orig_ovr_core_struct_link);  // оригинал — код гостя
    __atomic_add_fetch(&g_fn_epoch, 1, __ATOMIC_RELAXED);
    script_fix_linked(self);
}

// ---------------------------------------------------------------- SaveConfig: перечитывание конфига в умолчания
// UObject::SaveConfig пишет config-свойства объекта в кеш ini (GConfig), сбрасывает изменённые файлы на диск и
// перечитывает конфиг в умолчания всех классов под верхним config-классом цепочки: LoadConfig(1) умолчаний каждого,
// рекурсивно по уровням до верхнего, на каждом — обход всех полей цепочки и GetString на каждое config-свойство. У
// контроллеров это Controller и 51 подкласс. Скрипты SHGame на картах BEANSTALK* (Coin, CoinStack, MoneyBag,
// EnergyKeg, EnergyBar: AnnouncePickup) зовут SaveConfig контроллера героя на каждом подборе: на маке ≈2 мс, три
// четверти из них — это перечитывание. С пропуском ниже и GetString по памяти (overrides_launch.c) — 0,7 мс
// (SNX_CFG_TEST); запись User.ini уходит в фон (runtime/hle/kernel32_file.c).
// LoadConfig умолчаний класса читает только секции [Пакет.Класс] классов своей цепочки (globalconfig — секцию
// класса-владельца, он тоже в цепочке). Перед перечитыванием считается хеш содержимого этих секций во всех файлах
// кеша; совпал с хешем прошлого перечитывания этого класса — значения из ini те же, вызов пропускается. Подбор
// монеты меняет только [ShGame.ShHeroController]: перечитываются 3 класса из 51. Отличие от ПК: умолчание,
// изменённое скриптом без сохранения (default.X = ...), чужой SaveConfig к значению из ini не вернёт.
// SNX_CFG_CHECK=1 — пропусков нет: умолчания пропущенного класса сравниваются до и после LoadConfig.
// Раскладка кеша ini — runtime/ini_cache.h.
extern void (*const ovr_orig_ovr_core_save_config)(CPU *c);
extern void (*const ovr_orig_ovr_core_load_config)(CPU *c);

#define OBJ_INDEX 0x04u
#define OBJ_OUTER 0x18u
#define OBJ_NAME 0x20u
#define FIELD_SUPER 0x2Cu
#define CLASS_DEFAULTS 0x4ECu  // TArray<BYTE> Defaults: данные, число

#define SC_SECS 512u
#define SC_CLASSES 4096u

typedef struct {
    u64 name, body;  // хеш имени (без учёта регистра) и содержимого, body = 0 — ещё не считан
    u32 sec;
} sc_sec;

static struct {
    u32 body_lo, gconfig_va, names_va;  // SaveConfig (тело), адрес GConfig, FName::Names
    int depth, ready;                   // вложенность SaveConfig; таблица секций собрана
    u32 obj;                            // объект текущего SaveConfig (обход классов ниже)
    u32 nsec, loaded, skipped;
    sc_sec sec[SC_SECS];
    struct {
        u32 cls, epoch;
        u64 hash;
    } cls[SC_CLASSES];
} g_sc;

#define FNV_INIT 0xcbf29ce484222325ull
static inline u64 fnv(u64 h, u32 v) { return (h ^ v) * 0x100000001b3ull; }

static u64 hash_wstr(u64 h, u32 p, u32 n, int fold)  // FString гостя: n символов с нулём
{
    for (u32 i = 0; p && i < n; i++) {
        u32 ch = rd16(g_mem, p + 2 * i);
        if (!ch)
            break;
        h = fnv(h, fold && ch >= 'A' && ch <= 'Z' ? ch + 32 : ch);
    }
    return h;
}

static u64 hash_fname(u64 h, u32 obj)  // имя объекта (FNameEntry: строка с +0xC)
{
    u32 idx = rd32(g_mem, obj + OBJ_NAME), n = rd32(g_mem, g_sc.names_va + TARRAY_NUM);
    u32 e = idx < n ? rd32(g_mem, rd32(g_mem, g_sc.names_va) + 4 * idx) : 0;
    return e ? hash_wstr(h, e + 0xC, 1024, 1) : fnv(h, 0xFFFFFFFFu);
}

// все секции всех файлов кеша: хеш имени; содержимое — при первом запросе
static int sc_collect(void)
{
    u32 cfg = rd32(g_mem, g_sc.gconfig_va), files = rd32(g_mem, cfg + CFG_FILES);
    u32 nf = rd32(g_mem, cfg + CFG_FILES + TARRAY_NUM);
    g_sc.nsec = 0;
    for (u32 i = 0; i < nf; i++) {
        u32 f = files + i * CFG_FILE_SIZE, secs = rd32(g_mem, f + CFG_FILE_SECS);
        u32 ns = rd32(g_mem, f + CFG_FILE_SECS + TARRAY_NUM);
        for (u32 j = 0; j < ns; j++) {
            if (g_sc.nsec == SC_SECS)
                return 0;
            u32 s = secs + j * CFG_SEC_SIZE;
            g_sc.sec[g_sc.nsec++] = (sc_sec){hash_wstr(FNV_INIT, rd32(g_mem, s + 4), rd32(g_mem, s + 8), 1), 0, s};
        }
    }
    return 1;
}

static u64 sc_body(sc_sec *s)
{
    if (!s->body) {
        u32 kv = rd32(g_mem, s->sec + CFG_SEC_PAIRS), n = rd32(g_mem, s->sec + CFG_SEC_PAIRS + TARRAY_NUM);
        u64 h = fnv(FNV_INIT, n);
        for (u32 i = 0; i < n; i++) {
            u32 p = kv + i * CFG_KV_SIZE;
            h = fnv(hash_wstr(h, rd32(g_mem, p + 4), rd32(g_mem, p + 8), 0), '=');
            h = fnv(hash_wstr(h, rd32(g_mem, p + 0x10), rd32(g_mem, p + 0x14), 0), '\n');
        }
        s->body = h | 1;
    }
    return s->body;
}

// GetPathName: имена внешних объектов от самого внешнего через точку
static u64 hash_path(u64 h, u32 obj, int depth)
{
    u32 outer = rd32(g_mem, obj + OBJ_OUTER);
    if (outer && depth < 16)
        h = fnv(hash_path(h, outer, depth + 1), '.');
    return hash_fname(h, obj);
}

// содержимое секций [Пакет.Класс] цепочки класса во всех файлах кеша
static u64 sc_class_hash(u32 cls)
{
    u64 h = FNV_INIT;
    for (u32 k = cls, depth = 0; k && depth < 64; k = rd32(g_mem, k + FIELD_SUPER), depth++) {
        u64 name = hash_path(FNV_INIT, k, 0);
        h = fnv(h, (u32)name);
        for (u32 i = 0; i < g_sc.nsec; i++)
            if (g_sc.sec[i].name == name) {
                u64 b = sc_body(&g_sc.sec[i]);
                h = fnv(fnv(fnv(h, i), (u32)b), (u32)(b >> 32));
            }
    }
    return h;
}

void ovr_core_save_config(CPU *c)
{
    u32 obj = c->ecx;
    if (!g_sc.gconfig_va) {
        rt_module *core = rt_module_by_name("Core");
        u32 ex = rt_module_export(core, "?SaveConfig@UObject@@QAEXKPBG@Z", 0);
        g_sc.gconfig_va = rt_module_export(core, "?GConfig@@3PAVFConfigCache@@A", 0);
        g_sc.names_va = rt_module_export(core, "?Names@FName@@0V?$TArray@PAUFNameEntry@@@@A", 0);
        // экспорт — переходник инкрементальной компоновки (jmp rel32) на тело
        g_sc.body_lo = ex && g_mem[ex] == 0xE9 ? ex + 5 + rd32(g_mem, ex + 1) : ex;
        if (!g_sc.gconfig_va || !g_sc.names_va)
            g_sc.body_lo = 0;  // кеш не найти — без пропусков
    }
    u32 loaded = g_sc.loaded, skipped = g_sc.skipped, outer = g_sc.obj;
    g_sc.obj = obj;
    g_sc.ready = 0;
    g_sc.loaded = g_sc.skipped = 0;
    g_sc.depth++;
    u64 t0 = rt_ticks_ns(), file0 = __atomic_load_n(&rt_file_ns, __ATOMIC_RELAXED);
    RT_ENTER_GEN(c, ovr_orig_ovr_core_save_config);  // оригинал — код гостя
    g_sc.depth--;
    // строка — только для замеров или долгого вызова: на Switch fprintf через nxlink или в snx.log на SD стоит
    // больше миллисекунды, а скрипт зовёт SaveConfig на каждой монете
    static int verbose = -1;
    if (verbose < 0)
        verbose = getenv("SNX_CFG_TEST") || cfg_check();
    double ms = (double)(rt_ticks_ns() - t0) / 1e6;
    if (verbose || ms >= 30) {
        char cls[64] = "";
        u32 k = rd32(g_mem, obj + OBJ_CLASS), idx = rd32(g_mem, k + OBJ_NAME);
        u32 e = g_sc.names_va && idx < rd32(g_mem, g_sc.names_va + 4) ? rd32(g_mem, rd32(g_mem, g_sc.names_va) + 4 * idx) : 0;
        for (u32 i = 0; e && i < sizeof cls - 1; i++)
            if (!(cls[i] = (char)rd16(g_mem, e + 0xC + 2 * i)))
                break;
        fprintf(stderr, "[core] SaveConfig(%s): %.1f ms (files %.1f ms), defaults reloaded for %u of %u classes\n", cls,
                ms, (double)(__atomic_load_n(&rt_file_ns, __ATOMIC_RELAXED) - file0) / 1e6, g_sc.loaded,
                g_sc.loaded + g_sc.skipped);
    }
    g_sc.ready = 0;  // таблица секций — только на время одного SaveConfig (вложенный собирает свою)
    g_sc.loaded = loaded;
    g_sc.obj = outer;
    g_sc.skipped = skipped;
}

void ovr_core_load_config(CPU *c)
{
    u32 obj = c->ecx, ret = rd32(g_mem, c->esp);
    u32 propagate = rd32(g_mem, c->esp + 4), cls_arg = rd32(g_mem, c->esp + 8), file = rd32(g_mem, c->esp + 12);
    // только перечитывание из цикла SaveConfig: LoadConfig(1, NULL, NULL) у умолчаний класса
    if (!g_sc.depth || !g_sc.body_lo || ret - g_sc.body_lo >= 0x600u || propagate != 1 || cls_arg || file ||
        rd32(g_mem, obj + OBJ_INDEX) != 0xFFFFFFFFu) {
        RT_ENTER_GEN(c, ovr_orig_ovr_core_load_config);  // оригинал — код гостя
        return;
    }
    if (!g_sc.ready)
        g_sc.ready = sc_collect() ? 1 : -1;
    u32 cls = rd32(g_mem, obj + OBJ_CLASS);
    if (g_sc.ready < 0) {  // секций больше таблицы — без пропусков
        g_sc.loaded++;
        RT_ENTER_GEN(c, ovr_orig_ovr_core_load_config);  // оригинал — код гостя
        return;
    }
    u64 h = sc_class_hash(cls);
    u32 slot = (cls * 0x9E3779B1u) >> 20;  // 12 бит: SC_CLASSES
    for (u32 n = 0; n < SC_CLASSES && g_sc.cls[slot].cls && g_sc.cls[slot].cls != cls; n++)
        slot = (slot + 1) & (SC_CLASSES - 1);
    if (g_sc.cls[slot].cls && g_sc.cls[slot].cls != cls) {  // таблица полна — заново
        memset(g_sc.cls, 0, sizeof g_sc.cls);
        slot = (cls * 0x9E3779B1u) >> 20;
    }
    u32 gc_ep = __atomic_load_n(&rt_gc_epoch, __ATOMIC_RELAXED);
    int same = g_sc.cls[slot].cls == cls && g_sc.cls[slot].epoch == gc_ep && g_sc.cls[slot].hash == h;
    g_sc.cls[slot].cls = cls;
    g_sc.cls[slot].epoch = gc_ep;
    g_sc.cls[slot].hash = h;
    if (same && !cfg_check()) {
        g_sc.skipped++;
        c->esp += 16;  // thiscall, три аргумента: ret 12
        return;
    }
    if (!same) {
        g_sc.loaded++;
        RT_ENTER_GEN(c, ovr_orig_ovr_core_load_config);  // оригинал — код гостя
        return;
    }
    // SNX_CFG_CHECK: пропустили бы — умолчания должны остаться прежними
    g_sc.skipped++;
    u32 data = rd32(g_mem, cls + CLASS_DEFAULTS), size = rd32(g_mem, cls + CLASS_DEFAULTS + TARRAY_NUM);
    u8 *before = size ? calloc(1, size + 4) : NULL;
    if (before)
        memcpy(before, g_mem + data, size);
    RT_ENTER_GEN(c, ovr_orig_ovr_core_load_config);  // оригинал — код гостя
    for (u32 i = 0; before && i < size && data == rd32(g_mem, cls + CLASS_DEFAULTS); i += 4)
        if (memcmp(before + i, g_mem + data + i, size - i < 4 ? size - i : 4))
            fprintf(stderr, "[core] SNX_CFG_CHECK: class %08x defaults +%x changed: %08x -> %08x\n", cls, i,
                    rd32(before, i), rd32(g_mem, data + i));
    free(before);
}

// ---------------------------------------------------------------- обход классов из SaveConfig
// Core.dll!0x10125a00 — TObjectIterator<T>::operator++ (одна копия шаблона на все T; итератор: Class, Index):
// следующий объект GObjObjects, который IsA(Class), или Index = Num. SaveConfig после записи ini обходит так все
// классы (TObjectIterator<UClass>) и перечитывает умолчания тех, что IsChildOf(верхний config-класс цепочки
// объекта): на каждой монете — весь массив объектов, у каждого читается Class; на Switch ≈2,8 мс промахов кеша.
// Для вызовов из SaveConfig (по адресу возврата) следующий индекс берётся из списка подклассов верхнего класса:
// остальные классы цикл SaveConfig всё равно пропускает. Список строится полным обходом и годен, пока не было
// UStruct::Link и сборки мусора (g_fn_epoch): класс из пакета до исполнения скрипта проходит Link
// (ULinkerLoad::Preload в EndLoad), нативные регистрируются при старте. Каждый индекс из списка проверяется заново
// (объект на месте, IsA, IsChildOf). SNX_CFG_CHECK=1 — каждый шаг сверяется с оригиналом.
extern void (*const ovr_orig_ovr_core_objit_next)(CPU *c);

#define CLASS_FLAGS 0x49Cu
#define CLASS_CONFIG 0x4u

static struct {
    u32 objs_va, uclass;  // GObjObjects (TArray), UClass::PrivateStaticClass; uclass = 1 — не найдены
    u32 base, epoch, n, cap;
    u32 *idx;  // индексы подклассов base по возрастанию (эпоха epoch)
} g_oi;

static int oi_child(u32 k, u32 base)  // UStruct::IsChildOf: цепочка SuperField
{
    for (u32 d = 0; k && d < 64; k = rd32(g_mem, k + FIELD_SUPER), d++)
        if (k == base)
            return 1;
    return 0;
}

// объект i — класс (IsA(UClass)) и подкласс base
static int oi_match(u32 arr, u32 num, u32 i, u32 base)
{
    u32 o = i < num ? rd32(g_mem, arr + 4 * i) : 0;
    return o && oi_child(rd32(g_mem, o + OBJ_CLASS), g_oi.uclass) && oi_child(o, base);
}

void ovr_core_objit_next(CPU *c)
{
    u32 it = c->ecx, ret = rd32(g_mem, c->esp);
    if (g_sc.depth && !g_oi.uclass) {
        rt_module *core = rt_module_by_name("Core");
        g_oi.objs_va = core ? rt_module_export(core, "?GObjObjects@UObject@@0V?$TArray@PAVUObject@@@@A", 0) : 0;
        g_oi.uclass = core ? rt_module_export(core, "?PrivateStaticClass@UClass@@0V1@A", 0) : 0;
        if (!g_oi.objs_va || !g_oi.uclass)
            g_oi.uclass = 1;
    }
    if (!g_sc.depth || !g_sc.obj || !g_sc.body_lo || ret - g_sc.body_lo >= 0x600u || g_oi.uclass <= 1 ||
        rd32(g_mem, it) != g_oi.uclass) {
        RT_ENTER_GEN(c, ovr_orig_ovr_core_objit_next);  // оригинал — код гостя
        return;
    }
    // верхний config-класс, как в SaveConfig (Core.dll!0x10165071): пока у предка есть CLASS_Config
    u32 base = rd32(g_mem, g_sc.obj + OBJ_CLASS);
    for (u32 s, d = 0; d < 64 && (s = rd32(g_mem, base + FIELD_SUPER)) && (rd32(g_mem, s + CLASS_FLAGS) & CLASS_CONFIG); d++)
        base = s;
    u32 arr = rd32(g_mem, g_oi.objs_va), num = rd32(g_mem, g_oi.objs_va + TARRAY_NUM);
    u32 epoch = __atomic_load_n(&g_fn_epoch, __ATOMIC_RELAXED);
    if (!g_oi.idx || g_oi.base != base || g_oi.epoch != epoch) {
        g_oi.n = 0;
        for (u32 i = 0; i < num; i++)
            if (oi_match(arr, num, i, base)) {
                if (g_oi.n == g_oi.cap) {
                    u32 *p = realloc(g_oi.idx, (g_oi.cap = g_oi.cap ? 2 * g_oi.cap : 64) * sizeof *p);
                    if (!p) {
                        free(g_oi.idx);
                        g_oi.idx = NULL, g_oi.n = g_oi.cap = 0;
                        RT_ENTER_GEN(c, ovr_orig_ovr_core_objit_next);  // оригинал — код гостя
                        return;
                    }
                    g_oi.idx = p;
                }
                g_oi.idx[g_oi.n++] = i;
            }
        g_oi.base = base, g_oi.epoch = epoch;
    }
    s32 index = (s32)rd32(g_mem, it + 4);
    u32 lo = 0, hi = g_oi.n;  // первый индекс списка больше index
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if ((s64)g_oi.idx[mid] <= (s64)index)
            lo = mid + 1;
        else
            hi = mid;
    }
    u32 next = num;
    for (; lo < g_oi.n; lo++)
        if (oi_match(arr, num, g_oi.idx[lo], base)) {
            next = g_oi.idx[lo];
            break;
        }
    if (cfg_check()) {  // оригинал: шаги до подкласса base или конца
        u32 sp = c->esp, want;
        for (;;) {
            RT_ENTER_GEN(c, ovr_orig_ovr_core_objit_next);  // оригинал — код гостя; адрес возврата остаётся на sp
            c->esp = sp;
            want = rd32(g_mem, it + 4);
            if (want >= num || oi_match(arr, num, want, base))
                break;
        }
        if (want != next)
            fprintf(stderr, "[core] SNX_CFG_CHECK: class iterator after %d: %u, original %u\n", index, next, want);
    }
    wr32(g_mem, it + 4, next);
    c->eax = next < num ? g_oi.uclass : next;  // как у оригинала: найденный класс цепочки или индекс конца
    c->edx = g_oi.uclass;
    c->esp += 4;
}

// ---------------------------------------------------------------- memcpy модулей
// Своя копия у каждой DLL (из статического CRT, cdecl dst, src, n): на SSE — выравнивание и хвост переходами по
// вычисленному адресу (jmp ecx), а это поиск функции по адресу на каждом переходе. Зовут её и на 1–4 байта
// (FArchiveFileWriter::Serialize на каждое поле при сохранении).
void ovr_memcpy(CPU *c)
{
    u32 dst = rd32(g_mem, c->esp + 4), src = rd32(g_mem, c->esp + 8), n = rd32(g_mem, c->esp + 12);
    memmove(g_mem + dst, g_mem + src, n);
    c->eax = dst;
    c->esp += 4;
}

// ---------------------------------------------------------------- быстрые листья Core.dll (appMemcmp, FBox, FVector)
// Слабые определения NULL вынесены в runtime/ovr_orig_weak.c (чтобы GCC с -fvisibility=hidden не сворачивал
// const-указатели в NULL в этом TU). В сборках с Core.dll сильные определения из core_table.c их перекрывают.
extern void (*const ovr_orig_ovr_core_box_add_vec)(CPU *c);
extern void (*const ovr_orig_ovr_core_vec_safe_normal)(CPU *c);
extern void (*const ovr_orig_ovr_core_vec_normalize)(CPU *c);

static inline int core_math_check(void)
{
    static int v = -1;
    if (v < 0)
        v = getenv("SNX_MATH_CHECK") != NULL;
    return v;
}

// Core.dll!0x101286b0 (экспорт 0x101028a6, appMemcmp): cdecl int appMemcmp(const void *a, const void *b, int n).
// Оригинал через repe cmpsb + sbb возвращает строго -1, 0 или +1 (а не разность байтов).
void ovr_core_memcmp(CPU *c)
{
    u32 a = rd32(g_mem, c->esp + 4u);
    u32 b = rd32(g_mem, c->esp + 8u);
    u32 n = rd32(g_mem, c->esp + 12u);
    int r = n ? memcmp(g_mem + a, g_mem + b, n) : 0;
    c->eax = r < 0 ? 0xFFFFFFFFu : r > 0 ? 1u : 0u;
    c->esp += 4u;
}

// Core.dll!0x101186c0 (экспорт 0x10101aff, FBox::operator+=(const FVector &)): thiscall, ret 4, eax = this.
// Раскладка FBox: +0x00 Min (3 float), +0x0C Max (3 float), +0x18 IsValid (u8).
static void box_add_vec_apply(u8 *m, u32 box, u32 vec)
{
    if (m[box + 0x18u]) {
        float vx = rdf32(m, vec), vy = rdf32(m, vec + 4u), vz = rdf32(m, vec + 8u);
        float minx = rdf32(m, box), miny = rdf32(m, box + 4u), minz = rdf32(m, box + 8u);
        float maxx = rdf32(m, box + 0x0Cu), maxy = rdf32(m, box + 0x10u), maxz = rdf32(m, box + 0x14u);
        // Побитово как x87 fcomp/fnstsw в оригинале (включая NaN): Min = (Min <= V ? Min : V), Max = (Max >= V ? Max : V)
        wrf32(m, box, minx <= vx ? minx : vx);
        wrf32(m, box + 4u, miny <= vy ? miny : vy);
        wrf32(m, box + 8u, minz <= vz ? minz : vz);
        wrf32(m, box + 0x0Cu, maxx >= vx ? maxx : vx);
        wrf32(m, box + 0x10u, maxy >= vy ? maxy : vy);
        wrf32(m, box + 0x14u, maxz >= vz ? maxz : vz);
    } else {
        u32 x = rd32(m, vec), y = rd32(m, vec + 4u), z = rd32(m, vec + 8u);
        wr32(m, box, x);
        wr32(m, box + 4u, y);
        wr32(m, box + 8u, z);
        wr32(m, box + 0x0Cu, x);
        wr32(m, box + 0x10u, y);
        wr32(m, box + 0x14u, z);
        m[box + 0x18u] = 1u;
    }
}

void ovr_core_box_add_vec(CPU *c)
{
    u8 *m = g_mem;
    u32 box = c->ecx, vec = rd32(m, c->esp + 4u);
    if (core_math_check() && ovr_orig_ovr_core_box_add_vec) {
        u8 before[25], ours[25];
        memcpy(before, m + box, sizeof before);
        box_add_vec_apply(m, box, vec);
        memcpy(ours, m + box, sizeof ours);
        memcpy(m + box, before, sizeof before);
        RT_ENTER_GEN(c, ovr_orig_ovr_core_box_add_vec);
        if (c->eax != box || memcmp(ours, m + box, sizeof ours) != 0)
            fprintf(stderr, "[core] SNX_MATH_CHECK: FBox::+= mismatch (eax %08x vs %08x)\n", c->eax, box);
        return;
    }
    box_add_vec_apply(m, box, vec);
    c->eax = box;
    c->esp += 8u;  // thiscall, один аргумент: ret 4
}

// Core.dll!0x1014ad30 (экспорт 0x10103c97, FVector::SafeNormal): thiscall с sret [esp+4], ret 4, eax = out.
// Порог — константа double 1e-8 (Core.dll!0x101904c8). Для X и Y масштаб усечён до float (fst [esp+4] в 1014ad8c),
// для Z — double; при sq < 1e-8 возвращается {0, 0, 0}.
static void vec_safe_normal_apply(u8 *m, u32 v, u32 out)
{
    double x = (double)rdf32(m, v);
    double y = (double)rdf32(m, v + 4u);
    double z = (double)rdf32(m, v + 8u);
    double sq = x * x + y * y + z * z;
    if (sq < 1e-8) {
        wr32(m, out, 0u);
        wr32(m, out + 4u, 0u);
        wr32(m, out + 8u, 0u);
        return;
    }
    double s = 1.0 / sqrt(sq);
    float sf = (float)s;
    wrf32(m, out, (float)((double)sf * x));
    wrf32(m, out + 4u, (float)((double)sf * y));
    wrf32(m, out + 8u, (float)(s * z));
}

void ovr_core_vec_safe_normal(CPU *c)
{
    u8 *m = g_mem;
    u32 v = c->ecx, out = rd32(m, c->esp + 4u);
    if (core_math_check() && ovr_orig_ovr_core_vec_safe_normal) {
        u8 before_out[12], ours[12];
        memcpy(before_out, m + out, sizeof before_out);
        vec_safe_normal_apply(m, v, out);
        memcpy(ours, m + out, sizeof ours);
        memcpy(m + out, before_out, sizeof before_out);
        RT_ENTER_GEN(c, ovr_orig_ovr_core_vec_safe_normal);
        if (c->eax != out || memcmp(ours, m + out, sizeof ours) != 0)
            fprintf(stderr, "[core] SNX_MATH_CHECK: FVector::SafeNormal mismatch (eax %08x vs %08x)\n", c->eax, out);
        return;
    }
    vec_safe_normal_apply(m, v, out);
    c->eax = out;
    c->esp += 8u;  // thiscall, sret: ret 4
}

// Core.dll!0x1010cd60 (экспорт 0x101010b4, FVector::Normalize): thiscall без аргументов, ret, eax = 1/0.
// Порог — double 1e-8 (Core.dll!0x101904c8); при sq < 1e-8 или NaN возвращает 0 и не меняет вектор.
static u32 vec_normalize_apply(u8 *m, u32 v)
{
    double x = (double)rdf32(m, v);
    double y = (double)rdf32(m, v + 4u);
    double z = (double)rdf32(m, v + 8u);
    double sq = x * x + y * y + z * z;
    if (!(sq >= 1e-8))
        return 0u;
    double s = 1.0 / sqrt(sq);
    wrf32(m, v, (float)(s * x));
    wrf32(m, v + 4u, (float)(s * y));
    wrf32(m, v + 8u, (float)(s * z));
    return 1u;
}

void ovr_core_vec_normalize(CPU *c)
{
    u8 *m = g_mem;
    u32 v = c->ecx;
    if (core_math_check() && ovr_orig_ovr_core_vec_normalize) {
        u8 before[12], ours[12];
        memcpy(before, m + v, sizeof before);
        u32 r = vec_normalize_apply(m, v);
        memcpy(ours, m + v, sizeof ours);
        memcpy(m + v, before, sizeof before);
        RT_ENTER_GEN(c, ovr_orig_ovr_core_vec_normalize);
        if (c->eax != r || memcmp(ours, m + v, sizeof ours) != 0)
            fprintf(stderr, "[core] SNX_MATH_CHECK: FVector::Normalize mismatch (got %u, orig %u)\n", r, c->eax);
        return;
    }
    c->eax = vec_normalize_apply(m, v);
    c->esp += 4u;  // thiscall без аргументов: ret
}
