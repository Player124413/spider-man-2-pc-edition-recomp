// Единая копия медленного пути косвенного вызова (SNX_PIN) и компактных pinned-обёрток для COM/HLE-функций.
// Компилируется с закреплёнными регистрами x19–x28 (-ffixed-x28, runtime/gen_rt.h), чтобы:
// 1) в бинарнике была одна rt_icall_slow вместо отдельной static-копии на каждый файл build/gen/;
// 2) нативные функции хоста (COM/HLE/оверрайды) вызывались прямо из inline-кеша RT_ICALL через компактную
//    обёртку pin_wrap_K -> rt_pin_call_native (SPILL/fn/RELOAD), убирая проверку бита native и промахи COM в D3DDrv;
// 3) горячие секции (.text.rt_pin_call_native, .text.pin_wrap, .text.rt_icall_slow) раскладывались hot_order.ld
//    сразу рядом с rt_pin_enter в первых 48 КБ L1I.
#include "runtime/gen_rt.h"

#ifdef SNX_PIN
#define MAX_NAT_WRAPS 1024u

static guest_fn g_nat_fn[MAX_NAT_WRAPS];
static u32 g_nat_count;

// Единый мостик pinned-ABI (x19–x28, c в x28) -> хостовый C (COM/HLE/оверрайд) -> pinned-ABI.
static __attribute__((noinline, section(".text.rt_pin_call_native"))) void rt_pin_call_native(guest_fn fn)
{
    CPU *restrict c = g_pin_c;
    SPILL();
    fn(c);
    RELOAD();
}

// 1024 компактных переходника в единой секции .text.pin_wrap.
// Запись в g_nat_fn[k] однократная и синхронизирована с публикацией в rt_ic2/ic_ (release в rt_call_ic).
#define NW1(h)                                                                                             \
    static __attribute__((section(".text.pin_wrap"))) void pin_wrap_##h(void)                              \
    {                                                                                                      \
        rt_pin_call_native(__atomic_load_n(&g_nat_fn[0x##h], __ATOMIC_RELAXED));                           \
    }
#define NW16(p)                                                                                            \
    NW1(p##0) NW1(p##1) NW1(p##2) NW1(p##3) NW1(p##4) NW1(p##5) NW1(p##6) NW1(p##7)                        \
    NW1(p##8) NW1(p##9) NW1(p##a) NW1(p##b) NW1(p##c) NW1(p##d) NW1(p##e) NW1(p##f)
#define NW256(p)                                                                                           \
    NW16(p##0) NW16(p##1) NW16(p##2) NW16(p##3) NW16(p##4) NW16(p##5) NW16(p##6) NW16(p##7)                \
    NW16(p##8) NW16(p##9) NW16(p##a) NW16(p##b) NW16(p##c) NW16(p##d) NW16(p##e) NW16(p##f)

NW256(0) NW256(1) NW256(2) NW256(3)

#define NR1(h) pin_wrap_##h,
#define NR16(p)                                                                                            \
    NR1(p##0) NR1(p##1) NR1(p##2) NR1(p##3) NR1(p##4) NR1(p##5) NR1(p##6) NR1(p##7)                        \
    NR1(p##8) NR1(p##9) NR1(p##a) NR1(p##b) NR1(p##c) NR1(p##d) NR1(p##e) NR1(p##f)
#define NR256(p)                                                                                           \
    NR16(p##0) NR16(p##1) NR16(p##2) NR16(p##3) NR16(p##4) NR16(p##5) NR16(p##6) NR16(p##7)                \
    NR16(p##8) NR16(p##9) NR16(p##a) NR16(p##b) NR16(p##c) NR16(p##d) NR16(p##e) NR16(p##f)

static const pin_fn g_nat_wraps[MAX_NAT_WRAPS] = {NR256(0) NR256(1) NR256(2) NR256(3)};

#undef NW1
#undef NW16
#undef NW256
#undef NR1
#undef NR16
#undef NR256

// Выдаёт pinned-ABI обёртку для нативной функции fn (дедупликация по указателю fn, lock-free — не трогает x19–x28,
// безопасно вызывается из хостового rt_call_ic). При исчерпании 1024 слотов возвращает NULL (откат на rt_call).
__attribute__((cold)) guest_fn rt_pin_wrap_native(guest_fn fn)
{
    if (!fn)
        return NULL;
    u32 n = __atomic_load_n(&g_nat_count, __ATOMIC_ACQUIRE);
    for (u32 i = 0; i < n; i++)
        if (__atomic_load_n(&g_nat_fn[i], __ATOMIC_ACQUIRE) == fn)
            return SNX_GUEST_FN(g_nat_wraps[i]);
    for (u32 i = 0; i < MAX_NAT_WRAPS; i++) {
        guest_fn cur = __atomic_load_n(&g_nat_fn[i], __ATOMIC_ACQUIRE);
        if (cur == fn)
            return SNX_GUEST_FN(g_nat_wraps[i]);
        if (!cur) {
            guest_fn exp = NULL;
            if (__atomic_compare_exchange_n(&g_nat_fn[i], &exp, fn, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                u32 old = __atomic_load_n(&g_nat_count, __ATOMIC_RELAXED);
                while (old <= i && !__atomic_compare_exchange_n(&g_nat_count, &old, i + 1, 0, __ATOMIC_RELEASE,
                                                                __ATOMIC_RELAXED))
                    ;
                return SNX_GUEST_FN(g_nat_wraps[i]);
            }
            if (exp == fn)
                return SNX_GUEST_FN(g_nat_wraps[i]);
        }
    }
    return NULL;
}

__attribute__((cold)) void rt_ic_base(void)
{
    CPU *restrict c = g_pin_c;
    SPILL();
    rt_trap(c, 0, "call to unknown guest address");
}

__attribute__((section(".text.rt_icall_slow"))) void rt_icall_slow(u32 t, u64 *ic)
{
    u64 v = __atomic_load_n(&rt_ic2[RT_IC2_IDX(t)], __ATOMIC_RELAXED);
    if (__builtin_expect((u32)v == t, 1)) {
        __atomic_store_n(ic, v, __ATOMIC_RELAXED);
        ((pin_fn)((uintptr_t)rt_ic_base + (intptr_t)((s64)v >> 32)))();
        return;
    }
    CPU *restrict c = g_pin_c;
    SPILL();
    rt_call_ic(c, t, ic);
    RELOAD();
}
#else
typedef int snx_icall_pin_unused;
#endif
