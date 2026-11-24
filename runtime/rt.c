// Ядро рантайма: гостевая память, таблица функций, ловушки, CPUID/RDTSC, хелперы x87 и строковых операций.
#include "runtime/rt.h"
#include "runtime/process.h"

#include <pthread.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <time.h>

#if defined(__APPLE__) || defined(__linux__)
#include <sys/mman.h>
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif
#endif

u8 *g_mem;
rt_trap_hook_fn rt_trap_hook;
u32 rt_trace_ring[256];
u32 rt_trace_pos;
u32 rt_trace_watch;
int rt_trace_blocks;
static FILE *g_blocklog;

void rt_trace_block(u32 a, u32 eax, u32 ecx, u32 edx, u32 ebx, u32 esp, u32 ebp, u32 esi, u32 edi)
{
    if (!g_blocklog) {
        const char *p = getenv("RT_BLOCKLOG");
        g_blocklog = fopen(p ? p : "blocklog.txt", "w");
    }
    fprintf(g_blocklog, "%08x %08x %08x %08x %08x %08x %08x %08x %08x\n", a, eax, ecx, edx, ebx, esp, ebp, esi, edi);
}

void rt_trace_watch_hit(CPU *c, u32 a)
{
    fprintf(stderr, "[trace] enter %08x:", a);
    rt_dump_cpu(c);
}

void rt_dump_trace(int n)
{
    fprintf(stderr, "  last functions:");
    for (int i = n; i > 0; i--)
        fprintf(stderr, " %08x", rt_trace_ring[(rt_trace_pos - (u32)i) & 255]);
    fprintf(stderr, "\n");
}

// ---------------------------------------------------------------- память
static void cnt_init(void);

int rt_mem_init(void)
{
    cnt_init();
#if defined(__APPLE__) || defined(__linux__)
    // 4 ГБ виртуального адреса: любой u32-адрес попадает в резерв, мусорный указатель даёт SIGSEGV,
    // а не порчу хостовой памяти. Рабочее окно [64 КБ, RT_WINDOW) — чтение/запись, страница 0 закрыта.
    void *p = mmap(NULL, 0x100000000ull, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED)
        return -1;
    if (mprotect((u8 *)p + 0x10000, RT_WINDOW - 0x10000, PROT_READ | PROT_WRITE) != 0) {
        munmap(p, 0x100000000ull);
        return -1;
    }
    g_mem = p;
    return 0;
#else
    return rt_platform_mem_init();
#endif
}

// ---------------------------------------------------------------- таблица функций
typedef struct {
    u32 addr;
    u32 native;  // функция хоста (HLE, оверрайд), а не сгенерированная
    guest_fn fn;
} slot_t;

static slot_t *g_tab;
static u32 g_tab_mask;
static u32 g_tab_count;

static inline u32 hash32(u32 x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

static void tab_insert(u32 addr, guest_fn fn, u32 native)
{
    if (!g_tab || (g_tab_count + 1) * 2 > g_tab_mask + 1) {
        u32 newcap = g_tab ? (g_tab_mask + 1) * 2 : 1u << 16;
        slot_t *old = g_tab;
        u32 oldcap = g_tab ? g_tab_mask + 1 : 0;
        g_tab = calloc(newcap, sizeof(slot_t));
        g_tab_mask = newcap - 1;
        g_tab_count = 0;
        for (u32 i = 0; i < oldcap; i++)
            if (old[i].fn)
                tab_insert(old[i].addr, old[i].fn, old[i].native);
        free(old);
    }
    u32 i = hash32(addr) & g_tab_mask;
    while (g_tab[i].fn && g_tab[i].addr != addr)
        i = (i + 1) & g_tab_mask;
    if (!g_tab[i].fn)
        g_tab_count++;
    g_tab[i].addr = addr;
    g_tab[i].native = native;
    g_tab[i].fn = fn;
}

void rt_register_function(u32 addr, guest_fn fn) { tab_insert(addr, fn, 1); }  // трамплины HLE

void rt_register_functions(const rt_func_entry *tab, u32 count)
{
    for (u32 i = 0; i < count; i++)
        tab_insert(tab[i].addr, tab[i].fn, tab[i].native);
}

// SNX_CALLSTATS=<файл> (сборка с SNX_TRACE): сколько раз вошли в каждую функцию гостя (ENTER); пишется при
// выходе строками «адрес число», для tools/probe/callcount.py. Потоки не синхронизируются — счёт приблизительный.
int rt_trace_counts;
#define CNT_SIZE (1u << 20)
static u32 *g_cnt_addr;
static u64 *g_cnt_n;
static const char *g_cnt_path;

void rt_trace_count(u32 a)
{
    u32 i = hash32(a) & (CNT_SIZE - 1);
    while (g_cnt_addr[i] && g_cnt_addr[i] != a)
        i = (i + 1) & (CNT_SIZE - 1);
    g_cnt_addr[i] = a;
    g_cnt_n[i]++;
}

static void cnt_dump(void)
{
    FILE *f = fopen(g_cnt_path, "w");
    if (!f)
        return;
    for (u32 i = 0; i < CNT_SIZE; i++)
        if (g_cnt_addr[i])
            fprintf(f, "%08x %llu\n", g_cnt_addr[i], (unsigned long long)g_cnt_n[i]);
    fclose(f);
}

static void cnt_init(void)
{
    g_cnt_path = getenv("SNX_CALLSTATS");
    if (!g_cnt_path)
        return;
    g_cnt_addr = calloc(CNT_SIZE, sizeof *g_cnt_addr);
    g_cnt_n = calloc(CNT_SIZE, sizeof *g_cnt_n);
    if (g_cnt_addr && g_cnt_n && atexit(cnt_dump) == 0)
        rt_trace_counts = 1;
}

guest_fn rt_lookup_k(u32 addr, int *native)
{
    if (!g_tab)
        return NULL;
    u32 i = hash32(addr) & g_tab_mask;
    while (g_tab[i].fn) {
        if (g_tab[i].addr == addr) {
            if (native)
                *native = (int)g_tab[i].native;
            return g_tab[i].fn;
        }
        i = (i + 1) & g_tab_mask;
    }
    return NULL;
}

guest_fn rt_lookup(u32 addr) { return rt_lookup_k(addr, NULL); }

void rt_invoke(CPU *c, guest_fn fn, int native)
{
    if (native)
        fn(c);
    else
        RT_ENTER_GEN(c, fn);
}

#ifdef SNX_PIN
// Трамплин рантайм -> код гостя с закреплёнными регистрами (runtime/abi.h). Регистры как в runtime/gen_rt.h: eax x19,
// ecx x20, edx x21, ebx x22, esp x23, ebp x24, esi x25, edi x26, m (g_mem) x27, CPU *c (g_pin_c) x28; в CPU
// eax..edi — смещения 0..28. x19–x28 у хоста сохраняемые: свои значения рантайма — в кадре трамплина.
_Static_assert(offsetof(CPU, eax) == 0 && offsetof(CPU, esp) == 16 && offsetof(CPU, edi) == 28, "CPU layout");
__asm__(".section .text.rt_pin_enter,\"ax\",%progbits\n"
        ".global rt_pin_enter\n"
        ".hidden rt_pin_enter\n"
        ".type rt_pin_enter, %function\n"
        ".p2align 2\n"
        "rt_pin_enter:\n"
        "    stp x29, x30, [sp, #-96]!\n"
        "    mov x29, sp\n"
        "    stp x19, x20, [sp, #16]\n"
        "    stp x21, x22, [sp, #32]\n"
        "    stp x23, x24, [sp, #48]\n"
        "    stp x25, x26, [sp, #64]\n"
        "    stp x27, x28, [sp, #80]\n"
        "    mov x28, x0\n"
        "    ldp w19, w20, [x0]\n"
        "    ldp w21, w22, [x0, #8]\n"
        "    ldp w23, w24, [x0, #16]\n"
        "    ldp w25, w26, [x0, #24]\n"
        "    adrp x27, g_mem\n"
        "    ldr x27, [x27, #:lo12:g_mem]\n"
        "    blr x1\n"
        "    stp w19, w20, [x28]\n"
        "    stp w21, w22, [x28, #8]\n"
        "    stp w23, w24, [x28, #16]\n"
        "    stp w25, w26, [x28, #24]\n"
        "    ldp x19, x20, [sp, #16]\n"
        "    ldp x21, x22, [sp, #32]\n"
        "    ldp x23, x24, [sp, #48]\n"
        "    ldp x25, x26, [sp, #64]\n"
        "    ldp x27, x28, [sp, #80]\n"
        "    ldp x29, x30, [sp], #96\n"
        "    ret\n"
        ".size rt_pin_enter, .-rt_pin_enter\n"
        ".previous\n");
#endif

int rt_check_abi;

static void check_abi(CPU *c, u32 target, u32 ret, const u32 *saved)
{
    static int reported;
    if (c->esi == saved[0] && c->edi == saved[1] && c->ebx == saved[2] && c->ebp == saved[3])
        return;
    if (!rt_looks_like_ret(ret) || reported++ >= 64)  // rt_call — это и косвенные переходы (таблицы switch)
        return;
    char t[128], r[128];
    rt_symbolize(target, t, sizeof t);
    rt_symbolize(ret, r, sizeof r);
    fprintf(stderr, "[abi] %s (ret %s) clobbered:", t, r);
    const char *nm[4] = {"esi", "edi", "ebx", "ebp"};
    const u32 now[4] = {c->esi, c->edi, c->ebx, c->ebp};
    for (int i = 0; i < 4; i++)
        if (now[i] != saved[i])
            fprintf(stderr, " %s %08x->%08x", nm[i], saved[i], now[i]);
    fprintf(stderr, "\n");
}

void rt_regchk_fail(CPU *c, u32 site, u32 target, u32 bad)
{
    (void)c;
    static u32 seen[256];
    static int nseen;
    for (int i = 0; i < nseen; i++)
        if (seen[i] == site)
            return;
    if (nseen < 256)
        seen[nseen++] = site;
    char s[128], t[128];
    rt_symbolize(site, s, sizeof s);
    rt_symbolize(target, t, sizeof t);
    static const char *nm[8] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
    fprintf(stderr, "[regchk] %s -> %s changed:", s, t);
    for (int i = 0; i < 8; i++)
        if (bad >> i & 1)
            fprintf(stderr, " %s", nm[i]);
    fprintf(stderr, "\n");
}

// ---------------------------------------------------------------- setjmp/longjmp гостя
// Точки setjmp потока: jmp_buf гостя -> продолжение (адрес возврата и esp после него) и хостовый jmp_buf
// в кадре сгенерированной функции. longjmp гостя (msvcr70) сам раскручивает SEH и восстанавливает регистры,
// а заканчивает переходом `jmp [buf+0x14]` на продолжение — его и ловит rt_call.
#define MAX_SJ 32
typedef struct {
    u32 jbuf, ret, esp;
    rt_jmp *j;
} sj_entry;
static _Thread_local sj_entry t_sj[MAX_SJ];
static _Thread_local int t_sj_next;

void rt_setjmp_register(CPU *c, rt_jmp *j)
{
    u32 jbuf = rd32(g_mem, c->esp + 4);
    int k = -1;
    for (int i = 0; i < MAX_SJ && k < 0; i++)
        if (t_sj[i].jbuf == jbuf)
            k = i;
    if (k < 0) {
        k = t_sj_next;
        t_sj_next = (t_sj_next + 1) % MAX_SJ;
    }
    t_sj[k] = (sj_entry){jbuf, rd32(g_mem, c->esp), c->esp + 4, j};
}

static rt_jmp *setjmp_continuation(const CPU *c, u32 target)
{
    for (int i = 0; i < MAX_SJ; i++)
        if (t_sj[i].j && t_sj[i].ret == target && t_sj[i].esp == c->esp)
            return t_sj[i].j;
    return NULL;
}

static _Noreturn void host_longjmp(rt_jmp *j)
{
#if defined(__APPLE__) || defined(__ANDROID__)
    _longjmp(j->buf, 1);  // без восстановления маски сигналов (системного вызова)
#else
    longjmp(j->buf, 1);
#endif
}

// ---------------------------------------------------------------- продолжения catch гостя
// Стек кадров rt_eh_frame потока (в кадрах хоста обёрток, см. abi.h). Кадр ниже по адресу, чем новый, — мёртвый
// (его функцию покинули через longjmp), его снимаем при входе; при longjmp — всё, что глубже цели.
static _Thread_local rt_eh_frame *t_eh;
static _Thread_local int t_eh_raising;  // идёт разбор исключения (RaiseException)

static void eh_drop_below(const void *p)
{
    while (t_eh && (const void *)t_eh < p)
        t_eh = t_eh->prev;
}

void rt_eh_enter(CPU *c, rt_eh_frame *f)
{
    eh_drop_below(f);
    f->frame = c->esp - 4;  // push ebp; mov ebp, esp
    f->target = 0;
    f->raising = t_eh_raising;
    f->prev = t_eh;
    t_eh = f;
}

void rt_eh_leave(rt_eh_frame *f) { t_eh = f->prev; }

// харнесс дифф-тестов: тест, прерванный siglongjmp (падение, таймаут), оставляет в списке кадры мёртвого стека —
// следующий тест мог бы пройти по ним
void rt_eh_reset(void)
{
    t_eh = NULL;
    t_eh_raising = 0;
}

int rt_eh_raising(int on)
{
    int prev = t_eh_raising;
    t_eh_raising = on;
    return prev;
}

// _JumpToContinuation: EBP — кадр функции с catch; живой кадр есть — longjmp в него, иначе возврат
static void eh_jump(CPU *c, u32 target)
{
    for (rt_eh_frame *e = t_eh; e; e = e->prev)
        if (e->frame == c->ebp && (void *)e > (void *)&e) {
            if (getenv("SNX_LOG_EH"))
                fprintf(stderr, "[eh] continuation %08x frame %08x esp %08x\n", target, c->ebp, c->esp);
            e->target = target;
            t_eh = e;
            // как при входе в кадр: 0, если он вне разбора исключения, и 1, если внутри блока catch внешнего
            // исключения (вложенное исключение, пойманное внутри catch: внешний разбор ещё идёт)
            t_eh_raising = e->raising;
            host_longjmp(&e->j);
        }
}

// вне разбора исключения функцию на адресе продолжения зовёт само тело (переход на неё — хвостовой вызов):
// longjmp тоже был бы верен, но дорог
void rt_eh_continue(CPU *c, u32 target)
{
    if (t_eh_raising)
        eh_jump(c, target);
}

void rt_call(CPU *c, u32 target)
{
    int native = 0;
    guest_fn fn = rt_lookup_k(target, &native);
    if (!fn) {
        rt_jmp *j = setjmp_continuation(c, target);
        if (j) {
            eh_drop_below(j);
            host_longjmp(j);
        }
        eh_jump(c, target);
        rt_trap(c, target, "call to unknown guest address");
        return;
    }
    if (!rt_check_abi) {
        rt_invoke(c, fn, native);
        return;
    }
    const u32 saved[4] = {c->esi, c->edi, c->ebx, c->ebp}, ret = rd32(g_mem, c->esp);
    rt_invoke(c, fn, native);
    check_abi(c, target, ret, saved);
}

#ifndef SNX_PIN
__attribute__((cold)) void rt_ic_base(CPU *c) { rt_trap(c, 0, "call to unknown guest address"); }
#endif

// промахи мест вызова (abi.h): без общего кеша каждый раз шли бы в хеш-таблицу всех функций
_Alignas(64) u64 rt_ic2[RT_IC2_SIZE];
// теневая таблица прямого отображения для диспетчера UnrealScript GNatives (4096 слотов)
_Alignas(64) u64 rt_gnatives_cache[RT_GNATIVE_SIZE];

void rt_call_ic(CPU *c, u32 target, u64 *ic)
{
    u64 *e = &rt_ic2[RT_IC2_IDX(target)];
    u64 v = __atomic_load_n(e, __ATOMIC_RELAXED);
    if ((u32)v != target || !target) {
        int native = 0;
        guest_fn fn = rt_check_abi ? NULL : rt_lookup_k(target, &native);  // с проверкой ABI — всегда через rt_call
        if (!fn) {
            rt_call(c, target);
            return;
        }
#ifdef SNX_PIN
        // Нативная функция хоста (COM/HLE/оверрайд) кешируется через pinned-ABI обёртку (runtime/icall_pin.c):
        // место вызова зовёт её напрямую без бита native и без ухода в rt_icall_slow.
        guest_fn cached = native ? rt_pin_wrap_native(fn) : fn;
#else
        guest_fn cached = fn;
#endif
        intptr_t off = cached ? (intptr_t)(uintptr_t)cached - (intptr_t)(uintptr_t)rt_ic_base : 0;
        if (!cached || off != (s32)off) {
            rt_call(c, target);
            return;
        }
        v = (u64)(u32)(s32)off << 32 | target;
        __atomic_store_n(e, v, __ATOMIC_RELEASE);
        __atomic_store_n(ic, v, __ATOMIC_RELEASE);
        rt_invoke(c, fn, native);
        return;
    }
    __atomic_store_n(ic, v, __ATOMIC_RELAXED);
    rt_invoke(c, (guest_fn)((uintptr_t)rt_ic_base + (intptr_t)((s64)v >> 32)), 0);
}

// ---------------------------------------------------------------- ошибки
void rt_dump_cpu(const CPU *c)
{
    fprintf(stderr, "  eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x\n", c->eax, c->ecx,
            c->edx, c->ebx, c->esp, c->ebp, c->esi, c->edi);
    if (g_mem && c->esp >= 0x10000 && c->esp < RT_WINDOW - 64) {
        fprintf(stderr, "  stack:");
        for (int i = 0; i < 12; i++)
            fprintf(stderr, " %08x", rd32(g_mem, c->esp + 4 * i));
        fprintf(stderr, "\n");
    }
}

void rt_trap(CPU *c, u32 addr, const char *what)
{
    if (rt_trap_hook)
        rt_trap_hook(c, addr, what);
    fprintf(stderr, "[rt] TRAP at %08x: %s\n", addr, what);
    rt_dump_cpu(c);
    fflush(stderr);
    abort();
}

void rt_unimpl(CPU *c, u32 addr, const char *insn)
{
    char buf[160];
    snprintf(buf, sizeof buf, "unimplemented instruction: %s", insn);
    rt_trap(c, addr, buf);
}

void rt_divzero(CPU *c, u32 addr) { rt_trap(c, addr, "integer divide error"); }

void rt_hle_missing(CPU *c, const char *dll, const char *name)
{
    char buf[200];
    snprintf(buf, sizeof buf, "HLE not implemented: %s!%s", dll, name);
    rt_trap(c, c->esp >= 0x10000 && c->esp < RT_WINDOW ? rd32(g_mem, c->esp) : 0, buf);
}

// ---------------------------------------------------------------- CPUID / RDTSC
void rt_cpuid(u32 leaf, u32 r[4])
{
    // Профиль «Pentium Pro»: x87, TSC, CX8, CMOV; без MMX/SSE/3DNow, чтобы движок шёл x87-путями.
    r[0] = r[1] = r[2] = r[3] = 0;
    switch (leaf) {
    case 0:
        r[0] = 1;
        r[1] = 0x756e6547; // "Genu"
        r[3] = 0x49656e69; // "ineI"
        r[2] = 0x6c65746e; // "ntel"
        break;
    case 1:
        r[0] = 0x00000611;
        r[3] = (1u << 0) | (1u << 4) | (1u << 8) | (1u << 15);
        break;
    case 0x80000000u:
        r[0] = 0x80000000u;
        break;
    }
}

// часто зовётся гостем (rdtsc — замеры статистики UE2): без системных вызовов
u64 rt_file_ns;
u32 rt_gc_epoch = 1;
int rt_menu_aspect_4x3;

u64 rt_ticks_ns(void)
{
#if defined(__SWITCH__)
    u64 t;  // системный счётчик ARM, 19,2 МГц (как armGetSystemTick/armTicksToNs libnx)
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(t));
    return t * 625 / 12;
#elif defined(__APPLE__)
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);  // CLOCK_MONOTONIC здесь идёт через gettimeofday
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
#endif
}

// Паузируемые монотонные часы гостя: защищают UE2 от гигантских дельт времени после HOME/сна.
// Инвариант под g_clock_lock: g_clock_guest + g_clock_off == g_clock_raw, g_clock_guest не убывает.
#define RT_GUEST_MAX_STEP_NS 500000000ull  // 500 мс: защита от скачка при сне ОС без предварительной паузы
static pthread_mutex_t g_clock_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_clock_init;
static int g_clock_paused;
static u64 g_clock_raw;    // последнее учтённое rt_ticks_ns()
static u64 g_clock_guest;  // текущее гостевое время, нс
static u64 g_clock_off;    // накопленное вычтенное время пауз и срезанных скачков

static void clock_init_locked(u64 raw)
{
    if (g_clock_init)
        return;
    g_clock_init = 1;
    g_clock_raw = raw;
    g_clock_guest = raw;
    g_clock_off = 0;
}

static void clock_advance_locked(u64 raw)
{
    clock_init_locked(raw);
    if (raw <= g_clock_raw)
        return;
    u64 dt = raw - g_clock_raw;
    g_clock_raw = raw;
    if (g_clock_paused) {
        g_clock_off += dt;
        return;
    }
    if (dt > RT_GUEST_MAX_STEP_NS) {
        g_clock_off += dt - RT_GUEST_MAX_STEP_NS;
        dt = RT_GUEST_MAX_STEP_NS;
    }
    g_clock_guest += dt;
}

u64 rt_guest_ticks_ns(void)
{
    pthread_mutex_lock(&g_clock_lock);
    clock_advance_locked(rt_ticks_ns());
    u64 r = g_clock_guest;
    pthread_mutex_unlock(&g_clock_lock);
    return r;
}

void rt_clock_pause(int on)
{
    pthread_mutex_lock(&g_clock_lock);
    clock_advance_locked(rt_ticks_ns());
    g_clock_paused = on ? 1 : 0;
    pthread_mutex_unlock(&g_clock_lock);
}

void rt_clock_sync(void)
{
    pthread_mutex_lock(&g_clock_lock);
    u64 raw = rt_ticks_ns();
    clock_init_locked(raw);
    if (raw > g_clock_raw) {
        g_clock_off += raw - g_clock_raw;
        g_clock_raw = raw;
    }
    pthread_mutex_unlock(&g_clock_lock);
}

u64 (*rt_rdtsc_hook)(void);

u64 rt_rdtsc(void)
{
    // «Такты» = наносекунды (1 ГГц); движок калибрует частоту сам.
    return rt_rdtsc_hook ? rt_rdtsc_hook() : rt_ticks_ns();
}

// ---------------------------------------------------------------- строковые операции
// по элементу размера size (1, 2, 4) — константы в ветках, без вызова memcpy на элемент
#define REP_EACH(size, body)                                                                                         \
    switch (size) {                                                                                                  \
    case 1: { enum { SZ = 1 }; body; } break;                                                                        \
    case 2: { enum { SZ = 2 }; body; } break;                                                                        \
    default: { enum { SZ = 4 }; body; } break;                                                                       \
    }

static inline u32 rep_get(const u8 *p, int sz)
{
    return sz == 4 ? rd32(p, 0) : sz == 2 ? rd16(p, 0) : p[0];
}

static inline void rep_put(u8 *p, int sz, u32 v)
{
    if (sz == 4)
        wr32(p, 0, v);
    else if (sz == 2)
        wr16(p, 0, (u16)v);
    else
        p[0] = (u8)v;
}

void rt_rep_movs(u8 *m, u32 s, u32 d, u32 n, int size, int df)
{
    if (!n)
        return;
    u32 bytes = n * (u32)size;
    if (!df && (d >= s + bytes || d + bytes <= s || d <= s)) {
        // вперёд без перекрытия «назад» — побайтово эквивалентно memmove
        memmove(m + d, m + s, bytes);
        return;
    }
    // назад (df) или перекрытие вперёд (размножение образца): по элементу, как сам rep
    u32 step = df ? 0u - (u32)size : (u32)size;
    REP_EACH(size, for (u32 i = 0; i < n; i++, s += step, d += step) rep_put(m + d, SZ, rep_get(m + s, SZ)))
}

void rt_rep_stos(u8 *m, u32 d, u32 n, u32 val, int size, int df)
{
    u32 b = val & 0xFF;
    if (!df && (size == 1 || (size == 2 ? (val & 0xFFFF) == b * 0x101u : val == b * 0x1010101u))) {
        memset(m + d, (int)b, (size_t)n * (u32)size);  // обнуление и любая заливка из одинаковых байтов
        return;
    }
    u32 step = df ? 0u - (u32)size : (u32)size;
    REP_EACH(size, for (u32 i = 0; i < n; i++, d += step) rep_put(m + d, SZ, val))
}
#undef REP_EACH

// ---------------------------------------------------------------- x87
double rt_f80_to_f64(const u8 *p)
{
    u64 mant;
    u16 se;
    memcpy(&mant, p, 8);
    memcpy(&se, p + 8, 2);
    int sign = se >> 15;
    int exp = se & 0x7FFF;
    double v;
    u64 bits;
    if (exp == 0 && mant == 0)
        v = 0.0;
    else if (exp != 0 && !(mant >> 63)) {
        bits = 0xFFF8000000000000ull; // unnormal, псевдо-inf/NaN: x87 не поддерживает, даёт «неопределённое»
        memcpy(&v, &bits, 8);
        return v;
    } else if (exp == 0x7FFF && (mant << 1)) {
        // NaN: мантисса переносится, сигнальный становится тихим (как при fld)
        bits = (u64)sign << 63 | 0x7FF8000000000000ull | (mant >> 11 & 0x000FFFFFFFFFFFFFull);
        memcpy(&v, &bits, 8);
        return v;
    } else if (exp == 0x7FFF)
        v = INFINITY;
    else
        v = ldexp((double)mant, exp - 16383 - 63);
    return sign ? -v : v;
}

void rt_f64_to_f80(u8 *p, double v)
{
    u64 mant = 0;
    u16 se = 0;
    if (signbit(v)) {
        se = 0x8000;
        v = -v;
    }
    if (v == 0.0) {
    } else if (isinf(v)) {
        se |= 0x7FFF;
        mant = 0x8000000000000000ull;
    } else if (isnan(v)) {
        se |= 0x7FFF;
        mant = 0xC000000000000000ull;
    } else {
        int e;
        double f = frexp(v, &e); // v = f * 2^e, f in [0.5, 1)
        mant = (u64)ldexp(f, 64);
        se |= (u16)(e - 1 + 16383);
    }
    memcpy(p, &mant, 8);
    memcpy(p + 8, &se, 2);
}

static double round_mode(u32 cw, double v)
{
    switch ((cw >> 10) & 3) {
    case 0:
        return nearbyint(v); // хост по умолчанию — к ближайшему чётному
    case 1:
        return floor(v);
    case 2:
        return ceil(v);
    default:
        return trunc(v);
    }
}

s64 rt_fist(CPU *c, double v, int bits)
{
    double r = round_mode(c->fpucw, v);
    if (bits == 16) {
        if (!(r >= -32768.0 && r <= 32767.0))
            return (s64)(s16)0x8000;
        return (s64)r;
    }
    if (bits == 32) {
        if (!(r >= -2147483648.0 && r <= 2147483647.0))
            return (s64)(s32)0x80000000u;
        return (s64)r;
    }
    if (!(r >= -9223372036854775808.0 && r < 9223372036854775808.0))
        return INT64_MIN;
    return (s64)r;
}

s64 rt_ftrunc(double v, int bits)
{
    double r = trunc(v);
    if (bits == 16)
        return (r >= -32768.0 && r <= 32767.0) ? (s64)r : (s64)(s16)0x8000;
    if (bits == 32)
        return (r >= -2147483648.0 && r <= 2147483647.0) ? (s64)r : (s64)(s32)0x80000000u;
    return (r >= -9223372036854775808.0 && r < 9223372036854775808.0) ? (s64)r : INT64_MIN;
}

double rt_frndint(CPU *c, double v) { return round_mode(c->fpucw, v); }

u32 rt_fxam(double v)
{
    // C3 C2 C0: класс; C1 — знак
    u32 c1 = signbit(v) ? 0x200u : 0;
    if (isnan(v))
        return c1 | 0x0100u;
    if (isinf(v))
        return c1 | 0x0500u;
    if (v == 0.0)
        return c1 | 0x4000u;
    if (fpclassify(v) == FP_SUBNORMAL)
        return c1 | 0x4400u;
    return c1 | 0x0400u;
}
