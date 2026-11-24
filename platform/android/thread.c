// Android: потоки гостя и рендера — на «больших» ядрах, приоритет выше обычного, подсказки производительности ADPF.
// Телефоны — big.LITTLE (и «prime»-ядро): без подсказок планировщик то и дело держит главный поток гостя (узкое
// место порта: он исполняет переведённый код игры) на энергоэффективных ядрах — кадр в разы дольше.
#define _GNU_SOURCE  // cpu_set_t, sched_setaffinity, gettid (bionic: __USE_GNU)
#include "platform/android/perf.h"

#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

#include "runtime/rt.h"

#define MAX_CPUS 32

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static cpu_set_t g_big;    // ядра без самых медленных (если все одинаковые — пусто: не трогаем)
static int g_have_big, g_ncpu;

static long read_long(const char *path)
{
    FILE *f = fopen(path, "r");
    long v = -1;
    if (f) {
        if (fscanf(f, "%ld", &v) != 1)
            v = -1;
        fclose(f);
    }
    return v;
}

static void detect(void)
{
    long freq[MAX_CPUS], lo = -1, hi = -1;
    char path[96];
    for (int c = 0; c < MAX_CPUS; c++) {
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", c);
        freq[c] = read_long(path);
        if (freq[c] <= 0)
            continue;
        g_ncpu = c + 1;
        lo = lo < 0 || freq[c] < lo ? freq[c] : lo;
        hi = freq[c] > hi ? freq[c] : hi;
    }
    CPU_ZERO(&g_big);
    if (lo > 0 && hi > lo) {
        for (int c = 0; c < g_ncpu; c++)
            if (freq[c] > lo)
                CPU_SET(c, &g_big);
        g_have_big = CPU_COUNT(&g_big) > 0;
    }
    const char *e = getenv("SNX_AFFINITY");  // SNX_AFFINITY=0 — не закреплять потоки
    if (e && *e == '0')
        g_have_big = 0;
    fprintf(stderr, "[android] %d cpus, max freq %ld..%ld kHz, big cores: %d\n", g_ncpu, lo, hi,
            g_have_big ? CPU_COUNT(&g_big) : 0);
}

// ---------------------------------------------------------------- ADPF (APerformanceHint, Android 13+ в NDK)
typedef struct APerformanceHintManager APerformanceHintManager;
typedef struct APerformanceHintSession APerformanceHintSession;
static APerformanceHintManager *(*p_get_manager)(void);
static APerformanceHintSession *(*p_create)(APerformanceHintManager *, const int32_t *, size_t, int64_t);
static int (*p_report)(APerformanceHintSession *, int64_t);
static int (*p_update_target)(APerformanceHintSession *, int64_t);
static void (*p_close)(APerformanceHintSession *);

static pthread_mutex_t g_hint_lock = PTHREAD_MUTEX_INITIALIZER;
static APerformanceHintSession *g_session;
static int32_t g_tids[4];
static int g_ntids;
static int64_t g_target_ns = 16666667;

static void hint_load(void)
{
    void *h = dlopen("libandroid.so", RTLD_NOW);
    if (!h)
        return;
    p_get_manager = (APerformanceHintManager * (*)(void)) dlsym(h, "APerformanceHint_getManager");
    p_create = (APerformanceHintSession * (*)(APerformanceHintManager *, const int32_t *, size_t, int64_t))
        dlsym(h, "APerformanceHint_createSession");
    p_report = (int (*)(APerformanceHintSession *, int64_t))dlsym(h, "APerformanceHint_reportActualWorkDuration");
    p_update_target =
        (int (*)(APerformanceHintSession *, int64_t))dlsym(h, "APerformanceHint_updateTargetWorkDuration");
    p_close = (void (*)(APerformanceHintSession *))dlsym(h, "APerformanceHint_closeSession");
}

// сессия на все зарегистрированные потоки (пересоздаётся при добавлении: setThreads есть только с Android 14)
static void hint_add_thread(int32_t tid)
{
    pthread_mutex_lock(&g_hint_lock);
    if (g_ntids < 4)
        g_tids[g_ntids++] = tid;
    if (p_get_manager && p_create) {
        APerformanceHintManager *m = p_get_manager();
        APerformanceHintSession *s = m ? p_create(m, g_tids, (size_t)g_ntids, g_target_ns) : NULL;
        if (s) {
            if (g_session && p_close)
                p_close(g_session);
            g_session = s;
        }
    }
    pthread_mutex_unlock(&g_hint_lock);
}

static void init(void)
{
    detect();
    const char *e = getenv("SNX_ADPF");  // SNX_ADPF=0 — без подсказок производительности
    if (!(e && *e == '0'))
        hint_load();
}

void rt_platform_thread_core(int core)
{
    pthread_once(&g_once, init);
    if (core == 0)
        return;  // события SDL и фоновые потоки гостя: где решит планировщик
    pid_t tid = gettid();
    if (g_have_big && sched_setaffinity(tid, sizeof g_big, &g_big) != 0)
        fprintf(stderr, "[android] sched_setaffinity(%d) failed\n", (int)tid);
    // THREAD_PRIORITY_URGENT_DISPLAY (-8); не вышло — THREAD_PRIORITY_DISPLAY (-4)
    if (setpriority(PRIO_PROCESS, (id_t)tid, -8) != 0)
        setpriority(PRIO_PROCESS, (id_t)tid, -4);
    hint_add_thread((int32_t)tid);
}

void android_perf_target(uint64_t ns)
{
    pthread_mutex_lock(&g_hint_lock);
    g_target_ns = (int64_t)ns;
    if (g_session && p_update_target)
        p_update_target(g_session, g_target_ns);
    pthread_mutex_unlock(&g_hint_lock);
}

void android_perf_frame(uint64_t work_ns)
{
    if (!g_session || !p_report || !work_ns)
        return;
    p_report(g_session, (int64_t)work_ns);
}
