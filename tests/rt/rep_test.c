// rep movs/stos сгенерированного кода (REP_MOVS/REP_STOS из runtime/gen_rt.h и регистры после, как в
// recomp/emit.py i_string) против побайтовой модели x86: размеры 1/2/4, df, перекрытия в обе стороны, длина-константа
// (путь на месте) и переменная (рантайм). Запуск: build/mac/tests/rt/rep_test
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime/abi.h"
#include "runtime/gen_rt.h"

#define BUF 2048
static u8 ref[BUF], got[BUF];
static int fails;

static void model_movs(u8 *m, u32 *s, u32 *d, u32 n, int size, int df)
{
    for (u32 i = 0; i < n; i++) {
        u8 t[4];
        memcpy(t, m + *s, (size_t)size);
        memcpy(m + *d, t, (size_t)size);
        *s += df ? 0u - (u32)size : (u32)size;
        *d += df ? 0u - (u32)size : (u32)size;
    }
}

static void model_stos(u8 *m, u32 *d, u32 n, u32 v, int size, int df)
{
    for (u32 i = 0; i < n; i++) {
        memcpy(m + *d, &v, (size_t)size);
        *d += df ? 0u - (u32)size : (u32)size;
    }
}

static void fill(void)
{
    for (int i = 0; i < BUF; i++)
        ref[i] = got[i] = (u8)(i * 37 + 11);
}

static void check(const char *what, u32 n, int size, int df, int delta, u32 s1, u32 s2, u32 d1, u32 d2)
{
    if (memcmp(ref, got, BUF) || s1 != s2 || d1 != d2) {
        printf("FAIL %s n=%u size=%d df=%d delta=%d: esi %x/%x edi %x/%x mem %s\n", what, n, size, df, delta, s1, s2,
               d1, d2, memcmp(ref, got, BUF) ? "differs" : "ok");
        fails++;
    }
}

// регистры после — те же выражения, что пишет emit.py
#define GEN_MOVS(N, SIZE, DF)                                                                                        \
    do {                                                                                                             \
        u32 ecx = (N), gdf = (DF);                                                                                   \
        REP_MOVS(got, esi, edi, ecx, SIZE, gdf);                                                                     \
        esi += (gdf ? 0u - ecx * (u32)(SIZE) : ecx * (u32)(SIZE));                                                   \
        edi += (gdf ? 0u - ecx * (u32)(SIZE) : ecx * (u32)(SIZE));                                                   \
    } while (0)
#define GEN_STOS(N, V, SIZE, DF)                                                                                     \
    do {                                                                                                             \
        u32 ecx = (N), gdf = (DF);                                                                                   \
        REP_STOS(got, edi, ecx, V, SIZE, gdf);                                                                       \
        edi += (gdf ? 0u - ecx * (u32)(SIZE) : ecx * (u32)(SIZE));                                                   \
    } while (0)

static volatile u32 opaque_n;  // длина, которую компилятор не знает — путь через рантайм

static void movs_case(u32 n, int size, int df, int delta, int constant)
{
    u32 base = BUF / 2 - (df ? 0 : (u32)(n * (u32)size) / 2);
    u32 s0 = df ? base + (u32)(n * (u32)size) / 2 : base, d0 = s0 + (u32)delta;
    u32 rs = s0, rd = d0, esi = s0, edi = d0;
    fill();
    model_movs(ref, &rs, &rd, n, size, df);
    if (constant && !df && n == 3 && size == 4)
        GEN_MOVS(3, 4, 0);
    else if (constant && !df && n == 16 && size == 1)
        GEN_MOVS(16, 1, 0);
    else if (constant && !df && n == 5 && size == 2)
        GEN_MOVS(5, 2, 0);
    else {
        opaque_n = n;
        GEN_MOVS(opaque_n, size, df);
    }
    check(constant ? "movs const" : "movs", n, size, df, delta, rs, esi, rd, edi);
}

static void stos_case(u32 n, int size, int df, u32 v, int constant)
{
    u32 d0 = BUF / 2, rd = d0, edi = d0;
    fill();
    model_stos(ref, &rd, n, v, size, df);
    if (constant && !df && n == 4 && size == 4)
        GEN_STOS(4, v, 4, 0);
    else if (constant && !df && n == 7 && size == 2)
        GEN_STOS(7, v, 2, 0);
    else {
        opaque_n = n;
        GEN_STOS(opaque_n, v, size, df);
    }
    check(constant ? "stos const" : "stos", n, size, df, 0, 0, 0, rd, edi);
}

int main(void)
{
    static const u32 ns[] = {0, 1, 2, 3, 5, 7, 16, 17, 40};
    static const u32 vals[] = {0, 0xFFFFFFFFu, 0x41414141u, 0x12345678u, 0x00000041u, 0x4141u};
    int count = 0;
    for (int size = 1; size <= 4; size *= 2)
        for (int df = 0; df <= 1; df++)
            for (unsigned k = 0; k < sizeof ns / sizeof *ns; k++) {
                for (int delta = -9; delta <= 9; delta++, count++)
                    movs_case(ns[k], size, df, delta, 0);
                movs_case(ns[k], size, df, 200, 0), count++;
                movs_case(ns[k], size, df, -200, 0), count++;
                for (unsigned v = 0; v < sizeof vals / sizeof *vals; v++, count++)
                    stos_case(ns[k], size, df, vals[v], 0);
            }
    // путь на месте (длина-константа, df = 0), в том числе перекрытие вперёд — размножение образца
    for (int delta = -9; delta <= 9; delta++, count += 3) {
        movs_case(3, 4, 0, delta, 1);
        movs_case(16, 1, 0, delta, 1);
        movs_case(5, 2, 0, delta, 1);
    }
    for (unsigned v = 0; v < sizeof vals / sizeof *vals; v++, count += 2) {
        stos_case(4, 4, 0, vals[v], 1);
        stos_case(7, 2, 0, vals[v], 1);
    }
    printf("%d cases, %d failed\n", count, fails);
    return fails != 0;
}
