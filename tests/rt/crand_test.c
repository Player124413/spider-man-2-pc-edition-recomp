// rand/srand msvcr70 (замена runtime/overrides_crt.c) против последовательности MSVC: начальное зерно 1,
// srand(N), свой генератор у каждого потока (как _holdrand в данных потока CRT). cdecl через стек гостя.
#include <pthread.h>
#include <stdio.h>

#include "runtime/abi.h"
#include "runtime/rt.h"

void ovr_crt_rand(CPU *c);
void ovr_crt_srand(CPU *c);

#define STACK 0x00200000u

static int fails;

static u32 call(CPU *c, void (*fn)(CPU *), int nargs, u32 arg)
{
    c->esp = STACK - 4u * (u32)(nargs + 1);
    wr32(g_mem, c->esp, 0xDEADBEEFu);
    if (nargs)
        wr32(g_mem, c->esp + 4, arg);
    fn(c);
    if (c->esp != STACK - 4u * (u32)nargs)  // cdecl: аргументы снимает вызывающий
        printf("esp after call: %08x\n", c->esp), fails++;
    return c->eax;
}

static void expect(CPU *c, const char *what, const u32 *want, int n)
{
    for (int i = 0; i < n; i++) {
        u32 got = call(c, ovr_crt_rand, 0, 0);
        if (got != want[i] && fails++ < 10)
            printf("%s: rand #%d = %u, want %u\n", what, i, got, want[i]);
    }
}

static const u32 seed1[] = {41, 18467, 6334, 26500, 19169, 15724, 11478, 29358, 26962, 24464};

static void *other(void *arg)
{
    CPU c = {0};
    (void)arg;
    for (int i = 0; i < 10; i++) {
        c.esp = STACK - 0x10000u - 4u;  // своя область стека гостя
        wr32(g_mem, c.esp, 0xDEADBEEFu);
        ovr_crt_rand(&c);
        if (c.eax != seed1[i] && fails++ < 10)
            printf("other thread: rand #%d = %u, want %u\n", i, c.eax, seed1[i]);
    }
    return NULL;
}

int main(void)
{
    if (rt_mem_init() != 0)
        return 2;
    CPU c = {0};
    expect(&c, "seed 1", seed1, 10);
    call(&c, ovr_crt_srand, 1, 1);
    expect(&c, "srand(1)", seed1, 10);
    call(&c, ovr_crt_srand, 1, 12345);
    u32 h = 12345, want[1000];
    for (int i = 0; i < 1000; i++) {
        h = h * 214013u + 2531011u;
        want[i] = (h >> 16) & 0x7FFF;
    }
    expect(&c, "srand(12345)", want, 1000);
    pthread_t t;  // у другого потока — своё состояние, начиная с 1
    pthread_create(&t, NULL, other, NULL);
    pthread_join(t, NULL);
    printf("crand_test: %d failed\n", fails);
    return fails != 0;
}
