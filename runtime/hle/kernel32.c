// HLE kernel32: память, TLS, критические секции, версия/система, модули, окружение, кодовые страницы,
// локаль, время, потоки и синхронизация. Файлы и ресурсы — kernel32_file.c.
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <wctype.h>

#include "runtime/heap.h"
#include "runtime/hle/gl_cs.h"
#include "runtime/hle/hle.h"
#include "runtime/hle/openal.h"
#include "runtime/vmem.h"

// ================================================================ память
void hle_kernel32_HeapCreate(CPU *c) { HLE_ENTER("HeapCreate", 3); RET(3, heap_create(ARG(0))); }
void hle_kernel32_HeapDestroy(CPU *c) { HLE_ENTER("HeapDestroy", 1); RET(1, heap_destroy(ARG(0))); }
void hle_kernel32_GetProcessHeap(CPU *c) { HLE_ENTER("GetProcessHeap", 0); RET(0, heap_process()); }

void hle_kernel32_HeapAlloc(CPU *c)
{
    HLE_ENTER("HeapAlloc", 3);
    u32 p = heap_alloc(ARG(0), ARG(1), ARG(2));
    if (!p)
        rt_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
    RET(3, p);
}

void hle_kernel32_HeapReAlloc(CPU *c) { HLE_ENTER("HeapReAlloc", 4); RET(4, heap_realloc(ARG(0), ARG(1), ARG(2), ARG(3))); }
void hle_kernel32_HeapFree(CPU *c) { HLE_ENTER("HeapFree", 3); RET(3, heap_free(ARG(0), ARG(2))); }
void hle_kernel32_HeapSize(CPU *c) { HLE_ENTER("HeapSize", 3); RET(3, heap_size(ARG(0), ARG(2))); }
void hle_kernel32_HeapValidate(CPU *c) { HLE_ENTER("HeapValidate", 3); RET(3, 1); }
void hle_kernel32_HeapCompact(CPU *c) { HLE_ENTER("HeapCompact", 2); RET(2, 0x100000); }

void hle_kernel32_HeapWalk(CPU *c)
{
    HLE_ENTER("HeapWalk", 2);
    rt_set_last_error(259);  // ERROR_NO_MORE_ITEMS
    RET(2, 0);
}

void hle_kernel32_VirtualAlloc(CPU *c)
{
    HLE_ENTER("VirtualAlloc", 4);
    u32 a = vm_alloc(ARG(0), ARG(1), ARG(2), ARG(3));
    if (!a)
        rt_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
    RET(4, a);
}

void hle_kernel32_VirtualFree(CPU *c) { HLE_ENTER("VirtualFree", 3); RET(3, vm_free(ARG(0), ARG(1), ARG(2))); }

void hle_kernel32_VirtualQuery(CPU *c)
{
    HLE_ENTER("VirtualQuery", 3);
    vm_info vi;
    if (ARG(2) < sizeof vi || !vm_query(ARG(0), &vi))
        RET(3, 0);
    memcpy(g_mem + ARG(1), &vi, sizeof vi);
    RET(3, sizeof vi);
}

void hle_kernel32_VirtualProtect(CPU *c)
{
    HLE_ENTER("VirtualProtect", 4);
    if (ARG(3))
        wr32(g_mem, ARG(3), 0x04);  // защита не моделируется
    RET(4, 1);
}

void hle_kernel32_GlobalAlloc(CPU *c)
{
    HLE_ENTER("GlobalAlloc", 2);
    // описатель GMEM_MOVEABLE = указатель (GlobalLock возвращает его же)
    RET(2, heap_alloc(heap_process(), (ARG(0) & 0x40) ? HEAP_ZERO_MEMORY : 0, ARG(1)));
}

void hle_kernel32_GlobalLock(CPU *c) { HLE_ENTER("GlobalLock", 1); RET(1, ARG(0)); }

void hle_kernel32_GlobalUnlock(CPU *c)
{
    HLE_ENTER("GlobalUnlock", 1);
    rt_set_last_error(0);
    RET(1, 0);
}

void hle_kernel32_GlobalMemoryStatus(CPU *c)
{
    HLE_ENTER("GlobalMemoryStatus", 1);
    u32 p = ARG(0);
    const u32 v[8] = {32, 20, 512u << 20, 400u << 20, 1024u << 20, 800u << 20, 0x3FFF0000u, 0x30000000u};
    memcpy(g_mem + p, v, sizeof v);
    RET(1, 0);
}

// ================================================================ TLS
static u64 g_tls_used;
static pthread_mutex_t g_tls_lock = PTHREAD_MUTEX_INITIALIZER;

void hle_kernel32_TlsAlloc(CPU *c)
{
    HLE_ENTER("TlsAlloc", 0);
    pthread_mutex_lock(&g_tls_lock);
    u32 r = 0xFFFFFFFFu;
    for (u32 i = 0; i < RT_TLS_SLOTS; i++)
        if (!(g_tls_used >> i & 1)) {
            g_tls_used |= 1ull << i;
            r = i;
            break;
        }
    pthread_mutex_unlock(&g_tls_lock);
    RET(0, r);
}

void hle_kernel32_TlsFree(CPU *c)
{
    HLE_ENTER("TlsFree", 1);
    u32 i = ARG(0);
    if (i >= RT_TLS_SLOTS)
        RET(1, 0);
    pthread_mutex_lock(&g_tls_lock);
    g_tls_used &= ~(1ull << i);
    pthread_mutex_unlock(&g_tls_lock);
    RET(1, 1);
}

void hle_kernel32_TlsGetValue(CPU *c)
{
    HLE_ENTER("TlsGetValue", 1);
    u32 i = ARG(0);
    if (i >= RT_TLS_SLOTS) {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(1, 0);
    }
    rt_set_last_error(0);  // как Windows: успех сбрасывает код ошибки (значение может быть 0)
    RET(1, rd32(g_mem, rt_thread_current()->teb + TEB_TLS_SLOTS + 4 * i));
}

void hle_kernel32_TlsSetValue(CPU *c)
{
    HLE_ENTER("TlsSetValue", 2);
    u32 i = ARG(0);
    if (i >= RT_TLS_SLOTS)
        RET(2, 0);
    wr32(g_mem, rt_thread_current()->teb + TEB_TLS_SLOTS + 4 * i, ARG(1));
    RET(2, 1);
}

// ================================================================ критические секции
// CRITICAL_SECTION гостя: DebugInfo, LockCount, RecursionCount, OwningThread, LockSemaphore, SpinCount.
// В LockSemaphore хранится номер хостового рекурсивного мьютекса + 1.
#define MAX_CS 8192
static pthread_mutex_t *g_cs[MAX_CS];
static u32 g_ncs;
static pthread_mutex_t g_cs_lock = PTHREAD_MUTEX_INITIALIZER;

static u32 cs_new(void)
{
    pthread_mutex_t *m = malloc(sizeof *m);
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(m, &a);
    pthread_mutexattr_destroy(&a);
    pthread_mutex_lock(&g_cs_lock);
    u32 id = 0;
    for (u32 i = 0; i < MAX_CS; i++)
        if (!g_cs[i]) {
            g_cs[i] = m;
            id = i + 1;
            if (i >= g_ncs)
                g_ncs = i + 1;
            break;
        }
    pthread_mutex_unlock(&g_cs_lock);
    if (!id)
        rt_trap(&rt_thread_current()->cpu, 0, "too many critical sections");
    return id;
}

static void cs_init(u32 cs)
{
    memset(g_mem + cs, 0, 24);
    wr32(g_mem, cs + 4, 0xFFFFFFFFu);
    wr32(g_mem, cs + 0x10, cs_new());
}

static pthread_mutex_t *cs_get(u32 cs)
{
    u32 id = rd32(g_mem, cs + 0x10);
    if (!id || id > MAX_CS || !g_cs[id - 1]) {
        // неинициализированная (обнулённая) секция: создаём лениво под общим замком
        pthread_mutex_lock(&g_cs_lock);
        id = rd32(g_mem, cs + 0x10);
        int fresh = !id || id > MAX_CS || !g_cs[id - 1];
        pthread_mutex_unlock(&g_cs_lock);
        if (fresh) {
            hle_todo("EnterCriticalSection on uninitialized CRITICAL_SECTION");
            cs_init(cs);
            id = rd32(g_mem, cs + 0x10);
        }
    }
    return g_cs[id - 1];
}

void hle_kernel32_InitializeCriticalSection(CPU *c)
{
    HLE_ENTER("InitializeCriticalSection", 1);
    cs_init(ARG(0));
    RET(1, 0);
}

void hle_kernel32_InitializeCriticalSectionAndSpinCount(CPU *c)
{
    HLE_ENTER("InitializeCriticalSectionAndSpinCount", 2);
    cs_init(ARG(0));
    RET(2, 1);
}

void hle_kernel32_DeleteCriticalSection(CPU *c)
{
    HLE_ENTER("DeleteCriticalSection", 1);
    u32 cs = ARG(0), id = rd32(g_mem, cs + 0x10);
    if (id && id <= MAX_CS && g_cs[id - 1]) {
        pthread_mutex_lock(&g_cs_lock);
        pthread_mutex_t *m = g_cs[id - 1];
        g_cs[id - 1] = NULL;
        pthread_mutex_unlock(&g_cs_lock);
        pthread_mutex_destroy(m);
        free(m);
    }
    memset(g_mem + cs, 0, 24);
    RET(1, 0);
}

void hle_kernel32_EnterCriticalSection(CPU *c)
{
    HLE_ENTER("EnterCriticalSection", 1);
    u32 cs = ARG(0);
    pthread_mutex_lock(cs_get(cs));
    wr32(g_mem, cs + 8, rd32(g_mem, cs + 8) + 1);
    wr32(g_mem, cs + 0xC, rt_thread_current()->tid);
    wr32(g_mem, cs + 4, rd32(g_mem, cs + 4) + 1);
    RET(1, 0);
}

void hle_kernel32_LeaveCriticalSection(CPU *c)
{
    HLE_ENTER("LeaveCriticalSection", 1);
    u32 cs = ARG(0);
    u32 rc = rd32(g_mem, cs + 8);
    if (rc) {
        wr32(g_mem, cs + 8, rc - 1);
        wr32(g_mem, cs + 4, rd32(g_mem, cs + 4) - 1);
        if (rc == 1)
            wr32(g_mem, cs + 0xC, 0);
        pthread_mutex_unlock(cs_get(cs));
    }
    RET(1, 0);
}

// ================================================================ версия и система
static void put_version(u32 p, int wide)
{
    u32 size = rd32(g_mem, p);
    wr32(g_mem, p + 4, 5);
    wr32(g_mem, p + 8, 1);
    wr32(g_mem, p + 0xC, 2600);
    wr32(g_mem, p + 0x10, 2);
    if (wide)
        hle_put_w(p + 0x14, 128, "Service Pack 2");
    else
        hle_put_a(p + 0x14, 128, "Service Pack 2");
    u32 ex = p + 0x14 + (wide ? 256 : 128);
    if (size >= (wide ? 284u : 156u)) {  // OSVERSIONINFOEX
        wr16(g_mem, ex, 2);
        wr16(g_mem, ex + 2, 0);
        wr16(g_mem, ex + 4, 0x100);  // VER_SUITE_SINGLEUSERTS
        g_mem[ex + 6] = 1;           // VER_NT_WORKSTATION
        g_mem[ex + 7] = 0;
    }
}

void hle_kernel32_GetVersionExA(CPU *c) { HLE_ENTER("GetVersionExA", 1); put_version(ARG(0), 0); RET(1, 1); }
void hle_kernel32_GetVersionExW(CPU *c) { HLE_ENTER("GetVersionExW", 1); put_version(ARG(0), 1); RET(1, 1); }
void hle_kernel32_GetVersion(CPU *c) { HLE_ENTER("GetVersion", 0); RET(0, 0x0A280105u); }

void hle_kernel32_GetSystemInfo(CPU *c)
{
    HLE_ENTER("GetSystemInfo", 1);
    u32 p = ARG(0);
    memset(g_mem + p, 0, 36);
    wr32(g_mem, p + 4, 0x1000);
    wr32(g_mem, p + 8, 0x10000);
    wr32(g_mem, p + 0xC, RT_WINDOW - 0x10001u);
    wr32(g_mem, p + 0x10, 1);
    wr32(g_mem, p + 0x14, 1);
    wr32(g_mem, p + 0x18, 586);  // PROCESSOR_INTEL_PENTIUM (CPUID: Pentium Pro)
    wr32(g_mem, p + 0x1C, 0x10000);
    wr16(g_mem, p + 0x20, 6);
    wr16(g_mem, p + 0x22, 0x0101);
    RET(1, 0);
}

void hle_kernel32_IsProcessorFeaturePresent(CPU *c) { HLE_ENTER("IsProcessorFeaturePresent", 1); RET(1, 0); }
void hle_kernel32_GetCurrentProcess(CPU *c) { HLE_ENTER("GetCurrentProcess", 0); RET(0, 0xFFFFFFFFu); }
void hle_kernel32_GetCurrentProcessId(CPU *c) { HLE_ENTER("GetCurrentProcessId", 0); RET(0, 0x100); }
void hle_kernel32_GetCurrentThread(CPU *c) { HLE_ENTER("GetCurrentThread", 0); RET(0, 0xFFFFFFFEu); }
void hle_kernel32_GetCurrentThreadId(CPU *c) { HLE_ENTER("GetCurrentThreadId", 0); RET(0, rt_thread_current()->tid); }
void hle_kernel32_GetLastError(CPU *c) { HLE_ENTER("GetLastError", 0); RET(0, rt_get_last_error()); }
void hle_kernel32_SetLastError(CPU *c) { HLE_ENTER("SetLastError", 1); rt_set_last_error(ARG(0)); RET(1, 0); }
void hle_kernel32_SetErrorMode(CPU *c) { HLE_ENTER("SetErrorMode", 1); RET(1, 0); }
void hle_kernel32_Beep(CPU *c) { HLE_ENTER("Beep", 2); RET(2, 1); }

void hle_kernel32_GetComputerNameA(CPU *c)
{
    HLE_ENTER("GetComputerNameA", 2);
    u32 cap = rd32(g_mem, ARG(1));
    if (cap < 8) {
        wr32(g_mem, ARG(1), 8);
        rt_set_last_error(111);  // ERROR_BUFFER_OVERFLOW
        RET(2, 0);
    }
    wr32(g_mem, ARG(1), hle_put_a(ARG(0), cap, "SHREKNX"));
    RET(2, 1);
}

void hle_kernel32_GetComputerNameW(CPU *c)
{
    HLE_ENTER("GetComputerNameW", 2);
    u32 cap = rd32(g_mem, ARG(1));
    if (cap < 8) {
        wr32(g_mem, ARG(1), 8);
        rt_set_last_error(111);
        RET(2, 0);
    }
    wr32(g_mem, ARG(1), hle_put_w(ARG(0), cap, "SHREKNX"));
    RET(2, 1);
}

void hle_kernel32_GetWindowsDirectoryA(CPU *c) { HLE_ENTER("GetWindowsDirectoryA", 2); RET(2, hle_put_a(ARG(0), ARG(1), "C:\\WINDOWS")); }
void hle_kernel32_GetWindowsDirectoryW(CPU *c) { HLE_ENTER("GetWindowsDirectoryW", 2); RET(2, hle_put_w(ARG(0), ARG(1), "C:\\WINDOWS")); }
void hle_kernel32_GetSystemDirectoryA(CPU *c) { HLE_ENTER("GetSystemDirectoryA", 2); RET(2, hle_put_a(ARG(0), ARG(1), "C:\\WINDOWS\\system32")); }
void hle_kernel32_GetSystemDirectoryW(CPU *c) { HLE_ENTER("GetSystemDirectoryW", 2); RET(2, hle_put_w(ARG(0), ARG(1), "C:\\WINDOWS\\system32")); }

void hle_kernel32_GetProcessWorkingSetSize(CPU *c)
{
    HLE_ENTER("GetProcessWorkingSetSize", 3);
    wr32(g_mem, ARG(1), 200u << 10);
    wr32(g_mem, ARG(2), 1380u << 10);
    RET(3, 1);
}

// ================================================================ модули
// Внешние (HLE) DLL: поддельные описатели модулей со страницей «MZ» в служебной области.
static const char *const g_hle_dlls[] = {"kernel32.dll", "user32.dll", "gdi32.dll", "advapi32.dll", "winmm.dll",
                                          "shell32.dll", "ole32.dll", "comctl32.dll", "comdlg32.dll", "imm32.dll",
                                          "wsock32.dll", "d3d8.dll", "ddraw.dll", "dinput8.dll", "binkw32.dll",
                                          "vorbisfile.dll", "ifc23.dll", "ntdll.dll", "msvcrt.dll", "openal32.dll",
                                          "defopenal32.dll", NULL};
#define HLE_MOD_BASE 0x00E00000u

static u32 hle_module(const char *name)
{
    const char *b = name;
    for (const char *p = name; *p; p++)
        if (*p == '\\' || *p == '/')
            b = p + 1;
    for (u32 i = 0; g_hle_dlls[i]; i++) {
        size_t n = strlen(b), ml = strlen(g_hle_dlls[i]);
        if ((n == ml && !strncasecmp(b, g_hle_dlls[i], n)) ||
            (n + 4 == ml && !strncasecmp(b, g_hle_dlls[i], n))) {
            u32 h = HLE_MOD_BASE + 0x1000u * i;
            if (g_mem[h] != 'M') {
                g_mem[h] = 'M';
                g_mem[h + 1] = 'Z';
            }
            return h;
        }
    }
    return 0;
}

static const char *hle_module_name(u32 h)
{
    if (h < HLE_MOD_BASE || (h - HLE_MOD_BASE) % 0x1000)
        return NULL;
    u32 i = (h - HLE_MOD_BASE) / 0x1000;
    for (u32 k = 0; g_hle_dlls[k]; k++)
        if (k == i)
            return g_hle_dlls[k];
    return NULL;
}

static u32 module_handle(const char *name)
{
    if (!name)
        return rt_module_main()->desc->base;
    rt_module *m = rt_module_by_name(name);
    if (m)
        return m->desc->base;
    u32 h = hle_module(name);
    if (!h)
        rt_set_last_error(ERROR_MOD_NOT_FOUND);
    return h;
}

void hle_kernel32_GetModuleHandleA(CPU *c)
{
    HLE_ENTER("GetModuleHandleA", 1);
    RET(1, module_handle(ARG(0) ? hle_cstr(ARG(0)) : NULL));
}

void hle_kernel32_GetModuleHandleW(CPU *c)
{
    HLE_ENTER("GetModuleHandleW", 1);
    char name[520];
    if (ARG(0))
        hle_w2utf8(ARG(0), name, sizeof name);
    RET(1, module_handle(ARG(0) ? name : NULL));
}

static int module_path(u32 h, char *out, size_t cap)
{
    rt_module *m = h ? rt_module_by_base(h) : rt_module_main();
    const char *hn = m ? NULL : hle_module_name(h);
    if (!m && !hn)
        return 0;
    if (m)
        snprintf(out, cap, "%s\\System\\%s", rt_guest_root(), m->desc->name);
    else
        snprintf(out, cap, "C:\\WINDOWS\\system32\\%s", hn);
    return 1;
}

void hle_kernel32_GetModuleFileNameA(CPU *c)
{
    HLE_ENTER("GetModuleFileNameA", 3);
    char path[600];
    if (!module_path(ARG(0), path, sizeof path)) {
        rt_set_last_error(ERROR_MOD_NOT_FOUND);
        RET(3, 0);
    }
    RET(3, hle_put_a(ARG(1), ARG(2), path));
}

void hle_kernel32_GetModuleFileNameW(CPU *c)
{
    HLE_ENTER("GetModuleFileNameW", 3);
    char path[600];
    if (!module_path(ARG(0), path, sizeof path)) {
        rt_set_last_error(ERROR_MOD_NOT_FOUND);
        RET(3, 0);
    }
    RET(3, hle_put_w(ARG(1), ARG(2), path));
}

// Функции, которые ищут через GetProcAddress у внешних DLL, а не импортируют статически.
void hle_kernel32_IsDebuggerPresent(CPU *c) { HLE_ENTER("IsDebuggerPresent", 0); RET(0, 0); }

static hle_dyn_fn g_kernel32_dyn[] = {
    {"InitializeCriticalSectionAndSpinCount", hle_kernel32_InitializeCriticalSectionAndSpinCount, 0},
    {"IsDebuggerPresent", hle_kernel32_IsDebuggerPresent, 0},
    {"GetVersion", hle_kernel32_GetVersion, 0},
    {"IsProcessorFeaturePresent", hle_kernel32_IsProcessorFeaturePresent, 0},
    {NULL, NULL, 0},
};
static const struct {
    const char *dll;
    hle_dyn_fn *fns;
} g_dyn[] = {
    {"kernel32.dll", g_kernel32_dyn},
    {"openal32.dll", hle_openal_fns},
    {"defopenal32.dll", hle_openal_fns},
};

void hle_kernel32_GetProcAddress(CPU *c)
{
    HLE_ENTER("GetProcAddress", 2);
    u32 h = ARG(0), np = ARG(1);
    const char *name = np >= 0x10000 ? hle_cstr(np) : NULL;
    rt_module *m = rt_module_by_base(h);
    u32 r = 0;
    if (m) {
        r = rt_module_export(m, name, np);
    } else if (hle_module_name(h) && name) {
        for (size_t i = 0; i < sizeof g_dyn / sizeof *g_dyn && !r; i++)
            if (!strcasecmp(g_dyn[i].dll, hle_module_name(h)))
                r = hle_dyn_thunk(g_dyn[i].fns, g_dyn[i].dll, name);
        if (!r && getenv("SNX_LOG_LOADER"))
            fprintf(stderr, "[hle] GetProcAddress(%s, %s): not provided\n", hle_module_name(h), name);
    }
    if (!r)
        rt_set_last_error(ERROR_PROC_NOT_FOUND);
    RET(2, r);
}

static u32 load_library(const char *name)
{
    rt_module *m = rt_module_by_name(name);
    if (m) {
        m->refs++;
        if (!m->attached && rt_module_attach(m) != 0)
            return 0;
        return m->desc->base;
    }
    u32 h = hle_module(name);
    if (!h) {
        if (getenv("SNX_LOG_LOADER"))
            fprintf(stderr, "[hle] LoadLibrary(%s): not found\n", name);
        rt_set_last_error(ERROR_MOD_NOT_FOUND);
    }
    return h;
}

void hle_kernel32_LoadLibraryA(CPU *c) { HLE_ENTER("LoadLibraryA", 1); RET(1, load_library(hle_cstr(ARG(0)))); }

void hle_kernel32_LoadLibraryW(CPU *c)
{
    HLE_ENTER("LoadLibraryW", 1);
    char name[520];
    hle_w2utf8(ARG(0), name, sizeof name);
    RET(1, load_library(name));
}

void hle_kernel32_LoadLibraryExW(CPU *c)
{
    HLE_ENTER("LoadLibraryExW", 3);
    char name[520];
    hle_w2utf8(ARG(0), name, sizeof name);
    RET(3, load_library(name));
}

void hle_kernel32_FreeLibrary(CPU *c) { HLE_ENTER("FreeLibrary", 1); RET(1, 1); }

// ================================================================ командная строка и окружение
static u32 g_env_a, g_env_w;
static const char g_env[] = "COMPUTERNAME=SHREKNX\0OS=Windows_NT\0PATH=C:\\WINDOWS\\system32;C:\\WINDOWS\0"
                            "SystemRoot=C:\\WINDOWS\0TEMP=C:\\TEMP\0TMP=C:\\TEMP\0windir=C:\\WINDOWS\0\0";

static void env_init(void)
{
    if (g_env_a)
        return;
    u32 n = sizeof g_env;
    g_env_a = heap_alloc(heap_process(), 0, n);
    memcpy(g_mem + g_env_a, g_env, n);
    g_env_w = heap_alloc(heap_process(), 0, 2 * n);
    for (u32 i = 0; i < n; i++)
        wr16(g_mem, g_env_w + 2 * i, (u8)g_env[i]);
}

void hle_kernel32_GetCommandLineA(CPU *c) { HLE_ENTER("GetCommandLineA", 0); RET(0, rt_cmdline_a()); }
void hle_kernel32_GetCommandLineW(CPU *c) { HLE_ENTER("GetCommandLineW", 0); RET(0, rt_cmdline_w()); }
void hle_kernel32_GetEnvironmentStrings(CPU *c) { HLE_ENTER("GetEnvironmentStrings", 0); env_init(); RET(0, g_env_a); }
void hle_kernel32_GetEnvironmentStringsW(CPU *c) { HLE_ENTER("GetEnvironmentStringsW", 0); env_init(); RET(0, g_env_w); }
void hle_kernel32_FreeEnvironmentStringsA(CPU *c) { HLE_ENTER("FreeEnvironmentStringsA", 1); RET(1, 1); }
void hle_kernel32_FreeEnvironmentStringsW(CPU *c) { HLE_ENTER("FreeEnvironmentStringsW", 1); RET(1, 1); }

void hle_kernel32_GetEnvironmentVariableA(CPU *c)
{
    HLE_ENTER("GetEnvironmentVariableA", 3);
    const char *name = hle_cstr(ARG(0));
    size_t nl = strlen(name);
    for (const char *e = g_env; *e; e += strlen(e) + 1)
        if (!strncasecmp(e, name, nl) && e[nl] == '=') {
            const char *v = e + nl + 1;
            u32 len = (u32)strlen(v);
            if (len + 1 > ARG(2))
                RET(3, len + 1);
            RET(3, hle_put_a(ARG(1), ARG(2), v));
        }
    rt_set_last_error(ERROR_ENVVAR_NOT_FOUND);
    RET(3, 0);
}

void hle_kernel32_SetEnvironmentVariableA(CPU *c) { HLE_ENTER("SetEnvironmentVariableA", 2); hle_todo("SetEnvironmentVariable"); RET(2, 1); }
void hle_kernel32_SetEnvironmentVariableW(CPU *c) { HLE_ENTER("SetEnvironmentVariableW", 2); hle_todo("SetEnvironmentVariable"); RET(2, 1); }

void hle_kernel32_GetStartupInfoA(CPU *c)
{
    HLE_ENTER("GetStartupInfoA", 1);
    memset(g_mem + ARG(0), 0, 68);
    wr32(g_mem, ARG(0), 68);
    RET(1, 0);
}

void hle_kernel32_SetHandleCount(CPU *c) { HLE_ENTER("SetHandleCount", 1); RET(1, ARG(0)); }

// ================================================================ кодовые страницы
// CP_ACP = 1252: 0x80–0x9F — типографские знаки, остальное совпадает с Latin-1.
static const u16 g_cp1252[32] = {0x20AC, 0x81, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                                 0x2039, 0x0152, 0x8D, 0x017D, 0x8F, 0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                                 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D, 0x017E, 0x0178};

static u16 a2w(u8 b) { return b >= 0x80 && b < 0xA0 ? g_cp1252[b - 0x80] : b; }

static int w2a(u32 w, u8 *out)
{
    if (w < 0x80 || (w >= 0xA0 && w < 0x100)) {
        *out = (u8)w;
        return 1;
    }
    for (int i = 0; i < 32; i++)
        if (g_cp1252[i] == w) {
            *out = (u8)(0x80 + i);
            return 1;
        }
    *out = '?';
    return 0;
}

void hle_kernel32_GetACP(CPU *c) { HLE_ENTER("GetACP", 0); RET(0, 1252); }
void hle_kernel32_GetOEMCP(CPU *c) { HLE_ENTER("GetOEMCP", 0); RET(0, 437); }
void hle_kernel32_IsValidCodePage(CPU *c) { HLE_ENTER("IsValidCodePage", 1); RET(1, ARG(0) == 1252 || ARG(0) == 437 || ARG(0) == 65001 || ARG(0) == 20127); }
void hle_kernel32_IsDBCSLeadByteEx(CPU *c) { HLE_ENTER("IsDBCSLeadByteEx", 2); RET(2, 0); }

void hle_kernel32_GetCPInfo(CPU *c)
{
    HLE_ENTER("GetCPInfo", 2);
    u32 p = ARG(1);
    memset(g_mem + p, 0, 20);
    wr32(g_mem, p, 1);
    g_mem[p + 4] = '?';
    RET(2, 1);
}

void hle_kernel32_MultiByteToWideChar(CPU *c)
{
    HLE_ENTER("MultiByteToWideChar", 6);
    u32 cp = ARG(0), src = ARG(2), n = ARG(3), dst = ARG(4), cap = ARG(5);
    if (n == 0xFFFFFFFFu)
        n = (u32)strlen(hle_cstr(src)) + 1;
    u32 o = 0;
    for (u32 i = 0; i < n;) {
        u32 ch = g_mem[src + i++];
        if (cp == 65001 && ch >= 0xC0) {
            int k = ch >= 0xF0 ? 3 : ch >= 0xE0 ? 2 : 1;
            ch &= 0x3F >> k;
            while (k-- && i < n && (g_mem[src + i] & 0xC0) == 0x80)
                ch = ch << 6 | (g_mem[src + i++] & 0x3F);
            if (ch > 0xFFFF)
                ch = 0xFFFD;
        } else if (cp != 65001) {
            ch = a2w((u8)ch);
        }
        if (cap) {
            if (o >= cap) {
                rt_set_last_error(ERROR_INSUFFICIENT_BUFFER);
                RET(6, 0);
            }
            wr16(g_mem, dst + 2 * o, (u16)ch);
        }
        o++;
    }
    RET(6, o);
}

void hle_kernel32_WideCharToMultiByte(CPU *c)
{
    HLE_ENTER("WideCharToMultiByte", 8);
    u32 cp = ARG(0), src = ARG(2), n = ARG(3), dst = ARG(4), cap = ARG(5), used = ARG(7);
    if (n == 0xFFFFFFFFu)
        n = (u32)hle_wlen(src) + 1;
    u32 o = 0;
    int defaulted = 0;
    for (u32 i = 0; i < n; i++) {
        u32 ch = rd16(g_mem, src + 2 * i);
        u8 buf[4];
        int k = 1;
        if (cp == 65001) {
            if (ch < 0x80) buf[0] = (u8)ch;
            else if (ch < 0x800) { buf[0] = (u8)(0xC0 | ch >> 6); buf[1] = (u8)(0x80 | (ch & 0x3F)); k = 2; }
            else { buf[0] = (u8)(0xE0 | ch >> 12); buf[1] = (u8)(0x80 | (ch >> 6 & 0x3F)); buf[2] = (u8)(0x80 | (ch & 0x3F)); k = 3; }
        } else if (!w2a(ch, buf)) {
            defaulted = 1;
            if (ARG(6))
                buf[0] = g_mem[ARG(6)];
        }
        if (cap) {
            if (o + (u32)k > cap) {
                rt_set_last_error(ERROR_INSUFFICIENT_BUFFER);
                RET(8, 0);
            }
            memcpy(g_mem + dst + o, buf, (size_t)k);
        }
        o += (u32)k;
    }
    if (used)
        wr32(g_mem, used, (u32)defaulted);
    RET(8, o);
}

// CT_CTYPE1 для символа (Latin-1 и далее — по классификации хоста)
static u16 ctype1(u32 ch)
{
    u16 t = 0;
    if (ch < 0x80) {
        if (isupper((int)ch)) t |= 0x001;
        if (islower((int)ch)) t |= 0x002;
        if (isdigit((int)ch)) t |= 0x004;
        if (isspace((int)ch)) t |= 0x008;
        if (ispunct((int)ch)) t |= 0x010;
        if (iscntrl((int)ch)) t |= 0x020;
        if (ch == ' ' || ch == '\t') t |= 0x040;
        if (isxdigit((int)ch)) t |= 0x080;
        if (isalpha((int)ch)) t |= 0x100;
        return t;
    }
    if (ch < 0xA0)
        return 0x020;
    if (ch == 0xA0)
        return 0x048;
    if (ch < 0xC0 || ch == 0xD7 || ch == 0xF7)
        return 0x010;
    if (ch < 0x100)
        return (ch < 0xDF ? 0x001 : 0x002) | 0x100;
    if (towupper((wint_t)ch) != (wint_t)ch)
        return 0x102;
    if (towlower((wint_t)ch) != (wint_t)ch)
        return 0x101;
    return iswalpha((wint_t)ch) ? 0x100 : 0x010;
}

void hle_kernel32_GetStringTypeW(CPU *c)
{
    HLE_ENTER("GetStringTypeW", 4);
    u32 type = ARG(0), src = ARG(1), n = ARG(2), out = ARG(3);
    if (n == 0xFFFFFFFFu)
        n = (u32)hle_wlen(src) + 1;
    for (u32 i = 0; i < n; i++)
        wr16(g_mem, out + 2 * i, type == 1 ? ctype1(rd16(g_mem, src + 2 * i)) : 0);
    RET(4, 1);
}

void hle_kernel32_GetStringTypeA(CPU *c)
{
    HLE_ENTER("GetStringTypeA", 5);
    u32 type = ARG(1), src = ARG(2), n = ARG(3), out = ARG(4);
    if (n == 0xFFFFFFFFu)
        n = (u32)strlen(hle_cstr(src)) + 1;
    for (u32 i = 0; i < n; i++)
        wr16(g_mem, out + 2 * i, type == 1 ? ctype1(a2w(g_mem[src + i])) : 0);
    RET(5, 1);
}

static u32 map_case(u32 flags, u32 ch)
{
    if (flags & 0x100)
        return ch < 0x80 ? (u32)tolower((int)ch) : (ch >= 0xC0 && ch < 0xDF && ch != 0xD7) ? ch + 0x20 : (u32)towlower((wint_t)ch);
    if (flags & 0x200)
        return ch < 0x80 ? (u32)toupper((int)ch) : (ch >= 0xE0 && ch < 0xFF && ch != 0xF7) ? ch - 0x20 : (u32)towupper((wint_t)ch);
    return ch;
}

void hle_kernel32_LCMapStringW(CPU *c)
{
    HLE_ENTER("LCMapStringW", 6);
    u32 flags = ARG(1), src = ARG(2), n = ARG(3), dst = ARG(4), cap = ARG(5);
    if (n == 0xFFFFFFFFu)
        n = (u32)hle_wlen(src) + 1;
    if (!cap)
        RET(6, n);
    if (cap < n) {
        rt_set_last_error(ERROR_INSUFFICIENT_BUFFER);
        RET(6, 0);
    }
    for (u32 i = 0; i < n; i++)
        wr16(g_mem, dst + 2 * i, (u16)map_case(flags, rd16(g_mem, src + 2 * i)));
    RET(6, n);
}

void hle_kernel32_LCMapStringA(CPU *c)
{
    HLE_ENTER("LCMapStringA", 6);
    u32 flags = ARG(1), src = ARG(2), n = ARG(3), dst = ARG(4), cap = ARG(5);
    if (n == 0xFFFFFFFFu)
        n = (u32)strlen(hle_cstr(src)) + 1;
    if (!cap)
        RET(6, n);
    if (cap < n) {
        rt_set_last_error(ERROR_INSUFFICIENT_BUFFER);
        RET(6, 0);
    }
    for (u32 i = 0; i < n; i++) {
        u8 b;
        w2a(map_case(flags, a2w(g_mem[src + i])), &b);
        g_mem[dst + i] = b;
    }
    RET(6, n);
}

static int compare_strings(u32 flags, u32 a, u32 na, u32 b, u32 nb, int wide)
{
    if (na == 0xFFFFFFFFu)
        na = wide ? (u32)hle_wlen(a) : (u32)strlen(hle_cstr(a));
    if (nb == 0xFFFFFFFFu)
        nb = wide ? (u32)hle_wlen(b) : (u32)strlen(hle_cstr(b));
    for (u32 i = 0; i < na && i < nb; i++) {
        u32 x = wide ? rd16(g_mem, a + 2 * i) : a2w(g_mem[a + i]);
        u32 y = wide ? rd16(g_mem, b + 2 * i) : a2w(g_mem[b + i]);
        if (flags & 1) {  // NORM_IGNORECASE
            x = map_case(0x100, x);
            y = map_case(0x100, y);
        }
        if (x != y)
            return x < y ? 1 : 3;
    }
    return na < nb ? 1 : na > nb ? 3 : 2;
}

void hle_kernel32_CompareStringA(CPU *c) { HLE_ENTER("CompareStringA", 6); RET(6, compare_strings(ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), 0)); }
void hle_kernel32_CompareStringW(CPU *c) { HLE_ENTER("CompareStringW", 6); RET(6, compare_strings(ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), 1)); }

// ================================================================ локаль
static const char *locale_info(u32 type)
{
    switch (type & 0xFFFF) {
    case 0x0001: return "0409";           // LOCALE_ILANGUAGE
    case 0x0002: return "English (United States)";
    case 0x0003: return "ENU";            // LOCALE_SABBREVLANGNAME
    case 0x0004: return "English";
    case 0x0005: return "1";              // LOCALE_ICOUNTRY
    case 0x0006: return "United States";
    case 0x0007: return "USA";            // LOCALE_SABBREVCTRYNAME
    case 0x000B: return "437";            // LOCALE_IDEFAULTCODEPAGE
    case 0x000C: return ",";              // LOCALE_SLIST
    case 0x000E: return ".";              // LOCALE_SDECIMAL
    case 0x000F: return ",";              // LOCALE_STHOUSAND
    case 0x0010: return "3;0";            // LOCALE_SGROUPING
    case 0x0014: return "$";              // LOCALE_SCURRENCY
    case 0x001D: return "/";              // LOCALE_SDATE
    case 0x001E: return ":";              // LOCALE_STIME
    case 0x001F: return "M/d/yyyy";       // LOCALE_SSHORTDATE
    case 0x0020: return "dddd, MMMM dd, yyyy";
    case 0x0028: return "AM";
    case 0x0029: return "PM";
    case 0x1001: return "English";        // LOCALE_SENGLANGUAGE
    case 0x1002: return "United States";  // LOCALE_SENGCOUNTRY
    case 0x1004: return "1252";           // LOCALE_IDEFAULTANSICODEPAGE
    case 0x0059: return "en";             // LOCALE_SISO639LANGNAME
    case 0x005A: return "US";             // LOCALE_SISO3166CTRYNAME
    default: return NULL;
    }
}

static int locale_get(CPU *c, int wide)
{
    u32 type = ARG(1), buf = ARG(2), cap = ARG(3);
    const char *v = locale_info(type);
    if (!v) {
        hle_todo("GetLocaleInfo: unknown LCTYPE");
        if (g_hle_trace)
            fprintf(stderr, "[hle] GetLocaleInfo type %08x\n", type);
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (type & 0x20000000u) {  // LOCALE_RETURN_NUMBER
        if (cap < (wide ? 2u : 4u))
            return 0;
        wr32(g_mem, buf, (u32)strtoul(v, NULL, 10));
        return wide ? 2 : 4;
    }
    u32 n = (u32)strlen(v) + 1;
    if (!cap)
        return (int)n;
    if (cap < n) {
        rt_set_last_error(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    return (int)(wide ? hle_put_w(buf, cap, v) : hle_put_a(buf, cap, v)) + 1;
}

void hle_kernel32_GetLocaleInfoA(CPU *c) { HLE_ENTER("GetLocaleInfoA", 4); RET(4, locale_get(c, 0)); }
void hle_kernel32_GetLocaleInfoW(CPU *c) { HLE_ENTER("GetLocaleInfoW", 4); RET(4, locale_get(c, 1)); }
void hle_kernel32_GetUserDefaultLCID(CPU *c) { HLE_ENTER("GetUserDefaultLCID", 0); RET(0, 0x409); }
void hle_kernel32_GetSystemDefaultLangID(CPU *c) { HLE_ENTER("GetSystemDefaultLangID", 0); RET(0, 0x409); }
void hle_kernel32_IsValidLocale(CPU *c) { HLE_ENTER("IsValidLocale", 2); RET(2, ARG(0) == 0x409); }

void hle_kernel32_EnumSystemLocalesA(CPU *c)
{
    HLE_ENTER("EnumSystemLocalesA", 2);
    u32 proc = ARG(0), sp = c->esp;
    c->esp -= 64;  // строка — на гостевом стеке ниже аргументов
    u32 s = c->esp;
    hle_put_a(s, 16, "00000409");
    rt_guest_call(c, proc, 1, &s);
    c->esp = sp;
    RET(2, 1);
}

// ================================================================ время
static u64 now_filetime(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (u64)ts.tv_sec * 10000000ull + (u64)ts.tv_nsec / 100 + 116444736000000000ull;
}

static void tm_to_systemtime(const struct tm *tm, u16 ms, u32 st)
{
    wr16(g_mem, st + 0, (u16)(tm->tm_year + 1900));
    wr16(g_mem, st + 2, (u16)(tm->tm_mon + 1));
    wr16(g_mem, st + 4, (u16)tm->tm_wday);
    wr16(g_mem, st + 6, (u16)tm->tm_mday);
    wr16(g_mem, st + 8, (u16)tm->tm_hour);
    wr16(g_mem, st + 10, (u16)tm->tm_min);
    wr16(g_mem, st + 12, (u16)(tm->tm_sec > 59 ? 59 : tm->tm_sec));
    wr16(g_mem, st + 14, ms);
}

static void filetime_to_systemtime(u64 ft, u32 st)
{
    time_t t = (time_t)((ft - 116444736000000000ull) / 10000000ull);
    struct tm tm;
    gmtime_r(&t, &tm);
    tm_to_systemtime(&tm, (u16)(ft / 10000 % 1000), st);
}

static void local_now(struct tm *tm, u16 *ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, tm);
    if (ms)
        *ms = (u16)(ts.tv_nsec / 1000000);
}

void hle_kernel32_GetTickCount(CPU *c) { HLE_ENTER("GetTickCount", 0); RET(0, (u32)(rt_guest_ticks_ns() / 1000000u)); }

void hle_kernel32_QueryPerformanceCounter(CPU *c)
{
    HLE_ENTER("QueryPerformanceCounter", 1);
    wr64(g_mem, ARG(0), rt_guest_ticks_ns());
    RET(1, 1);
}

void hle_kernel32_QueryPerformanceFrequency(CPU *c)
{
    HLE_ENTER("QueryPerformanceFrequency", 1);
    wr64(g_mem, ARG(0), 1000000000ull);
    RET(1, 1);
}

void hle_kernel32_GetSystemTimeAsFileTime(CPU *c)
{
    HLE_ENTER("GetSystemTimeAsFileTime", 1);
    wr64(g_mem, ARG(0), now_filetime());
    RET(1, 0);
}

void hle_kernel32_GetLocalTime(CPU *c)
{
    HLE_ENTER("GetLocalTime", 1);
    struct tm tm;
    u16 ms;
    local_now(&tm, &ms);
    tm_to_systemtime(&tm, ms, ARG(0));
    RET(1, 0);
}

void hle_kernel32_FileTimeToSystemTime(CPU *c)
{
    HLE_ENTER("FileTimeToSystemTime", 2);
    filetime_to_systemtime(rd64(g_mem, ARG(0)), ARG(1));
    RET(2, 1);
}

void hle_kernel32_FileTimeToLocalFileTime(CPU *c) { HLE_ENTER("FileTimeToLocalFileTime", 2); wr64(g_mem, ARG(1), rd64(g_mem, ARG(0))); RET(2, 1); }
void hle_kernel32_LocalFileTimeToFileTime(CPU *c) { HLE_ENTER("LocalFileTimeToFileTime", 2); wr64(g_mem, ARG(1), rd64(g_mem, ARG(0))); RET(2, 1); }

void hle_kernel32_SystemTimeToFileTime(CPU *c)
{
    HLE_ENTER("SystemTimeToFileTime", 2);
    u32 st = ARG(0);
    // дни от 1970-01-01 по григорианскому календарю (timegm в newlib нет)
    s64 y = rd16(g_mem, st), m = rd16(g_mem, st + 2), d = rd16(g_mem, st + 6);
    y -= m <= 2;
    s64 era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400;
    s64 doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    s64 days = era * 146097 + doe - 719468;
    s64 t = days * 86400 + rd16(g_mem, st + 8) * 3600 + rd16(g_mem, st + 10) * 60 + rd16(g_mem, st + 12);
    wr64(g_mem, ARG(1), (u64)(t * 10000000 + rd16(g_mem, st + 14) * 10000LL) + 116444736000000000ull);
    RET(2, 1);
}

void hle_kernel32_GetTimeZoneInformation(CPU *c)
{
    HLE_ENTER("GetTimeZoneInformation", 1);
    memset(g_mem + ARG(0), 0, 172);
    RET(1, 0);  // TIME_ZONE_ID_UNKNOWN
}

void hle_kernel32_SetLocalTime(CPU *c) { HLE_ENTER("SetLocalTime", 1); RET(1, 0); }

void hle_kernel32_Sleep(CPU *c)
{
    HLE_ENTER("Sleep", 1);
    u32 ms = ARG(0);
    // WinDrv UWindowsViewport::TryRenderDevice ждёт 0,5 с после смены видеорежима (монитор на ПК) — нам незачем
    if (rd32(g_mem, c->esp) == 0x11106a85u)
        ms = 0;
    if (ms == 0)
        sched_yield();
    else if (ms != 0xFFFFFFFFu)
        usleep(ms * 1000u);
    else
        rt_trap(c, rd32(g_mem, c->esp), "Sleep(INFINITE)");
    RET(1, 0);
}

// Форматирование даты/времени Win32 (локализация EN-US 0x409): безопасная запись без переполнения гостевого буфера.
typedef struct {
    char buf[256];
    size_t len;
} dt_buf;

static void dt_ch(dt_buf *b, char ch)
{
    if (b->len + 1 < sizeof b->buf)
        b->buf[b->len++] = ch;
}

static void dt_str(dt_buf *b, const char *s)
{
    for (; *s; s++)
        dt_ch(b, *s);
}

static void dt_num(dt_buf *b, int v, int width)
{
    char tmp[32];
    if (width == 2)
        snprintf(tmp, sizeof tmp, "%02d", v);
    else if (width == 4)
        snprintf(tmp, sizeof tmp, "%04d", v);
    else
        snprintf(tmp, sizeof tmp, "%d", v);
    dt_str(b, tmp);
}

static int guest_range_ok(u32 addr, u32 size)
{
    return addr >= 0x10000u && (u64)addr + size <= RT_WINDOW;
}

static int days_in_month(int y, int m)
{
    static const u8 dm[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m < 1 || m > 12)
        return 0;
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0))
        return 29;
    return dm[m - 1];
}

// День недели (0 — воскресенье .. 6 — суббота) по алгоритму Сакамото; Win32 GetDateFormat игнорирует lpDate->wDayOfWeek.
static int gregorian_wday(int y, int m, int d)
{
    static const int t[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3)
        y--;
    int w = (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
    return w < 0 ? w + 7 : w;
}

static const char *dt_quote(dt_buf *b, const char *p)
{
    p++;  // пропустить открывающую кавычку '\''
    if (*p == '\'') {
        dt_ch(b, '\'');
        return p + 1;
    }
    while (*p) {
        if (*p == '\'') {
            if (p[1] == '\'') {
                dt_ch(b, '\'');
                p += 2;
            } else {
                p++;
                break;
            }
        } else {
            dt_ch(b, *p++);
        }
    }
    return p;
}

static u32 dt_finish(const dt_buf *b, u32 dst, s32 cch)
{
    if (cch < 0) {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    u32 need = (u32)b->len + 1;
    if (cch == 0)
        return need;
    if (!guest_range_ok(dst, (u32)cch)) {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if ((u32)cch < need) {
        rt_set_last_error(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    memcpy(g_mem + dst, b->buf, need);
    return need;
}

void hle_kernel32_GetDateFormatA(CPU *c)
{
    HLE_ENTER("GetDateFormatA", 6);
    static const char *const wday_short[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *const wday_long[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    static const char *const mon_short[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    static const char *const mon_long[12] = {"January", "February", "March", "April", "May", "June",
                                             "July", "August", "September", "October", "November", "December"};
    u32 flags = ARG(1), lp_date = ARG(2), lp_fmt = ARG(3), dst = ARG(4);
    s32 cch = (s32)ARG(5);
    int year, mon, day;
    if (lp_date) {
        if (!guest_range_ok(lp_date, 16)) {
            rt_set_last_error(ERROR_INVALID_PARAMETER);
            RET(6, 0);
        }
        year = rd16(g_mem, lp_date + 0);
        mon = rd16(g_mem, lp_date + 2);
        day = rd16(g_mem, lp_date + 6);
        if (mon < 1 || mon > 12 || day < 1 || day > days_in_month(year, mon)) {
            rt_set_last_error(ERROR_INVALID_PARAMETER);
            RET(6, 0);
        }
    } else {
        struct tm tm;
        local_now(&tm, NULL);
        year = tm.tm_year + 1900;
        mon = tm.tm_mon + 1;
        day = tm.tm_mday;
    }
    const char *fmt;
    if (lp_fmt) {
        if (!guest_range_ok(lp_fmt, 1)) {
            rt_set_last_error(ERROR_INVALID_PARAMETER);
            RET(6, 0);
        }
        fmt = hle_cstr(lp_fmt);
    } else if (flags & 0x00000002u) {  // DATE_LONGDATE
        fmt = "dddd, MMMM dd, yyyy";
    } else if (flags & 0x00000008u) {  // DATE_YEARMONTH
        fmt = "MMMM, yyyy";
    } else {                           // DATE_SHORTDATE (по умолчанию)
        fmt = "M/d/yyyy";
    }
    int wday = gregorian_wday(year, mon, day);
    dt_buf b = {.len = 0};
    for (const char *p = fmt; *p;) {
        if (*p == '\'') {
            p = dt_quote(&b, p);
            continue;
        }
        if (*p == 'd' || *p == 'M' || *p == 'y' || *p == 'g') {
            char ch = *p;
            int n = 0;
            while (*p == ch)
                p++, n++;
            if (ch == 'd') {
                if (n == 1) dt_num(&b, day, 1);
                else if (n == 2) dt_num(&b, day, 2);
                else if (n == 3) dt_str(&b, wday_short[wday]);
                else dt_str(&b, wday_long[wday]);
            } else if (ch == 'M') {
                if (n == 1) dt_num(&b, mon, 1);
                else if (n == 2) dt_num(&b, mon, 2);
                else if (n == 3) dt_str(&b, mon_short[mon - 1]);
                else dt_str(&b, mon_long[mon - 1]);
            } else if (ch == 'y') {
                if (n == 1) dt_num(&b, year % 100, 1);
                else if (n == 2) dt_num(&b, year % 100, 2);
                else dt_num(&b, year, 4);
            } else {  // 'g'
                if (n >= 2) dt_str(&b, "A.D.");
                else dt_ch(&b, 'g');
            }
        } else {
            dt_ch(&b, *p++);
        }
    }
    b.buf[b.len] = 0;
    RET(6, dt_finish(&b, dst, cch));
}

void hle_kernel32_GetTimeFormatA(CPU *c)
{
    HLE_ENTER("GetTimeFormatA", 6);
    u32 flags = ARG(1), lp_time = ARG(2), lp_fmt = ARG(3), dst = ARG(4);
    s32 cch = (s32)ARG(5);
    int hour, min, sec;
    if (lp_time) {
        if (!guest_range_ok(lp_time, 16)) {
            rt_set_last_error(ERROR_INVALID_PARAMETER);
            RET(6, 0);
        }
        hour = rd16(g_mem, lp_time + 8);
        min = rd16(g_mem, lp_time + 10);
        sec = rd16(g_mem, lp_time + 12);
        if (hour > 23 || min > 59 || sec > 59) {
            rt_set_last_error(ERROR_INVALID_PARAMETER);
            RET(6, 0);
        }
    } else {
        struct tm tm;
        local_now(&tm, NULL);
        hour = tm.tm_hour;
        min = tm.tm_min;
        sec = tm.tm_sec > 59 ? 59 : tm.tm_sec;
    }
    char def_fmt[32];
    const char *fmt;
    if (lp_fmt) {
        if (!guest_range_ok(lp_fmt, 1)) {
            rt_set_last_error(ERROR_INVALID_PARAMETER);
            RET(6, 0);
        }
        fmt = hle_cstr(lp_fmt);
    } else {
        // TIME_NOMINUTESORSECONDS=1, TIME_NOSECONDS=2, TIME_NOTIMEMARKER=4, TIME_FORCE24HOURFORMAT=8
        int f24 = (flags & 8u) != 0;
        snprintf(def_fmt, sizeof def_fmt, "%s%s%s%s",
                 f24 ? "H" : "h",
                 (flags & 1u) ? "" : ":mm",
                 (flags & 3u) ? "" : ":ss",
                 (f24 || (flags & 4u)) ? "" : " tt");
        fmt = def_fmt;
    }
    int h12 = hour % 12;
    if (h12 == 0)
        h12 = 12;
    dt_buf b = {.len = 0};
    for (const char *p = fmt; *p;) {
        if (*p == '\'') {
            p = dt_quote(&b, p);
            continue;
        }
        if (*p == 'h' || *p == 'H' || *p == 'm' || *p == 's' || *p == 't') {
            char ch = *p;
            int n = 0;
            while (*p == ch)
                p++, n++;
            if (ch == 'h' || ch == 'H') {
                int v = (ch == 'H' || (flags & 8u)) ? hour : h12;
                dt_num(&b, v, n >= 2 ? 2 : 1);
            } else if (ch == 'm') {
                if (flags & 1u) {
                    if (b.len > 0 && b.buf[b.len - 1] == ':')
                        b.len--;
                } else {
                    dt_num(&b, min, n >= 2 ? 2 : 1);
                }
            } else if (ch == 's') {
                if (flags & 3u) {
                    if (b.len > 0 && b.buf[b.len - 1] == ':')
                        b.len--;
                } else {
                    dt_num(&b, sec, n >= 2 ? 2 : 1);
                }
            } else {  // 't'
                if (flags & 4u) {
                    if (b.len > 0 && b.buf[b.len - 1] == ' ')
                        b.len--;
                } else if (n == 1) {
                    dt_ch(&b, hour < 12 ? 'A' : 'P');
                } else {
                    dt_str(&b, hour < 12 ? "AM" : "PM");
                }
            }
        } else {
            dt_ch(&b, *p++);
        }
    }
    b.buf[b.len] = 0;
    RET(6, dt_finish(&b, dst, cch));
}

// ================================================================ отладка, исключения, завершение
static void debug_string(const char *s)
{
    size_t n = strlen(s);
    fprintf(stderr, "[guest] %s%s", s, n && s[n - 1] == '\n' ? "" : "\n");
}

void hle_kernel32_OutputDebugStringA(CPU *c) { HLE_ENTER("OutputDebugStringA", 1); debug_string(hle_cstr(ARG(0))); RET(1, 0); }

void hle_kernel32_OutputDebugStringW(CPU *c)
{
    HLE_ENTER("OutputDebugStringW", 1);
    char buf[4096];
    hle_w2utf8(ARG(0), buf, sizeof buf);
    debug_string(buf);
    RET(1, 0);
}

void hle_kernel32_DebugBreak(CPU *c)
{
    HLE_ENTER("DebugBreak", 0);
    fprintf(stderr, "[hle] DebugBreak from %08x\n", rd32(g_mem, c->esp));
    RET(0, 0);
}

static u32 g_unhandled_filter;
void hle_kernel32_SetUnhandledExceptionFilter(CPU *c)
{
    HLE_ENTER("SetUnhandledExceptionFilter", 1);
    u32 prev = g_unhandled_filter;
    g_unhandled_filter = ARG(0);
    RET(1, prev);
}

void hle_kernel32_UnhandledExceptionFilter(CPU *c) { HLE_ENTER("UnhandledExceptionFilter", 1); RET(1, 1); }

// ---------------------------------------------------------------- исключения (SEH)
// Цепочка SEH — в TEB (fs:[0]): узлы {next, handler}. EXCEPTION_RECORD и CONTEXT кладутся в стек гостя ниже
// вершины, обработчики зовутся как __cdecl handler(rec, frame, ctx, dispatcher_ctx). C++ catch (__CxxFrameHandler
// msvcr70) сюда не возвращается: после фанклета catch он прыгает на продолжение (rt_call -> rt_eh_*, abi.h).
// На время вызова обработчика на вершине цепочки — кадр диспетчера, как у ExecuteHandler2 в ntdll (exc_call).
#define EXC_NONCONTINUABLE 1u
#define EXC_UNWINDING 2u
#define EXC_EXIT_UNWIND 4u
#define EXC_NESTED_CALL 0x10u
#define SEH_END 0xFFFFFFFFu
#define EXC_REC_SIZE 0x50u
#define EXC_CTX_SIZE 0x2CCu

// место под запись и CONTEXT ниже вершины стека; c->esp опускается под них (обработчики — ниже)
static u32 exc_frame(CPU *c)
{
    u32 rec = (c->esp - 0x100u - EXC_REC_SIZE - EXC_CTX_SIZE - 4u) & ~15u;
    memset(g_mem + rec, 0, EXC_REC_SIZE + EXC_CTX_SIZE + 4u);
    c->esp = rec - 16u;
    return rec;
}

// CONTEXT i386 на момент вызова: eip — адрес возврата, esp — после снятия аргументов stdcall
static void exc_context(const u32 regs[8], u32 ctx, u32 eip, u32 esp)
{
    static const u32 off[8] = {0xB0, 0xAC, 0xA8, 0xA4, 0, 0xB4, 0xA0, 0x9C};  // eax ecx edx ebx esp ebp esi edi
    wr32(g_mem, ctx, 0x10007u);  // CONTEXT_FULL
    for (int i = 0; i < 8; i++)
        if (off[i])
            wr32(g_mem, ctx + off[i], regs[i]);
    wr32(g_mem, ctx + 0xB8, eip);
    wr32(g_mem, ctx + 0xBC, 0x1Bu);
    wr32(g_mem, ctx + 0xC0, 0x202u);
    wr32(g_mem, ctx + 0xC4, esp);
    wr32(g_mem, ctx + 0xC8, 0x23u);
}

// Обработчик кадра диспетчера: исключение внутри обработчика — «вложенное» (ExceptionNestedException), в
// *dispatcher_ctx — кадр, чей обработчик исполнялся; при раскрутке — дальше по цепочке.
static void seh_nested_handler(CPU *c)
{
    u32 rec = ARG(0), frame = ARG(1), disp = ARG(3);
    if (rd32(g_mem, rec + 4) & (EXC_UNWINDING | EXC_EXIT_UNWIND))
        RET(0, 1);
    wr32(g_mem, disp, rd32(g_mem, frame + 8));
    RET(0, 2);
}

static u32 g_nested_thunk;
static pthread_once_t g_nested_once = PTHREAD_ONCE_INIT;
static void nested_init(void) { g_nested_thunk = rt_hle_thunk("kernel32.dll", seh_nested_handler); }

// Обработчик узла rn — под кадром диспетчера {next = вершина, вложенный обработчик, rn} на вершине цепочки
// (ExecuteHandler2). msvcr70 на него рассчитывает: _UnwindNestedFrames запоминает вершину, раскручивает до своего
// кадра и вставляет её обратно (pDef->Next = FS:[0]), _JumpToContinuation снимает верхний узел как «кадр NT».
// Без него вершиной был сам кадр функции, он начинал ссылаться на себя, и следующее исключение крутилось по
// циклу в цепочке (загрузка 3_The_Hunt_Part4). После возврата — как ExecuteHandler2: снять верхний узел.
static u32 exc_call(CPU *c, u32 rn, u32 rec, u32 ctx, u32 disp)
{
    u32 args[4] = {rec, rn, ctx, disp}, sp = c->esp, d = sp - 12u;
    pthread_once(&g_nested_once, nested_init);
    wr32(g_mem, d, rd32(g_mem, c->fs_base));
    wr32(g_mem, d + 4, g_nested_thunk);
    wr32(g_mem, d + 8, rn);
    wr32(g_mem, c->fs_base, d);
    c->esp = d;
    u32 r = rt_guest_call(c, rd32(g_mem, rn + 4), 4, args);
    wr32(g_mem, c->fs_base, rd32(g_mem, rd32(g_mem, c->fs_base)));
    c->esp = sp;
    return r;
}

static void save_regs(const CPU *c, u32 r[8]) { memcpy(r, &c->eax, 8 * sizeof(u32)); }
static void load_regs(CPU *c, const u32 r[8]) { memcpy(&c->eax, r, 8 * sizeof(u32)); }

static void exc_unhandled(CPU *c, u32 code, u32 nargs, u32 args)
{
    char buf[1100];
    snprintf(buf, sizeof buf, "unhandled guest exception %08x", code);
    if (code == 0xE06D7363u && nargs >= 3) {  // C++ throw: magic, объект, ThrowInfo
        u32 obj = rd32(g_mem, args + 4), ti = rd32(g_mem, args + 8);
        u32 cta = ti ? rd32(g_mem, ti + 12) : 0, ct = cta ? rd32(g_mem, cta + 4) : 0, td = ct ? rd32(g_mem, ct + 4) : 0;
        const char *type = td ? (const char *)g_mem + td + 8 : "?";
        char text[1024] = "";
        if (!strcmp(type, ".PAG") || !strcmp(type, ".PBG"))
            hle_w2utf8(rd32(g_mem, obj), text, sizeof text);
        snprintf(buf, sizeof buf, "unhandled C++ throw %s %s", type, text);
    }
    rt_trap(c, rd32(g_mem, c->esp), buf);
}

void hle_kernel32_RaiseException(CPU *c)
{
    HLE_ENTER("RaiseException", 4);
    u32 code = ARG(0), flags = ARG(1) & EXC_NONCONTINUABLE, nargs = ARG(2) < 15 ? ARG(2) : 15, args = ARG(3);
    u32 ret = rd32(g_mem, c->esp), regs[8];
    save_regs(c, regs);
    u32 rec = exc_frame(c), ctx = rec + EXC_REC_SIZE, disp = ctx + EXC_CTX_SIZE;
    wr32(g_mem, rec, code);
    wr32(g_mem, rec + 4, flags);
    wr32(g_mem, rec + 12, ret);  // ExceptionAddress
    wr32(g_mem, rec + 16, nargs);
    for (u32 i = 0; i < nargs; i++)
        wr32(g_mem, rec + 20 + 4 * i, rd32(g_mem, args + 4 * i));
    exc_context(regs, ctx, ret, regs[4] + 20);
    if (getenv("SNX_LOG_EH")) {
        fprintf(stderr, "[eh] RaiseException %08x at %08x esp %08x chain", code, ret, regs[4]);
        u32 n = 0;
        for (u32 rn = rd32(g_mem, c->fs_base); rn && rn != SEH_END && n < 24; rn = rd32(g_mem, rn), n++)
            fprintf(stderr, " %08x(%08x)", rn, rd32(g_mem, rn + 4));
        fprintf(stderr, "\n");
    }
    int raising = rt_eh_raising(1);  // продолжение catch уходит longjmp мимо этого кадра
    u32 nested = 0;  // до этого кадра (включительно) обработчики зовутся с EXCEPTION_NESTED_CALL
    for (u32 rn = rd32(g_mem, c->fs_base); rn && rn != SEH_END; rn = rd32(g_mem, rn)) {
        if (rn == nested) {
            wr32(g_mem, rec + 4, rd32(g_mem, rec + 4) & ~EXC_NESTED_CALL);
            nested = 0;
        }
        // 0 — ExceptionContinueExecution; 1 — дальше по цепочке; 2 — исключение внутри обработчика (кадр
        // диспетчера): дальше, с флагом «вложенное» до кадра, чей обработчик исполнялся; 3 — столкновение — дальше
        u32 r = exc_call(c, rn, rec, ctx, disp);
        if (r == 0 && !(flags & EXC_NONCONTINUABLE)) {
            rt_eh_raising(raising);
            load_regs(c, regs);
            RET(4, 0);
        }
        if (r == 2) {
            wr32(g_mem, rec + 4, rd32(g_mem, rec + 4) | EXC_NESTED_CALL);
            if (rd32(g_mem, disp) > nested)
                nested = rd32(g_mem, disp);
        }
    }
    rt_eh_raising(raising);
    load_regs(c, regs);
    exc_unhandled(c, code, nargs, args);
}

// RtlUnwind(TargetFrame, TargetIp, ExceptionRecord, ReturnValue): обработчики узлов выше TargetFrame — с флагом
// раскрутки (деструкторы кадров C++), узлы снимаются; затем обычный возврат (TargetIp у msvcr70 — сразу за вызовом)
void hle_kernel32_RtlUnwind(CPU *c)
{
    HLE_ENTER("RtlUnwind", 4);
    u32 target = ARG(0), given = ARG(2), retval = ARG(3), ret = rd32(g_mem, c->esp), regs[8];
    save_regs(c, regs);
    u32 rec = exc_frame(c), ctx = rec + EXC_REC_SIZE, disp = ctx + EXC_CTX_SIZE;
    u32 flags = EXC_UNWINDING | (target ? 0 : EXC_EXIT_UNWIND);
    if (given) {
        wr32(g_mem, given + 4, rd32(g_mem, given + 4) | flags);
        rec = given;
    } else {
        wr32(g_mem, rec, 0xC0000027u);  // STATUS_UNWIND
        wr32(g_mem, rec + 4, flags);
        wr32(g_mem, rec + 12, ret);
    }
    exc_context(regs, ctx, ret, regs[4] + 20);
    if (getenv("SNX_LOG_EH"))
        fprintf(stderr, "[eh] RtlUnwind to %08x from %08x\n", target, rd32(g_mem, c->fs_base));
    for (u32 rn = rd32(g_mem, c->fs_base); rn && rn != SEH_END && rn != target; rn = rd32(g_mem, c->fs_base)) {
        exc_call(c, rn, rec, ctx, disp);
        wr32(g_mem, c->fs_base, rd32(g_mem, rn));
    }
    load_regs(c, regs);
    RET(4, retval);
}

void hle_kernel32_IsBadReadPtr(CPU *c) { HLE_ENTER("IsBadReadPtr", 2); RET(2, ARG(0) < 0x10000u || (u64)ARG(0) + ARG(1) > RT_WINDOW); }
void hle_kernel32_IsBadWritePtr(CPU *c) { HLE_ENTER("IsBadWritePtr", 2); RET(2, ARG(0) < 0x10000u || (u64)ARG(0) + ARG(1) > RT_WINDOW); }
void hle_kernel32_IsBadCodePtr(CPU *c) { HLE_ENTER("IsBadCodePtr", 1); RET(1, rt_lookup(ARG(0)) == NULL); }

static _Atomic int g_bg_threads;

static void guest_process_cleanup(void)
{
    int bg = atomic_load(&g_bg_threads);
    if (bg > 0)
        fprintf(stderr, "[hle] exiting with %d active background guest thread(s)\n", bg);
    cs_stop();
    openal_shutdown();
    hle_file_flush_all();
    fflush(stdout);
    fflush(stderr);
}

void hle_kernel32_ExitProcess(CPU *c)
{
    HLE_ENTER("ExitProcess", 1);
    u32 code = ARG(0);
    fprintf(stderr, "[hle] ExitProcess(%u)\n", code);
    guest_process_cleanup();
    exit((int)code);
}

void hle_kernel32_TerminateProcess(CPU *c)
{
    HLE_ENTER("TerminateProcess", 2);
    if (ARG(0) == 0xFFFFFFFFu) {
        u32 code = ARG(1);
        fprintf(stderr, "[hle] TerminateProcess(self, %u)\n", code);
        guest_process_cleanup();
        exit((int)code);
    }
    RET(2, 0);
}

void hle_kernel32_GetExitCodeProcess(CPU *c) { HLE_ENTER("GetExitCodeProcess", 2); wr32(g_mem, ARG(1), 259); RET(2, 1); }

void hle_kernel32_FormatMessageA(CPU *c)
{
    HLE_ENTER("FormatMessageA", 7);
    char msg[64];
    snprintf(msg, sizeof msg, "Error %u.\r\n", ARG(2));
    u32 n = (u32)strlen(msg);
    if (ARG(0) & 0x100) {  // FORMAT_MESSAGE_ALLOCATE_BUFFER
        u32 p = heap_alloc(heap_process(), 0, n + 1);
        hle_put_a(p, n + 1, msg);
        wr32(g_mem, ARG(4), p);
        RET(7, n);
    }
    RET(7, hle_put_a(ARG(4), ARG(5), msg));
}

void hle_kernel32_FormatMessageW(CPU *c)
{
    HLE_ENTER("FormatMessageW", 7);
    char msg[64];
    snprintf(msg, sizeof msg, "Error %u.\r\n", ARG(2));
    u32 n = (u32)strlen(msg);
    if (ARG(0) & 0x100) {
        u32 p = heap_alloc(heap_process(), 0, 2 * (n + 1));
        hle_put_w(p, n + 1, msg);
        wr32(g_mem, ARG(4), p);
        RET(7, n);
    }
    RET(7, hle_put_w(ARG(4), ARG(5), msg));
}

static u32 g_next_atom = 0xC000;
void hle_kernel32_GlobalAddAtomW(CPU *c) { HLE_ENTER("GlobalAddAtomW", 1); RET(1, __atomic_fetch_add(&g_next_atom, 1, __ATOMIC_RELAXED)); }
void hle_kernel32_GlobalDeleteAtom(CPU *c) { HLE_ENTER("GlobalDeleteAtom", 1); RET(1, 0); }

// ================================================================ потоки и синхронизация
typedef struct {
    pthread_mutex_t m;
    pthread_cond_t cv;
    int signaled, manual;   // событие
    u32 owner, count;       // мьютекс: владелец (tid) и глубина
} sync_obj;

static sync_obj *sync_new(void)
{
    sync_obj *s = calloc(1, sizeof *s);
    pthread_mutex_init(&s->m, NULL);
    pthread_cond_init(&s->cv, NULL);
    return s;
}

static void *thread_main(void *arg)
{
    GuestThread *t = arg;
    rt_thread_bind(t);
    rt_platform_thread_core(0);  // фоновые потоки гостя: ядро 1 — главный поток гостя, 2 — поток рендера
    jmp_buf jb;
    t->exit_jmp = &jb;
    if (!setjmp(jb))
        t->exit_code = rt_guest_call(&t->cpu, t->start, 1, &t->param);
    sync_obj *s = t->sync;
    pthread_mutex_lock(&s->m);
    t->finished = 1;
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->m);
    atomic_fetch_sub(&g_bg_threads, 1);
    return NULL;
}

static void thread_start(GuestThread *t)
{
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, RT_HOST_STACK_THREAD);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_t th;
    atomic_fetch_add(&g_bg_threads, 1);
    if (pthread_create(&th, &a, thread_main, t) != 0) {
        atomic_fetch_sub(&g_bg_threads, 1);
        rt_trap(&t->cpu, t->start, "pthread_create failed");
    }
    pthread_attr_destroy(&a);
}

void hle_kernel32_CreateThread(CPU *c)
{
    HLE_ENTER("CreateThread", 6);
    GuestThread *t = rt_thread_create(ARG(1));
    if (!t)
        RET(6, 0);
    t->start = ARG(2);
    t->param = ARG(3);
    t->sync = sync_new();
    t->handle = hle_obj_new(OBJ_THREAD, t);
    if (ARG(5))
        wr32(g_mem, ARG(5), t->tid);
    if (ARG(4) & 4)  // CREATE_SUSPENDED
        t->suspended = 1;
    else
        thread_start(t);
    RET(6, t->handle);
}

void hle_kernel32_ResumeThread(CPU *c)
{
    HLE_ENTER("ResumeThread", 1);
    GuestThread *t = hle_obj_get(ARG(0), OBJ_THREAD);
    if (!t)
        RET(1, 0xFFFFFFFFu);
    if (t->suspended) {
        t->suspended = 0;
        thread_start(t);
        RET(1, 1);
    }
    RET(1, 0);
}

void hle_kernel32_ExitThread(CPU *c)
{
    HLE_ENTER("ExitThread", 1);
    GuestThread *t = rt_thread_current();
    t->exit_code = ARG(0);
    if (!t->exit_jmp) {
        u32 code = ARG(0);
        fprintf(stderr, "[hle] ExitThread on the main thread\n");
        guest_process_cleanup();
        exit((int)code);
    }
    longjmp(*(jmp_buf *)t->exit_jmp, 1);
}

void hle_kernel32_SetThreadPriority(CPU *c) { HLE_ENTER("SetThreadPriority", 2); RET(2, 1); }

void hle_kernel32_CreateMutexA(CPU *c)
{
    HLE_ENTER("CreateMutexA", 3);
    sync_obj *s = sync_new();
    if (ARG(1)) {
        s->owner = rt_thread_current()->tid;
        s->count = 1;
    }
    rt_set_last_error(0);  // именованный мьютекс «не существовал» — единственный экземпляр игры
    RET(3, hle_obj_new(OBJ_MUTEX, s));
}

void hle_kernel32_CreateMutexW(CPU *c)
{
    HLE_ENTER("CreateMutexW", 3);
    hle_kernel32_CreateMutexA(c);
}

void hle_kernel32_WaitForSingleObject(CPU *c)
{
    HLE_ENTER("WaitForSingleObject", 2);
    u32 h = ARG(0), ms = ARG(1);
    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);
    u64 ns = (u64)dl.tv_nsec + (u64)(ms == 0xFFFFFFFFu ? 0 : ms) * 1000000ull;
    dl.tv_sec += (time_t)(ns / 1000000000ull);
    dl.tv_nsec = (long)(ns % 1000000000ull);
    int type = hle_obj_type(h);
    u32 r = 0xFFFFFFFFu;  // WAIT_FAILED
    if (type == OBJ_THREAD) {
        GuestThread *t = hle_obj_get(h, OBJ_THREAD);
        sync_obj *s = t->sync;
        pthread_mutex_lock(&s->m);
        int e = 0;
        while (!t->finished && e != ETIMEDOUT)
            e = ms == 0xFFFFFFFFu ? pthread_cond_wait(&s->cv, &s->m) : pthread_cond_timedwait(&s->cv, &s->m, &dl);
        r = t->finished ? 0 : 0x102;
        pthread_mutex_unlock(&s->m);
    } else if (type == OBJ_EVENT || type == OBJ_MUTEX) {
        sync_obj *s = hle_obj_get(h, type);
        u32 tid = rt_thread_current()->tid;
        pthread_mutex_lock(&s->m);
        int e = 0;
        if (type == OBJ_EVENT) {
            while (!s->signaled && e != ETIMEDOUT)
                e = ms == 0xFFFFFFFFu ? pthread_cond_wait(&s->cv, &s->m) : pthread_cond_timedwait(&s->cv, &s->m, &dl);
            r = s->signaled ? 0 : 0x102;
            if (s->signaled && !s->manual)
                s->signaled = 0;
        } else {
            while (s->count && s->owner != tid && e != ETIMEDOUT)
                e = ms == 0xFFFFFFFFu ? pthread_cond_wait(&s->cv, &s->m) : pthread_cond_timedwait(&s->cv, &s->m, &dl);
            if (!s->count || s->owner == tid) {
                s->owner = tid;
                s->count++;
                r = 0;
            } else {
                r = 0x102;
            }
        }
        pthread_mutex_unlock(&s->m);
    } else if (type == OBJ_PROCESS || type == OBJ_FILE) {
        r = 0;
    }
    RET(2, r);
}

void hle_kernel32_DuplicateHandle(CPU *c)
{
    HLE_ENTER("DuplicateHandle", 7);
    u32 h = ARG(1);
    if (h == 0xFFFFFFFEu)  // псевдоописатель текущего потока
        h = rt_thread_current()->handle ? rt_thread_current()->handle : hle_obj_new(OBJ_THREAD, rt_thread_current());
    if (ARG(3))
        wr32(g_mem, ARG(3), h);
    RET(7, 1);
}

void hle_kernel32_CreatePipe(CPU *c) { HLE_ENTER("CreatePipe", 4); hle_todo("CreatePipe"); RET(4, 0); }
void hle_kernel32_PeekNamedPipe(CPU *c) { HLE_ENTER("PeekNamedPipe", 6); RET(6, 0); }
void hle_kernel32_CreateProcessA(CPU *c) { HLE_ENTER("CreateProcessA", 10); hle_todo("CreateProcess"); RET(10, 0); }
void hle_kernel32_CreateProcessW(CPU *c) { HLE_ENTER("CreateProcessW", 10); hle_todo("CreateProcess"); RET(10, 0); }

// ================================================================ консоль (процесс не консольный)
void hle_kernel32_GetConsoleCP(CPU *c) { HLE_ENTER("GetConsoleCP", 0); RET(0, 437); }
void hle_kernel32_GetConsoleOutputCP(CPU *c) { HLE_ENTER("GetConsoleOutputCP", 0); RET(0, 437); }
void hle_kernel32_GetConsoleMode(CPU *c) { HLE_ENTER("GetConsoleMode", 2); RET(2, 0); }
void hle_kernel32_SetConsoleMode(CPU *c) { HLE_ENTER("SetConsoleMode", 2); RET(2, 0); }
void hle_kernel32_SetConsoleCtrlHandler(CPU *c) { HLE_ENTER("SetConsoleCtrlHandler", 2); RET(2, 1); }
void hle_kernel32_GetNumberOfConsoleInputEvents(CPU *c) { HLE_ENTER("GetNumberOfConsoleInputEvents", 2); RET(2, 0); }
void hle_kernel32_PeekConsoleInputA(CPU *c) { HLE_ENTER("PeekConsoleInputA", 4); RET(4, 0); }
void hle_kernel32_ReadConsoleInputA(CPU *c) { HLE_ENTER("ReadConsoleInputA", 4); RET(4, 0); }
void hle_kernel32_ReadConsoleInputW(CPU *c) { HLE_ENTER("ReadConsoleInputW", 4); RET(4, 0); }
void hle_kernel32_ReadConsoleA(CPU *c) { HLE_ENTER("ReadConsoleA", 5); RET(5, 0); }
void hle_kernel32_ReadConsoleW(CPU *c) { HLE_ENTER("ReadConsoleW", 5); RET(5, 0); }
void hle_kernel32_WriteConsoleA(CPU *c) { HLE_ENTER("WriteConsoleA", 5); RET(5, 0); }
void hle_kernel32_WriteConsoleW(CPU *c) { HLE_ENTER("WriteConsoleW", 5); RET(5, 0); }
