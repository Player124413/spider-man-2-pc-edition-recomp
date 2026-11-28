// Семантика математики CRT MSVC 7 поверх libm хоста. Общая для замен msvcr70 (overrides_crt.c)
// и вложенных x87-интринсиков _CI* (recomp/emit.py). Особые случаи сверены с оригиналом
// в unicorn (tests/difftest/crtmath.py).
#pragma once
#include "runtime/abi.h"

static inline double crt_from_bits(u64 b) { double d; memcpy(&d, &b, 8); return d; }
static inline u64 crt_to_bits(double d) { u64 b; memcpy(&b, &d, 8); return b; }

// NaN как у x87: NaN-операнд возвращается «тихим» (из двух — с большей мантиссой),
// недопустимая операция над числами даёт «неопределённое» -NaN (на ARM было бы +NaN).
static inline double crt_nan(double r, double x, double y)
{
    if (!isnan(r))
        return r;
    const u64 q = 1ull << 51, man = (1ull << 52) - 1;
    u64 bx = crt_to_bits(x), by = crt_to_bits(y);
    if (isnan(x) && isnan(y))
        return crt_from_bits(((bx & man) >= (by & man) ? bx : by) | q);
    if (isnan(x))
        return crt_from_bits(bx | q);
    if (isnan(y))
        return crt_from_bits(by | q);
    return crt_from_bits(0xFFF8000000000000ull);
}

static inline double crt_exp(double x)
{
    return x < -744.44007192138122 ? 0.0 : exp(x); // MSVC: ниже ln(минимального денормала) — 0
}

static inline double crt_fmod(double x, double y)
{
    if (isinf(y) && !isnan(x))
        return crt_nan(NAN, x, y); // MSVC: fmod(x, ±inf) — ошибка области
    if (x == 0.0 && y != 0.0 && !isnan(y))
        return 0.0; // MSVC: fmod(-0, y) = +0
    return crt_nan(fmod(x, y), x, y);
}

static inline double crt_pow(double x, double y)
{
    // MSVC: NaN в любом аргументе и (±1)^±inf — ошибка области (C99 дал бы 1)
    if (isnan(x) || isnan(y) || (fabs(x) == 1.0 && isinf(y)))
        return crt_nan(NAN, x, y);
    return crt_nan(pow(x, y), x, y);
}

static inline double crt_atan2(double y, double x)
{
    if (isinf(y) && isinf(x))
        return crt_nan(NAN, y, x); // MSVC: ошибка области (C99 дал бы ±pi/4, ±3pi/4)
    return crt_nan(atan2(y, x), y, x);
}
