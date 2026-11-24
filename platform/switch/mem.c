// Окно гостевой памяти на Switch (спайк S2): один блок кучи RT_WINDOW (1 ГБ), выровненный на 64 КБ.
// Подкачки по требованию у Horizon нет — память занята сразу, поэтому нужен полный режим (запуск через игру
// с зажатой R, ~3,2 ГБ); в applet-режиме (~400 МБ) блок не выделится. Первые 64 КБ гостя (NULL и окрестности)
// закрываются, если ядро позволяет; адреса за окном не охраняются (на маке — резерв 4 ГБ).
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include <sys/iosupport.h>

#include "platform/platform.h"
#include "runtime/rt.h"

static int is_full_mode(void)
{
    AppletType at = appletGetAppletType();
    return at == AppletType_Application || at == AppletType_SystemApplication;
}

// Applet-режим (hbmenu из альбома): процессу ~400 МБ, окно гостя 1 ГБ не выделится. Прежде здесь показывалось
// системное окно ошибки (errorApplicationShow, тип ApplicationError) из потока гостя при живых SDL/EGL — на Switch
// запуск в applet-режиме ронял Atmosphère (что именно падало — окно ошибки из библиотечного апплета или его стык с
// EGL — [проверить]). Поэтому проверка — первой в main, до SDL и потоков, а подсказка — текстовой консолью libnx
// (без библиотечных апплетов); выход — обычным возвратом в hbloader.
// Текст на консоль libnx мимо stdout: nxlinkStdio() или лог-файл (host/boot/main.c) уже перенаправили дескриптор 1
// через dup2 на сокет или файл, а consoleInit подменяет лишь устройство STD_OUT в devoptab_list — printf ушёл бы в
// лог, а экран остался бы чёрным (так и было на Switch). Пишем прямо в устройство консоли.
static void con_printf(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n <= 0)
        return;
    if (n >= (int)sizeof buf)
        n = (int)sizeof buf - 1;
    const devoptab_t *d = devoptab_list[STD_OUT];
    if (d && d->write_r)
        d->write_r(_REENT, NULL, buf, (size_t)n);
}

int rt_platform_preflight(void)
{
    u64 total = 0;
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    if (is_full_mode() || total == 0 || total > (u64)RT_WINDOW)
        return 1;
    fprintf(stderr, "[mem] applet mode (%llu MB < %u MB): launch through a game (hold R) for full memory\n",
            (unsigned long long)(total >> 20), RT_WINDOW >> 20);
    fflush(stderr);
    consoleInit(NULL);
    padConfigureInput(8, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeAny(&pad);
    // шрифт консоли libnx — только ASCII, поэтому текст на экране английский
    con_printf("\n\n  Spider-Man 2 (webheadNX): not enough memory.\n\n"
           "  The game needs a %u MB guest memory window, but this process only has %llu MB\n"
           "  (homebrew menu started in applet mode, e.g. from the Album).\n\n"
           "  Hold R while launching any installed game to open the homebrew menu\n"
           "  in full memory mode (title override), then start webheadNX from there.\n\n"
           "  Press + or B to exit.\n",
           RT_WINDOW >> 20, (unsigned long long)(total >> 20));
    consoleUpdate(NULL);
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & (HidNpadButton_Plus | HidNpadButton_B))
            break;
        consoleUpdate(NULL);
    }
    consoleExit(NULL);
    return 0;
}

// Системный диалог Switch при нехватке памяти в полном режиме (приложение может показать окно ошибки).
static void show_oom_dialog(int applet_mode, u64 total_mb)
{
    ErrorApplicationConfig cfg;
    const char *brief = "Spider-Man 2 (webheadNX): недостаточно памяти. Запустите hbmenu через игру, удерживая R (title override).";
    char detail[512];
    snprintf(detail, sizeof detail,
             "Для окна памяти гостя требуется %u МБ (полный режим памяти ~3,2 ГБ).\n"
             "Доступно процессу: %llu МБ (%s).\n\n"
             "Удерживайте R при запуске любой установленной игры, чтобы открыть hbmenu в полном режиме.",
             RT_WINDOW >> 20, (unsigned long long)total_mb, applet_mode ? "applet mode" : "full mode");
    if (R_SUCCEEDED(errorApplicationCreate(&cfg, brief, detail)))
        errorApplicationShow(&cfg);
}

int rt_platform_mem_init(void)
{
    u64 total = 0, used = 0;
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    int full_mode = is_full_mode();
    fprintf(stderr, "[mem] process memory %llu MB (used %llu MB), %s\n", (unsigned long long)(total >> 20),
            (unsigned long long)(used >> 20), full_mode ? "full mode" : "applet mode");
    if (!full_mode && total > 0 && total <= (u64)RT_WINDOW) {
        // сюда не доходим: rt_platform_preflight() в main уже вышел; окно ошибки из апплета не показываем
        fprintf(stderr, "[mem] applet mode (%llu MB < %u MB): launch through a game (hold R) for full memory\n",
                (unsigned long long)(total >> 20), RT_WINDOW >> 20);
        return -1;
    }
    u8 *p = aligned_alloc(0x10000, RT_WINDOW);
    if (!p) {
        fprintf(stderr, "[mem] cannot allocate the %u MB guest window: launch through a game (hold R) for full "
                        "memory\n", RT_WINDOW >> 20);
        if (full_mode)
            show_oom_dialog(0, total >> 20);
        return -1;
    }
    memset(p, 0, RT_WINDOW);
    Result r = svcSetMemoryPermission(p, 0x10000, Perm_None);
    fprintf(stderr, "[mem] guest window at %p, NULL guard %s\n", (void *)p, R_SUCCEEDED(r) ? "on" : "off");
    g_mem = p;
    return 0;
}

void rt_platform_thread_core(int core)
{
    Result r = svcSetThreadCoreMask(threadGetCurHandle(), core, 1u << core);
    if (R_FAILED(r))
        fprintf(stderr, "[mem] svcSetThreadCoreMask(core %d) failed: %#x\n", core, r);
}

void rt_platform_thread_background(void)
{
    rt_platform_thread_core(0);
    // Приоритет ниже стандартного (0x2C = 44; большее число — ниже приоритет, 0x34 = 52)
    Result r = svcSetThreadPriority(threadGetCurHandle(), 0x34);
    if (R_FAILED(r))
        fprintf(stderr, "[mem] svcSetThreadPriority(0x34) failed: %#x\n", r);
}

static pthread_mutex_t g_boost_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_boost_refs;

void rt_platform_cpu_boost(int on)
{
    if (!is_full_mode())
        return;
    pthread_mutex_lock(&g_boost_lock);
    if (on) {
        if (g_boost_refs++ == 0) {
            Result r = appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
            if (R_FAILED(r))
                fprintf(stderr, "[mem] appletSetCpuBoostMode(FastLoad) failed: %#x\n", r);
        }
    } else if (g_boost_refs > 0) {
        if (--g_boost_refs == 0) {
            Result r = appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
            if (R_FAILED(r))
                fprintf(stderr, "[mem] appletSetCpuBoostMode(Normal) failed: %#x\n", r);
        }
    }
    pthread_mutex_unlock(&g_boost_lock);
}

void rt_platform_cpu_boost_reset(void)
{
    if (!is_full_mode())
        return;
    // При аварийном выходе (on_trap) не ждём мьютекс, чтобы не зависнуть на чужой блокировке
    if (pthread_mutex_trylock(&g_boost_lock) == 0) {
        int was = g_boost_refs;
        g_boost_refs = 0;
        if (was > 0)
            appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
        pthread_mutex_unlock(&g_boost_lock);
    } else {
        appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
    }
}
