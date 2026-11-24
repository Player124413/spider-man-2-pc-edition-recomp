// fist/fisttp сгенерированного кода на месте (FIST/FTRUNC из runtime/gen_rt.h) против рантайма (rt_fist/rt_ftrunc,
// runtime/rt.c): границы диапазонов 16/32/64 бит, половинки (к ближайшему чётному), NaN, бесконечности, все режимы
// округления fldcw. Запуск: build/mac/tests/rt/fist_test
#include <math.h>
#include <stdio.h>

#include "runtime/abi.h"
#include "runtime/gen_rt.h"

static int fails, cases;

static void one(CPU *c, double v)
{
    static const int bits[3] = {16, 32, 64};
    for (int i = 0; i < 3; i++) {
        int b = bits[i];
        s64 want = rt_fist(c, v, b), got = b == 16 ? FIST(c, v, 16) : b == 32 ? FIST(c, v, 32) : FIST(c, v, 64);
        cases++;
        if (want != got && fails++ < 20)
            printf("FIST%d cw=%04x %.17g: want %lld got %lld\n", b, c->fpucw, v, (long long)want, (long long)got);
        want = rt_ftrunc(v, b);
        got = b == 16 ? FTRUNC(v, 16) : b == 32 ? FTRUNC(v, 32) : FTRUNC(v, 64);
        cases++;
        if (want != got && fails++ < 20)
            printf("FTRUNC%d %.17g: want %lld got %lld\n", b, v, (long long)want, (long long)got);
    }
}

int main(void)
{
    static const double edge[] = {0x1p15, 0x1p31, 0x1p63};
    static CPU cpu;
    for (u32 rc = 0; rc < 4; rc++) {
        cpu.fpucw = 0x027F | rc << 10;
        for (int k = 0; k < 3; k++)
            for (int s = -1; s <= 1; s += 2)
                for (int d = -6; d <= 6; d++) {
                    double e = s * edge[k];
                    one(&cpu, e + d * 0.25);
                    one(&cpu, nextafter(e + d * 0.5, INFINITY));
                    one(&cpu, nextafter(e + d * 0.5, -INFINITY));
                    one(&cpu, nextafter(e, d < 0 ? -INFINITY : INFINITY));
                    one(&cpu, e * (1 + d * 0x1p-52));
                }
        for (int i = -40; i <= 40; i++)
            one(&cpu, i * 0.25);
        one(&cpu, -0.0);
        one(&cpu, NAN);
        one(&cpu, -NAN);
        one(&cpu, INFINITY);
        one(&cpu, -INFINITY);
        one(&cpu, 1e300);
        one(&cpu, -1e300);
        one(&cpu, 0x1p-1074);
    }
    printf("fist_test: %d cases, %d failed\n", cases, fails);
    return fails != 0;
}
