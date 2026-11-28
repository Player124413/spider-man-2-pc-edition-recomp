// Процесс гостя: загрузка образов, PEB, потоки с TEB и стеком, DllMain в порядке зависимостей.
#include "runtime/process.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

#include "runtime/heap.h"
#include "runtime/hle/vfs.h"
#include "runtime/rt.h"
#include "runtime/vmem.h"

#define MAX_MODS 32

static rt_module g_mods[MAX_MODS];
static int g_nmods;
static char g_game_root[1024], g_guest_root[260];
static u32 g_cmd_a, g_cmd_w;
static GuestThread *g_threads[RT_MAX_THREADS];
static u32 g_next_tid = 0x104;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local GuestThread *t_cur;

const char *rt_game_root(void) { return g_game_root; }
const char *rt_guest_root(void) { return g_guest_root; }
u32 rt_cmdline_a(void) { return g_cmd_a; }
u32 rt_cmdline_w(void) { return g_cmd_w; }

static u32 guest_str_a(const char *s)
{
    u32 n = (u32)strlen(s) + 1, p = heap_alloc(heap_process(), 0, n);
    memcpy(g_mem + p, s, n);
    return p;
}

static u32 guest_str_w(const char *s)  // latin-1 -> UTF-16
{
    u32 n = (u32)strlen(s) + 1, p = heap_alloc(heap_process(), 0, 2 * n);
    for (u32 i = 0; i < n; i++)
        wr16(g_mem, p + 2 * i, (u8)s[i]);
    return p;
}

int rt_process_init(const rt_module_desc *const *mods, const char *game_root, const char *user_root,
                    const char *guest_root, const char *cmdline)
{
    if (rt_mem_init() != 0) {
        fprintf(stderr, "[process] guest memory init failed\n");
        return -1;
    }
    vm_init();
    heap_init();
    snprintf(g_game_root, sizeof g_game_root, "%s", game_root);
    snprintf(g_guest_root, sizeof g_guest_root, "%s", guest_root);
    vfs_init(game_root, user_root, guest_root);
    char sysdir[1100];
    snprintf(sysdir, sizeof sysdir, "%s/System", game_root);
    for (int i = 0; mods[i]; i++) {
        if (g_nmods == MAX_MODS || rt_load_module(mods[i], sysdir) != 0)
            return -1;
        vm_mark_image(mods[i]->base, mods[i]->size);
        g_mods[g_nmods++] = (rt_module){mods[i], 0, 0};
    }
    rt_module *exe = rt_module_main();
    // PEB (раскладка XP SP2): образ, куча процесса, версия ОС
    memset(g_mem + RT_PEB, 0, 0x1000);
    wr32(g_mem, RT_PEB + 0x08, exe ? exe->desc->base : 0);
    wr32(g_mem, RT_PEB + 0x18, heap_process());
    wr32(g_mem, RT_PEB + 0x64, 1);  // NumberOfProcessors
    wr32(g_mem, RT_PEB + 0xA4, 5);
    wr32(g_mem, RT_PEB + 0xA8, 1);
    wr16(g_mem, RT_PEB + 0xAC, 2600);
    wr32(g_mem, RT_PEB + 0xB0, 2);  // VER_PLATFORM_WIN32_NT
    char cl[2048];
    snprintf(cl, sizeof cl, "\"%s\\System\\%s\"%s%s", guest_root, exe ? exe->desc->name : "webhead.exe",
             cmdline && *cmdline ? " " : "", cmdline ? cmdline : "");
    g_cmd_a = guest_str_a(cl);
    g_cmd_w = guest_str_w(cl);
    GuestThread *t = rt_thread_create(16u << 20);  // главный поток: стек 16 МБ
    if (!t)
        return -1;
    rt_thread_bind(t);
    return 0;
}

rt_module *rt_module_main(void)
{
    for (int i = 0; i < g_nmods; i++)
        if (!g_mods[i].desc->is_dll)
            return &g_mods[i];
    return NULL;
}

rt_module *rt_module_by_base(u32 base)
{
    for (int i = 0; i < g_nmods; i++)
        if (g_mods[i].desc->base == base)
            return &g_mods[i];
    return NULL;
}

static rt_module *module_containing(u32 va)
{
    for (int i = 0; i < g_nmods; i++)
        if (va >= g_mods[i].desc->base && va - g_mods[i].desc->base < g_mods[i].desc->size)
            return &g_mods[i];
    return NULL;
}

// Символ гостевого адреса: модуль!ближайший_экспорт+смещение (для логов и бэктрейсов)
void rt_symbolize(u32 addr, char *out, size_t cap)
{
    rt_module *m = module_containing(addr);
    if (!m) {
        snprintf(out, cap, "%08x", addr);
        return;
    }
    const rt_module_desc *d = m->desc;
    const rt_export_entry *best = NULL;
    for (u32 i = 0; i < d->nnamed; i++)
        if (d->exports[i].addr <= addr && (!best || d->exports[i].addr > best->addr))
            best = &d->exports[i];
    if (best && addr - best->addr < 0x10000)
        snprintf(out, cap, "%08x %s!%s+0x%x", addr, d->name, best->name, addr - best->addr);
    else
        snprintf(out, cap, "%08x %s", addr, d->name);
}

int rt_looks_like_ret(u32 a)
{
    if (!module_containing(a) || a < 7)
        return 0;
    const u8 *p = g_mem + a;
    return p[-5] == 0xE8 || (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10) || (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10)
        || (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10) || (p[-7] == 0xFF && (p[-6] & 0x38) == 0x10);
}

void rt_backtrace(const CPU *c)
{
    char sym[256];
    GuestThread *t = t_cur;
    u32 lo = t ? t->stack_lo : 0x10000, hi = t ? t->stack_hi : RT_WINDOW;
    fprintf(stderr, "  ebp chain:\n");
    u32 ebp = c->ebp;
    for (int n = 0; n < 48 && ebp >= lo && ebp + 8 <= hi && !(ebp & 3); n++) {
        u32 ret = rd32(g_mem, ebp + 4);
        rt_symbolize(ret, sym, sizeof sym);
        fprintf(stderr, "    [%08x] %s\n", ebp, sym);
        u32 next = rd32(g_mem, ebp);
        if (next <= ebp)
            break;
        ebp = next;
    }
    fprintf(stderr, "  stack scan:\n");
    int n = 0;
    for (u32 sp = c->esp; sp + 4 <= hi && sp >= lo && n < 40; sp += 4) {
        u32 v = rd32(g_mem, sp);
        if (rt_looks_like_ret(v)) {
            rt_symbolize(v, sym, sizeof sym);
            fprintf(stderr, "    [%08x] %s\n", sp, sym);
            n++;
        }
    }
}

rt_module *rt_module_by_name(const char *name)
{
    const char *b = name;
    for (const char *p = name; *p; p++)
        if (*p == '\\' || *p == '/')
            b = p + 1;
    size_t n = strlen(b);
    for (int i = 0; i < g_nmods; i++) {
        const char *mn = g_mods[i].desc->name;
        size_t ml = strlen(mn);
        if ((n == ml && !strncasecmp(b, mn, n)) || (n + 4 == ml && !strncasecmp(b, mn, n) && !strcasecmp(mn + n, ".dll")))
            return &g_mods[i];
    }
    return NULL;
}

u32 rt_module_export(const rt_module *m, const char *name, u32 ordinal)
{
    const rt_module_desc *d = m->desc;
    if (name) {
        u32 lo = 0, hi = d->nnamed;
        while (lo < hi) {
            u32 mid = (lo + hi) / 2;
            int r = strcmp(d->exports[mid].name, name);
            if (!r)
                return d->exports[mid].addr;
            if (r < 0)
                lo = mid + 1;
            else
                hi = mid;
        }
        return 0;
    }
    for (u32 i = 0; i < d->nexports; i++)
        if (d->exports[i].ordinal == ordinal)
            return d->exports[i].addr;
    return 0;
}

int rt_module_attach(rt_module *m)
{
    if (m->attached)
        return 0;
    m->attached = 1;
    const rt_module_desc *d = m->desc;
    for (u32 i = 0; i < d->niat; i++) {
        rt_module *dep = d->iat[i].guest_target ? module_containing(d->iat[i].guest_target) : NULL;
        if (dep && dep != m && rt_module_attach(dep) != 0)
            return -1;
    }
    if (!d->is_dll || !d->entry)
        return 0;
    GuestThread *t = rt_thread_current();
    u32 args[3] = {d->base, 1 /* DLL_PROCESS_ATTACH */, 1 /* статическая загрузка */};
    if (getenv("SNX_LOG_LOADER"))
        fprintf(stderr, "[process] DllMain %s (PROCESS_ATTACH)\n", d->name);
    u32 r = rt_guest_call(&t->cpu, d->entry, 3, args);
    if (!r) {
        fprintf(stderr, "[process] DllMain %s returned FALSE\n", d->name);
        return -1;
    }
    return 0;
}

GuestThread *rt_thread_current(void) { return t_cur; }

void rt_thread_bind(GuestThread *t) { t_cur = t; }

GuestThread *rt_thread_create(u32 stack_size)
{
    if (!stack_size)
        stack_size = RT_STACK_DEFAULT;
    stack_size = (stack_size + 0xFFFFu) & ~0xFFFFu;
    pthread_mutex_lock(&g_lock);
    u32 slot = 0;
    while (slot < RT_MAX_THREADS && g_threads[slot])
        slot++;
    GuestThread *t = NULL;
    if (slot < RT_MAX_THREADS && (t = calloc(1, sizeof *t))) {
        g_threads[slot] = t;
        t->tid = g_next_tid;
        g_next_tid += 4;
    }
    pthread_mutex_unlock(&g_lock);
    if (!t)
        return NULL;
    u32 lo = vm_alloc(0, stack_size, VM_MEM_RESERVE | VM_MEM_COMMIT, 0x04);
    if (!lo) {
        rt_thread_release(t);
        return NULL;
    }
    t->stack_lo = lo;
    t->stack_hi = lo + stack_size;
    t->teb = RT_TEB_BASE + slot * 0x1000u;
    u32 teb = t->teb;
    memset(g_mem + teb, 0, 0x1000);
    wr32(g_mem, teb + TEB_SEH, 0xFFFFFFFFu);
    wr32(g_mem, teb + TEB_STACK_BASE, t->stack_hi);
    wr32(g_mem, teb + TEB_STACK_LIMIT, t->stack_lo);
    wr32(g_mem, teb + TEB_SELF, teb);
    wr32(g_mem, teb + TEB_PID, 0x100);
    wr32(g_mem, teb + TEB_TID, t->tid);
    wr32(g_mem, teb + TEB_PEB, RT_PEB);
    t->cpu.fs_base = teb;
    t->cpu.esp = t->stack_hi - 16;
    t->cpu.fpucw = 0x27F;  // Windows: двойная точность, исключения замаскированы
    t->cpu.mxcsr = 0x1F80;
    t->cpu.thread = t;
    return t;
}

void rt_thread_release(GuestThread *t)
{
    pthread_mutex_lock(&g_lock);
    for (u32 i = 0; i < RT_MAX_THREADS; i++)
        if (g_threads[i] == t)
            g_threads[i] = NULL;
    pthread_mutex_unlock(&g_lock);
    if (t->stack_lo)
        vm_free(t->stack_lo, 0, VM_MEM_RELEASE);
    free(t);
}

u32 rt_guest_call(CPU *c, u32 fn, int nargs, const u32 *args)
{
    u32 sp = c->esp;
    for (int i = nargs - 1; i >= 0; i--) {
        c->esp -= 4;
        wr32(g_mem, c->esp, args[i]);
    }
    c->esp -= 4;
    wr32(g_mem, c->esp, RT_RET_SENTINEL);
    int native = 0;
    guest_fn f = rt_lookup_k(fn, &native);
    if (!f)
        rt_trap(c, fn, "host call to unknown guest function");
    else
        rt_invoke(c, f, native);
    c->esp = sp;
    return c->eax;
}

void rt_set_last_error(u32 e)
{
    GuestThread *t = rt_thread_current();
    if (t)
        wr32(g_mem, t->teb + TEB_LAST_ERROR, e);
}

u32 rt_get_last_error(void)
{
    GuestThread *t = rt_thread_current();
    return t ? rd32(g_mem, t->teb + TEB_LAST_ERROR) : 0;
}
