// Запуск игры: модули по фиксированной карте, DllMain по зависимостям, затем точка входа game.exe в своём
// потоке; главный поток — окно и события SDL (platform/window.c). Останавливается на первой ловушке.
//   snx_boot [--game game] [--user build/userdata] [--dlls-only] [--headless] [--seconds N] [--res WxH|0]
//            [--env ИМЯ=ЗНАЧЕНИЕ] [--prof FILE [--prof-after S] [--prof-for S]] [--pgo-dump S] [-- аргументы игры]
// --headless: без SDL (конфигурация с NullRenderDevice); --seconds N: через N секунд сбросить лог движка
// (game.log) и выйти с кодом 0 (проверка «тикает ли»); --res: полноэкранное разрешение игры в game.ini
// (на Switch по умолчанию — размер экрана, 0 — не трогать); --env: переменная окружения (SNX_*; на Switch
// иначе не задать); --prof: профиль (host/boot/prof.h); --pgo-dump: сборка со счётчиками PGO — когда писать профиль.
// Switch (NRO): игра — sdmc:/switch/spiderman2/game, запись — sdmc:/switch/spiderman2/user; при запуске через nxlink
// stdout/stderr идут в терминал Мака. Русская версия (SNX_VARIANT=rus) — то же в shrek2rus, на маке —
// build/game-rus и build/userdata-rus.
// Android (libmain.so, SDL_main): каталоги игры и записи, разрешение и переменные SNX_* передаёт лаунчер
// (android/…/GameActivity.kt: --game, --user, --res, --env); лог — <user>/snx.log (прошлый — snx.log.old).
#ifdef __ANDROID__
#include <SDL_main.h>  // main -> SDL_main: её зовёт SDLActivity
#include <SDL_locale.h>
#include <SDL_messagebox.h>
#include <SDL_stdinc.h>
#include <sys/stat.h>
#endif
#include <pthread.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef __SWITCH__
#include <switch.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#if !defined(__ANDROID__) || __ANDROID_API__ >= 33
#include <execinfo.h>
#define SNX_HAVE_BACKTRACE 1
#endif
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
#endif

#include "boot_modules.h"
#include "host/boot/config.h"
#include "host/boot/input.h"
#include "host/boot/prof.h"
#include "platform/platform.h"
#include "platform/window.h"
#include "runtime/game/uobj.h"
#include "runtime/hle/gl_cs.h"
#include "runtime/hle/hle.h"
#include "runtime/hle/openal.h"
#include "runtime/hle/user32.h"
#include "runtime/process.h"
#include "runtime/rt.h"

#ifdef SNX_VARIANT_RUS
#define SNX_SD_DIR "sdmc:/switch/spiderman2rus"
#define SNX_MAC_GAME "build/game-rus"
#define SNX_MAC_USER "build/userdata-rus"
#else
#define SNX_SD_DIR "sdmc:/switch/spiderman2"
#define SNX_MAC_GAME "game"
#define SNX_MAC_USER "build/userdata"
#endif
#ifdef __SWITCH__
static const char *g_game = SNX_SD_DIR "/game", *g_user = SNX_SD_DIR "/user";
#else
static const char *g_game = SNX_MAC_GAME, *g_user = SNX_MAC_USER;
#endif
static char g_args[1024];
static int g_dlls_only, g_headless;
static u64 g_deadline_ns;
static const char *g_prof_path;
static double g_prof_after, g_prof_for;
static double g_pgo_dump = 300;
static int g_res_w = -1, g_res_h = -1;  // --res; -1 — на Switch размер экрана, на маке — как в game.ini
#ifndef __SWITCH__
static u8 *g_stack;  // стек потока гостя (свой mmap: по выходу видно, сколько его тронуто)
static size_t g_stack_size = RT_HOST_STACK_MAIN;

// глубина стека гостя: страницы, которых коснулись (стек растёт вниз от конца области)
static void stack_report(void)
{
    size_t pg = (size_t)getpagesize(), n = g_stack_size / pg, lo = n;
#ifdef __linux__
    unsigned char *v = malloc(n);
#else
    char *v = malloc(n);
#endif
    if (v && mincore(g_stack, g_stack_size, v) == 0)
        for (size_t i = 0; i < n && lo == n; i++)
            if (v[i] & 0x7F)  // в памяти, сжата или выгружена (0x80 — просто «анонимная»)
                lo = i;
    free(v);
    fprintf(stderr, "[boot] guest thread stack touched: %zu KB of %zu MB\n", (n - lo) * pg / 1024, g_stack_size >> 20);
}
#endif

// GLog->Flush() (FOutputDevice: [0] Serialize, [1] Flush) и буферы CRT гостя (game.log пишется через fwrite
// msvcr71) — однократно: при ловушке внутри самого сброса повторно не входим
static void flush_guest_logs(CPU *c)
{
    static int busy;
    if (busy)
        return;
    busy = 1;
    rt_module *core = rt_module_by_name("Core"), *crt = rt_module_by_name("msvcr71");
    u32 glog_p = core ? rt_module_export(core, "?GLog@@3PAVFOutputDevice@@A", 0) : 0;
    u32 glog = glog_p ? rd32(g_mem, glog_p) : 0;
    u32 flushall = crt ? rt_module_export(crt, "_flushall", 0) : 0;
    if (glog) {
        c->ecx = glog;
        rt_guest_call(c, rd32(g_mem, rd32(g_mem, glog) + 4), 0, NULL);
    }
    if (flushall)
        rt_guest_call(c, flushall, 0, NULL);
}

// текст последней фатальной ошибки движка (appErrorf копирует его в GErrorHist перед throw); 0 — пусто
static int error_hist(char *text, size_t cap)
{
    rt_module *core = rt_module_by_name("Core");
    u32 hist = core ? rt_module_export(core, "?GErrorHist@@3PAGA", 0) : 0;
    if (!hist || !rd16(g_mem, hist))
        return 0;
    hle_w2utf8(hist, text, cap);
    return 1;
}

#ifdef SNX_PGO_GEN
static void pgo_dump(void);
#endif

// Ловушка: бэктрейс гостя, текст фатальной ошибки движка, сброс логов
static void on_trap(CPU *c, u32 addr, const char *what)
{
    fprintf(stderr, "[rt] TRAP at %08x: %s\n", addr, what);
    rt_dump_cpu(c);
    rt_backtrace(c);
    char text[4096];
    if (error_hist(text, sizeof text))
        fprintf(stderr, "[boot] GErrorHist: %s\n", text);
    GuestThread *t = rt_thread_current();
    if (!t || (c->esp > t->stack_lo + 64 && c->esp <= t->stack_hi))  // esp мусорный (ловушка вне кода гостя) — без
        flush_guest_logs(c);                                          // вызовов гостя: rt_guest_call кладёт на стек
    hle_file_flush_all();
    rt_platform_cpu_boost_reset();
#ifdef SNX_PGO_GEN
    pgo_dump();  // счётчики — в памяти хоста: прогон до ловушки не пропадает
#endif
    fflush(stderr);
#ifdef __SWITCH__
    // abort() вернул бы управление hbloader, а поток рендера, звук и фоновые потоки гостя жили бы дальше:
    // замёрзший кадр вместо выхода. Процесс — целиком, как при обычном выходе (exit_process).
    fflush(NULL);
    svcExitProcess();
#endif
    abort();
}

// Модальные диалоги гостя без окна на экране (runtime/hle/user32.h). Фатальная ошибка движка: game.exe ловит
// исключение и показывает «Critical Error» с текстом ошибки, а по OK завершает игру. В порте окна нет — текст в лог
// (и на Switch — системным окном ошибки), затем OK; иначе игра «висела» бы в цикле сообщений диалога.
static u32 on_modal(const char *title, const char *texts)
{
    if (!strstr(title, "Error") && !strstr(title, "error"))
        return 0;
    char hist[4096] = "";
    error_hist(hist, sizeof hist);
    fprintf(stderr, "[boot] %s: %s\n%s", title, hist, texts);
    fflush(stderr);
#ifdef __SWITCH__
    ErrorApplicationConfig cfg;
    char brief[512];
    snprintf(brief, sizeof brief, "Spider-Man 2: %s\n%.400s", title, hist[0] ? hist : texts);
    if (R_SUCCEEDED(errorApplicationCreate(&cfg, brief, hist[0] ? hist : texts)))
        errorApplicationShow(&cfg);
#endif
    return 1;  // IDOK
}

// SNX_EH_TEST=<с>: через столько секунд поток гостя загружает несуществующий объект — StaticLoadObject бросает
// исключение C++ и сам его ловит (так падал магазин на шрифте); проверка исключений гостя без прохода по игре
static u64 g_eh_test_ns;

static void eh_test(void)
{
    GuestThread *t = rt_thread_current();
    rt_module *core = rt_module_by_name("Core");
    if (!t || !core)
        return;
    CPU *c = &t->cpu, saved = *c;
    u32 cls_fn = rt_module_export(core, "?StaticClass@UObject@@SAPAVUClass@@XZ", 0);
    u32 load = rt_module_export(core, "?StaticLoadObject@UObject@@SAPAV1@PAVUClass@@PAV1@PBG2KPAVUPackageMap@@@Z", 0);
    static const char name[] = "Engine.NoSuchObjectForEHTest";
    c->esp -= 64;
    for (u32 i = 0; i < sizeof name; i++)
        wr16(g_mem, c->esp + 2 * i, (u8)name[i]);
    u32 args[6] = {rt_guest_call(c, cls_fn, 0, NULL), 0, c->esp, 0, 0, 0};
    u32 r = rt_guest_call(c, load, 6, args);
    fprintf(stderr, "[boot] SNX_EH_TEST: StaticLoadObject -> %08x, esp %08x (was %08x)\n", r, c->esp + 64, saved.esp);
    *c = saved;
}

// SNX_ERR_TEST=<с>: через столько секунд поток гостя делает то же, что appErrorf, — GError->Serialize(текст)
// (FOutputDevice: [0] Serialize(текст, событие)); это путь фатальной ошибки движка: исключение C++, game.exe
// ловит его в WinMain и показывает окно ошибки
static u64 g_err_test_ns;

static void err_test(void)
{
    GuestThread *t = rt_thread_current();
    rt_module *core = rt_module_by_name("Core");
    u32 gerr_p = core ? rt_module_export(core, "?GError@@3PAVFOutputDeviceError@@A", 0) : 0;
    u32 gerr = gerr_p ? rd32(g_mem, gerr_p) : 0;
    if (!t || !gerr)
        return;
    CPU *c = &t->cpu;
    static const char text[] = "SNX_ERR_TEST";
    c->esp -= 64;
    for (u32 i = 0; i < sizeof text; i++)
        wr16(g_mem, c->esp + 2 * i, (u8)text[i]);
    u32 args[2] = {c->esp, 0};
    fprintf(stderr, "[boot] SNX_ERR_TEST: GError->Serialize\n");
    c->ecx = gerr;
    rt_guest_call(c, rd32(g_mem, rd32(g_mem, gerr)), 2, args);  // не возвращается: исключение C++ гостя
}

// SNX_CFG_TEST=<с>[,<n>[,<мс>]]: через столько секунд n раз (20) с паузой <мс> (250; 0 — все подряд в одном кадре) то
// же, что подбор монеты на бонусном уровне (SHGame Coin.AnnouncePickup на картах BEANSTALK*): bonuscoins контроллера
// героя +1 и его SaveConfig. Время каждого — строкой [core] SaveConfig из runtime/overrides_core.c. Подряд каждый
// следующий ждёт фоновую запись User.ini предыдущего (строка [file] waited). Нужен уровень с героем.
static u64 g_cfg_test_ns, g_cfg_test_gap_ns = 250000000;
static int g_cfg_test_n = 20;
static u32 g_cfg_test_obj, g_cfg_test_save, g_cfg_test_coins;  // контроллер героя, SaveConfig, смещение bonuscoins

static void cfg_test(void)
{
    GuestThread *t = rt_thread_current();
    if (!g_cfg_test_obj) {
        rt_module *core = rt_module_by_name("Core");
        u32 obj = 0, save = core ? rt_module_export(core, "?SaveConfig@UObject@@QAEXKPBG@Z", 0) : 0;
        for (u32 i = 0, n = uo_init() ? uo_count() : 0; i < n && !obj; i++)
            if (uo_is_a(uo_at(i), "ShHeroController"))
                obj = uo_at(i);
        uo_prop coins;
        if (!t || !save || !obj || !uo_find_prop(uo_class(obj), "bonuscoins", &coins)) {
            fprintf(stderr, "[boot] SNX_CFG_TEST: no hero controller\n");
            g_cfg_test_ns = 0;
            return;
        }
        g_cfg_test_obj = obj, g_cfg_test_save = save, g_cfg_test_coins = coins.offset;
    }
    u32 at = g_cfg_test_obj + g_cfg_test_coins;
    do {
        CPU *c = &t->cpu, saved = *c;
        wr32(g_mem, at, rd32(g_mem, at) + 1);
        u32 args[2] = {0x4000 /* CPF_Config */, 0};
        c->esp -= 64;
        c->ecx = g_cfg_test_obj;
        rt_guest_call(c, g_cfg_test_save, 2, args);
        *c = saved;
    } while (--g_cfg_test_n > 0 && !g_cfg_test_gap_ns);
    g_cfg_test_ns = g_cfg_test_n > 0 ? rt_ticks_ns() + g_cfg_test_gap_ns : 0;
}

// --seconds, отладочные таймеры и доставка отложенного WM_ACTIVATEAPP: проверяются в цикле сообщений гостя
static u64 g_uo_dump_ns;  // SNX_UO_DUMP=<с>: разведка объектов гостя (runtime/game/uobj.h)
static _Atomic int g_pending_focus;  // 0 — нет, 1 — потеря фокуса, 2 — возврат фокуса
static _Atomic int g_guest_done;
static _Atomic int g_exit_code;
static _Atomic int g_shutdown_done;
static void boot_shutdown(void);

// События жизненного цикла ОС (главный поток SDL/libnx -> гость, звук, игровые часы)
static void on_focus(int focused)
{
    fprintf(stderr, "[boot] focus %s\n", focused ? "gained" : "lost");
    rt_clock_pause(!focused);
    openal_pause(!focused);
    u32 hwnd = user32_focus();
    if (hwnd) {
        atomic_store(&g_pending_focus, 0);
        user32_post(hwnd, 0x1C /* WM_ACTIVATEAPP */, focused ? 1u : 0u, 0);
    } else {
        atomic_store(&g_pending_focus, focused ? 2 : 1);
    }
}

static void on_resume(void)
{
    fprintf(stderr, "[boot] resume from sleep\n");
    // Поглотить время во сне в смещении часов гостя, чтобы UE2 не получил гигантский dt
    rt_clock_sync();
}

static void on_screen_mode(int w, int h)
{
    fprintf(stderr, "[boot] screen mode changed: %dx%d\n", w, h);
    user32_set_screen(w, h);
}

static void on_exit_request(void)
{
    fprintf(stderr, "[boot] system exit requested\n");
    u32 hwnd = user32_focus();
    if (hwnd)
        user32_post(hwnd, 0x10 /* WM_CLOSE */, 0, 0);
}

static void pump_hook(void)
{
    int pf = atomic_exchange_explicit(&g_pending_focus, 0, memory_order_acq_rel);
    if (pf) {
        u32 hwnd = user32_focus();
        if (hwnd) {
            user32_post(hwnd, 0x1C /* WM_ACTIVATEAPP */, pf == 2 ? 1u : 0u, 0);
        } else {
            int expected = 0;
            atomic_compare_exchange_strong(&g_pending_focus, &expected, pf);
        }
    }
    if (g_eh_test_ns && rt_ticks_ns() >= g_eh_test_ns) {
        g_eh_test_ns = 0;
        eh_test();
    }
    if (g_err_test_ns && rt_ticks_ns() >= g_err_test_ns) {
        g_err_test_ns = 0;
        err_test();
    }
    if (g_cfg_test_ns && rt_ticks_ns() >= g_cfg_test_ns)
        cfg_test();  // ставит время следующего подбора
    if (g_uo_dump_ns && rt_ticks_ns() >= g_uo_dump_ns) {
        g_uo_dump_ns = 0;
        uo_explore();
    }
    if (!g_deadline_ns || rt_ticks_ns() < g_deadline_ns)
        return;
    GuestThread *t = rt_thread_current();
    fprintf(stderr, "[boot] --seconds elapsed, exiting\n");
    if (t)
        flush_guest_logs(&t->cpu);
    boot_shutdown();
#ifndef __SWITCH__
    stack_report();
#endif
    exit(0);
}

// Обращение гостя вне отображённой памяти (NULL и т. п.): на ПК — access violation. Пока исключения гостя
// (SEH) не доставляются, это ловушка; регистры в контексте — на момент последнего сброса из функции.
#ifdef __SWITCH__
// on_trap зовёт код гостя (сброс GLog и буферов CRT) прямо на этом стеке: рекурсия гостя — рекурсия хоста
alignas(16) u8 __nx_exception_stack[0x40000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);
u32 __nx_exception_ignoredebug = 1;

void __libnx_exception_handler(ThreadExceptionDump *ctx)
{
    u64 far = ctx->far.x;
    GuestThread *t = rt_thread_current();
    char what[160];
    // На Switch окно гостя — RT_WINDOW (1 ГБ), дальше идёт куча хоста: не принимать её сбои за гостевой AV
    if (t && far >= (u64)(uintptr_t)g_mem && far < (u64)(uintptr_t)g_mem + (u64)RT_WINDOW) {
#ifdef SNX_PIN
        // регистры гостя закреплены за x19–x26 (runtime/gen_rt.h): в отчёте — из контекста исключения
        t->cpu.eax = (u32)ctx->cpu_gprs[19].x, t->cpu.ecx = (u32)ctx->cpu_gprs[20].x;
        t->cpu.edx = (u32)ctx->cpu_gprs[21].x, t->cpu.ebx = (u32)ctx->cpu_gprs[22].x;
        t->cpu.esp = (u32)ctx->cpu_gprs[23].x, t->cpu.ebp = (u32)ctx->cpu_gprs[24].x;
        t->cpu.esi = (u32)ctx->cpu_gprs[25].x, t->cpu.edi = (u32)ctx->cpu_gprs[26].x;
        const char *regs_note = "registers from the fault context";
#else
        const char *regs_note = "registers are stale";
#endif
        snprintf(what, sizeof what, "guest access violation at %08x (host pc %#llx; %s)",
                 (u32)(far - (u64)(uintptr_t)g_mem), (unsigned long long)ctx->pc.x, regs_note);
    } else
        snprintf(what, sizeof what, "host exception %#x at pc %#llx, far %#llx", ctx->error_desc,
                 (unsigned long long)ctx->pc.x, (unsigned long long)far);
    // место в образе: смещения от _start — адреса snx_boot.elf (addr2line -f -e snx_boot.elf); lr — вызывающий для
    // листовых функций (memcpy), затем цепочка x29 в пределах стека потока
    extern char _start[];
    u64 base = (u64)(uintptr_t)_start, fp = ctx->fp.x, sp = ctx->sp.x;
    fprintf(stderr, "[boot] host pc +%#llx, lr +%#llx (snx_boot.elf), frames:", (unsigned long long)(ctx->pc.x - base),
            (unsigned long long)(ctx->lr.x - base));
    for (int i = 0; i < 12 && fp >= sp && fp < sp + (512ull << 20) && !(fp & 15); i++) {
        u64 next = ((u64 *)(uintptr_t)fp)[0], ret = ((u64 *)(uintptr_t)fp)[1];
        fprintf(stderr, " +%#llx", (unsigned long long)(ret - base));
        if (next <= fp)
            break;
        fp = next;
    }
    fprintf(stderr, "\n");
    if (t)
        on_trap(&t->cpu, 0, what);
    fprintf(stderr, "[boot] %s\n", what);
    fflush(NULL);
    svcExitProcess();
}
#else
static void on_fault(int sig, siginfo_t *si, void *uc)
{
    (void)uc;
    u8 *a = (u8 *)si->si_addr;
    GuestThread *t = rt_thread_current();
    if (!t || a < g_mem || a >= g_mem + 0x100000000ull) {
        signal(sig, SIG_DFL);
        return;
    }
    char what[96];
    snprintf(what, sizeof what, "guest access violation at %08x (host signal %d; registers are stale)", (u32)(a - g_mem), sig);
#ifdef SNX_HAVE_BACKTRACE
    void *bt[48];
    backtrace_symbols_fd(bt, backtrace(bt, 48), 2);  // где в сгенерированном коде (f_<адрес>)
#endif
    on_trap(&t->cpu, 0, what);
}
#endif

#ifdef SNX_PGO_GEN
// Сборка со счётчиками профиля (cmake -DSNX_PGO=gen, AGENTS.md, «PGO»): счётчики уходят в
// sdmc:/switch/spiderman2/pgo/*.gcda через --pgo-dump S секунд или при выходе из игры — что раньше. Выход завершает
// процесс сразу (exit_process), деструкторы libgcov не успевают; закрытие через HOME профиль не пишет
void __gcov_dump(void);

// Профиль косвенных вызовов — свой, вместо объектника libgcov (_gcov_indirect_call_profiler_v4.o): libgcov.a
// devkitA64 собрана с TLS через tpidr_el0, а libnx держит указатель TLS в tpidrro_el0 (наш код — -mtp=soft), и
// первый же косвенный вызов писал мимо (сборка со счётчиками висела на чёрном экране). Тело — как в GCC 16
// (libgcov-profiler.c): вызов попал в функцию, которую отметило место вызова, — счётчик top-N места; сами функции
// не инструментируются (вход каждой функции зовёт этот профайлер, пока callee не NULL)
typedef long long gcov_type;
struct gcov_ic_tuple {
    void *callee;
    gcov_type *counters;
};
__thread struct gcov_ic_tuple __gcov_indirect_call;
void __gcov_topn_values_profiler(gcov_type *counters, gcov_type value);
void __gcov_topn_values_profiler_atomic(gcov_type *counters, gcov_type value);

__attribute__((no_profile_instrument_function)) void __gcov_indirect_call_profiler_v4(gcov_type value, void *cur_func)
{
    if (cur_func == __gcov_indirect_call.callee)
        __gcov_topn_values_profiler(__gcov_indirect_call.counters, value);
    __gcov_indirect_call.callee = NULL;
}

__attribute__((no_profile_instrument_function)) void __gcov_indirect_call_profiler_v4_atomic(gcov_type value,
                                                                                            void *cur_func)
{
    if (cur_func == __gcov_indirect_call.callee)
        __gcov_topn_values_profiler_atomic(__gcov_indirect_call.counters, value);
    __gcov_indirect_call.callee = NULL;
}

static void pgo_dump(void)
{
    static int done;
    if (__atomic_fetch_add(&done, 1, __ATOMIC_RELAXED))
        return;
    u64 t0 = rt_ticks_ns();
    fprintf(stderr, "[pgo] writing profile counters...\n");
    mkdir("sdmc:/switch/spiderman2/pgo", 0755);  // libgcov на newlib каталогов не создаёт (cmake/gen.cmake: путь)
    __gcov_dump();
    fprintf(stderr, "[pgo] profile written in %.1f s: sdmc:/switch/spiderman2/pgo\n", (double)(rt_ticks_ns() - t0) / 1e9);
}

static void *pgo_timer(void *arg)
{
    (void)arg;
    struct timespec ts = {(time_t)g_pgo_dump, (long)((g_pgo_dump - (double)(time_t)g_pgo_dump) * 1e9)};
    while (nanosleep(&ts, &ts) != 0) {
    }
    pgo_dump();
    return NULL;
}
#endif

// Аккуратная остановка подсистем при штатном выходе (идемпотентна). Не вызывается из on_trap.
static void boot_shutdown(void)
{
    if (atomic_exchange(&g_shutdown_done, 1))
        return;
    plat_quit();
    cs_stop();
    openal_shutdown();
    hle_file_flush_all();
    rt_platform_cpu_boost_reset();
    prof_dump();
#ifdef SNX_PGO_GEN
    pgo_dump();
#endif
    fflush(NULL);
}

#ifdef __SWITCH__
// NRO исполняется в процессе hbloader: поток рендера и OpenAL останавливаются в boot_shutdown(), а файлы сбрасываются
// через hle_file_flush_all(), но фоновые потоки файлового слоя (kernel32_file.c: ws_worker, wc_worker, fimg_worker)
// и фоновые потоки гостя (CreateThread) пока не имеют кооперативного завершения. Обычный возврат в hbmenu выгрузил
// бы код NRO при живых потоках -> падение; поэтому завершаем процесс целиком через svcExitProcess [проверить на Switch].
static void exit_process(void)
{
    boot_shutdown();
    svcExitProcess();
}
#elif defined(__ANDROID__)
// Android — то же: обычный exit() зовёт деструкторы libmain/OpenAL/SDL, пока поток рендера, звук и потоки гостя
// ещё работают, — падение или зависание. Зависший процесс :game оставался жить, и следующий запуск из лаунчера
// попадал в него: картинка есть, а касания и игра молчат. Файлы гостя к этому времени сброшены
// (hle_file_flush_all — обработчик atexit, поставленный позже этого, идёт раньше).
static void exit_process(void)
{
    fflush(NULL);
    _exit(0);
}
#endif

static void *boot(void *arg)
{
    (void)arg;
    int rc = 1;
    rt_platform_thread_core(1);
    prof_guest_thread();
    if (rt_process_init(boot_modules, g_game, g_user, "C:\\Spider-Man2", g_args) != 0) {
        fprintf(stderr, "[boot] process init failed\n");
        goto done;
    }
    boot_config_apply(g_res_w, g_res_h);
    rt_module *exe = rt_module_main();
    if (!exe) {
        fprintf(stderr, "[boot] no webhead.exe among recompiled modules\n");
        goto done;
    }
    // как загрузчик Windows: сначала DllMain всех статически импортируемых модулей
    if (rt_module_attach(exe) != 0)
        goto done;
    fprintf(stderr, "[boot] static DLLs initialized\n");
    if (g_dlls_only) {
        rc = 0;
        goto done;
    }
    GuestThread *t = rt_thread_current();
    u32 r = rt_guest_call(&t->cpu, exe->desc->entry, 0, NULL);
    fprintf(stderr, "[boot] webhead.exe entry returned %u\n", r);
    rc = (int)r;
done:
    atomic_store(&g_exit_code, rc);
    atomic_store(&g_guest_done, 1);
    plat_quit();
    return NULL;
}

int main(int argc, char **argv)
{
#ifdef __SWITCH__
    socketInitializeDefault();
    int nxlink = nxlinkStdio();  // без nxlink — -1, вывод тогда — в файл (ниже)
#endif
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--game") && i + 1 < argc)
            g_game = argv[++i];
        else if (!strcmp(argv[i], "--user") && i + 1 < argc)
            g_user = argv[++i];
        else if (!strcmp(argv[i], "--dlls-only"))
            g_dlls_only = 1;
        else if (!strcmp(argv[i], "--headless"))
            g_headless = 1;
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc)
            g_deadline_ns = rt_ticks_ns() + (u64)(atof(argv[++i]) * 1e9);
        else if (!strcmp(argv[i], "--res") && i + 1 < argc) {
            if (sscanf(argv[++i], "%dx%d", &g_res_w, &g_res_h) != 2)
                g_res_w = g_res_h = 0;
        } else if (!strcmp(argv[i], "--env") && i + 1 < argc)
            putenv(argv[++i]);  // ИМЯ=ЗНАЧЕНИЕ: через nxlink переменные окружения не передать
        else if (!strcmp(argv[i], "--prof") && i + 1 < argc)
            g_prof_path = argv[++i];
        else if (!strcmp(argv[i], "--prof-after") && i + 1 < argc)
            g_prof_after = atof(argv[++i]);
        else if (!strcmp(argv[i], "--prof-for") && i + 1 < argc)
            g_prof_for = atof(argv[++i]);
        else if (!strcmp(argv[i], "--pgo-dump") && i + 1 < argc)
            g_pgo_dump = atof(argv[++i]);
        else if (!strcmp(argv[i], "--")) {
            for (i++; i < argc; i++) {
                strncat(g_args, g_args[0] ? " " : "", sizeof g_args - strlen(g_args) - 1);
                strncat(g_args, argv[i], sizeof g_args - strlen(g_args) - 1);
            }
        } else {
            fprintf(stderr, "usage: %s [--game DIR] [--user DIR] [--dlls-only] [--headless] [--seconds N] "
                            "[--res WxH|0] [--env NAME=VALUE] [--prof FILE [--prof-after S] [--prof-for S]] [--pgo-dump S] [-- game args]\n", argv[0]);
            return 2;
        }
    }
#ifdef __ANDROID__
    int nxlink = -1;  // вывод в logcat не идёт: в файл, как на Switch
#endif
#if defined(__SWITCH__) || defined(__ANDROID__)
    // запуск из hbmenu без nxlink: stdout/stderr — в <user>/snx.log (прошлый — snx.log.old), по строкам: после
    // зависания или падения в файле есть всё до последней строки
    if (nxlink < 0) {
        char path[1100], old[1100];
        snprintf(path, sizeof path, "%s/snx.log", g_user);
        snprintf(old, sizeof old, "%s.old", path);
        mkdir(g_user, 0755);  // первый запуск: каталога записи ещё нет
        remove(old);
        rename(path, old);
        if (freopen(path, "w", stderr)) {
            setvbuf(stderr, NULL, _IOLBF, 0);
            dup2(fileno(stderr), fileno(stdout));
        }
    }
#ifdef __SWITCH__
    if (!rt_platform_preflight())
        return 1;  // applet-режим: подсказка показана, SDL и потоков ещё нет — обычный возврат в hbloader
#endif
#endif
#ifdef SNX_NO_GAME
    // APK из CI без файлов игры (host/boot/CMakeLists.txt): код порта собран, модулей игры нет
    fprintf(stderr, "[boot] сборка без модулей игры (SNX_NO_GAME): нужен APK, собранный из своей копии игры\n");
    int ru = 0;  // язык телефона (лаунчер — так же, android/…/L.kt)
    SDL_Locale *loc = SDL_GetPreferredLocales();
    if (loc && loc[0].language)
        ru = !strcmp(loc[0].language, "ru") || !strcmp(loc[0].language, "be") || !strcmp(loc[0].language, "kk");
    SDL_free(loc);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "webheadNX",
                             ru ? "Этот APK собран без игры: в нём только лаунчер, управление и движок порта.\n\n"
                                  "Код игры переводится из DLL вашей копии Spider-Man 2 и входит в APK при сборке — "
                                  "соберите APK со ссылкой на свою игру (GitHub Actions → Run workflow)."
                                : "This APK was built without the game: it only has the launcher, the controls and "
                                  "the port engine.\n\nThe game code is translated from the DLLs of your copy of "
                                  "Spider-Man 2 at build time — build the APK with a link to your game "
                                  "(GitHub Actions → Run workflow).",
                             NULL);
    return 0;
#endif
    g_hle_trace = getenv("SNX_HLE_TRACE") != NULL;
    rt_check_abi = getenv("SNX_CHECK_ABI") != NULL;
    rt_trap_hook = on_trap;
    user32_modal_hook = on_modal;
    const char *eh_test_s = getenv("SNX_EH_TEST");
    if (eh_test_s)
        g_eh_test_ns = rt_ticks_ns() + (u64)(atof(eh_test_s) * 1e9);
    if (getenv("SNX_UO_DUMP"))
        g_uo_dump_ns = rt_ticks_ns() + (u64)(atof(getenv("SNX_UO_DUMP")) * 1e9);
    if (getenv("SNX_ERR_TEST"))
        g_err_test_ns = rt_ticks_ns() + (u64)(atof(getenv("SNX_ERR_TEST")) * 1e9);
    if (getenv("SNX_CFG_TEST")) {
        const char *n = strchr(getenv("SNX_CFG_TEST"), ',');
        g_cfg_test_ns = rt_ticks_ns() + (u64)(atof(getenv("SNX_CFG_TEST")) * 1e9);
        if (n && atoi(n + 1) > 0)
            g_cfg_test_n = atoi(n + 1);
        const char *gap = n ? strchr(n + 1, ',') : NULL;
        if (gap && atof(gap + 1) >= 0)
            g_cfg_test_gap_ns = (u64)(atof(gap + 1) * 1e6);
    }
    user32_pump_hook = pump_hook;
    rt_file_size_hook = hle_file_size_known;  // FileSize без открытия (runtime/overrides_launch.c)
    // гостевая рекурсия исполняется рекурсией хоста: главному потоку гостя — большой стек
    pthread_attr_t a;
    pthread_attr_init(&a);
#ifdef __SWITCH__
    pthread_attr_setstacksize(&a, RT_HOST_STACK_MAIN);
#else
    struct sigaction sa = {0};
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    g_stack = mmap(NULL, g_stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g_stack == MAP_FAILED)
        return 1;
    pthread_attr_setstack(&a, g_stack, g_stack_size);
    if (!g_prof_path)
        g_prof_path = getenv("SNX_PROF");
#endif
    prof_init(g_prof_path, g_prof_after, g_prof_for);
#ifdef SNX_PGO_GEN
    pthread_t pgo_th;
    fprintf(stderr, "[pgo] instrumented build: profile is written after %.0f s or on exit\n", g_pgo_dump);
    if (pthread_create(&pgo_th, NULL, pgo_timer, NULL) == 0)
        pthread_detach(pgo_th);
#endif
    plat_on_focus = on_focus;
    plat_on_resume = on_resume;
    plat_on_screen_mode = on_screen_mode;
    plat_on_exit_request = on_exit_request;
    if (!g_headless && plat_init() != 0) {
        fprintf(stderr, "[boot] no SDL video, running headless\n");
        g_headless = 1;
    }
#ifdef __SWITCH__
    atexit(exit_process);  // после SDL: обработчики atexit идут в обратном порядке, этот — первым
    // игра рисует в разрешении экрана (1280x720 в руках, 1920x1080 в доке), а не 800x600 с полосами
    if (g_res_w < 0 && !g_headless)
        plat_screen_size(&g_res_w, &g_res_h);
#elif defined(__ANDROID__)
    atexit(exit_process);  // после SDL: обработчики atexit идут в обратном порядке, этот — первым
    // по умолчанию — экран телефона с масштабом SNX_RENDER_SCALE (лаунчер: «Качество картинки»); кадр
    // растягивается на экран при показе. Чётные размеры, не меньше 640x360
    if (g_res_w < 0 && !g_headless) {
        plat_screen_size(&g_res_w, &g_res_h);
        const char *sc = getenv("SNX_RENDER_SCALE");
        float k = sc ? (float)atof(sc) : 1.0f;
        // SNX_ASPECT — предел пропорций кадра (лаунчер: «Формат кадра»). Без него кадр во весь экран, и на 20:9–21:9
        // 3D шире (Hor+: overrides_engine.c), но 2D-интерфейс игры растянут по ширине; с пределом кадр уже
        // экрана, по бокам чёрные поля (вписывание — d3d8_present, касания в меню с теми же полями пересчитывает
        // d3d8_window_to_target)
        float lim = boot_render_size(&g_res_w, &g_res_h, k, getenv("SNX_ASPECT"));
        k = k < 0.25f ? 0.25f : k > 1.0f ? 1.0f : k;
        fprintf(stderr, "[boot] render %dx%d (scale %.2f, aspect %.3f%s)\n", g_res_w, g_res_h, (double)k,
                (double)g_res_w / (double)g_res_h, lim > 0 ? ", limited" : "");
    }
#else
    atexit(boot_shutdown);
#endif
    if (g_res_w > 0)
        user32_set_screen(g_res_w, g_res_h);
    input_init(g_user);
    plat_on_event = input_event;
    plat_on_tick = input_tick;
    pthread_t th;
    if (pthread_create(&th, &a, boot, NULL) != 0)
        return 1;
    if (g_headless) {
        pthread_join(th, NULL);
        boot_shutdown();
    } else {
        plat_run();  // до выхода гостя или системного OnExitRequest
        // Дать потоку гостя короткое время завершиться после WM_CLOSE (если выход инициировал хост/HOME)
        for (int i = 0; i < 50 && !atomic_load(&g_guest_done); i++)
            usleep(10000);
        if (atomic_load(&g_guest_done)) {
            pthread_join(th, NULL);
            boot_shutdown();
#ifdef __ANDROID__
            // процесс :game завершаем целиком, без деструкторов SDL/EGL (см. exit_process): иначе он может остаться
            // жить, и следующий запуск из лаунчера попадёт в него
            exit_process();
#endif
            plat_shutdown();
        } else {
            // Гость ещё занят (например, загрузка карты при OnExitRequest): кольцо gl_cs не освобождаем на ходу,
            // чтобы не получить гонку с писателем, но закрываем звук и сбрасываем файлы на диск.
            atomic_store(&g_shutdown_done, 1);
            openal_shutdown();
            hle_file_flush_all();
            rt_platform_cpu_boost_reset();
            prof_dump();
#ifdef SNX_PGO_GEN
            pgo_dump();
#endif
            fflush(NULL);
#ifdef __SWITCH__
            svcExitProcess();
#else
            _exit(atomic_load(&g_exit_code));
#endif
        }
    }
#ifndef __SWITCH__
    stack_report();
#endif
    return atomic_load(&g_exit_code);
}
