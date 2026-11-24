// Синтетический тест куч гостя (runtime/heap.c):
// heap_init/heap_create/heap_alloc/heap_realloc/heap_free/heap_size/heap_destroy для мелких,
// средних и крупных (> BIG) блоков, HEAP_ZERO_MEMORY, стресс-тест разных классов размеров
// (проверяет и текущий, и будущий slab-аллокатор) и многопоточность.
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime/heap.h"
#include "runtime/rt.h"
#include "runtime/vmem.h"

static atomic_int g_fails;

#define CHECK(cond, ...)                                                                                             \
    do {                                                                                                             \
        if (!(cond)) {                                                                                               \
            if (atomic_fetch_add(&g_fails, 1) < 32) {                                                                \
                printf("FAIL (%s:%d): ", __FILE__, __LINE__);                                                        \
                printf(__VA_ARGS__);                                                                                 \
                printf("\n");                                                                                        \
            }                                                                                                        \
        }                                                                                                            \
    } while (0)

static u64 next_rng(u64 *s)
{
    u64 x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *s = x;
    return x;
}

static void fill_pattern(u32 p, u32 size, u8 tag)
{
    if (!size)
        return;
    g_mem[p] = tag;
    if (size > 1)
        g_mem[p + size - 1] = (u8)(tag ^ 0x5Au);
    for (u32 off = 1; off + 1 < size; off += (size > 4096 ? 1021u : 61u))
        g_mem[p + off] = (u8)(tag ^ (u8)(off * 31u));
}

static int check_pattern(u32 p, u32 size, u8 tag)
{
    if (!size)
        return 1;
    if (g_mem[p] != tag)
        return 0;
    if (size > 1 && g_mem[p + size - 1] != (u8)(tag ^ 0x5Au))
        return 0;
    for (u32 off = 1; off + 1 < size; off += (size > 4096 ? 1021u : 61u))
        if (g_mem[p + off] != (u8)(tag ^ (u8)(off * 31u)))
            return 0;
    return 1;
}

static void test_sizes_and_zero_memory(void)
{
    u32 proc = heap_process();
    CHECK(proc != 0, "heap_process вернул 0");
    CHECK(heap_alloc(0x12345678u, 0, 32) == 0, "alloc в несуществующей куче");
    CHECK(heap_destroy(0x12345678u) == 0, "destroy несуществующей кучи");
    CHECK(heap_free(proc, 0) == 1, "heap_free(NULL) должен вернуть 1");
    CHECK(heap_size(proc, 0) == 0xFFFFFFFFu, "heap_size(0)");
    CHECK(heap_size(proc, 0x20000003u) == 0xFFFFFFFFu, "heap_size невыровненного указателя");

    u32 h = heap_create(0);
    CHECK(h != 0 && h != proc, "heap_create вернул %08x", h);

    static const u32 sizes[] = {
        0, 1, 7, 8, 9, 15, 16, 24, 31, 32, 48, 63, 64, 127, 128, 255, 256, 512, 1024,
        1025, 2048, 4096, 8192, 16384, 32768, 65536, 131072, 262144, 262145, 524288, 1048576,
    };
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        u32 sz = sizes[i];
        u32 p = heap_alloc(h, 0, sz);
        CHECK(p != 0 && (p & 7u) == 0, "alloc(%u) -> %08x (выравнивание 8)", sz, p);
        if (!p)
            continue;
        CHECK(heap_size(h, p) == sz, "heap_size(%u) = %u", sz, heap_size(h, p));
        if (sz) {
            memset(g_mem + p, 0xA5, sz);
            CHECK(g_mem[p] == 0xA5 && g_mem[p + sz - 1] == 0xA5, "запись в блок %u", sz);
        }
        CHECK(heap_free(h, p) == 1, "free(%u)", sz);

        // Повторное выделение того же размера с HEAP_ZERO_MEMORY (переиспользование освобождённого слота)
        u32 z = heap_alloc(h, HEAP_ZERO_MEMORY, sz);
        CHECK(z != 0 && (z & 7u) == 0, "alloc zero(%u) -> %08x", sz, z);
        if (z) {
            int nz = 0;
            for (u32 k = 0; k < sz; k++)
                if (g_mem[z + k] != 0) {
                    nz = 1;
                    break;
                }
            CHECK(!nz, "HEAP_ZERO_MEMORY(%u): остались ненулевые байты", sz);
            CHECK(heap_free(h, z) == 1, "free zero(%u)", sz);
        }
    }

    CHECK(heap_destroy(h) == 1, "heap_destroy приватной кучи");
    CHECK(heap_alloc(h, 0, 16) == 0, "alloc после heap_destroy должен вернуть 0");
}

static void test_realloc(void)
{
    u32 h = heap_create(0);
    CHECK(h != 0, "heap_create для realloc");
    CHECK(heap_realloc(h, 0, 0, 32) == 0, "realloc(0) должен вернуть 0");

    // Рост: мелкий -> средний -> крупный (> BIG) с HEAP_ZERO_MEMORY
    u32 p = heap_alloc(h, 0, 17);
    CHECK(p != 0, "alloc(17)");
    for (u32 i = 0; i < 17; i++)
        g_mem[p + i] = (u8)(0x30 + i);

    // IN_PLACE_ONLY на резкий рост должен вернуть 0 и сохранить исходный блок
    CHECK(heap_realloc(h, HEAP_REALLOC_IN_PLACE_ONLY, p, 8192) == 0, "IN_PLACE_ONLY(17->8192) должен отказать");
    CHECK(heap_size(h, p) == 17 && g_mem[p] == 0x30 && g_mem[p + 16] == 0x40, "данные испорчены после отказа realloc");

    // Рост в пределах того же 8-байтного класса (17 -> 24) на месте
    u32 p_same = heap_realloc(h, HEAP_REALLOC_IN_PLACE_ONLY | HEAP_ZERO_MEMORY, p, 24);
    CHECK(p_same == p && heap_size(h, p_same) == 24, "IN_PLACE_ONLY(17->24): %08x vs %08x", p_same, p);
    for (u32 i = 17; i < 24; i++)
        CHECK(g_mem[p_same + i] == 0, "хвост IN_PLACE ZERO [%u]=%02x", i, g_mem[p_same + i]);

    // Рост в средний класс (24 -> 4096)
    u32 p_med = heap_realloc(h, HEAP_ZERO_MEMORY, p_same, 4096);
    CHECK(p_med != 0 && (p_med & 7u) == 0 && heap_size(h, p_med) == 4096, "realloc 24->4096");
    for (u32 i = 0; i < 17; i++)
        CHECK(g_mem[p_med + i] == (u8)(0x30 + i), "сохранение данных 24->4096 [%u]", i);
    for (u32 i = 24; i < 4096; i++)
        if (g_mem[p_med + i] != 0) {
            CHECK(0, "хвост 24->4096 не обнулён на %u", i);
            break;
        }
    g_mem[p_med + 4095] = 0xEE;

    // Рост в крупный блок (> BIG: 4096 -> 400000)
    u32 p_big = heap_realloc(h, HEAP_ZERO_MEMORY, p_med, 400000);
    CHECK(p_big != 0 && (p_big & 7u) == 0 && heap_size(h, p_big) == 400000, "realloc 4096->400000");
    CHECK(g_mem[p_big] == 0x30 && g_mem[p_big + 16] == 0x40 && g_mem[p_big + 4095] == 0xEE,
          "данные после роста в BIG");
    CHECK(g_mem[p_big + 4096] == 0 && g_mem[p_big + 399999] == 0, "нули в хвосте BIG");

    // Уменьшение крупного блока (400000 -> 100)
    u32 p_shr = heap_realloc(h, 0, p_big, 100);
    CHECK(p_shr != 0 && heap_size(h, p_shr) == 100 && g_mem[p_shr] == 0x30 && g_mem[p_shr + 16] == 0x40,
          "уменьшение 400000->100");
    CHECK(heap_free(h, p_shr) == 1, "free уменьшенного блока");
    CHECK(heap_destroy(h) == 1, "destroy после realloc");
}

typedef struct {
    u32 ptr, size;
    u8 tag;
} slot_t;

static u32 pick_size(u64 *rng)
{
    u32 r = (u32)(next_rng(rng) % 100u);
    if (r < 55)
        return 1u + (u32)(next_rng(rng) % 1024u);
    if (r < 88)
        return 1025u + (u32)(next_rng(rng) % 64000u);
    return 65536u + (u32)(next_rng(rng) % 340000u);
}

static void test_stress_and_slab_churn(void)
{
    u32 h = heap_create(0);
    CHECK(h != 0, "heap_create для стресс-теста");
    slot_t slots[256];
    memset(slots, 0, sizeof slots);
    u64 rng = 0x123456789ABCDEF0ull;

    for (u32 step = 0; step < 4000; step++) {
        u32 idx = (u32)(next_rng(&rng) % 256u);
        u32 op = (u32)(next_rng(&rng) % 10u);
        if (!slots[idx].ptr) {
            u32 sz = pick_size(&rng);
            u32 fl = (step & 1u) ? HEAP_ZERO_MEMORY : 0;
            u32 p = heap_alloc(h, fl, sz);
            CHECK(p != 0 && (p & 7u) == 0, "stress alloc(%u) шаг %u", sz, step);
            if (!p)
                continue;
            if (fl && sz)
                CHECK(g_mem[p] == 0 && g_mem[p + sz - 1] == 0, "stress ZERO_MEMORY шаг %u", step);
            CHECK(heap_size(h, p) == sz, "stress heap_size шаг %u", step);
            u8 tag = (u8)((idx ^ step) | 1u);
            fill_pattern(p, sz, tag);
            slots[idx] = (slot_t){p, sz, tag};
        } else if (op < 3) {
            CHECK(check_pattern(slots[idx].ptr, slots[idx].size, slots[idx].tag), "повреждение до realloc шаг %u", step);
            u32 nsz = pick_size(&rng);
            u32 old_sz = slots[idx].size;
            u8 old_first = g_mem[slots[idx].ptr];
            u32 np = heap_realloc(h, HEAP_ZERO_MEMORY, slots[idx].ptr, nsz);
            CHECK(np != 0 && (np & 7u) == 0 && heap_size(h, np) == nsz, "stress realloc %u->%u", old_sz, nsz);
            if (np) {
                CHECK(g_mem[np] == old_first, "stress realloc потерял первый байт");
                if (nsz > old_sz)
                    CHECK(g_mem[np + old_sz] == 0 && g_mem[np + nsz - 1] == 0, "stress realloc хвост не 0");
                u8 tag = (u8)((idx ^ step ^ 0x77u) | 1u);
                fill_pattern(np, nsz, tag);
                slots[idx] = (slot_t){np, nsz, tag};
            }
        } else {
            CHECK(check_pattern(slots[idx].ptr, slots[idx].size, slots[idx].tag), "повреждение перед free шаг %u", step);
            CHECK(heap_free(h, slots[idx].ptr) == 1, "stress free шаг %u", step);
            slots[idx].ptr = 0;
        }
    }
    for (u32 i = 0; i < 256; i++)
        if (slots[i].ptr) {
            CHECK(check_pattern(slots[i].ptr, slots[i].size, slots[i].tag), "финальная проверка слота %u", i);
            CHECK(heap_free(h, slots[i].ptr) == 1, "финальный free слота %u", i);
        }

    // Волны массовых мелких/средних выделений и полных освобождений (проверка возврата/переиспользования слабов)
    enum { NSMALL = 2000, NMED = 400, N4K = 100 };
    u32 *smalls = malloc(NSMALL * sizeof *smalls);
    u32 *meds = malloc(NMED * sizeof *meds);
    u32 *p4k = malloc(N4K * sizeof *p4k);
    for (int wave = 0; wave < 5; wave++) {
        for (int i = 0; i < NSMALL; i++) {
            smalls[i] = heap_alloc(h, 0, 64);
            CHECK(smalls[i] != 0 && (smalls[i] & 7u) == 0, "волна %d small %d", wave, i);
            if (smalls[i])
                wr32(g_mem, smalls[i], (u32)(wave * 10000 + i));
        }
        for (int i = 0; i < NMED; i++) {
            meds[i] = heap_alloc(h, 0, 512);
            CHECK(meds[i] != 0 && (meds[i] & 7u) == 0, "волна %d med %d", wave, i);
            if (meds[i])
                wr32(g_mem, meds[i], (u32)(wave * 20000 + i));
        }
        for (int i = 0; i < N4K; i++) {
            p4k[i] = heap_alloc(h, 0, 4096);
            CHECK(p4k[i] != 0 && (p4k[i] & 7u) == 0, "волна %d 4k %d", wave, i);
            if (p4k[i])
                wr32(g_mem, p4k[i], (u32)(wave * 30000 + i));
        }
        for (int i = 0; i < NSMALL; i++)
            if (smalls[i]) {
                CHECK(rd32(g_mem, smalls[i]) == (u32)(wave * 10000 + i), "целостность волны small");
                CHECK(heap_free(h, smalls[i]) == 1, "free волны small");
            }
        for (int i = 0; i < NMED; i++)
            if (meds[i]) {
                CHECK(rd32(g_mem, meds[i]) == (u32)(wave * 20000 + i), "целостность волны med");
                CHECK(heap_free(h, meds[i]) == 1, "free волны med");
            }
        for (int i = 0; i < N4K; i++)
            if (p4k[i]) {
                CHECK(rd32(g_mem, p4k[i]) == (u32)(wave * 30000 + i), "целостность волны 4k");
                CHECK(heap_free(h, p4k[i]) == 1, "free волны 4k");
            }
    }
    free(smalls);
    free(meds);
    free(p4k);
    CHECK(heap_destroy(h) == 1, "destroy после стресс-теста");
}

typedef struct {
    u32 heap;
    u32 tid;
} mt_arg;

static void *heap_worker(void *arg)
{
    mt_arg *a = arg;
    slot_t s[32];
    memset(s, 0, sizeof s);
    u64 rng = 0x55AA55AA11223344ull ^ ((u64)(a->tid + 1) * 0x9E3779B97F4A7C15ull);
    for (u32 step = 0; step < 300; step++) {
        u32 k = (u32)(next_rng(&rng) % 32u);
        if (!s[k].ptr) {
            u32 sz = (step % 25u == 0) ? (300000u + (u32)(next_rng(&rng) % 40000u)) : (8u + (u32)(next_rng(&rng) % 8192u));
            u32 p = heap_alloc(a->heap, HEAP_ZERO_MEMORY, sz);
            CHECK(p != 0 && (p & 7u) == 0 && heap_size(a->heap, p) == sz, "MT alloc поток %u", a->tid);
            if (!p)
                continue;
            CHECK(g_mem[p] == 0 && g_mem[p + sz - 1] == 0, "MT ZERO поток %u", a->tid);
            u8 tag = (u8)(((a->tid + 1) << 5) | (k & 31u));
            fill_pattern(p, sz, tag);
            s[k] = (slot_t){p, sz, tag};
        } else if (step % 3u == 0) {
            CHECK(check_pattern(s[k].ptr, s[k].size, s[k].tag), "MT проверка до realloc поток %u", a->tid);
            u32 nsz = 16u + (u32)(next_rng(&rng) % 12000u);
            u32 np = heap_realloc(a->heap, HEAP_ZERO_MEMORY, s[k].ptr, nsz);
            CHECK(np != 0 && (np & 7u) == 0 && heap_size(a->heap, np) == nsz, "MT realloc поток %u", a->tid);
            if (np) {
                u8 tag = (u8)(((a->tid + 1) << 5) | ((k + step) & 31u));
                fill_pattern(np, nsz, tag);
                s[k] = (slot_t){np, nsz, tag};
            }
        } else {
            CHECK(check_pattern(s[k].ptr, s[k].size, s[k].tag), "MT проверка перед free поток %u", a->tid);
            CHECK(heap_free(a->heap, s[k].ptr) == 1, "MT free поток %u", a->tid);
            s[k].ptr = 0;
        }
    }
    for (u32 k = 0; k < 32; k++)
        if (s[k].ptr) {
            CHECK(check_pattern(s[k].ptr, s[k].size, s[k].tag), "MT финал поток %u", a->tid);
            CHECK(heap_free(a->heap, s[k].ptr) == 1, "MT финальный free поток %u", a->tid);
        }
    return NULL;
}

int main(void)
{
    if (rt_mem_init() != 0) {
        printf("rt_mem_init failed\n");
        return 2;
    }
    vm_init();
    heap_init();

    test_sizes_and_zero_memory();
    test_realloc();
    test_stress_and_slab_churn();

    u32 mh = heap_create(0);
    CHECK(mh != 0, "heap_create для MT");
    pthread_t th[4];
    mt_arg args[4];
    for (u32 i = 0; i < 4; i++) {
        args[i] = (mt_arg){mh, i};
        if (pthread_create(&th[i], NULL, heap_worker, &args[i]) != 0) {
            printf("pthread_create failed\n");
            return 2;
        }
    }
    for (u32 i = 0; i < 4; i++)
        pthread_join(th[i], NULL);
    CHECK(heap_destroy(mh) == 1, "heap_destroy после MT");

    int fails = atomic_load(&g_fails);
    printf("heap_test: %d failed\n", fails);
    return fails != 0;
}
