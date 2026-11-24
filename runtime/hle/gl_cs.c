// Поток рендера: очередь вызовов GL слоя D3D8 (см. gl_cs.h).
// Кольцо одного писателя (поток гостя) и одного читателя (поток рендера): записи по 16 байт, позиции растут
// монотонно; запись, не помещающаяся до конца кольца, начинается с нуля (перед ней — запись-пропуск).
#define GL_CS_IMPL
#include "runtime/hle/gl_cs.h"

#include <pthread.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/window.h"
#include "runtime/rt.h"

typedef struct cs_rec cs_rec;
typedef void (*cs_exec)(void *r);
struct cs_rec {
    cs_exec exec;  // NULL — пропуск до начала кольца
    uint32_t size; // байт записи вместе с заголовком, кратно 16
    uint32_t flags;  // 0 — передать r; 1 — передать r + 1 (компактная команда cs_cmd_push*)
};

#define RING_SIZE (64u << 20)
#define BIG_DATA (4u << 20)  // данные больше — отдельным блоком malloc (освобождает поток рендера)
#define MAX_LAG 1            // кадров в очереди сверх исполняемого
// спящий поток рендера будится, когда записано столько с прошлого раза, и в точках ожидания (cs_sync, кадр,
// конец кольца): пробуждение — системный вызов, по одному на команду оно съедало почти всё время гостя
#define WAKE_BYTES (32u << 10)
// голова очереди публикуется порциями: поток рендера читает её в цикле, и запись после каждой команды гоняла
// строку кеша между ядрами (cs_end ≈1,6 % потока гостя на Switch)
#define PUBLISH_BYTES 2048u
// хвост очереди обновляется пакетами записей (и сразу в конце пачки или при ожидании гостя), чтобы не
// гонять строку кеша g_tail между Core 2 и Core 1 на каждой из ~15 000 команд кадра
#define TAIL_BATCH 32u

static uint8_t *g_ring;
// Разнесение горячих атомарных счётчиков по 64-байтным строкам кеша (убирает false sharing Core 1 <-> Core 2)
alignas(64) static _Atomic uint64_t g_wr;  // позиция писателя (пишет поток гостя; читается cs_stop при выходе)
static uint64_t g_woken;                   // g_wr при последнем пробуждении потока рендера (только поток гостя)
static uint64_t g_published;               // g_wr при последней публикации g_head (только поток гостя)
static uint64_t g_swap_end[4], g_frames;   // конец записи показа кадра (по номеру кадра)
alignas(64) static _Atomic uint64_t g_head;  // опубликовано писателем (Core 1 -> Core 2)
alignas(64) static _Atomic uint64_t g_tail;  // исполнено читателем (Core 2 -> Core 1)
alignas(64) static _Atomic uint64_t g_wait_target = UINT64_MAX;  // поток гостя ждёт хвост >= target
alignas(64) static _Atomic int g_render_sleeping;                // поток рендера спит на g_cv_work
static _Atomic int g_on;
static _Atomic int g_stop;                 // запрос завершения потока рендера после дренажа очереди
static _Atomic int g_exited;               // поток рендера завершил цикл
static _Thread_local int t_render;
static pthread_t g_thread;

static pthread_mutex_t g_life_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv_work = PTHREAD_COND_INITIALIZER, g_cv_progress = PTHREAD_COND_INITIALIZER;

// статистика для строки fps (cs_stats_take): g_st — пишет поток гостя, busy/swap — поток рендера
static cs_stats g_st;
static u64 g_wait_total_ns;  // то же ожидание с начала работы (строка [hitch], d3d8_present)
static _Atomic uint64_t g_busy_ns, g_swap_ns;
void (*cs_render_thread_hook)(void);

static void pool_reset(void);

int cs_enabled(void) { return atomic_load_explicit(&g_on, memory_order_acquire); }
int cs_on_render_thread(void) { return t_render; }

static inline int direct(void) { return !atomic_load_explicit(&g_on, memory_order_acquire) || t_render; }

static inline void cpu_relax(void)
{
#if defined(__aarch64__)
    __asm__ volatile("yield");
#endif
}

// ---------------------------------------------------------------- ожидание
static void publish(void)
{
    uint64_t wr = atomic_load_explicit(&g_wr, memory_order_relaxed);
    g_published = wr;
    atomic_store(&g_head, wr);
}

static void wake_render(void)
{
    publish();
    g_woken = atomic_load_explicit(&g_wr, memory_order_relaxed);
    if (atomic_load(&g_render_sleeping)) {
        pthread_mutex_lock(&g_mx);
        pthread_cond_signal(&g_cv_work);
        pthread_mutex_unlock(&g_mx);
    }
}

static void wake_guest(uint64_t tail)
{
    uint64_t wt = atomic_load(&g_wait_target);
    if (wt != UINT64_MAX && tail >= wt) {
        pthread_mutex_lock(&g_mx);
        pthread_cond_broadcast(&g_cv_progress);
        pthread_mutex_unlock(&g_mx);
    }
}

// поток гостя: ждать, пока поток рендера не исполнит всё до позиции target (будят, только когда дошёл)
static void wait_tail(uint64_t target)
{
    if (atomic_load(&g_tail) >= target || atomic_load(&g_exited))
        return;
    wake_render();
    u64 t0 = rt_ticks_ns();
    for (int i = 0; i < 4000; i++) {
        if (atomic_load(&g_tail) >= target || atomic_load(&g_exited)) {
            g_st.wait_ns += rt_ticks_ns() - t0;
            g_wait_total_ns += rt_ticks_ns() - t0;
            return;
        }
        cpu_relax();
    }
    pthread_mutex_lock(&g_mx);
    atomic_store(&g_wait_target, target);
    while (atomic_load(&g_tail) < target && !atomic_load(&g_exited))
        pthread_cond_wait(&g_cv_progress, &g_mx);
    atomic_store(&g_wait_target, UINT64_MAX);
    pthread_mutex_unlock(&g_mx);
    g_st.wait_ns += rt_ticks_ns() - t0;
    g_wait_total_ns += rt_ticks_ns() - t0;
}

unsigned long long cs_wait_total_ns(void) { return g_wait_total_ns; }

// ---------------------------------------------------------------- запись
static void *cs_begin(size_t size, cs_exec exec)
{
    size = (size + 15) & ~(size_t)15;
    uint64_t pos = atomic_load_explicit(&g_wr, memory_order_relaxed), off = pos % RING_SIZE;
    if (off + size > RING_SIZE) {
        wait_tail(pos + (RING_SIZE - off) + size - RING_SIZE);
        cs_rec *skip = (cs_rec *)(g_ring + off);
        skip->exec = NULL;
        skip->size = (uint32_t)(RING_SIZE - off);
        pos += RING_SIZE - off;
        off = 0;
    } else if (pos + size > RING_SIZE)
        wait_tail(pos + size - RING_SIZE);
    cs_rec *r = (cs_rec *)(g_ring + off);
    r->exec = exec;
    r->size = (uint32_t)size;
    r->flags = 0;
    atomic_store_explicit(&g_wr, pos + size, memory_order_relaxed);
    g_st.recs++;
    g_st.bytes += size;
    return r;
}

static void cs_end(void)
{
    uint64_t wr = atomic_load_explicit(&g_wr, memory_order_relaxed);
    if (wr - g_published < PUBLISH_BYTES)
        return;
    if (wr - g_woken >= WAKE_BYTES)
        wake_render();
    else
        publish();
}

// запись с копией n байт данных: сразу за заголовком head (или в куче, если велики); *heap освобождает исполнитель
static void *cs_begin_data(size_t head, const void *src, size_t n, cs_exec exec, const void **dst, void **heap)
{
    head = (head + 15) & ~(size_t)15;
    if (!src || n > BIG_DATA) {
        void *r = cs_begin(head, exec);
        *heap = src ? malloc(n) : NULL;
        if (*heap)
            memcpy(*heap, src, n);
        *dst = *heap;
        return r;
    }
    uint8_t *r = cs_begin(head + n, exec);
    memcpy(r + head, src, n);
    *dst = r + head;
    *heap = NULL;
    return r;
}

// ---------------------------------------------------------------- поток рендера
static int g_core;

static void *render_main(void *arg)
{
    (void)arg;
    t_render = 1;
    rt_platform_thread_core(g_core);
    if (cs_render_thread_hook)
        cs_render_thread_hook();
    uint64_t tail = 0;
    for (;;) {
        uint64_t head = atomic_load(&g_head);
        if (head == tail) {
            for (int i = 0; i < 2000 && head == tail && !atomic_load(&g_stop); i++) {
                cpu_relax();
                head = atomic_load(&g_head);
            }
            head = atomic_load(&g_head);
            if (head == tail) {
                pthread_mutex_lock(&g_mx);
                atomic_store(&g_render_sleeping, 1);
                while ((head = atomic_load(&g_head)) == tail && !atomic_load(&g_stop))
                    pthread_cond_wait(&g_cv_work, &g_mx);
                atomic_store(&g_render_sleeping, 0);
                pthread_mutex_unlock(&g_mx);
                // Перечитать g_head после наблюдения g_stop == 1: cs_stop сначала публикует g_head, затем g_stop.
                head = atomic_load(&g_head);
                if (head == tail && atomic_load(&g_stop))
                    break;
            }
        }
        u64 t0 = rt_ticks_ns();
        uint32_t since_tail = 0;
        while (tail != head) {
            cs_rec *r = (cs_rec *)(g_ring + tail % RING_SIZE);
            uint32_t size = r->size;
            if (r->exec)
                r->exec(r->flags ? (void *)(r + 1) : (void *)r);
            tail += size;
            since_tail++;
            if (since_tail >= TAIL_BATCH || tail == head ||
                atomic_load_explicit(&g_wait_target, memory_order_relaxed) != UINT64_MAX) {
                atomic_store(&g_tail, tail);  // seq_cst: пара к atomic_store(&g_wait_target) в wait_tail
                since_tail = 0;
                wake_guest(tail);
            }
        }
        atomic_fetch_add(&g_busy_ns, rt_ticks_ns() - t0);
    }
    plat_gl_unbind();
    pthread_mutex_lock(&g_mx);
    atomic_store(&g_exited, 1);
    pthread_cond_broadcast(&g_cv_progress);
    pthread_mutex_unlock(&g_mx);
    return NULL;
}

int cs_start(int core)
{
    pthread_mutex_lock(&g_life_lock);
    if (atomic_load_explicit(&g_on, memory_order_relaxed)) {
        pthread_mutex_unlock(&g_life_lock);
        return 0;
    }
    g_ring = malloc(RING_SIZE);
    if (!g_ring) {
        pthread_mutex_unlock(&g_life_lock);
        return -1;
    }
    atomic_store(&g_head, 0);
    atomic_store(&g_tail, 0);
    atomic_store(&g_wr, 0);
    atomic_store(&g_stop, 0);
    atomic_store(&g_exited, 0);
    atomic_store(&g_render_sleeping, 0);
    atomic_store(&g_wait_target, UINT64_MAX);
    g_woken = g_published = g_frames = 0;
    memset(g_swap_end, 0, sizeof g_swap_end);
    pool_reset();
    g_core = core;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 1u << 20);
    int rc = pthread_create(&g_thread, &a, render_main, NULL);
    pthread_attr_destroy(&a);
    if (rc != 0) {
        free(g_ring);
        g_ring = NULL;
        pthread_mutex_unlock(&g_life_lock);
        return -1;
    }
    atomic_store_explicit(&g_on, 1, memory_order_release);
    pthread_mutex_unlock(&g_life_lock);
    fprintf(stderr, "[cs] render thread on core %d\n", core);
    return 0;
}

void cs_stop(void)
{
    if (t_render)
        return;  // сам поток рендера не может ждать себя
    pthread_mutex_lock(&g_life_lock);
    if (!atomic_load_explicit(&g_on, memory_order_relaxed)) {
        pthread_mutex_unlock(&g_life_lock);
        return;
    }
    // Допубликовать оставшиеся записи писателя, разбудить поток рендера и дождаться полного дренажа
    uint64_t wr = atomic_load(&g_wr);
    if (atomic_load(&g_head) < wr)
        atomic_store(&g_head, wr);
    pthread_mutex_lock(&g_mx);
    atomic_store(&g_stop, 1);
    pthread_cond_broadcast(&g_cv_work);
    pthread_mutex_unlock(&g_mx);
    pthread_join(g_thread, NULL);
    atomic_store_explicit(&g_on, 0, memory_order_release);
    free(g_ring);
    g_ring = NULL;
    pool_reset();
    pthread_mutex_unlock(&g_life_lock);
}

// ---------------------------------------------------------------- замыкания
typedef struct {
    cs_rec h;
    void (*fn)(void *);
    void *arg;
} r_sync;

static void x_sync(void *r)
{
    r_sync *q = (r_sync *)r;
    q->fn(q->arg);
}

void cs_sync(void (*fn)(void *), void *arg)
{
    if (direct()) {
        fn(arg);
        return;
    }
    g_st.syncs++;
    r_sync *q = cs_begin(sizeof *q, x_sync);
    q->fn = fn;
    q->arg = arg;
    cs_end();
    wait_tail(atomic_load_explicit(&g_wr, memory_order_relaxed));  // хвост сдвигается после исполнения записи
}

typedef struct {
    cs_rec h;
    void (*fn)(void *);
    void *arg, *heap;
} r_async;

static void x_async(void *r)
{
    r_async *q = (r_async *)r;
    q->fn(q->arg);
    free(q->heap);
}

void cs_async(void (*fn)(void *), const void *arg, size_t size)
{
    if (direct()) {
        fn((void *)arg);
        return;
    }
    const void *dst;
    void *heap;
    r_async *q = cs_begin_data(sizeof *q, arg, size, x_async, &dst, &heap);
    q->fn = fn;
    q->arg = (void *)dst;
    q->heap = heap;
    cs_end();
}

void cs_cmd_push(cs_cmd_fn fn, const void *payload, size_t payload_size)
{
    if (direct()) {
        fn((void *)payload);
        return;
    }
    cs_rec *r = cs_begin(sizeof(cs_rec) + payload_size, fn);
    r->flags = 1;
    if (payload_size)
        memcpy(r + 1, payload, payload_size);
    cs_end();
}

void cs_cmd_push_var(cs_cmd_fn fn, const void *hdr, size_t hdr_size, const void *data, size_t data_size,
                     size_t ptr_field_off, size_t heap_field_off)
{
    if (direct()) {
        fn((void *)hdr);
        return;
    }
    size_t head = (sizeof(cs_rec) + hdr_size + 15) & ~(size_t)15;
    const void *dst = NULL;
    void *heap = NULL;
    cs_rec *r;
    if (!data || !data_size) {
        r = cs_begin(head, fn);
    } else if (data_size > BIG_DATA) {
        r = cs_begin(head, fn);
        heap = malloc(data_size);
        if (heap)
            memcpy(heap, data, data_size);
        dst = heap;
    } else {
        r = cs_begin(head + data_size, fn);
        memcpy((uint8_t *)r + head, data, data_size);
        dst = (uint8_t *)r + head;
    }
    r->flags = 1;
    uint8_t *pay = (uint8_t *)(r + 1);
    memcpy(pay, hdr, hdr_size);
    memcpy(pay + ptr_field_off, &dst, sizeof dst);
    memcpy(pay + heap_field_off, &heap, sizeof heap);
    cs_end();
}

uint64_t cs_wr_pos(void) { return atomic_load_explicit(&g_wr, memory_order_relaxed); }
uint64_t cs_tail_pos(void) { return atomic_load_explicit(&g_tail, memory_order_acquire); }

static void noop(void *arg) { (void)arg; }
void cs_finish(void) { cs_sync(noop, NULL); }

void cs_stats_take(cs_stats *out)
{
    *out = g_st;
    out->busy_ns = atomic_exchange(&g_busy_ns, 0);
    out->swap_ns = atomic_exchange(&g_swap_ns, 0);
    memset(&g_st, 0, sizeof g_st);
}

static void x_swap(void *r)
{
    (void)r;
    u64 t0 = rt_ticks_ns();
    plat_swap();
    atomic_fetch_add(&g_swap_ns, rt_ticks_ns() - t0);
}

void cs_swap(void)
{
    if (direct()) {
        plat_swap();
        return;
    }
    cs_begin(sizeof(cs_rec), x_swap);
    cs_end();
    wake_render();
    // кадр k записан; пока поток рендера исполняет его, гость пишет k+1, но не дальше (MAX_LAG = 1)
    g_swap_end[g_frames % 4] = atomic_load_explicit(&g_wr, memory_order_relaxed);
    if (g_frames >= MAX_LAG)
        wait_tail(g_swap_end[(g_frames - MAX_LAG) % 4]);
    g_frames++;
}

// ---------------------------------------------------------------- обёртки без результата
#define CS_ASYNC(n, name, ...) CS_ASYNC_##n(name, __VA_ARGS__)
#define CS_REC(name, ...) typedef struct { cs_rec h; __VA_ARGS__ } r_##name;
#define CS_ASYNC_1(name, T1)                                                                                \
    CS_REC(name, T1 a1;)                                                                                    \
    static void x_##name(void *r) { r_##name *q = (r_##name *)r; gl##name(q->a1); }                         \
    void cs_gl##name(T1 a1)                                                                                 \
    {                                                                                                       \
        if (direct()) { gl##name(a1); return; }                                                             \
        r_##name *q = cs_begin(sizeof *q, x_##name);                                                        \
        q->a1 = a1;                                                                                         \
        cs_end();                                                                                           \
    }
#define CS_ASYNC_2(name, T1, T2)                                                                            \
    CS_REC(name, T1 a1; T2 a2;)                                                                             \
    static void x_##name(void *r) { r_##name *q = (r_##name *)r; gl##name(q->a1, q->a2); }                  \
    void cs_gl##name(T1 a1, T2 a2)                                                                          \
    {                                                                                                       \
        if (direct()) { gl##name(a1, a2); return; }                                                         \
        r_##name *q = cs_begin(sizeof *q, x_##name);                                                        \
        q->a1 = a1; q->a2 = a2;                                                                             \
        cs_end();                                                                                           \
    }
#define CS_ASYNC_3(name, T1, T2, T3)                                                                        \
    CS_REC(name, T1 a1; T2 a2; T3 a3;)                                                                      \
    static void x_##name(void *r) { r_##name *q = (r_##name *)r; gl##name(q->a1, q->a2, q->a3); }           \
    void cs_gl##name(T1 a1, T2 a2, T3 a3)                                                                   \
    {                                                                                                       \
        if (direct()) { gl##name(a1, a2, a3); return; }                                                     \
        r_##name *q = cs_begin(sizeof *q, x_##name);                                                        \
        q->a1 = a1; q->a2 = a2; q->a3 = a3;                                                                 \
        cs_end();                                                                                           \
    }
#define CS_ASYNC_4(name, T1, T2, T3, T4)                                                                    \
    CS_REC(name, T1 a1; T2 a2; T3 a3; T4 a4;)                                                               \
    static void x_##name(void *r) { r_##name *q = (r_##name *)r; gl##name(q->a1, q->a2, q->a3, q->a4); }    \
    void cs_gl##name(T1 a1, T2 a2, T3 a3, T4 a4)                                                            \
    {                                                                                                       \
        if (direct()) { gl##name(a1, a2, a3, a4); return; }                                                 \
        r_##name *q = cs_begin(sizeof *q, x_##name);                                                        \
        q->a1 = a1; q->a2 = a2; q->a3 = a3; q->a4 = a4;                                                     \
        cs_end();                                                                                           \
    }
#define CS_ASYNC_5(name, T1, T2, T3, T4, T5)                                                                \
    CS_REC(name, T1 a1; T2 a2; T3 a3; T4 a4; T5 a5;)                                                        \
    static void x_##name(void *r)                                                                           \
    {                                                                                                       \
        r_##name *q = (r_##name *)r;                                                                        \
        gl##name(q->a1, q->a2, q->a3, q->a4, q->a5);                                                        \
    }                                                                                                       \
    void cs_gl##name(T1 a1, T2 a2, T3 a3, T4 a4, T5 a5)                                                     \
    {                                                                                                       \
        if (direct()) { gl##name(a1, a2, a3, a4, a5); return; }                                             \
        r_##name *q = cs_begin(sizeof *q, x_##name);                                                        \
        q->a1 = a1; q->a2 = a2; q->a3 = a3; q->a4 = a4; q->a5 = a5;                                         \
        cs_end();                                                                                           \
    }
#define CS_ASYNC_6(name, T1, T2, T3, T4, T5, T6)                                                            \
    CS_REC(name, T1 a1; T2 a2; T3 a3; T4 a4; T5 a5; T6 a6;)                                                 \
    static void x_##name(void *r)                                                                           \
    {                                                                                                       \
        r_##name *q = (r_##name *)r;                                                                        \
        gl##name(q->a1, q->a2, q->a3, q->a4, q->a5, q->a6);                                                 \
    }                                                                                                       \
    void cs_gl##name(T1 a1, T2 a2, T3 a3, T4 a4, T5 a5, T6 a6)                                              \
    {                                                                                                       \
        if (direct()) { gl##name(a1, a2, a3, a4, a5, a6); return; }                                         \
        r_##name *q = cs_begin(sizeof *q, x_##name);                                                        \
        q->a1 = a1; q->a2 = a2; q->a3 = a3; q->a4 = a4; q->a5 = a5; q->a6 = a6;                             \
        cs_end();                                                                                           \
    }
#define CS_ASYNC_10(name, T1, T2, T3, T4, T5, T6, T7, T8, T9, T10)                                          \
    CS_REC(name, T1 a1; T2 a2; T3 a3; T4 a4; T5 a5; T6 a6; T7 a7; T8 a8; T9 a9; T10 a10;)                   \
    static void x_##name(void *r)                                                                           \
    {                                                                                                       \
        r_##name *q = (r_##name *)r;                                                                        \
        gl##name(q->a1, q->a2, q->a3, q->a4, q->a5, q->a6, q->a7, q->a8, q->a9, q->a10);                    \
    }                                                                                                       \
    void cs_gl##name(T1 a1, T2 a2, T3 a3, T4 a4, T5 a5, T6 a6, T7 a7, T8 a8, T9 a9, T10 a10)                \
    {                                                                                                       \
        if (direct()) { gl##name(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10); return; }                        \
        r_##name *q = cs_begin(sizeof *q, x_##name);                                                        \
        q->a1 = a1; q->a2 = a2; q->a3 = a3; q->a4 = a4; q->a5 = a5;                                         \
        q->a6 = a6; q->a7 = a7; q->a8 = a8; q->a9 = a9; q->a10 = a10;                                       \
        cs_end();                                                                                           \
    }
CS_GL_ASYNC(CS_ASYNC)

// ---------------------------------------------------------------- обёртки с ожиданием
// аргументы — в структуре на стеке потока гостя (он ждёт), исполнитель — в потоке рендера
#define CS_RET(n, ret, name, ...) CS_RET_##n(ret, name, __VA_ARGS__)
#define CS_RET_0(ret, name, unused)                                                                         \
    typedef struct { ret r; } s_##name;                                                                     \
    static void y_##name(void *p) { ((s_##name *)p)->r = gl##name(); }                                      \
    ret cs_gl##name(void)                                                                                   \
    {                                                                                                       \
        if (direct()) return gl##name();                                                                    \
        s_##name q;                                                                                         \
        cs_sync(y_##name, &q);                                                                              \
        return q.r;                                                                                         \
    }
#define CS_RET_1(ret, name, T1)                                                                             \
    typedef struct { ret r; T1 a1; } s_##name;                                                              \
    static void y_##name(void *p) { s_##name *q = p; q->r = gl##name(q->a1); }                              \
    ret cs_gl##name(T1 a1)                                                                                  \
    {                                                                                                       \
        if (direct()) return gl##name(a1);                                                                  \
        s_##name q = {.a1 = a1};                                                                            \
        cs_sync(y_##name, &q);                                                                              \
        return q.r;                                                                                         \
    }
#define CS_RET_2(ret, name, T1, T2)                                                                         \
    typedef struct { ret r; T1 a1; T2 a2; } s_##name;                                                       \
    static void y_##name(void *p) { s_##name *q = p; q->r = gl##name(q->a1, q->a2); }                       \
    ret cs_gl##name(T1 a1, T2 a2)                                                                           \
    {                                                                                                       \
        if (direct()) return gl##name(a1, a2);                                                              \
        s_##name q = {.a1 = a1, .a2 = a2};                                                                  \
        cs_sync(y_##name, &q);                                                                              \
        return q.r;                                                                                         \
    }
#define CS_RET_4(ret, name, T1, T2, T3, T4)                                                                 \
    typedef struct { ret r; T1 a1; T2 a2; T3 a3; T4 a4; } s_##name;                                         \
    static void y_##name(void *p) { s_##name *q = p; q->r = gl##name(q->a1, q->a2, q->a3, q->a4); }         \
    ret cs_gl##name(T1 a1, T2 a2, T3 a3, T4 a4)                                                             \
    {                                                                                                       \
        if (direct()) return gl##name(a1, a2, a3, a4);                                                      \
        s_##name q = {.a1 = a1, .a2 = a2, .a3 = a3, .a4 = a4};                                              \
        cs_sync(y_##name, &q);                                                                              \
        return q.r;                                                                                         \
    }
CS_GL_SYNC_RET(CS_RET)

#define CS_VOID(n, name, ...) CS_VOID_##n(name, __VA_ARGS__)
#define CS_VOID_2(name, T1, T2)                                                                             \
    typedef struct { T1 a1; T2 a2; } s_##name;                                                              \
    static void y_##name(void *p) { s_##name *q = p; gl##name(q->a1, q->a2); }                              \
    void cs_gl##name(T1 a1, T2 a2)                                                                          \
    {                                                                                                       \
        s_##name q = {a1, a2};                                                                              \
        cs_sync(y_##name, &q);                                                                              \
    }
#define CS_VOID_3(name, T1, T2, T3)                                                                         \
    typedef struct { T1 a1; T2 a2; T3 a3; } s_##name;                                                       \
    static void y_##name(void *p) { s_##name *q = p; gl##name(q->a1, q->a2, q->a3); }                       \
    void cs_gl##name(T1 a1, T2 a2, T3 a3)                                                                   \
    {                                                                                                       \
        s_##name q = {a1, a2, a3};                                                                          \
        cs_sync(y_##name, &q);                                                                              \
    }
#define CS_VOID_4(name, T1, T2, T3, T4)                                                                     \
    typedef struct { T1 a1; T2 a2; T3 a3; T4 a4; } s_##name;                                                \
    static void y_##name(void *p) { s_##name *q = p; gl##name(q->a1, q->a2, q->a3, q->a4); }                \
    void cs_gl##name(T1 a1, T2 a2, T3 a3, T4 a4)                                                            \
    {                                                                                                       \
        s_##name q = {a1, a2, a3, a4};                                                                      \
        cs_sync(y_##name, &q);                                                                              \
    }
#define CS_VOID_5(name, T1, T2, T3, T4, T5)                                                                 \
    typedef struct { T1 a1; T2 a2; T3 a3; T4 a4; T5 a5; } s_##name;                                         \
    static void y_##name(void *p) { s_##name *q = p; gl##name(q->a1, q->a2, q->a3, q->a4, q->a5); }         \
    void cs_gl##name(T1 a1, T2 a2, T3 a3, T4 a4, T5 a5)                                                     \
    {                                                                                                       \
        s_##name q = {a1, a2, a3, a4, a5};                                                                  \
        cs_sync(y_##name, &q);                                                                              \
    }
#define CS_VOID_7(name, T1, T2, T3, T4, T5, T6, T7)                                                         \
    typedef struct { T1 a1; T2 a2; T3 a3; T4 a4; T5 a5; T6 a6; T7 a7; } s_##name;                           \
    static void y_##name(void *p)                                                                           \
    {                                                                                                       \
        s_##name *q = p;                                                                                    \
        gl##name(q->a1, q->a2, q->a3, q->a4, q->a5, q->a6, q->a7);                                          \
    }                                                                                                       \
    void cs_gl##name(T1 a1, T2 a2, T3 a3, T4 a4, T5 a5, T6 a6, T7 a7)                                       \
    {                                                                                                       \
        s_##name q = {a1, a2, a3, a4, a5, a6, a7};                                                          \
        cs_sync(y_##name, &q);                                                                              \
    }
CS_GL_SYNC_VOID(CS_VOID)

// ---------------------------------------------------------------- данные по указателю
typedef struct {
    cs_rec h;
    GLint loc;
    GLsizei count;
    GLboolean transpose;
    const GLfloat *v;
    void *heap;
} r_uniformv;

static void x_uniform4fv(void *r)
{
    r_uniformv *q = (r_uniformv *)r;
    glUniform4fv(q->loc, q->count, q->v);
    free(q->heap);
}

static void x_uniform_matrix4fv(void *r)
{
    r_uniformv *q = (r_uniformv *)r;
    glUniformMatrix4fv(q->loc, q->count, q->transpose, q->v);
    free(q->heap);
}

static void put_uniformv(GLint loc, GLsizei count, GLboolean tr, const GLfloat *v, size_t floats, cs_exec x)
{
    const void *dst;
    void *heap;
    r_uniformv *q = cs_begin_data(sizeof *q, v, floats * sizeof(GLfloat), x, &dst, &heap);
    q->loc = loc;
    q->count = count;
    q->transpose = tr;
    q->v = dst;
    q->heap = heap;
    cs_end();
}

void cs_glUniform4fv(GLint loc, GLsizei count, const GLfloat *v)
{
    if (direct())
        glUniform4fv(loc, count, v);
    else
        put_uniformv(loc, count, GL_FALSE, v, 4u * (size_t)count, x_uniform4fv);
}

void cs_glUniformMatrix4fv(GLint loc, GLsizei count, GLboolean transpose, const GLfloat *v)
{
    if (direct())
        glUniformMatrix4fv(loc, count, transpose, v);
    else
        put_uniformv(loc, count, transpose, v, 16u * (size_t)count, x_uniform_matrix4fv);
}

typedef struct {
    cs_rec h;
    GLuint s;
    GLenum pname;
    GLfloat v[4];
} r_samplerfv;

static void x_samplerfv(void *r)
{
    r_samplerfv *q = (r_samplerfv *)r;
    glSamplerParameterfv(q->s, q->pname, q->v);
}

void cs_glSamplerParameterfv(GLuint s, GLenum pname, const GLfloat *v)
{
    if (direct()) {
        glSamplerParameterfv(s, pname, v);
        return;
    }
    r_samplerfv *q = cs_begin(sizeof *q, x_samplerfv);
    q->s = s;
    q->pname = pname;
    memcpy(q->v, v, sizeof q->v);
    cs_end();
}

typedef struct {
    cs_rec h;
    GLenum target, usage;
    GLintptr off;
    GLsizeiptr size;
    const void *data;
    void *heap;
} r_buffer;

static void x_buffer_data(void *r)
{
    r_buffer *q = (r_buffer *)r;
    glBufferData(q->target, q->size, q->data, q->usage);
    free(q->heap);
}

static void x_buffer_subdata(void *r)
{
    r_buffer *q = (r_buffer *)r;
    glBufferSubData(q->target, q->off, q->size, q->data);
    free(q->heap);
}

static void put_buffer(GLenum target, GLintptr off, GLsizeiptr size, const void *data, GLenum usage, cs_exec x)
{
    const void *dst;
    void *heap;
    r_buffer *q = cs_begin_data(sizeof *q, data, (size_t)size, x, &dst, &heap);
    q->target = target;
    q->usage = usage;
    q->off = off;
    q->size = size;
    q->data = dst;
    q->heap = heap;
    cs_end();
}

static void write_unsync(GLenum target, GLintptr off, GLsizeiptr size, const void *data)
{
    void *p = glMapBufferRange(target, off, size,
                               GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT | GL_MAP_INVALIDATE_RANGE_BIT);
    if (p) {
        memcpy(p, data, (size_t)size);
        glUnmapBuffer(target);
    } else
        glBufferSubData(target, off, size, data);
}

static void x_buffer_unsync(void *r)
{
    r_buffer *q = (r_buffer *)r;
    write_unsync(q->target, q->off, q->size, q->data);
    free(q->heap);
}

void cs_BufferWriteUnsync(GLenum target, GLintptr off, GLsizeiptr size, const void *data)
{
    if (direct())
        write_unsync(target, off, size, data);
    else
        put_buffer(target, off, size, data, 0, x_buffer_unsync);
}

void cs_glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage)
{
    if (direct())
        glBufferData(target, size, data, usage);
    else
        put_buffer(target, 0, size, data, usage, x_buffer_data);
}

void cs_glBufferSubData(GLenum target, GLintptr off, GLsizeiptr size, const void *data)
{
    if (direct())
        glBufferSubData(target, off, size, data);
    else
        put_buffer(target, off, size, data, 0, x_buffer_subdata);
}

typedef struct {
    cs_rec h;
    GLenum target, format, type;
    GLint level, internal;
    GLsizei width, height, size;
    const void *data;
    void *heap;
} r_teximage;

static void x_teximage(void *r)
{
    r_teximage *q = (r_teximage *)r;
    glTexImage2D(q->target, q->level, q->internal, q->width, q->height, 0, q->format, q->type, q->data);
    free(q->heap);
}

static void x_compressed_teximage(void *r)
{
    r_teximage *q = (r_teximage *)r;
    glCompressedTexImage2D(q->target, q->level, (GLenum)q->internal, q->width, q->height, 0, q->size, q->data);
    free(q->heap);
}

static void put_teximage(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h, GLenum format,
                         GLenum type, const void *data, size_t size, cs_exec x)
{
    const void *dst;
    void *heap;
    r_teximage *q = cs_begin_data(sizeof *q, data, data ? size : 0, x, &dst, &heap);
    q->target = target;
    q->level = level;
    q->internal = internal;
    q->width = w;
    q->height = h;
    q->format = format;
    q->type = type;
    q->size = (GLsizei)size;
    q->data = dst;
    q->heap = heap;
    cs_end();
}

void cs_TexImage2D(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h, GLenum format, GLenum type,
                   const void *data, size_t size)
{
    if (direct())
        glTexImage2D(target, level, internal, w, h, 0, format, type, data);
    else
        put_teximage(target, level, internal, w, h, format, type, data, size, x_teximage);
}

typedef struct {
    cs_rec h;
    GLenum target, format, type;
    GLint level, x, y;
    GLsizei width, height;
    const void *data;
    void *heap;
} r_texsub;

static void x_texsub(void *r)
{
    r_texsub *q = (r_texsub *)r;
    glTexSubImage2D(q->target, q->level, q->x, q->y, q->width, q->height, q->format, q->type, q->data);
    free(q->heap);
}

void cs_TexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format,
                      GLenum type, const void *data, size_t row, size_t pitch)
{
    if (direct()) {
        glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)(pitch * (size_t)w / row));
        glTexSubImage2D(target, level, x, y, w, h, format, type, data);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        return;
    }
    size_t n = row * (size_t)h, head = (sizeof(r_texsub) + 15) & ~(size_t)15;
    void *heap = n > BIG_DATA ? malloc(n) : NULL;
    r_texsub *q = cs_begin(heap ? head : head + n, x_texsub);
    uint8_t *dst = heap ? heap : (uint8_t *)q + head;
    for (GLsizei i = 0; i < h; i++)
        memcpy(dst + (size_t)i * row, (const uint8_t *)data + (size_t)i * pitch, row);
    *q = (r_texsub){q->h, target, format, type, level, x, y, w, h, dst, heap};
    cs_end();
}

typedef struct {
    GLenum target, format, type;
    GLint level, internal;
    GLsizei width, height;
    const void *data;
} s_teximage;

static void y_teximage(void *p)
{
    s_teximage *q = p;
    glTexImage2D(q->target, q->level, q->internal, q->width, q->height, 0, q->format, q->type, q->data);
}

void cs_glTexImage2D(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h, GLint border,
                     GLenum format, GLenum type, const void *data)
{
    (void)border;
    if (direct())
        glTexImage2D(target, level, internal, w, h, 0, format, type, data);
    else if (!data)
        put_teximage(target, level, internal, w, h, format, type, NULL, 0, x_teximage);
    else {  // размер данных неизвестен (зависит от состояния распаковки) — поток гостя ждёт
        s_teximage q = {target, format, type, level, internal, w, h, data};
        cs_sync(y_teximage, &q);
    }
}

void cs_glCompressedTexImage2D(GLenum target, GLint level, GLenum internal, GLsizei w, GLsizei h, GLint border,
                               GLsizei size, const void *data)
{
    (void)border;
    if (direct())
        glCompressedTexImage2D(target, level, internal, w, h, 0, size, data);
    else
        put_teximage(target, level, (GLint)internal, w, h, 0, 0, data, (size_t)size, x_compressed_teximage);
}

// ---------------------------------------------------------------- имена объектов
// glGen* только резервирует имена: запас на 256 имён заготавливается потоком рендера одним ожиданием
enum { POOL_TEX, POOL_BUF, POOL_FBO, POOL_RBO, NPOOLS };
#define POOL_N 256
static struct {
    GLuint names[POOL_N];
    int n;
} g_pool[NPOOLS];

static void pool_reset(void) { memset(g_pool, 0, sizeof g_pool); }

static void y_fill(void *p)
{
    int k = (int)(intptr_t)p;
    switch (k) {
    case POOL_TEX: glGenTextures(POOL_N, g_pool[k].names); break;
    case POOL_BUF: glGenBuffers(POOL_N, g_pool[k].names); break;
    case POOL_FBO: glGenFramebuffers(POOL_N, g_pool[k].names); break;
    default: glGenRenderbuffers(POOL_N, g_pool[k].names); break;
    }
}

static void pool_take(int k, GLsizei n, GLuint *out)
{
    for (GLsizei i = 0; i < n; i++) {
        if (!g_pool[k].n) {
            cs_sync(y_fill, (void *)(intptr_t)k);
            g_pool[k].n = POOL_N;
        }
        out[i] = g_pool[k].names[--g_pool[k].n];
    }
}

void cs_glGenTextures(GLsizei n, GLuint *out)
{
    if (direct())
        glGenTextures(n, out);
    else
        pool_take(POOL_TEX, n, out);
}

void cs_glGenBuffers(GLsizei n, GLuint *out)
{
    if (direct())
        glGenBuffers(n, out);
    else
        pool_take(POOL_BUF, n, out);
}

void cs_glGenFramebuffers(GLsizei n, GLuint *out)
{
    if (direct())
        glGenFramebuffers(n, out);
    else
        pool_take(POOL_FBO, n, out);
}

void cs_glGenRenderbuffers(GLsizei n, GLuint *out)
{
    if (direct())
        glGenRenderbuffers(n, out);
    else
        pool_take(POOL_RBO, n, out);
}

typedef struct {
    cs_rec h;
    int kind;
    GLsizei n;
    GLuint names[];
} r_delete;

static void x_delete(void *r)
{
    r_delete *q = (r_delete *)r;
    switch (q->kind) {
    case POOL_TEX: glDeleteTextures(q->n, q->names); break;
    case POOL_BUF: glDeleteBuffers(q->n, q->names); break;
    case POOL_FBO: glDeleteFramebuffers(q->n, q->names); break;
    default: glDeleteRenderbuffers(q->n, q->names); break;
    }
}

static void put_delete(int kind, GLsizei n, const GLuint *names)
{
    r_delete *q = cs_begin(sizeof *q + (size_t)n * sizeof(GLuint), x_delete);
    q->kind = kind;
    q->n = n;
    memcpy(q->names, names, (size_t)n * sizeof(GLuint));
    cs_end();
}

void cs_glDeleteTextures(GLsizei n, const GLuint *names)
{
    if (direct())
        glDeleteTextures(n, names);
    else
        put_delete(POOL_TEX, n, names);
}

void cs_glDeleteBuffers(GLsizei n, const GLuint *names)
{
    if (direct())
        glDeleteBuffers(n, names);
    else
        put_delete(POOL_BUF, n, names);
}

void cs_glDeleteFramebuffers(GLsizei n, const GLuint *names)
{
    if (direct())
        glDeleteFramebuffers(n, names);
    else
        put_delete(POOL_FBO, n, names);
}

void cs_glDeleteRenderbuffers(GLsizei n, const GLuint *names)
{
    if (direct())
        glDeleteRenderbuffers(n, names);
    else
        put_delete(POOL_RBO, n, names);
}
