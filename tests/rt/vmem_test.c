// Синтетический тест виртуальной памяти гостя (runtime/vmem.c):
// vm_init, vm_alloc (RESERVE/COMMIT, фиксированный адрес, коллизии, OOM),
// vm_free (DECOMMIT/RELEASE, обнуление страниц), vm_query, vm_mark_image и многопоточность.
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime/rt.h"
#include "runtime/vmem.h"

#define PAGE 0x1000u
#define GRAN 0x10000u

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

static int is_all_zero(u32 addr, u32 size)
{
    for (u32 i = 0; i < size; i++)
        if (g_mem[addr + i] != 0)
            return 0;
    return 1;
}

static void test_basic_and_query(void)
{
    vm_info vi;
    CHECK(vm_query(RT_WINDOW, &vi) == 0, "vm_query за окном должен вернуть 0");
    CHECK(vm_free(RT_WINDOW, 0, VM_MEM_RELEASE) == 0, "vm_free за окном должен вернуть 0");
    CHECK(vm_alloc(0, 0, VM_MEM_RESERVE | VM_MEM_COMMIT, 0x04) == 0, "vm_alloc(size=0) должен вернуть 0");
    CHECK(vm_alloc(0, PAGE, 0, 0x04) == 0, "vm_alloc(type=0) должен вернуть 0");
    CHECK(vm_alloc(RT_WINDOW - PAGE, 2 * PAGE, VM_MEM_RESERVE, 0x04) == 0, "vm_alloc за окном должен вернуть 0");

    // Служебная область и свободная арена после vm_init
    CHECK(vm_query(0x00010000u, &vi) == 1, "vm_query служебной области");
    CHECK(vi.state == VM_MEM_COMMIT && vi.type == VM_MEM_PRIVATE, "служебная область: state=%x type=%x", vi.state,
          vi.type);
    CHECK(vm_query(0x01000000u, &vi) == 1, "vm_query свободной арены");
    CHECK(vi.state == VM_MEM_FREE && vi.protect == 0x01, "свободная арена: state=%x prot=%x", vi.state, vi.protect);

    // Резервирование 256 КБ (4 гранулы) без фиксации
    u32 base = vm_alloc(0, 4 * GRAN, VM_MEM_RESERVE, 0x04);
    CHECK(base != 0 && (base & (GRAN - 1)) == 0, "RESERVE вернул %08x", base);
    CHECK(vm_query(base, &vi) == 1, "vm_query после RESERVE");
    CHECK(vi.base == base && vi.alloc_base == base && vi.size == 4 * GRAN && vi.state == VM_MEM_RESERVE &&
              vi.protect == 0 && vi.alloc_protect == 0x04 && vi.type == VM_MEM_PRIVATE,
          "неверная информация после RESERVE: base=%08x size=%x state=%x prot=%x", vi.base, vi.size, vi.state,
          vi.protect);

    // Частичная фиксация страниц 2..4 внутри резерва
    u32 caddr = vm_alloc(base + 2 * PAGE, 3 * PAGE, VM_MEM_COMMIT, 0x04);
    CHECK(caddr == base + 2 * PAGE, "частичный COMMIT вернул %08x вместо %08x", caddr, base + 2 * PAGE);
    CHECK(is_all_zero(caddr, 3 * PAGE), "свежезафиксированные страницы не нулевые");
    memset(g_mem + caddr, 0x5A, 3 * PAGE);

    CHECK(vm_query(base, &vi) == 1 && vi.state == VM_MEM_RESERVE && vi.size == 2 * PAGE,
          "начало резерва до COMMIT: state=%x size=%x", vi.state, vi.size);
    CHECK(vm_query(caddr, &vi) == 1 && vi.state == VM_MEM_COMMIT && vi.size == 3 * PAGE && vi.alloc_base == base &&
              vi.protect == 0x04,
          "участок COMMIT: state=%x size=%x", vi.state, vi.size);
    CHECK(vm_query(caddr + 3 * PAGE, &vi) == 1 && vi.state == VM_MEM_RESERVE && vi.size == 4 * GRAN - 5 * PAGE,
          "хвост резерва после COMMIT: state=%x size=%x", vi.state, vi.size);

    // Снятие фиксации средней страницы -> обнуление при повторной фиксации
    CHECK(vm_free(base + 3 * PAGE, PAGE, VM_MEM_DECOMMIT) == 1, "DECOMMIT средней страницы");
    CHECK(vm_query(base + 2 * PAGE, &vi) == 1 && vi.state == VM_MEM_COMMIT && vi.size == PAGE, "левый сосед DECOMMIT");
    CHECK(vm_query(base + 3 * PAGE, &vi) == 1 && vi.state == VM_MEM_RESERVE && vi.size == PAGE, "снятая страница");
    CHECK(vm_query(base + 4 * PAGE, &vi) == 1 && vi.state == VM_MEM_COMMIT && vi.size == PAGE, "правый сосед DECOMMIT");
    CHECK(g_mem[base + 2 * PAGE] == 0x5A && g_mem[base + 4 * PAGE] == 0x5A, "соседние страницы испорчены");
    CHECK(vm_alloc(base + 3 * PAGE, PAGE, VM_MEM_COMMIT, 0x04) == base + 3 * PAGE, "повторный COMMIT");
    CHECK(is_all_zero(base + 3 * PAGE, PAGE), "после DECOMMIT+COMMIT страница не обнулена");

    // Ошибочные аргументы DECOMMIT и RELEASE
    CHECK(vm_free(base + 4 * GRAN - PAGE, 2 * PAGE, VM_MEM_DECOMMIT) == 0, "DECOMMIT за границей должен падать");
    CHECK(vm_free(base, PAGE, VM_MEM_RELEASE) == 0, "RELEASE с size!=0 должен падать");
    CHECK(vm_free(base + GRAN, 0, VM_MEM_RELEASE) == 0, "RELEASE не от начала должен падать");

    // DECOMMIT всей области (size=0) и RELEASE
    CHECK(vm_free(base, 0, VM_MEM_DECOMMIT) == 1, "DECOMMIT всей области");
    CHECK(vm_query(base, &vi) == 1 && vi.state == VM_MEM_RESERVE && vi.size == 4 * GRAN, "после полного DECOMMIT");
    CHECK(vm_alloc(base, PAGE, VM_MEM_COMMIT, 0x04) == base && is_all_zero(base, PAGE), "нули после полного DECOMMIT");
    memset(g_mem + base, 0xCC, PAGE);

    CHECK(vm_free(base, 0, VM_MEM_RELEASE) == 1, "RELEASE области");
    CHECK(vm_query(base, &vi) == 1 && vi.state == VM_MEM_FREE, "область после RELEASE должна быть FREE");
    CHECK(vm_free(base, 0, VM_MEM_RELEASE) == 0, "повторный RELEASE должен вернуть 0");
}

static void test_fixed_collisions_oom_and_image(void)
{
    vm_info vi;
    const u32 fixed = 0x05000000u;
    u32 a = vm_alloc(fixed, 3 * GRAN, VM_MEM_RESERVE | VM_MEM_COMMIT, 0x04);
    CHECK(a == fixed, "фиксированный адрес %08x -> %08x", fixed, a);
    CHECK(is_all_zero(fixed, 3 * GRAN), "фиксированная область не нулевая");
    memset(g_mem + fixed, 0xA5, 3 * GRAN);

    // Коллизии с занятым фиксированным диапазоном
    CHECK(vm_alloc(fixed, GRAN, VM_MEM_RESERVE, 0x04) == 0, "коллизия в начале");
    CHECK(vm_alloc(fixed + GRAN, GRAN, VM_MEM_RESERVE | VM_MEM_COMMIT, 0x04) == 0, "коллизия в середине");
    CHECK(vm_alloc(fixed - GRAN, 2 * GRAN, VM_MEM_RESERVE, 0x04) == 0, "частичное пересечение слева");
    CHECK(vm_alloc(0x06000000u, PAGE, VM_MEM_COMMIT, 0x04) == 0, "COMMIT по незарезервированному адресу");

    // После освобождения повторное выделение по тому же адресу должно быть чистым
    CHECK(vm_free(fixed, 0, VM_MEM_RELEASE) == 1, "RELEASE фиксированной области");
    a = vm_alloc(fixed, 3 * GRAN, VM_MEM_RESERVE | VM_MEM_COMMIT, 0x04);
    CHECK(a == fixed && is_all_zero(fixed, 3 * GRAN), "повторное выделение после RELEASE не обнулено");
    CHECK(vm_free(fixed, 0, VM_MEM_RELEASE) == 1, "финальный RELEASE фиксированной области");

    // OOM: запрос больше любой арены, затем исчерпание большой арены и возврат
    CHECK(vm_alloc(0, 0x30000000u, VM_MEM_RESERVE, 0x04) == 0, "OOM на 768 МБ должен вернуть 0");
    u32 huge = vm_alloc(0, 0x20000000u, VM_MEM_RESERVE, 0x04);
    CHECK(huge == 0x20000000u, "резерв 512 МБ в арене кучи: %08x", huge);
    CHECK(vm_alloc(0, 0x20000000u, VM_MEM_RESERVE, 0x04) == 0, "повторный резерв 512 МБ должен дать OOM");
    CHECK(vm_free(huge, 0, VM_MEM_RELEASE) == 1, "освобождение резерва 512 МБ");
    u32 huge2 = vm_alloc(0, 0x20000000u, VM_MEM_RESERVE, 0x04);
    CHECK(huge2 == 0x20000000u, "резерв 512 МБ снова доступен после RELEASE");
    CHECK(vm_free(huge2, 0, VM_MEM_RELEASE) == 1, "освобождение повторного резерва 512 МБ");

    // Пометка образа модуля
    const u32 img_base = 0x12000000u, img_size = 0x35000u;
    vm_mark_image(img_base, img_size);
    CHECK(vm_query(img_base, &vi) == 1, "vm_query начала образа");
    CHECK(vi.type == VM_MEM_IMAGE && vi.state == VM_MEM_COMMIT && vi.alloc_base == img_base && vi.size == img_size,
          "образ начало: type=%x state=%x alloc_base=%08x size=%x", vi.type, vi.state, vi.alloc_base, vi.size);
    CHECK(vm_query(img_base + 0x12345u, &vi) == 1, "vm_query середины образа");
    CHECK(vi.type == VM_MEM_IMAGE && vi.base == img_base + 0x12000u && vi.alloc_base == img_base &&
              vi.size == img_size - 0x12000u,
          "образ середина: base=%08x size=%x", vi.base, vi.size);
    CHECK(vm_free(img_base, 0, VM_MEM_RELEASE) == 0, "vm_free по MEM_IMAGE должен вернуть 0");
    CHECK(vm_alloc(img_base, GRAN, VM_MEM_RESERVE, 0x04) == 0, "vm_alloc поверх MEM_IMAGE должен вернуть 0");
}

typedef struct {
    u32 tid;
} worker_arg;

static void *vm_worker(void *arg)
{
    u32 tid = ((worker_arg *)arg)->tid;
    u64 rng = 0x9E3779B97F4A7C15ull ^ ((u64)(tid + 1) * 0xBF58476D1CE4E5B9ull);
    for (int i = 0; i < 80; i++) {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        u32 pages = 1u + (u32)(rng % 48u);
        u32 sz = pages * PAGE;
        u32 a = vm_alloc(0, sz, VM_MEM_RESERVE | VM_MEM_COMMIT, 0x04);
        CHECK(a != 0 && (a & (GRAN - 1)) == 0, "поток %u итерация %d: vm_alloc(%u) вернул %08x", tid, i, sz, a);
        if (!a)
            continue;
        CHECK(g_mem[a] == 0 && g_mem[a + sz - 1] == 0, "поток %u: память не нулевая", tid);
        u8 pat = (u8)((tid << 4) | (u32)(i & 15) | 1u);
        for (u32 p = 0; p < pages; p++) {
            g_mem[a + p * PAGE] = pat;
            g_mem[a + p * PAGE + PAGE - 1] = (u8)(pat ^ (u8)p);
        }
        // Снимаем и возвращаем первую страницу, проверяем обнуление и сохранность остальных
        CHECK(vm_free(a, PAGE, VM_MEM_DECOMMIT) == 1, "поток %u: DECOMMIT", tid);
        CHECK(vm_alloc(a, PAGE, VM_MEM_COMMIT, 0x04) == a, "поток %u: повторный COMMIT", tid);
        CHECK(is_all_zero(a, PAGE), "поток %u: первая страница не обнулена", tid);
        for (u32 p = 1; p < pages; p++) {
            CHECK(g_mem[a + p * PAGE] == pat && g_mem[a + p * PAGE + PAGE - 1] == (u8)(pat ^ (u8)p),
                  "поток %u: повреждение данных на странице %u (пересечение выделений?)", tid, p);
        }
        CHECK(vm_free(a, 0, VM_MEM_RELEASE) == 1, "поток %u: RELEASE", tid);
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
    test_basic_and_query();
    test_fixed_collisions_oom_and_image();

    pthread_t th[4];
    worker_arg wa[4];
    for (u32 i = 0; i < 4; i++) {
        wa[i].tid = i;
        if (pthread_create(&th[i], NULL, vm_worker, &wa[i]) != 0) {
            printf("pthread_create failed\n");
            return 2;
        }
    }
    for (u32 i = 0; i < 4; i++)
        pthread_join(th[i], NULL);

    int fails = atomic_load(&g_fails);
    printf("vmem_test: %d failed\n", fails);
    return fails != 0;
}
