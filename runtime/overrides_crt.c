// Нативные замены функций msvcr70 (таблица — recomp/overrides.py). Математика CRT передаёт x87-стек
// нестандартно и статически не переводится; разбор чисел проще и надёжнее сделать на хосте.
// cdecl: [esp] — адрес возврата, аргументы с esp+4; double — в c->st_ret (ST0), int — в eax.
// Ограничение: errno гостя не выставляется.
#include <float.h>
#include <stdio.h>
#include <stdlib.h>

#include "runtime/crt_math.h"
#include "runtime/rt.h"

#define ARG(i) (c->esp + 4 + 4 * (u32)(i)) // гостевой адрес i-го 4-байтового слова аргументов
#define F64(i) rdf64(g_mem, ARG(i))
#define U32(i) rd32(g_mem, ARG(i))

static void ret_f64(CPU *c, double v)
{
    c->st_ret = v;
    c->esp += 4;
}

// --- математика ------------------------------------------------------------------
#define UNARY(name, expr)                                                                                   \
    void ovr_crt_##name(CPU *c)                                                                             \
    {                                                                                                       \
        double x = F64(0);                                                                                  \
        ret_f64(c, crt_nan(expr, x, 0.0));                                                                  \
    }
#define BINARY(name, expr)                                                                                  \
    void ovr_crt_##name(CPU *c)                                                                             \
    {                                                                                                       \
        double x = F64(0), y = F64(2);                                                                      \
        ret_f64(c, crt_nan(expr, x, y));                                                                    \
    }

UNARY(acos, acos(x))
UNARY(asin, asin(x))
UNARY(atan, atan(x))
UNARY(ceil, ceil(x))
UNARY(cos, cos(x))
UNARY(cosh, cosh(x))
UNARY(exp, crt_exp(x))
UNARY(fabs, fabs(x))
UNARY(floor, floor(x))
UNARY(log, log(x))
UNARY(log10, log10(x))
UNARY(sin, sin(x))
UNARY(sinh, sinh(x))
UNARY(sqrt, sqrt(x))
UNARY(tan, tan(x))
UNARY(tanh, tanh(x))
UNARY(logb, isinf(x) ? x : logb(x)) // как в MSVC: _logb(-inf) = -inf
UNARY(y0, y0(x))
UNARY(y1, y1(x))
BINARY(atan2, crt_atan2(x, y)) // atan2(y, x): первый аргумент — y
BINARY(fmod, crt_fmod(x, y))
BINARY(pow, crt_pow(x, y))
BINARY(nextafter, nextafter(x, y))

void ovr_crt_frexp(CPU *c) // double frexp(double, int *)
{
    double x = F64(0);
    int e = -1;
    double r = crt_nan(NAN, x, 0.0); // MSVC: frexp(±inf/nan) — NaN и показатель -1
    if (x == 0.0) {
        e = 0;
        r = 0.0; // MSVC: frexp(-0) = +0
    } else if (isfinite(x))
        r = frexp(x, &e);
    wr32(g_mem, U32(2), (u32)e);
    ret_f64(c, crt_nan(r, x, 0.0));
}

void ovr_crt_ldexp(CPU *c) // double ldexp(double, int)
{
    double x = F64(0);
    ret_f64(c, crt_nan(ldexp(x, (int)(s32)U32(2)), x, 0.0));
}

void ovr_crt_modf(CPU *c) // double modf(double, double *)
{
    double x = F64(0), ip = 0;
    double r = modf(x, &ip);
    wrf64(g_mem, U32(2), crt_nan(ip, x, 0.0));
    ret_f64(c, crt_nan(r, x, 0.0));
}

void ovr_crt_yn(CPU *c) // double _yn(int, double)
{
    double x = F64(1);
    ret_f64(c, crt_nan(yn((int)(s32)U32(0), x), x, 0.0));
}

// --- разбор чисел ----------------------------------------------------------------
// Символ гостевой строки: узкой (байт) или широкой (UTF-16).
static u32 gch(u32 s, int wide, u32 i) { return wide ? rd16(g_mem, s + 2 * i) : g_mem[s + i]; }
static int is_ws(u32 ch) { return ch == ' ' || (ch >= 9 && ch <= 13); }
static int is_dig(u32 ch) { return ch >= '0' && ch <= '9'; }

#define MAX_DIGITS 21 // CRT MSVC учитывает не больше 21 значащей цифры, остальные отбрасывает

typedef struct {
    u32 len;   // длина принятого текста с начальными пробелами (0 — числа нет)
    int over;  // переполнение double (v = ±inf)
    int under; // антипереполнение double: денормал или 0 из ненулевых цифр
    double v;  // значение, которое CRT получает до проверки флагов
} num_t;

// Число по грамматике CRT MSVC 7: [пробелы][знак]цифры[.цифры][(e|E|d|D)[знак]цифры]; без inf/nan/hex.
// Без цифр len = 0, а v — ноль со знаком (так его отдаёт _atodbl).
static num_t parse_num(u32 s, int wide)
{
    num_t r = {0, 0, 0, 0.0};
    u32 i = 0, ch;
    int neg = 0, n = 0, ndig = 0;
    long exp10 = 0;
    char buf[MAX_DIGITS + 32];
    while (is_ws(gch(s, wide, i)))
        i++;
    if ((ch = gch(s, wide, i)) == '+' || ch == '-') {
        neg = ch == '-';
        i++;
    }
    for (; is_dig(ch = gch(s, wide, i)); i++, ndig++) {
        if (n == 0 && ch == '0')
            continue;
        if (n < MAX_DIGITS)
            buf[n++] = (char)ch;
        else
            exp10++;
    }
    if (ch == '.') {
        for (i++; is_dig(ch = gch(s, wide, i)); i++, ndig++) {
            if (n == 0 && ch == '0')
                exp10--;
            else if (n < MAX_DIGITS) {
                buf[n++] = (char)ch;
                exp10--;
            }
        }
    }
    if (!ndig) {
        r.v = neg ? -0.0 : 0.0;
        return r;
    }
    if (ch == 'e' || ch == 'E' || ch == 'd' || ch == 'D') {
        u32 j = i + 1;
        int eneg = 0;
        if ((ch = gch(s, wide, j)) == '+' || ch == '-') {
            eneg = ch == '-';
            j++;
        }
        if (is_dig(gch(s, wide, j))) {
            long e = 0;
            for (; is_dig(ch = gch(s, wide, j)); j++)
                if (e < 100000)
                    e = e * 10 + (long)(ch - '0');
            exp10 += eneg ? -e : e;
            i = j;
        }
    }
    r.len = i;
    if (n) {
        snprintf(buf + n, 32, "e%ld", exp10);
        r.v = strtod(buf, NULL); // в buf только цифры и экспонента — от локали не зависит
        if (isinf(r.v))
            r.over = 1;
        else if (r.v < DBL_MIN) {
            // ниже минимального денормала long double (3.6e-4951) CRT молча даёт ноль без флага
            buf[n] = 0;
            r.under = log10(strtod(buf, NULL)) + (double)exp10 >= -4950.4383;
        }
    }
    if (neg)
        r.v = -r.v;
    return r;
}

// strtod/wcstod (strict: антипереполнение -> +0; endptr, если не 0) и atof/_wtof (значение как есть).
static double str_to_d(u32 s, u32 endp, int wide, int strict)
{
    num_t r = parse_num(s, wide);
    if (endp)
        wr32(g_mem, endp, s + r.len * (wide ? 2 : 1));
    return !r.len || (strict && r.under) ? 0.0 : r.v;
}

void ovr_crt_strtod(CPU *c) { ret_f64(c, str_to_d(U32(0), U32(1), 0, 1)); }
void ovr_crt_wcstod(CPU *c) { ret_f64(c, str_to_d(U32(0), U32(1), 1, 1)); }
void ovr_crt_atof(CPU *c) { ret_f64(c, str_to_d(U32(0), 0, 0, 0)); }
void ovr_crt_wtof(CPU *c) { ret_f64(c, str_to_d(U32(0), 0, 1, 0)); }

// int _atodbl(_CRT_DOUBLE *, char *): 0 — успех, 1 — переполнение, 2 — антипереполнение.
void ovr_crt_atodbl(CPU *c)
{
    num_t r = parse_num(U32(1), 0);
    wrf64(g_mem, U32(0), r.v);
    c->eax = r.over ? 1 : r.under ? 2 : 0;
    c->esp += 4;
}

// --- rand/srand ------------------------------------------------------------------
// Генератор MSVC (_holdrand · 214013 + 2531011, биты 16–30), состояние своё у потока, начальное — 1. У оригинала
// оно в данных потока CRT, и каждый rand — _getptd с тремя вызовами HLE (GetLastError, TlsGetValue,
// SetLastError): частицы зовут appFrand тысячи раз за кадр — ≈3 % времени гостя на Switch.
static __thread u32 t_holdrand = 1;

void ovr_crt_rand(CPU *c)
{
    t_holdrand = t_holdrand * 214013u + 2531011u;
    c->eax = (t_holdrand >> 16) & 0x7FFFu;
    c->esp += 4;
}

void ovr_crt_srand(CPU *c)
{
    t_holdrand = U32(0);
    c->esp += 4;
}

// --- qsort -----------------------------------------------------------------------
// msvcr70.dll!0x1300317f: cdecl void qsort(void *base, size_t num, size_t width, int (*comp)(const void *, const void *)).
// В рендере (Engine.dll!0x121a3f90) сортируются 16-байтные записи примитивов компаратором Engine.dll!0x121a46a0
// (5 инструкций: *(s32*)b - *(s32*)a); в оригинале каждое сравнение идёт через x86 qsort + RT_ICALL -> ~3,1 % Core 1.
// Быстрый путь: при width == 16 и совпадении 13 байт компаратора сортируем нативно без вызовов гостя (introsort).
// Общий путь: нативный introsort с однократным поиском функции компаратора (rt_lookup_k) и вызовом через rt_invoke.
// SNX_QSORT_CHECK=1: сверка порядка ключей / отсортированности с оригиналом msvcr70!qsort.
extern void (*const ovr_orig_ovr_crt_qsort)(CPU *c);

static inline int qsort_check(void)
{
    static int v = -1;
    if (v < 0)
        v = getenv("SNX_QSORT_CHECK") != NULL;
    return v;
}

// Engine.dll!0x121a46a0 (en и rus): mov eax,[esp+8]; mov ecx,[esp+4]; mov eax,[eax]; sub eax,[ecx]; ret
static const u8 k_cmp_desc_i32[13] = {0x8B, 0x44, 0x24, 0x08, 0x8B, 0x4C, 0x24, 0x04, 0x8B, 0x00, 0x2B, 0x01, 0xC3};

typedef struct {
    u32 w[4];
} qelem16;

static inline int q16_less(u32 ka, u32 kb)
{
    // Знак (s32)(kb - ka) < 0: соответствует sub eax,[ecx] компаратора (убывание ключа)
    return (s32)(kb - ka) < 0;
}

static inline qelem16 q16_ld(const u8 *p)
{
    qelem16 e;
    memcpy(&e, p, sizeof e);
    return e;
}

static inline void q16_st(u8 *p, qelem16 e) { memcpy(p, &e, sizeof e); }

static inline void q16_swap(u8 *a, u8 *b)
{
    qelem16 t = q16_ld(a);
    q16_st(a, q16_ld(b));
    q16_st(b, t);
}

static void q16_ins(u8 *arr, u32 lo, u32 hi)
{
    for (u32 i = lo + 1u; i < hi; i++) {
        qelem16 cur = q16_ld(arr + i * 16u);
        u32 j = i;
        while (j > lo) {
            qelem16 prev = q16_ld(arr + (j - 1u) * 16u);
            if (!q16_less(cur.w[0], prev.w[0]))
                break;
            q16_st(arr + j * 16u, prev);
            j--;
        }
        q16_st(arr + j * 16u, cur);
    }
}

static void q16_sift(u8 *arr, u32 base, u32 n, u32 root)
{
    for (;;) {
        u32 child = root * 2u + 1u;
        if (child >= n)
            break;
        u32 rkey = rd32(arr, (base + root) * 16u);
        u32 ckey = rd32(arr, (base + child) * 16u);
        u32 best = root, bkey = rkey;
        if (q16_less(bkey, ckey)) {
            best = child;
            bkey = ckey;
        }
        if (child + 1u < n) {
            u32 r2key = rd32(arr, (base + child + 1u) * 16u);
            if (q16_less(bkey, r2key))
                best = child + 1u;
        }
        if (best == root)
            break;
        q16_swap(arr + (base + root) * 16u, arr + (base + best) * 16u);
        root = best;
    }
}

static void q16_heapsort(u8 *arr, u32 lo, u32 hi)
{
    u32 n = hi - lo;
    if (n < 2u)
        return;
    for (u32 i = n / 2u; i-- > 0;)
        q16_sift(arr, lo, n, i);
    for (u32 end = n - 1u; end > 0; end--) {
        q16_swap(arr + lo * 16u, arr + (lo + end) * 16u);
        q16_sift(arr, lo, end, 0u);
    }
}

static void q16_sort(u8 *arr, u32 lo, u32 hi, u32 depth)
{
    while (hi - lo > 16u) {
        if (depth == 0) {
            q16_heapsort(arr, lo, hi);
            return;
        }
        depth--;
        // Медиана из трёх: arr[lo], arr[mid], arr[hi - 1]
        u32 mid = lo + ((hi - lo) >> 1);
        u32 last = hi - 1u;
        if (q16_less(rd32(arr, mid * 16u), rd32(arr, lo * 16u)))
            q16_swap(arr + lo * 16u, arr + mid * 16u);
        if (q16_less(rd32(arr, last * 16u), rd32(arr, mid * 16u))) {
            q16_swap(arr + mid * 16u, arr + last * 16u);
            if (q16_less(rd32(arr, mid * 16u), rd32(arr, lo * 16u)))
                q16_swap(arr + lo * 16u, arr + mid * 16u);
        }
        // Опорный элемент кладём в last - 1: arr[lo] <= pivot <= arr[last] — оба конца служат стражами
        u32 piv = last - 1u;
        q16_swap(arr + mid * 16u, arr + piv * 16u);
        u32 pivot = rd32(arr, piv * 16u);
        u32 i = lo, j = piv;
        for (;;) {
            while (++i < piv && q16_less(rd32(arr, i * 16u), pivot))
                ;
            while (j > lo && q16_less(pivot, rd32(arr, --j * 16u)))
                ;
            if (i >= j)
                break;
            q16_swap(arr + i * 16u, arr + j * 16u);
        }
        q16_swap(arr + i * 16u, arr + piv * 16u);
        if (i - lo < hi - (i + 1u)) {
            q16_sort(arr, lo, i, depth);
            lo = i + 1u;
        } else {
            q16_sort(arr, i + 1u, hi, depth);
            hi = i;
        }
    }
    q16_ins(arr, lo, hi);
}

typedef struct {
    CPU *c;
    u32 base, width, comp;
    guest_fn fn;
    int native;
} qgen_ctx;

static int qgen_cmp(const qgen_ctx *q, u32 ia, u32 ib)
{
    CPU *c = q->c;
    u32 sp = c->esp;
    c->esp = sp - 12u;
    wr32(g_mem, c->esp, 0u);  // фиктивный адрес возврата: ret снимает 4 байта
    wr32(g_mem, c->esp + 4u, q->base + ia * q->width);
    wr32(g_mem, c->esp + 8u, q->base + ib * q->width);
    if (q->fn)
        rt_invoke(c, q->fn, q->native);
    else
        rt_call(c, q->comp);
    c->esp = sp;
    return (s32)c->eax;
}

static void qgen_swap(const qgen_ctx *q, u32 ia, u32 ib)
{
    if (ia == ib)
        return;
    u8 *a = g_mem + q->base + ia * q->width;
    u8 *b = g_mem + q->base + ib * q->width;
    u32 rem = q->width;
    u8 tmp[256];
    while (rem > 0) {
        u32 chunk = rem < sizeof tmp ? rem : (u32)sizeof tmp;
        memcpy(tmp, a, chunk);
        memcpy(a, b, chunk);
        memcpy(b, tmp, chunk);
        a += chunk;
        b += chunk;
        rem -= chunk;
    }
}

static void qgen_ins(const qgen_ctx *q, u32 lo, u32 hi)
{
    for (u32 i = lo + 1u; i < hi; i++)
        for (u32 j = i; j > lo && qgen_cmp(q, j, j - 1u) < 0; j--)
            qgen_swap(q, j, j - 1u);
}

static void qgen_sift(const qgen_ctx *q, u32 base, u32 n, u32 root)
{
    for (;;) {
        u32 child = root * 2u + 1u;
        if (child >= n)
            break;
        u32 best = root;
        if (qgen_cmp(q, base + best, base + child) < 0)
            best = child;
        if (child + 1u < n && qgen_cmp(q, base + best, base + child + 1u) < 0)
            best = child + 1u;
        if (best == root)
            break;
        qgen_swap(q, base + root, base + best);
        root = best;
    }
}

static void qgen_heapsort(const qgen_ctx *q, u32 lo, u32 hi)
{
    u32 n = hi - lo;
    if (n < 2u)
        return;
    for (u32 i = n / 2u; i-- > 0;)
        qgen_sift(q, lo, n, i);
    for (u32 end = n - 1u; end > 0; end--) {
        qgen_swap(q, lo, lo + end);
        qgen_sift(q, lo, end, 0u);
    }
}

static void qgen_sort(const qgen_ctx *q, u32 lo, u32 hi, u32 depth)
{
    while (hi - lo > 8u) {
        if (depth == 0) {
            qgen_heapsort(q, lo, hi);
            return;
        }
        depth--;
        u32 mid = lo + ((hi - lo) >> 1);
        u32 last = hi - 1u;
        if (qgen_cmp(q, mid, lo) < 0)
            qgen_swap(q, lo, mid);
        if (qgen_cmp(q, last, mid) < 0) {
            qgen_swap(q, mid, last);
            if (qgen_cmp(q, mid, lo) < 0)
                qgen_swap(q, lo, mid);
        }
        // Опорный элемент кладём в last - 1, чтобы его индекс не менялся во время разбиения
        u32 piv = last - 1u;
        qgen_swap(q, mid, piv);
        u32 i = lo, j = piv;
        for (;;) {
            while (++i < piv && qgen_cmp(q, i, piv) < 0)
                ;
            while (j > lo && qgen_cmp(q, piv, --j) < 0)
                ;
            if (i >= j)
                break;
            qgen_swap(q, i, j);
        }
        qgen_swap(q, i, piv);
        if (i - lo < hi - (i + 1u)) {
            qgen_sort(q, lo, i, depth);
            lo = i + 1u;
        } else {
            qgen_sort(q, i + 1u, hi, depth);
            hi = i;
        }
    }
    qgen_ins(q, lo, hi);
}

static u32 qsort_depth(u32 n)
{
    u32 d = 0;
    for (u32 x = n; x > 1u; x >>= 1)
        d += 2u;
    return d;
}

void ovr_crt_qsort(CPU *c)
{
    u32 base = U32(0), num = U32(1), width = U32(2), comp = U32(3);
    if (num < 2u || width == 0u) {
        c->esp += 4u;
        return;
    }
    u64 bytes = (u64)num * (u64)width;
    if (base >= RT_WINDOW || bytes > (u64)RT_WINDOW - base) {
        if (ovr_orig_ovr_crt_qsort) {
            RT_ENTER_GEN(c, ovr_orig_ovr_crt_qsort);
            return;
        }
        c->esp += 4u;
        return;
    }
    int native = 0;
    guest_fn fn = rt_lookup_k(comp, &native);
    int fast16 = fn && !native && width == 16u && comp <= RT_WINDOW - sizeof k_cmp_desc_i32 &&
                 memcmp(g_mem + comp, k_cmp_desc_i32, sizeof k_cmp_desc_i32) == 0;
    int chk = qsort_check() && ovr_orig_ovr_crt_qsort != NULL;
    u8 *saved = NULL, *ours = NULL;
    if (chk) {
        saved = malloc((size_t)bytes);
        ours = malloc((size_t)bytes);
        if (saved && ours)
            memcpy(saved, g_mem + base, (size_t)bytes);
        else
            chk = 0;
    }
    u32 ebx = c->ebx, esi = c->esi, edi = c->edi, ebp = c->ebp, sp = c->esp;
    if (fast16) {
        q16_sort(g_mem + base, 0u, num, qsort_depth(num));
    } else {
        qgen_ctx q = {c, base, width, comp, fn, native};
        qgen_sort(&q, 0u, num, qsort_depth(num));
    }
    c->ebx = ebx;
    c->esi = esi;
    c->edi = edi;
    c->ebp = ebp;
    c->esp = sp;
    if (chk) {
        memcpy(ours, g_mem + base, (size_t)bytes);
        memcpy(g_mem + base, saved, (size_t)bytes);
        RT_ENTER_GEN(c, ovr_orig_ovr_crt_qsort);
        if (fast16) {
            for (u32 i = 0; i < num; i++) {
                u32 ka = rd32(ours, i * 16u), kb = rd32(g_mem, base + i * 16u);
                if (ka != kb) {
                    fprintf(stderr, "[crt] SNX_QSORT_CHECK: fast16 key[%u] %08x != orig %08x (n=%u)\n", i, ka, kb, num);
                    break;
                }
            }
        } else {
            // Проверим упорядоченность нашего результата компаратором гостя (qsort нестабилен при равных ключах)
            memcpy(g_mem + base, ours, (size_t)bytes);
            u32 sp2 = c->esp;
            c->esp = sp;
            qgen_ctx q = {c, base, width, comp, fn, native};
            for (u32 i = 0; i + 1u < num; i++)
                if (qgen_cmp(&q, i, i + 1u) > 0) {
                    fprintf(stderr, "[crt] SNX_QSORT_CHECK: unsorted at %u (n=%u, w=%u)\n", i, num, width);
                    break;
                }
            c->esp = sp2;
        }
        free(saved);
        free(ours);
        return;
    }
    free(saved);
    free(ours);
    c->esp += 4u;  // cdecl: аргументы снимает вызывающий
}
