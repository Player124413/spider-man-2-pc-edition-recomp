// Кучи гостя (slab-аллокатор поверх vmem). Блок: 8 байт заголовка (запрошенный размер, метка с классом)
// + полезная часть, выровнено на 8 (как HeapAlloc в Win32). Мелкие и средние блоки (<= BIG) живут в slab-страницах
// по 64 КБ (равных грануле vm_alloc = 0x10000), по одному классу размера на slab; пустой slab возвращается в vm_free
// с гистерезисом в 1 кешированный пустой slab на класс. Блоки > BIG выделяются и освобождаются напрямую через vm_alloc/vm_free.
#include "runtime/heap.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#include "runtime/rt.h"
#include "runtime/vmem.h"

#define SLAB_SIZE 0x10000u  // 64 КБ = гранула vm_alloc
#define SLAB_SHIFT 16u
#define NGRAN (RT_WINDOW / SLAB_SIZE)
#define BIG 0x8000u         // 32 КБ: блоки больше — отдельным выделением vm_alloc/vm_free
#define HDR 8u
#define TAG 0x48500000u
#define TAG_MASK 0xFFF00000u
#define F_USED 0x80000u
#define F_BIG 0x40000u
#define NCLASS 160
#define MAX_HEAPS 64

enum { SLAB_NONE = 0, SLAB_PARTIAL, SLAB_FULL, SLAB_EMPTY };

typedef struct slab_t {
    struct slab_t *prev, *next;  // двусвязный список в классе кучи (partial или full)
    u32 base;                    // гостевой адрес начала slab-страницы (кратен SLAB_SIZE)
    u32 free_head;               // локальный гостевой список свободных блоков (0 — пуст)
    u32 bump;                    // гостевой адрес начала неразмеченной части slab-страницы
    u16 cls;                     // индекс класса размера
    u16 live;                    // число живых блоков
    u16 max_blocks;              // вместимость slab-страницы в блоках этого класса
    u8 heap_idx;                 // индекс кучи-владельца в g_heaps
    u8 state;                    // SLAB_NONE / SLAB_PARTIAL / SLAB_FULL / SLAB_EMPTY
} slab_t;

typedef struct {
    u32 handle;
    slab_t *partial[NCLASS];  // слабы со свободными и живыми блоками (0 < live < max_blocks)
    slab_t *full[NCLASS];     // полностью занятые слабы (live == max_blocks)
    slab_t *empty[NCLASS];    // гистерезис: до одного пустого слаба (live == 0) на класс
    u32 *bigs;                // крупные блоки (> BIG) для проверки и HeapDestroy
    u32 nbigs, cap_bigs;
} heap_t;

static heap_t g_heaps[MAX_HEAPS];
static slab_t g_slabs[NGRAN];  // метаданные slab-страниц на хосте по индексу гранулы (addr >> 16)
static u32 g_class_cap[NCLASS];
static u32 g_nclass;
static u32 g_process_heap;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void build_classes(void)
{
    u32 n = 0;
    for (u32 s = 8; s <= 1024; s += 8)
        g_class_cap[n++] = s;
    for (u32 p = 1024; p < BIG; p *= 2)
        for (u32 k = 1; k <= 4; k++)
            g_class_cap[n++] = p + p / 4 * k;
    g_nclass = n;
}

static u32 class_of(u32 need)
{
    if (need <= 1024)
        return (need + 7u) / 8u - 1u;
    u32 lo = 128, hi = g_nclass - 1;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (g_class_cap[mid] >= need)
            hi = mid;
        else
            lo = mid + 1;
    }
    return lo;
}

static heap_t *find(u32 handle)
{
    if (!handle)
        return NULL;
    for (int i = 0; i < MAX_HEAPS; i++)
        if (g_heaps[i].handle == handle)
            return &g_heaps[i];
    return NULL;
}

static void slab_push(slab_t **head, slab_t *s)
{
    s->prev = NULL;
    s->next = *head;
    if (*head)
        (*head)->prev = s;
    *head = s;
}

static void slab_remove(slab_t **head, slab_t *s)
{
    if (s->prev)
        s->prev->next = s->next;
    else
        *head = s->next;
    if (s->next)
        s->next->prev = s->prev;
    s->prev = s->next = NULL;
}

static void release_slab(slab_t *s)
{
    u32 base = s->base;
    memset(s, 0, sizeof *s);
    vm_free(base, 0, VM_MEM_RELEASE);
}

static void release_list(slab_t *s)
{
    while (s) {
        slab_t *next = s->next;
        release_slab(s);
        s = next;
    }
}

static int track_big(heap_t *h, u32 a)
{
    if (h->nbigs == h->cap_bigs) {
        u32 ncap = h->cap_bigs ? h->cap_bigs * 2 : 16;
        u32 *nb = realloc(h->bigs, ncap * sizeof *nb);
        if (!nb)
            return 0;
        h->bigs = nb;
        h->cap_bigs = ncap;
    }
    h->bigs[h->nbigs++] = a;
    return 1;
}

static int has_big(const heap_t *h, u32 a)
{
    for (u32 i = 0; i < h->nbigs; i++)
        if (h->bigs[i] == a)
            return 1;
    return 0;
}

static int untrack_big(heap_t *h, u32 a)
{
    for (u32 i = 0; i < h->nbigs; i++)
        if (h->bigs[i] == a) {
            h->bigs[i] = h->bigs[--h->nbigs];
            return 1;
        }
    return 0;
}

static int valid_block_in_heap(const heap_t *h, u32 p)
{
    if (p < 0x10000u + HDR || p >= RT_WINDOW || (p & 7u))
        return 0;
    u32 t = rd32(g_mem, p - 4);
    if ((t & TAG_MASK) != TAG || !(t & F_USED))
        return 0;
    if (t & F_BIG)
        return ((p - HDR) & (SLAB_SIZE - 1)) == 0 && has_big(h, p - HDR);
    u32 c = t & 0xFFFFu;
    if (c >= g_nclass)
        return 0;
    const slab_t *s = &g_slabs[p >> SLAB_SHIFT];
    if (s->state == SLAB_NONE || s->cls != c || s->heap_idx != (u8)(h - g_heaps))
        return 0;
    u32 off = p - s->base;
    u32 stride = HDR + g_class_cap[c];
    return off >= HDR && (off - HDR) < (s->bump - s->base) && (off - HDR) % stride == 0;
}

static u32 capacity(u32 p)
{
    u32 t = rd32(g_mem, p - 4);
    if (t & F_BIG) {
        vm_info vi;
        if (!vm_query(p - HDR, &vi) || vi.alloc_base + vi.size <= p)
            return 0;
        return vi.alloc_base + vi.size - p;
    }
    return g_class_cap[t & 0xFFFFu];
}

static u32 alloc_locked(heap_t *h, u32 flags, u32 size)
{
    if (size > RT_WINDOW - HDR)
        return 0;
    u32 need = size ? (size + 7u) & ~7u : 8u;
    if (need > BIG) {
        u32 a = vm_alloc(0, need + HDR, VM_MEM_RESERVE | VM_MEM_COMMIT, 0x04);
        if (!a)
            return 0;
        if (!track_big(h, a)) {
            vm_free(a, 0, VM_MEM_RELEASE);
            return 0;
        }
        wr32(g_mem, a, size);
        wr32(g_mem, a + 4, TAG | F_USED | F_BIG);
        return a + HDR;  // свежая память vm_alloc уже нулевая
    }
    u32 c = class_of(need), cap = g_class_cap[c], stride = HDR + cap;
    slab_t *s = h->partial[c];
    if (!s) {
        if (h->empty[c]) {
            s = h->empty[c];
            h->empty[c] = NULL;
            s->state = SLAB_NONE;
        } else {
            u32 a = vm_alloc(0, SLAB_SIZE, VM_MEM_RESERVE | VM_MEM_COMMIT, 0x04);
            if (!a)
                return 0;
            s = &g_slabs[a >> SLAB_SHIFT];
            s->prev = s->next = NULL;
            s->base = a;
            s->free_head = 0;
            s->bump = a;
            s->cls = (u16)c;
            s->live = 0;
            s->max_blocks = (u16)(SLAB_SIZE / stride);
            s->heap_idx = (u8)(h - g_heaps);
            s->state = SLAB_NONE;
        }
    }
    u32 p;
    if (s->free_head) {
        p = s->free_head;
        s->free_head = rd32(g_mem, p);
    } else {
        p = s->bump + HDR;
        s->bump += stride;
    }
    s->live++;
    if (s->live == s->max_blocks) {
        if (s->state == SLAB_PARTIAL)
            slab_remove(&h->partial[c], s);
        slab_push(&h->full[c], s);
        s->state = SLAB_FULL;
    } else if (s->state != SLAB_PARTIAL) {
        slab_push(&h->partial[c], s);
        s->state = SLAB_PARTIAL;
    }
    wr32(g_mem, p - HDR, size);
    wr32(g_mem, p - 4, TAG | F_USED | c);
    if (flags & HEAP_ZERO_MEMORY)
        memset(g_mem + p, 0, cap);
    return p;
}

static int free_locked(heap_t *h, u32 p)
{
    if (!valid_block_in_heap(h, p))
        return 0;
    u32 t = rd32(g_mem, p - 4);
    if (t & F_BIG) {
        untrack_big(h, p - HDR);
        return vm_free(p - HDR, 0, VM_MEM_RELEASE);
    }
    u32 c = t & 0xFFFFu;
    slab_t *s = &g_slabs[p >> SLAB_SHIFT];
    wr32(g_mem, p - 4, TAG | c);
    wr32(g_mem, p, s->free_head);
    s->free_head = p;
    s->live--;
    if (s->live == 0) {
        if (s->state == SLAB_PARTIAL)
            slab_remove(&h->partial[c], s);
        else if (s->state == SLAB_FULL)
            slab_remove(&h->full[c], s);
        if (!h->empty[c]) {
            // гистерезис: держим 1 пустой slab на класс, чтобы не дёргать vm_alloc/vm_free на границе
            s->free_head = 0;
            s->bump = s->base;
            s->state = SLAB_EMPTY;
            h->empty[c] = s;
        } else {
            release_slab(s);
        }
    } else if (s->state == SLAB_FULL) {
        slab_remove(&h->full[c], s);
        slab_push(&h->partial[c], s);
        s->state = SLAB_PARTIAL;
    }
    return 1;
}

void heap_init(void)
{
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_HEAPS; i++) {
        free(g_heaps[i].bigs);
        memset(&g_heaps[i], 0, sizeof g_heaps[i]);
    }
    memset(g_slabs, 0, sizeof g_slabs);
    build_classes();
    g_process_heap = 0;
    pthread_mutex_unlock(&g_lock);
    g_process_heap = heap_create(0);
}

u32 heap_process(void) { return g_process_heap; }

u32 heap_create(u32 flags)
{
    (void)flags;
    pthread_mutex_lock(&g_lock);
    if (!g_nclass)
        build_classes();
    u32 ret = 0;
    for (int i = 0; i < MAX_HEAPS; i++)
        if (!g_heaps[i].handle) {
            // описатель — гостевой адрес служебной страницы (код CRT сравнивает описатели, но не читает их)
            u32 a = vm_alloc(0, 0x1000u, VM_MEM_RESERVE | VM_MEM_COMMIT, 0x04);
            if (!a)
                break;
            heap_t *h = &g_heaps[i];
            memset(h, 0, sizeof *h);
            h->handle = a;
            ret = a;
            break;
        }
    pthread_mutex_unlock(&g_lock);
    return ret;
}

int heap_destroy(u32 handle)
{
    pthread_mutex_lock(&g_lock);
    heap_t *h = find(handle);
    if (h && handle != g_process_heap) {
        for (u32 c = 0; c < g_nclass; c++) {
            release_list(h->partial[c]);
            release_list(h->full[c]);
            if (h->empty[c])
                release_slab(h->empty[c]);
        }
        for (u32 i = 0; i < h->nbigs; i++)
            vm_free(h->bigs[i], 0, VM_MEM_RELEASE);
        free(h->bigs);
        u32 haddr = h->handle;
        memset(h, 0, sizeof *h);
        vm_free(haddr, 0, VM_MEM_RELEASE);
    }
    pthread_mutex_unlock(&g_lock);
    return h != NULL;
}

u32 heap_alloc(u32 handle, u32 flags, u32 size)
{
    pthread_mutex_lock(&g_lock);
    heap_t *h = find(handle);
    u32 p = h ? alloc_locked(h, flags, size) : 0;
    pthread_mutex_unlock(&g_lock);
    return p;
}

u32 heap_realloc(u32 handle, u32 flags, u32 p, u32 size)
{
    pthread_mutex_lock(&g_lock);
    heap_t *h = find(handle);
    u32 ret = 0;
    if (h && size <= RT_WINDOW - HDR && valid_block_in_heap(h, p)) {
        u32 old = rd32(g_mem, p - HDR), cap = capacity(p);
        if (size <= cap) {
            if ((flags & HEAP_ZERO_MEMORY) && size > old)
                memset(g_mem + p + old, 0, size - old);
            wr32(g_mem, p - HDR, size);
            ret = p;
        } else if (!(flags & HEAP_REALLOC_IN_PLACE_ONLY)) {
            ret = alloc_locked(h, flags & HEAP_ZERO_MEMORY, size);
            if (ret) {
                memcpy(g_mem + ret, g_mem + p, old);
                free_locked(h, p);
            }
        }
    }
    pthread_mutex_unlock(&g_lock);
    return ret;
}

int heap_free(u32 handle, u32 p)
{
    if (!p)
        return 1;
    pthread_mutex_lock(&g_lock);
    heap_t *h = find(handle);
    int ok = h ? free_locked(h, p) : 0;
    pthread_mutex_unlock(&g_lock);
    if (!ok)
        fprintf(stderr, "[heap] HeapFree(%08x, %08x): not a live block\n", handle, p);
    return ok;
}

u32 heap_size(u32 handle, u32 p)
{
    pthread_mutex_lock(&g_lock);
    heap_t *h = find(handle);
    u32 sz = (h && valid_block_in_heap(h, p)) ? rd32(g_mem, p - HDR) : 0xFFFFFFFFu;
    pthread_mutex_unlock(&g_lock);
    return sz;
}
