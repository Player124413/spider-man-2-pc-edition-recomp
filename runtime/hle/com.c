// HLE COM: гостевые объекты с vtable из трамплинов и таблица хостовых данных.
#include <pthread.h>

#include "runtime/heap.h"
#include "runtime/hle/com.h"

#define MAX_COM 8192

typedef struct {
    u32 guest, refs;
    u8 st;  // 1 — строго однопоточный объект (D3D8): счётчик без атомиков
    void *host;
    void (*destroy)(void *);
} com_entry;

static com_entry g_com[MAX_COM];
static pthread_mutex_t g_com_lock = PTHREAD_MUTEX_INITIALIZER;

// ---------------------------------------------------------------- ловушки нереализованных методов
// Трамплин не знает, какой метод вызван, поэтому у каждого слота свой обработчик (до 128 методов на vtable).
#define MAX_SLOTS 128
#define MAX_VTBL 64
static const char *g_vt_name[MAX_VTBL];
static u32 g_vt_addr[MAX_VTBL];
static int g_nvt;

static void unimpl(CPU *c, int slot)
{
    u32 vt = rd32(g_mem, ARG(0));
    const char *name = "?";
    for (int i = 0; i < g_nvt; i++)
        if (g_vt_addr[i] == vt)
            name = g_vt_name[i];
    char msg[128];
    snprintf(msg, sizeof msg, "COM method %s[%d] (vtable +0x%x) not implemented", name, slot, slot * 4);
    rt_trap(c, rd32(g_mem, c->esp), msg);
}

#define U(n, k) static void unimpl_##n##_##k(CPU *c) { unimpl(c, (n) * 8 + (k)); }
#define U8(n) U(n, 0) U(n, 1) U(n, 2) U(n, 3) U(n, 4) U(n, 5) U(n, 6) U(n, 7)
U8(0) U8(1) U8(2) U8(3) U8(4) U8(5) U8(6) U8(7) U8(8) U8(9) U8(10) U8(11) U8(12) U8(13) U8(14) U8(15)
#undef U
#undef U8
#define R(n, k) unimpl_##n##_##k,
#define R8(n) R(n, 0) R(n, 1) R(n, 2) R(n, 3) R(n, 4) R(n, 5) R(n, 6) R(n, 7)
static const guest_fn g_unimpl[MAX_SLOTS] = {R8(0) R8(1) R8(2) R8(3) R8(4) R8(5) R8(6) R8(7) R8(8) R8(9) R8(10)
                                                 R8(11) R8(12) R8(13) R8(14) R8(15)};
#undef R
#undef R8

u32 com_vtable(const char *name, const guest_fn *methods, int n)
{
    pthread_mutex_lock(&g_com_lock);
    u32 vt = heap_alloc(heap_process(), 0, 4u * (u32)n);
    for (int i = 0; i < n; i++)
        wr32(g_mem, vt + 4u * (u32)i, rt_hle_thunk(name, methods[i] ? methods[i] : g_unimpl[i < MAX_SLOTS ? i : 0]));
    if (g_nvt < MAX_VTBL) {
        g_vt_name[g_nvt] = name;
        g_vt_addr[g_nvt++] = vt;
    }
    pthread_mutex_unlock(&g_com_lock);
    return vt;
}

// ---------------------------------------------------------------- объекты
static com_entry *entry(u32 obj)
{
    if (!obj || obj >= RT_WINDOW - 8)
        return NULL;
    u32 i = rd32(g_mem, obj + 4);
    return i < MAX_COM && g_com[i].guest == obj ? &g_com[i] : NULL;
}

static u32 com_create_impl(u32 vtbl, void *host, void (*destroy)(void *), u8 st)
{
    u32 obj = heap_alloc(heap_process(), HEAP_ZERO_MEMORY, 16);
    if (!obj)
        return 0;
    pthread_mutex_lock(&g_com_lock);
    for (u32 i = 1; i < MAX_COM; i++)
        if (!g_com[i].guest) {
            g_com[i] = (com_entry){obj, 1, st, host, destroy};
            wr32(g_mem, obj, vtbl);
            wr32(g_mem, obj + 4, i);
            pthread_mutex_unlock(&g_com_lock);
            return obj;
        }
    pthread_mutex_unlock(&g_com_lock);
    heap_free(heap_process(), obj);
    hle_todo("COM: out of object slots");
    return 0;
}

u32 com_create(u32 vtbl, void *host, void (*destroy)(void *)) { return com_create_impl(vtbl, host, destroy, 0); }

u32 com_create_st(u32 vtbl, void *host, void (*destroy)(void *)) { return com_create_impl(vtbl, host, destroy, 1); }

void *com_host(u32 obj)
{
    com_entry *e = entry(obj);
    return e ? e->host : NULL;
}

u32 com_addref(u32 obj)
{
    com_entry *e = entry(obj);
    if (!e)
        return 0;
    return e->st ? ++e->refs : __atomic_add_fetch(&e->refs, 1, __ATOMIC_ACQ_REL);
}

u32 com_release(u32 obj)
{
    com_entry *e = entry(obj);
    if (!e)
        return 0;
    u32 r = e->st ? --e->refs : __atomic_sub_fetch(&e->refs, 1, __ATOMIC_ACQ_REL);
    if (r == 0) {
        void *host = e->host;
        void (*destroy)(void *) = e->destroy;
        pthread_mutex_lock(&g_com_lock);
        *e = (com_entry){0};
        pthread_mutex_unlock(&g_com_lock);
        wr32(g_mem, obj, 0);
        heap_free(heap_process(), obj);
        if (destroy)
            destroy(host);
    }
    return r;
}

void com_AddRef(CPU *c) { RET(1, com_addref(ARG(0))); }
void com_Release(CPU *c) { RET(1, com_release(ARG(0))); }

int com_iid_eq(u32 g, u32 d1, u16 d2, u16 d3, u64 d4_be)
{
    if (!g || rd32(g_mem, g) != d1 || rd16(g_mem, g + 4) != d2 || rd16(g_mem, g + 6) != d3)
        return 0;
    for (int i = 0; i < 8; i++)
        if (g_mem[g + 8 + (u32)i] != (u8)(d4_be >> (56 - 8 * i)))
            return 0;
    return 1;
}

void com_iid_str(u32 g, char *out, size_t cap)
{
    if (!g) {
        snprintf(out, cap, "NULL");
        return;
    }
    const u8 *b = g_mem + g + 8;
    snprintf(out, cap, "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", rd32(g_mem, g), rd16(g_mem, g + 4),
             rd16(g_mem, g + 6), b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
}
