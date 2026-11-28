// Виртуальная память гостя (VirtualAlloc и т. п.). Карта — recomp/addrmap.json: выделения без адреса идут
// в «низкую» арену (стеки, служебное) и в арену кучи; образы модулей — отдельные области MEM_IMAGE.
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "runtime/vmem.h"

#include <pthread.h>
#if defined(__APPLE__) || defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "runtime/rt.h"

#define GRAN 0x10000u
#define PAGE 0x1000u
#define NGRAN (RT_WINDOW / GRAN)
#define NPAGE (RT_WINDOW / PAGE)

static const u32 g_arenas[][2] = {{0x01000000u, 0x10000000u}, {0x20000000u, RT_WINDOW}};

enum { G_SYSTEM = 0, G_FREE, G_ALLOC, G_IMAGE };
static u8 g_kind[NGRAN];
static u32 g_owner[NGRAN];  // гранула начала выделения/образа
static u32 g_size[NGRAN];   // размер выделения в байтах (в грануле начала)
static u32 g_prot[NGRAN];   // защита при выделении (в грануле начала)
static u8 g_commit[NPAGE / 8];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static int committed(u32 page) { return g_commit[page >> 3] >> (page & 7) & 1; }
static void set_commit(u32 page, int on)
{
    if (on)
        g_commit[page >> 3] |= (u8)(1u << (page & 7));
    else
        g_commit[page >> 3] &= (u8)~(1u << (page & 7));
}

void vm_init(void)
{
    memset(g_kind, G_SYSTEM, sizeof g_kind);
    memset(g_commit, 0, sizeof g_commit);
    for (unsigned i = 0; i < sizeof g_arenas / sizeof g_arenas[0]; i++)
        for (u32 g = g_arenas[i][0] / GRAN; g < g_arenas[i][1] / GRAN; g++)
            g_kind[g] = G_FREE;
}

void vm_mark_image(u32 base, u32 size)
{
    pthread_mutex_lock(&g_lock);
    u32 b = base / GRAN, e = (base + size + GRAN - 1) / GRAN;
    for (u32 g = b; g < e; g++) {
        g_kind[g] = G_IMAGE;
        g_owner[g] = b;
    }
    g_size[b] = size;
    pthread_mutex_unlock(&g_lock);
}

void vm_zero(u32 addr, u32 size)
{
    if (!size)
        return;
#if defined(__APPLE__) || defined(__linux__)
    // целые страницы хоста сбрасываем: нули и возврат физических страниц системе
    long p = sysconf(_SC_PAGESIZE);
    size_t pg = p > 0 ? (size_t)p : 4096u;
    if (((uintptr_t)g_mem & (pg - 1)) == 0) {
        size_t lo = ((size_t)addr + pg - 1) & ~(pg - 1);
        size_t hi = ((size_t)addr + size) & ~(pg - 1);
        if (hi > lo) {
            int done = 0;
#if defined(__linux__)
            // на Linux MADV_DONTNEED для анонимного отображения сразу отдаёт нули и не дробит VMA
            done = madvise(g_mem + lo, hi - lo, MADV_DONTNEED) == 0;
            if (!done)
                done = mmap(g_mem + lo, hi - lo, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1,
                            0) != MAP_FAILED;
#else
            done = mmap(g_mem + lo, hi - lo, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) !=
                   MAP_FAILED;
#endif
            if (done) {
                memset(g_mem + addr, 0, lo - addr);
                memset(g_mem + hi, 0, (size_t)addr + size - hi);
                return;
            }
        }
    }
#endif
    memset(g_mem + addr, 0, size);
}

static u32 find_free(u32 n)
{
    for (unsigned i = 0; i < sizeof g_arenas / sizeof g_arenas[0]; i++) {
        u32 run = 0;
        for (u32 g = g_arenas[i][0] / GRAN; g < g_arenas[i][1] / GRAN; g++) {
            run = g_kind[g] == G_FREE ? run + 1 : 0;
            if (run == n)
                return g - n + 1;
        }
    }
    return 0;
}

u32 vm_alloc(u32 addr, u32 size, u32 type, u32 protect)
{
    if (!size || (u64)addr + size > RT_WINDOW || !(type & (VM_MEM_COMMIT | VM_MEM_RESERVE)))
        return 0;
    pthread_mutex_lock(&g_lock);
    u32 ret = 0;
    int reserve = (type & VM_MEM_RESERVE) || !addr;
    if (reserve) {
        u32 b, n;
        if (!addr) {
            n = (size + GRAN - 1) / GRAN;
            b = find_free(n);
        } else {
            b = addr / GRAN;
            n = (addr + size + GRAN - 1) / GRAN - b;
            for (u32 g = b; g < b + n; g++)
                if (g_kind[g] != G_FREE)
                    b = 0;
        }
        if (!b)
            goto out;
        for (u32 g = b; g < b + n; g++) {
            g_kind[g] = G_ALLOC;
            g_owner[g] = b;
        }
        g_size[b] = n * GRAN;
        g_prot[b] = protect;
        ret = b * GRAN;
        if (!addr)
            addr = ret;
    }
    if (type & VM_MEM_COMMIT) {
        u32 lo = addr & ~(PAGE - 1), hi = (addr + size + PAGE - 1) & ~(PAGE - 1);
        for (u32 p = lo; p < hi; p += PAGE)
            if (g_kind[p / GRAN] != G_ALLOC) {
                ret = 0;
                goto out;
            }
        for (u32 p = lo; p < hi; p += PAGE)
            set_commit(p / PAGE, 1);  // освобождённое обнулено в vm_free, свежее — нулевое
        if (!reserve)
            ret = lo;
    }
out:
    pthread_mutex_unlock(&g_lock);
    return ret;
}

int vm_free(u32 addr, u32 size, u32 type)
{
    if (addr >= RT_WINDOW)
        return 0;
    pthread_mutex_lock(&g_lock);
    int ok = 0;
    u32 g = addr / GRAN;
    if (g_kind[g] != G_ALLOC)
        goto out;
    u32 b = g_owner[g], base = b * GRAN, end = base + g_size[b];
    if (type & VM_MEM_RELEASE) {
        if (size || addr != base)
            goto out;
        for (u32 p = base; p < end; p += PAGE)
            if (committed(p / PAGE)) {
                u32 q = p;
                while (q < end && committed(q / PAGE)) {
                    set_commit(q / PAGE, 0);
                    q += PAGE;
                }
                vm_zero(p, q - p);
                p = q;
            }
        for (u32 k = b; k < end / GRAN; k++)
            g_kind[k] = G_FREE;
        ok = 1;
    } else if (type & VM_MEM_DECOMMIT) {
        if (size && (u64)addr + size > end)
            goto out;
        u32 lo = size ? addr & ~(PAGE - 1) : base, hi = size ? (addr + size + PAGE - 1) & ~(PAGE - 1) : end;
        if (hi > end)
            goto out;
        for (u32 p = lo; p < hi; p += PAGE)
            if (committed(p / PAGE)) {
                set_commit(p / PAGE, 0);
                vm_zero(p, PAGE);
            }
        ok = 1;
    }
out:
    pthread_mutex_unlock(&g_lock);
    return ok;
}

int vm_query(u32 addr, vm_info *o)
{
    if (addr >= RT_WINDOW)
        return 0;
    pthread_mutex_lock(&g_lock);
    memset(o, 0, sizeof *o);
    u32 g = addr / GRAN, page = addr & ~(PAGE - 1);
    o->base = page;
    if (g_kind[g] == G_ALLOC) {
        u32 b = g_owner[g], end = b * GRAN + g_size[b];
        int c = committed(page / PAGE);
        u32 e = page;
        while (e < end && committed(e / PAGE) == c)
            e += PAGE;
        o->alloc_base = b * GRAN;
        o->alloc_protect = g_prot[b];
        o->size = e - page;
        o->state = c ? VM_MEM_COMMIT : VM_MEM_RESERVE;
        o->protect = c ? g_prot[b] : 0;
        o->type = VM_MEM_PRIVATE;
    } else if (g_kind[g] == G_IMAGE) {
        u32 b = g_owner[g], end = b * GRAN + g_size[b];
        o->alloc_base = b * GRAN;
        o->alloc_protect = 0x80;  // PAGE_EXECUTE_WRITECOPY
        o->size = (end > page ? end - page : PAGE);
        o->state = VM_MEM_COMMIT;
        o->protect = 0x04;  // PAGE_READWRITE (защита не моделируется)
        o->type = VM_MEM_IMAGE;
    } else {
        u32 e = g + 1;
        while (e < NGRAN && g_kind[e] == g_kind[g])
            e++;
        o->size = e * GRAN - page;
        if (g_kind[g] == G_FREE) {
            o->state = VM_MEM_FREE;
            o->protect = 0x01;  // PAGE_NOACCESS
        } else {  // служебная память рантайма (TEB/PEB, трамплины): видна как зафиксированная
            o->alloc_base = g * GRAN;
            o->alloc_protect = o->protect = 0x04;
            o->state = VM_MEM_COMMIT;
            o->type = VM_MEM_PRIVATE;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return 1;
}
