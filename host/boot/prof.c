// Сэмплирующий профайлер snx_boot (см. prof.h).
#include "host/boot/prof.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "platform/platform.h"
#include "runtime/hle/gl_cs.h"
#include "runtime/hle/hle.h"
#include "runtime/process.h"
#include "runtime/rt.h"

#define PROF_DEPTH 24
#ifdef __SWITCH__
#define PROF_MAX 150000  // память на Switch занята сразу (≈29 МБ): 75 с при 1 кГц на два потока
#else
#define PROF_MAX 400000
#endif

static uintptr_t (*g_prof)[PROF_DEPTH + 1];
// отметки кадров (поток гостя, d3d8_present): строка «m <кадр>» перед выборкой g_mark_at — по ним
// tools/probe/hitchprof.py сводит выборки долгих кадров
#define MARK_MAX (1 << 17)
static int g_mark_frame[MARK_MAX], g_mark_at[MARK_MAX];
static volatile int g_mark_n;
static char *g_prof_kind;  // 'g' — поток гостя, 'r' — поток рендера
static volatile int g_prof_n;
static _Atomic int g_prof_done;
static const char *g_prof_path;
static u64 g_from_ns, g_until_ns;  // окно выборок; g_until_ns == 0 — до выхода

static void write_file(uintptr_t base, uintptr_t slide)
{
    FILE *f = fopen(g_prof_path, "w");
    if (!f) {
        fprintf(stderr, "[prof] cannot write %s\n", g_prof_path);
        return;
    }
    // На Switch стандартный буфер newlib (BUFSIZ = 1 КБ) порождает ~15 000 IPC fsFileWrite
    // на 15 МБ дамп; 256 КБ сокращает число записей до ~60 и убирает фризы SD
    char *vbuf = malloc(256u << 10);
    if (vbuf)
        setvbuf(f, vbuf, _IOFBF, 256u << 10);
    fprintf(f, "base %lx slide %lx\n", (unsigned long)base, (unsigned long)slide);
    int m = 0;
    for (int i = 0; i < g_prof_n; i++) {
        for (; m < g_mark_n && g_mark_at[m] <= i; m++)
            fprintf(f, "m %d\n", g_mark_frame[m]);
        fputc(g_prof_kind[i], f);
        for (int k = 0; k <= PROF_DEPTH && g_prof[i][k]; k++)
            fprintf(f, " %lx", (unsigned long)g_prof[i][k]);
        fputc('\n', f);
    }
    fclose(f);
    free(vbuf);
    fprintf(stderr, "[prof] %d samples -> %s\n", g_prof_n, g_prof_path);
}

// цепочка кадров arm64: [fp] — прошлый fp, [fp+8] — lr; o[0] — pc, o[1] — lr (лист без своего кадра)
static void walk(uintptr_t *o, uintptr_t pc, uintptr_t lr, uintptr_t fp, uintptr_t lo, uintptr_t hi)
{
    o[0] = pc;
    o[1] = lr;
    int k = 2;
    for (; k <= PROF_DEPTH && fp >= lo && fp + 16 <= hi && !(fp & 7); k++) {
        uintptr_t next = ((uintptr_t *)fp)[0], ret = ((uintptr_t *)fp)[1];
        o[k] = ret;
        if (next <= fp)
            break;
        fp = next;
    }
    for (; k <= PROF_DEPTH; k++)
        o[k] = 0;
}

static int alloc_buffers(void)
{
    g_prof = calloc(PROF_MAX, sizeof *g_prof);
    g_prof_kind = calloc(PROF_MAX, 1);
    if (!g_prof || !g_prof_kind) {
        fprintf(stderr, "[prof] no memory for samples\n");
        return -1;
    }
    return 0;
}

static void set_window(double after_s, double for_s)
{
    u64 now = rt_ticks_ns();
    g_from_ns = now + (u64)(after_s * 1e9);
    g_until_ns = for_s > 0 ? g_from_ns + (u64)(for_s * 1e9) : 0;
}

// ---------------------------------------------------------------- приостановка потоков
// Поток-сэмплер раз в 1 мс приостанавливает каждый зарегистрированный поток, читает pc/lr/fp и проходит цепочку
// кадров по его стеку. Время — по часам, с ожиданиями (видно, сколько поток гостя ждёт поток рендера и наоборот).
typedef struct {
    _Atomic uintptr_t h;  // Handle (Switch) / порт Mach (мак); 0 — поток ещё не зарегистрирован
    uintptr_t lo, hi;     // стек: границы для цепочки fp
    char kind;
} target_t;
static target_t g_targets[2] = {{.kind = 'g'}, {.kind = 'r'}};

#ifdef __SWITCH__
#include <switch.h>

extern void _start(void);  // начало образа NRO (адрес 0 в ELF)

static void self_info(uintptr_t *h, uintptr_t *lo, uintptr_t *hi)
{
    Thread *th = threadGetSelf();
    *lo = (uintptr_t)th->stack_mirror;
    *hi = *lo + th->stack_sz;
    *h = th->handle;
}

static int t_pause(uintptr_t h) { return (int)svcSetThreadActivity((Handle)h, ThreadActivity_Paused); }
static void t_resume(uintptr_t h) { svcSetThreadActivity((Handle)h, ThreadActivity_Runnable); }

static int t_regs(uintptr_t h, uintptr_t *pc, uintptr_t *lr, uintptr_t *fp)
{
    ThreadContext ctx;
    Result r = svcGetThreadContext3(&ctx, (Handle)h);
    if (R_SUCCEEDED(r)) {
        *pc = ctx.pc.x;
        *lr = ctx.lr;
        *fp = ctx.fp;
    }
    return (int)r;
}

static void sleep_1ms(void) { svcSleepThread(1000000); }

static void image_base(uintptr_t *base, uintptr_t *slide) { *base = *slide = (uintptr_t)&_start; }

static Thread g_sampler;
static void sampler(void *arg);

static int start_sampler(void)
{
    // ядро 0: цикл событий SDL и фоновые потоки гостя (гость — на 1, поток рендера — на 2)
    Result r = threadCreate(&g_sampler, sampler, NULL, NULL, 0x10000, 0x20, 0);
    if (R_SUCCEEDED(r))
        r = threadStart(&g_sampler);
    if (R_FAILED(r))
        fprintf(stderr, "[prof] sampler thread: %#x\n", r);
    return R_FAILED(r) ? -1 : 0;
}

#elif defined(__ANDROID__)
// Android: профайлер выборками не реализован (нужна остановка потока по ptrace или сигналу) — SNX_PROF игнорируется;
// для замеров — simpleperf из NDK
static void self_info(uintptr_t *h, uintptr_t *lo, uintptr_t *hi) { *h = *lo = *hi = 0; }
static int t_pause(uintptr_t h) { (void)h; return -1; }
static void t_resume(uintptr_t h) { (void)h; }
static int t_regs(uintptr_t h, uintptr_t *pc, uintptr_t *lr, uintptr_t *fp)
{
    (void)h;
    *pc = *lr = *fp = 0;
    return -1;
}
static void image_base(uintptr_t *base, uintptr_t *slide) { *base = *slide = 0; }
static void sleep_1ms(void) {}
static void sampler(void *arg);

static int start_sampler(void)
{
    (void)sampler;
    fprintf(stderr, "[prof] sampling profiler is not available on Android (use simpleperf)\n");
    return -1;
}

#else  // mac: thread_suspend / thread_get_state (выборки по SIGPROF липли к выходам из системных вызовов)
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <pthread.h>
#include <unistd.h>

static void self_info(uintptr_t *h, uintptr_t *lo, uintptr_t *hi)
{
    pthread_t t = pthread_self();
    *hi = (uintptr_t)pthread_get_stackaddr_np(t);
    *lo = *hi - pthread_get_stacksize_np(t);
    *h = pthread_mach_thread_np(t);
}

static int t_pause(uintptr_t h) { return thread_suspend((thread_act_t)h); }
static void t_resume(uintptr_t h) { thread_resume((thread_act_t)h); }

static int t_regs(uintptr_t h, uintptr_t *pc, uintptr_t *lr, uintptr_t *fp)
{
    arm_thread_state64_t st;
    mach_msg_type_number_t n = ARM_THREAD_STATE64_COUNT;
    kern_return_t k = thread_get_state((thread_act_t)h, ARM_THREAD_STATE64, (thread_state_t)&st, &n);
    if (k == KERN_SUCCESS) {
        *pc = (uintptr_t)arm_thread_state64_get_pc(st);
        *lr = (uintptr_t)arm_thread_state64_get_lr(st);
        *fp = (uintptr_t)arm_thread_state64_get_fp(st);
    }
    return k;
}

static void sleep_1ms(void) { usleep(1000); }

static void image_base(uintptr_t *base, uintptr_t *slide)
{
    *base = (uintptr_t)_dyld_get_image_header(0);
    *slide = (uintptr_t)_dyld_get_image_vmaddr_slide(0);
}

static void *sampler_main(void *arg);

static int start_sampler(void)
{
    pthread_t t;
    if (pthread_create(&t, NULL, sampler_main, NULL) != 0) {
        fprintf(stderr, "[prof] sampler thread failed\n");
        return -1;
    }
    pthread_detach(t);
    return 0;
}
#endif

static void register_self(target_t *t)
{
    if (!g_prof_path)
        return;
    uintptr_t h;
    self_info(&h, &t->lo, &t->hi);
    atomic_store(&t->h, h);
}

void prof_guest_thread(void) { register_self(&g_targets[0]); }

static void prof_frame(int frame)
{
    int n = g_mark_n;
    u64 now = rt_ticks_ns();
    if (n >= MARK_MAX || g_prof_done || now < g_from_ns || (g_until_ns && now >= g_until_ns))
        return;
    g_mark_frame[n] = frame;
    g_mark_at[n] = g_prof_n;
    g_mark_n = n + 1;
}
static void prof_render_thread(void) { register_self(&g_targets[1]); }

// 0 — выборка взята (или поток ещё не зарегистрирован), иначе ошибка
static int sample(target_t *t)
{
    uintptr_t h = atomic_load(&t->h), pc, lr, fp;
    if (!h || g_prof_n >= PROF_MAX)
        return 0;
    // пока поток стоит, здесь нельзя ничего, что берёт блокировки (malloc, stdio): он может их держать
    int r = t_pause(h);
    if (r) {
        fprintf(stderr, "[prof] cannot pause thread %c: %#x\n", t->kind, r);
        return r;
    }
    r = t_regs(h, &pc, &lr, &fp);
    if (!r) {
        int i = g_prof_n;
        walk(g_prof[i], pc, lr, fp, t->lo, t->hi);
        g_prof_kind[i] = t->kind;
        g_prof_n = i + 1;
    }
    t_resume(h);
    if (r)
        fprintf(stderr, "[prof] cannot read registers of thread %c: %#x\n", t->kind, r);
    return r;
}

static void sampler(void *arg)
{
    (void)arg;
    for (;;) {
        sleep_1ms();
        u64 now = rt_ticks_ns();
        if (now < g_from_ns)
            continue;
        if ((g_until_ns && now >= g_until_ns) || g_prof_n >= PROF_MAX || g_prof_done)
            break;
        if (sample(&g_targets[0]) || sample(&g_targets[1]))
            break;
    }
    // Перед сбросом профиля на диск понизить приоритет потока sampler (на Switch 0x20 -> 0x34),
    // чтобы запись 15 МБ не вытесняла игру/фоновые потоки на ядре 0 и не душила fs sysmodule
    rt_platform_thread_background();
    prof_dump();
}

#if !defined(__SWITCH__) && !defined(__ANDROID__)
static void *sampler_main(void *arg)
{
    sampler(arg);
    return NULL;
}
#endif

void prof_init(const char *path, double after_s, double for_s)
{
    if (!path || alloc_buffers() != 0)
        return;
    g_prof_path = path;
    set_window(after_s, for_s);
    cs_render_thread_hook = prof_render_thread;
    hle_frame_hook = prof_frame;
    if (start_sampler() != 0)
        g_prof_path = NULL;
}

void prof_dump(void)
{
    if (!g_prof_path || atomic_exchange(&g_prof_done, 1))
        return;
    uintptr_t base, slide;
    image_base(&base, &slide);
    write_file(base, slide);
}
