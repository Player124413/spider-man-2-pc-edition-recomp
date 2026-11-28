// ABI между сгенерированным кодом, рантаймом и HLE.
// Гостевой адрес a (u32) живёт в хостовой памяти по адресу g_mem + a.
#pragma once
#include <math.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

typedef union {
    float f[4];
    double d[2];
    u32 u[4];
    s32 i[4];
    u64 q[2];
    u16 w[8];
    u8 b[16];
} xmm_t;

// Состояние гостевого потока. Сгенерированные функции держат регистры в локальных
// переменных и сбрасывают их сюда на вызовах и выходах. Выравнивание 16 байт безопасно
// для calloc() в runtime/process.c под UBSan (64-байтное требует aligned_alloc).
typedef struct __attribute__((aligned(16))) CPU {
    u32 eax, ecx, edx, ebx, esp, ebp, esi, edi; // порядок как в кодировке x86
    u32 fs_base;     // гостевой адрес TEB
    u32 fpucw;       // управляющее слово x87 (режим округления)
    double st_ret;   // ST(0) при возврате float/double из функции
    u64 mm[8];
    xmm_t xmm[8];
    u32 mxcsr;
    u32 depth;       // глубина хостовой рекурсии (защита стека)
    u32 last_addr;   // последний гостевой адрес для диагностики
    void *thread;    // GuestThread рантайма
} CPU;

typedef void (*guest_fn)(CPU *c);

// hidden: всё линкуется в один исполняемый файл — адрес берётся напрямую (adrp), а не через GOT: на каждом входе
// в функцию гостя (m = g_mem) и в каждом кеше косвенного вызова (rt_ic_base) на одну зависимую загрузку меньше
#define RT_HIDDEN __attribute__((visibility("hidden")))
#define RT_NORETURN_COLD __attribute__((noreturn, cold))

// Закреплённые регистры (SNX_PIN — Switch, GCC; runtime/gen_rt.h): в сгенерированном коде регистры гостя живут в
// x19–x26, m — в x27, указатель CPU *c — в x28 и между функциями гостя через CPU/x0 не ходят. Рантайм и HLE
// работают с CPU: вход из них в код гостя — трамплин rt_pin_enter (сохраняет регистры хоста, берёт регистры
// гостя из CPU и ставит x28 = c, после — кладёт обратно), вызов из кода гостя в функцию хоста — с SPILL/RELOAD.
// Без SNX_PIN — обычный вызов с передачей c в x0.
#ifdef SNX_PIN
typedef void (*pin_fn)(void);
void rt_pin_enter(CPU *c, guest_fn fn) RT_HIDDEN;
guest_fn rt_pin_wrap_native(guest_fn fn) RT_HIDDEN;
#define RT_ENTER_GEN(c, fn) rt_pin_enter((c), (fn))
#else
#define RT_ENTER_GEN(c, fn) ((fn)(c))
#endif

extern u8 *g_mem RT_HIDDEN;

// --- доступ к гостевой памяти -------------------------------------------------
static inline u16 rd16(const u8 *m, u32 a) { u16 v; memcpy(&v, m + a, 2); return v; }
static inline u32 rd32(const u8 *m, u32 a) { u32 v; memcpy(&v, m + a, 4); return v; }
static inline u64 rd64(const u8 *m, u32 a) { u64 v; memcpy(&v, m + a, 8); return v; }
static inline void wr16(u8 *m, u32 a, u16 v) { memcpy(m + a, &v, 2); }
static inline void wr32(u8 *m, u32 a, u32 v) { memcpy(m + a, &v, 4); }
static inline void wr64(u8 *m, u32 a, u64 v) { memcpy(m + a, &v, 8); }
static inline float rdf32(const u8 *m, u32 a) { float v; memcpy(&v, m + a, 4); return v; }
static inline double rdf64(const u8 *m, u32 a) { double v; memcpy(&v, m + a, 8); return v; }
static inline void wrf32(u8 *m, u32 a, float v) { memcpy(m + a, &v, 4); }
static inline void wrf64(u8 *m, u32 a, double v) { memcpy(m + a, &v, 8); }

// Указатель хоста на гостевой адрес (для HLE).
#define GPTR(a) ((void *)(g_mem + (u32)(a)))

// --- рантайм ------------------------------------------------------------------
void rt_call(CPU *c, u32 target);                      // косвенный вызов/переход по гостевому адресу
// Косвенный вызов с кешем на месте вызова (vtable, call reg, jmp [mem]): ic — свой static u64 у каждого места,
// младшие 32 бита — гостевой адрес, старшие — смещение функции хоста от rt_ic_base (одна атомарная запись:
// потоки гостя не увидят адрес от одной функции, а смещение от другой). Пустой кеш (0) совпадает только с
// адресом 0 и ведёт в rt_ic_base — ловушку.
#ifdef SNX_PIN
void rt_ic_base(void) RT_HIDDEN;
#else
void rt_ic_base(CPU *c) RT_HIDDEN;
#endif
void rt_call_ic(CPU *c, u32 target, u64 *ic);
// промахи мест вызова: общий кеш прямого отображения в том же формате (полиморфные vtable в обходе сцены и
// UProperty::CopySingleValue); 4096 слотов = 32 КБ (влезает в L1D, сдвиг >> 4 учитывает 16-байтное выравнивание
// функций MSVC, вдвое сокращая число активных кэш-линий и коллизий). Заполняет rt_call_ic, с SNX_PIN сначала
// смотрит rt_icall_slow (runtime/icall_pin.c)
#define RT_IC2_SIZE 4096u
#define RT_IC2_IDX(t) ((((t) ^ ((t) >> 12)) >> 4) & (RT_IC2_SIZE - 1))
extern u64 rt_ic2[RT_IC2_SIZE] RT_HIDDEN;
// Теневая таблица прямого отображения для диспетчера байткода UnrealScript GNatives (Core.dll, 4096 слотов):
// формат слота тот же, что в ic_/rt_ic2 (младшие 32 бита — гостевой адрес, старшие — смещение от rt_ic_base).
#define RT_GNATIVE_SIZE 4096u
extern u64 rt_gnatives_cache[RT_GNATIVE_SIZE] RT_HIDDEN;
#define RT_ICALL(c, t)                                                                                     \
    do {                                                                                                   \
        static u64 ic_;                                                                                    \
        u64 v_ = __atomic_load_n(&ic_, __ATOMIC_RELAXED);                                                  \
        if (__builtin_expect((u32)v_ == (t), 1))                                                           \
            ((guest_fn)((uintptr_t)rt_ic_base + (intptr_t)((s64)v_ >> 32)))(c);                            \
        else                                                                                               \
            rt_call_ic((c), (t), &ic_);                                                                    \
    } while (0)
#define RT_GNATIVE_CALL(c, idx, t)                                                                         \
    do {                                                                                                   \
        u64 *ge_ = &rt_gnatives_cache[(u32)(idx) & (RT_GNATIVE_SIZE - 1)];                                 \
        u64 v_ = __atomic_load_n(ge_, __ATOMIC_RELAXED);                                                   \
        if (__builtin_expect((u32)v_ == (t), 1))                                                           \
            ((guest_fn)((uintptr_t)rt_ic_base + (intptr_t)((s64)v_ >> 32)))(c);                            \
        else                                                                                               \
            rt_call_ic((c), (t), ge_);                                                                     \
    } while (0)
// Проверка сводок по регистрам (генерация с SNX_REGCHECK=1, recomp/regs.py): после прямого вызова регистры вне
// out цели должны остаться такими же; bad — маска разошедшихся (порядок как в CPU), печатается первое по месту.
void rt_regchk_fail(CPU *c, u32 site, u32 target, u32 bad);
#define RT_REGCHK(site, target, bad)                                                                       \
    do {                                                                                                   \
        u32 b_ = (bad);                                                                                    \
        if (b_)                                                                                            \
            rt_regchk_fail(c, (site), (target), b_);                                                       \
    } while (0)
// setjmp/longjmp гостя: продолжение после вызова _setjmp3 ([esp] — адрес возврата, [esp+4] — jmp_buf гостя).
// Переход rt_call на это продолжение с тем же esp (так заканчивает longjmp гостя) — хостовый longjmp сюда.
typedef struct rt_jmp {
    jmp_buf buf;
} rt_jmp;
void rt_setjmp_register(CPU *c, rt_jmp *j);
// Исключения C++ гостя: функция, чьи catch возвращают адрес продолжения (recomp/discover.py), держит в кадре
// хоста rt_eh_frame. _JumpToContinuation гостя прыгает на продолжение с EBP её кадра — rt_eh_continue (из rt_call
// или из функции, заведённой на адресе продолжения) находит кадр по EBP и делает longjmp в обёртку, та входит
// в тело с адресом продолжения. То же для блоков __except (SEH): _except_handler3 переходит на них call-ом с EBP
// кадра.
typedef struct rt_eh_frame {
    rt_jmp j;
    u32 frame, target;  // EBP кадра гостя (esp на входе - 4); адрес продолжения
    int raising;        // шёл ли разбор исключения при входе (кадр внутри блока catch) — восстанавливается при переходе
    struct rt_eh_frame *prev;
} rt_eh_frame;
void rt_eh_enter(CPU *c, rt_eh_frame *f);
void rt_eh_leave(rt_eh_frame *f);
void rt_eh_continue(CPU *c, u32 target);  // нет живого кадра — возврат
int rt_eh_raising(int on);  // RaiseException: идёт разбор исключения; возвращает прежнее значение
void rt_eh_reset(void);    // забыть кадры потока (харнесс дифф-тестов: перед каждым тестом)
guest_fn rt_lookup(u32 target);                        // NULL, если функции нет
guest_fn rt_lookup_k(u32 target, int *native);         // то же; native — функция хоста (HLE, оверрайд)
void rt_invoke(CPU *c, guest_fn fn, int native);       // вызов из рантайма: код гостя — через RT_ENTER_GEN
void rt_trap(CPU *c, u32 addr, const char *what) RT_NORETURN_COLD;      // фатальная ошибка гостя
void rt_unimpl(CPU *c, u32 addr, const char *insn) RT_NORETURN_COLD;    // нереализованная инструкция
void rt_cpuid(u32 leaf, u32 r[4]);  // r: eax, ebx, ecx, edx
u64 rt_rdtsc(void);                 // edx:eax
void rt_divzero(CPU *c, u32 addr) RT_NORETURN_COLD;
void rt_hle_missing(CPU *c, const char *dll, const char *name) RT_NORETURN_COLD;
// rep movs/stos: n элементов по size байт; esi/edi/ecx после — считает вызывающий (REP_MOVS в gen_rt.h)
void rt_rep_movs(u8 *m, u32 s, u32 d, u32 n, int size, int df);
void rt_rep_stos(u8 *m, u32 d, u32 n, u32 val, int size, int df);

// x87
double rt_f80_to_f64(const u8 *p);
void rt_f64_to_f80(u8 *p, double v);
s64 rt_fist(CPU *c, double v, int bits);  // округление по c->fpucw, «неопределённое целое» при переполнении
s64 rt_ftrunc(double v, int bits);        // усечение (fisttp, _ftol), «неопределённое целое» при переполнении
double rt_frndint(CPU *c, double v);

// Регистрация таблиц функций модуля (генерируется рекомпилятором).
typedef struct {
    u32 addr;
    guest_fn fn;
    u32 native;  // 1 — функция хоста (оверрайд): регистры гостя — в CPU
} rt_func_entry;
void rt_register_functions(const rt_func_entry *tab, u32 count);

#ifdef __cplusplus
}
#endif
