// Синтетические тесты быстрых нативных оверрайдов (Шаги 1, 2 и 3):
// - Engine.dll: FConvexVolume::BoxCheck и SphereCheck, MeshSkinVerts (скиннинг), USpriteEmitter::FillVertexBuffer,
//               UShadowBitmapMaterial::Get / FCanvasUtil::DrawTile (1-проходное GPU-размытие теней)
// - D3DDrv.dll: __ehvec_ctor (0x12802100), SetMaterialPass (0x12819eb0), CommitState (0x1281fe80)
// - msvcr70.dll: qsort (быстрый путь 16-байтных записей и общий путь с гостевым компаратором)
// - Core.dll: appMemcmp, FBox::operator+=(FVector), FVector::SafeNormal, FVector::Normalize
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime/abi.h"
#include "runtime/hle/d3d8_direct.h"
#include "runtime/rt.h"

extern void ovr_engine_box_check(CPU *c);
extern void ovr_engine_sphere_check(CPU *c);
extern void ovr_engine_skin_verts(CPU *c);
extern void ovr_engine_sprite_fill_vb(CPU *c);
extern void ovr_engine_shadow_get(CPU *c);
extern void ovr_engine_shadow_draw_tile(CPU *c);
extern void ovr_d3ddrv_ehvec_ctor(CPU *c);
extern void ovr_d3ddrv_set_mat_pass(CPU *c);
extern void ovr_d3ddrv_commit_state(CPU *c);
extern void ovr_crt_qsort(CPU *c);
extern void ovr_core_memcmp(CPU *c);
extern void ovr_core_box_add_vec(CPU *c);
extern void ovr_core_vec_safe_normal(CPU *c);
extern void ovr_core_vec_normalize(CPU *c);

// Заглушки сильных ссылок на ovr_orig_* из overrides_engine.c и overrides_core.c (наши новые — weak)
static void stub_orig(CPU *c) { (void)c; }
void (*const ovr_orig_ovr_engine_camera_matrices)(CPU *c) = stub_orig;
void (*const ovr_orig_ovr_engine_save_game)(CPU *c) = stub_orig;
void (*const ovr_orig_ovr_engine_flush)(CPU *c) = stub_orig;
void (*const ovr_orig_ovr_core_collect_garbage)(CPU *c) = stub_orig;
void (*const ovr_orig_ovr_core_struct_link)(CPU *c) = stub_orig;
void (*const ovr_orig_ovr_core_save_config)(CPU *c) = stub_orig;
void (*const ovr_orig_ovr_core_load_config)(CPU *c) = stub_orig;
void (*const ovr_orig_ovr_core_objit_next)(CPU *c) = stub_orig;

#define STACK 0x00200000u
#define ARENA 0x00300000u
#define CMP_FAST_VA 0x00180000u
#define CMP_GEN_VA 0x00180100u

static int g_fails;

#define CHECK(cond, ...)                                                                                             \
    do {                                                                                                             \
        if (!(cond)) {                                                                                               \
            if (g_fails++ < 32) {                                                                                    \
                printf("FAIL (%s:%d): ", __FILE__, __LINE__);                                                        \
                printf(__VA_ARGS__);                                                                                 \
                printf("\n");                                                                                        \
            }                                                                                                        \
        }                                                                                                            \
    } while (0)

static u32 rng_u32(u32 *s)
{
    *s = *s * 1664525u + 1013904223u;
    return *s;
}

static float rng_f(u32 *s, float lo, float hi)
{
    return lo + (hi - lo) * ((float)(rng_u32(s) >> 8) / 16777216.0f);
}

// --- эталоны BoxCheck / SphereCheck ----------------------------------------------
static u8 ref_box_check(const float *planes, int n, const float o[3], const float e[3])
{
    u8 res = 1u;
    for (int i = 0; i < n; i++) {
        const float *p = planes + i * 4;
        float dist = o[0] * p[0] + o[1] * p[1] + o[2] * p[2] - p[3];
        float push = fabsf(e[0] * p[0]) + fabsf(e[1] * p[1]) + fabsf(e[2] * p[2]);
        if (dist > -push) {
            res = 3u;
            if (dist > push)
                return 2u;
        }
    }
    return res;
}

static u8 ref_sphere_check(const float *planes, int n, const float sph[4])
{
    u8 res = 1u;
    for (int i = 0; i < n; i++) {
        const float *p = planes + i * 4;
        float dist = sph[0] * p[0] + sph[1] * p[1] + sph[2] * p[2] - p[3];
        if (dist > -sph[3]) {
            res = 3u;
            if (dist > sph[3])
                return 2u;
        }
    }
    return res;
}

static u8 call_box_check(u32 vol, u32 orig, u32 ext)
{
    CPU c = {0};
    c.ecx = vol;
    c.esp = STACK - 12u;
    c.ebx = 0x11111111u;
    c.esi = 0x22222222u;
    c.edi = 0x33333333u;
    c.ebp = 0x44444444u;
    wr32(g_mem, c.esp, 0xDEADBEEFu);
    wr32(g_mem, c.esp + 4u, orig);
    wr32(g_mem, c.esp + 8u, ext);
    ovr_engine_box_check(&c);
    CHECK(c.esp == STACK, "BoxCheck esp=%08x", c.esp);
    CHECK(c.ebx == 0x11111111u && c.esi == 0x22222222u && c.edi == 0x33333333u && c.ebp == 0x44444444u,
          "BoxCheck попортил callee-saved");
    return (u8)(c.eax & 0xFFu);
}

static u8 call_sphere_check(u32 vol, u32 sph)
{
    CPU c = {0};
    c.ecx = vol;
    c.esp = STACK - 8u;
    c.ebx = 0x55555555u;
    c.esi = 0x66666666u;
    c.edi = 0x77777777u;
    c.ebp = 0x88888888u;
    wr32(g_mem, c.esp, 0xDEADBEEFu);
    wr32(g_mem, c.esp + 4u, sph);
    ovr_engine_sphere_check(&c);
    CHECK(c.esp == STACK, "SphereCheck esp=%08x", c.esp);
    CHECK(c.ebx == 0x55555555u && c.esi == 0x66666666u && c.edi == 0x77777777u && c.ebp == 0x88888888u,
          "SphereCheck попортил callee-saved");
    return (u8)(c.eax & 0xFFu);
}

static void test_convex_volume(void)
{
    u32 vol = ARENA;
    u32 orig_va = ARENA + 0x400u;
    u32 ext_va = ARENA + 0x410u;
    u32 sph_va = ARENA + 0x420u;

    // 6 плоскостей куба [-10, 10]^3: внешние нормали (±1,0,0), (0,±1,0), (0,0,±1), W = 10
    const float cube[6 * 4] = {
        1,  0,  0, 10, -1, 0,  0, 10,
        0,  1,  0, 10,  0, -1, 0, 10,
        0,  0,  1, 10,  0, 0, -1, 10,
    };
    memcpy(g_mem + vol, cube, sizeof cube);
    wr32(g_mem, vol + 0x200u, 6u);

    // Полностью внутри -> 1
    float o1[3] = {0, 0, 0}, e1[3] = {2, 2, 2}, s1[4] = {0, 0, 0, 5};
    memcpy(g_mem + orig_va, o1, sizeof o1);
    memcpy(g_mem + ext_va, e1, sizeof e1);
    memcpy(g_mem + sph_va, s1, sizeof s1);
    CHECK(call_box_check(vol, orig_va, ext_va) == 1u, "BoxCheck внутри");
    CHECK(call_sphere_check(vol, sph_va) == 1u, "SphereCheck внутри");

    // Пересекает плоскость -> 3
    float o2[3] = {9, 0, 0}, e2[3] = {2, 1, 1}, s2[4] = {9, 0, 0, 2};
    memcpy(g_mem + orig_va, o2, sizeof o2);
    memcpy(g_mem + ext_va, e2, sizeof e2);
    memcpy(g_mem + sph_va, s2, sizeof s2);
    CHECK(call_box_check(vol, orig_va, ext_va) == 3u, "BoxCheck пересекает");
    CHECK(call_sphere_check(vol, sph_va) == 3u, "SphereCheck пересекает");

    // Полностью снаружи -> 2
    float o3[3] = {15, 0, 0}, e3[3] = {2, 2, 2}, s3[4] = {0, 0, -15, 2};
    memcpy(g_mem + orig_va, o3, sizeof o3);
    memcpy(g_mem + ext_va, e3, sizeof e3);
    memcpy(g_mem + sph_va, s3, sizeof s3);
    CHECK(call_box_check(vol, orig_va, ext_va) == 2u, "BoxCheck снаружи");
    CHECK(call_sphere_check(vol, sph_va) == 2u, "SphereCheck снаружи");

    // 0 плоскостей -> 1
    wr32(g_mem, vol + 0x200u, 0u);
    CHECK(call_box_check(vol, orig_va, ext_va) == 1u, "BoxCheck 0 плоскостей");
    CHECK(call_sphere_check(vol, sph_va) == 1u, "SphereCheck 0 плоскостей");

    // NaN в координате -> не срабатывает условие > (возвращает 1)
    wr32(g_mem, vol + 0x200u, 6u);
    float nanv = NAN;
    float onan[3] = {nanv, 0, 0}, snan[4] = {nanv, 0, 0, 1};
    memcpy(g_mem + orig_va, onan, sizeof onan);
    memcpy(g_mem + sph_va, snan, sizeof snan);
    CHECK(call_box_check(vol, orig_va, ext_va) == 1u, "BoxCheck NaN");
    CHECK(call_sphere_check(vol, sph_va) == 1u, "SphereCheck NaN");

    // Разное число плоскостей (1..32, включая хвосты 1..3 после NEON-блоков по 4) против скалярного эталона
    u32 seed = 0x12345678u;
    const int counts[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 13, 16, 31, 32};
    for (size_t ci = 0; ci < sizeof counts / sizeof counts[0]; ci++) {
        int n = counts[ci];
        for (int t = 0; t < 64; t++) {
            float pl[32 * 4];
            for (int i = 0; i < n; i++) {
                float x = rng_f(&seed, -1.0f, 1.0f);
                float y = rng_f(&seed, -1.0f, 1.0f);
                float z = rng_f(&seed, -1.0f, 1.0f);
                float len = sqrtf(x * x + y * y + z * z) + 1e-3f;
                pl[i * 4 + 0] = x / len;
                pl[i * 4 + 1] = y / len;
                pl[i * 4 + 2] = z / len;
                pl[i * 4 + 3] = rng_f(&seed, 0.5f, 20.0f);
            }
            memcpy(g_mem + vol, pl, (size_t)n * 16u);
            wr32(g_mem, vol + 0x200u, (u32)n);

            float o[3] = {rng_f(&seed, -15, 15), rng_f(&seed, -15, 15), rng_f(&seed, -15, 15)};
            float e[3] = {rng_f(&seed, 0, 5), rng_f(&seed, 0, 5), rng_f(&seed, 0, 5)};
            float s[4] = {o[0], o[1], o[2], rng_f(&seed, 0, 6)};
            memcpy(g_mem + orig_va, o, sizeof o);
            memcpy(g_mem + ext_va, e, sizeof e);
            memcpy(g_mem + sph_va, s, sizeof s);

            u8 got_b = call_box_check(vol, orig_va, ext_va);
            u8 want_b = ref_box_check(pl, n, o, e);
            CHECK(got_b == want_b, "BoxCheck n=%d t=%d: got %u != want %u", n, t, got_b, want_b);

            u8 got_s = call_sphere_check(vol, sph_va);
            u8 want_s = ref_sphere_check(pl, n, s);
            CHECK(got_s == want_s, "SphereCheck n=%d t=%d: got %u != want %u", n, t, got_s, want_s);
        }
    }
}

// --- тесты qsort -----------------------------------------------------------------
static void mock_fast_dummy(CPU *c)
{
    // Должен НИКОГДА не вызываться на быстром пути fast16!
    CHECK(0, "fast16 вызвал гостевой компаратор вместо нативного пути");
    c->eax = 0;
    c->esp += 4u;
}

static int g_gen_cmp_calls;
static void mock_gen_cmp_asc(CPU *c)
{
    u32 a = rd32(g_mem, c->esp + 4u), b = rd32(g_mem, c->esp + 8u);
    s32 ka = (s32)rd32(g_mem, a), kb = (s32)rd32(g_mem, b);
    g_gen_cmp_calls++;
    c->eax = (ka < kb) ? 0xFFFFFFFFu : (ka > kb) ? 1u : 0u;
    c->esp += 4u;  // cdecl: ret
}

static void call_qsort(u32 base, u32 num, u32 width, u32 comp)
{
    CPU c = {0};
    c.esp = STACK - 20u;
    c.ebx = 0xAABBCCDDu;
    c.esi = 0x11223344u;
    c.edi = 0x55667788u;
    c.ebp = 0x99AABBCCu;
    wr32(g_mem, c.esp, 0xDEADBEEFu);
    wr32(g_mem, c.esp + 4u, base);
    wr32(g_mem, c.esp + 8u, num);
    wr32(g_mem, c.esp + 12u, width);
    wr32(g_mem, c.esp + 16u, comp);
    ovr_crt_qsort(&c);
    CHECK(c.esp == STACK - 16u, "qsort esp=%08x", c.esp);
    CHECK(c.ebx == 0xAABBCCDDu && c.esi == 0x11223344u && c.edi == 0x55667788u && c.ebp == 0x99AABBCCu,
          "qsort попортил callee-saved");
}

static void test_qsort(void)
{
    // Записываем байты компаратора Engine.dll!0x121a46a0 в гостевую память и регистрируем как гостевую функцию (native=0)
    static const u8 k_cmp_bytes[13] = {0x8B, 0x44, 0x24, 0x08, 0x8B, 0x4C, 0x24, 0x04, 0x8B, 0x00, 0x2B, 0x01, 0xC3};
    memcpy(g_mem + CMP_FAST_VA, k_cmp_bytes, sizeof k_cmp_bytes);
    // Общий компаратор регистрируем с native=1 (безопасно и при SNX_PIN=ON без ассемблерного переходника)
    const rt_func_entry tab[] = {
        {CMP_FAST_VA, mock_fast_dummy, 0u},
        {CMP_GEN_VA, mock_gen_cmp_asc, 1u},
    };
    rt_register_functions(tab, 2u);

    // 1. Быстрый путь fast16: разные размеры, дубликаты, отрицательные ключи, сохранность всех 16 байт
    const u32 sizes[] = {0, 1, 2, 3, 5, 16, 17, 33, 128, 1000};
    u32 seed = 0xCAFEBABEu;
    for (size_t si = 0; si < sizeof sizes / sizeof sizes[0]; si++) {
        u32 n = sizes[si];
        u32 base = ARENA + 0x1000u;
        u64 sum_before = 0;
        for (u32 i = 0; i < n; i++) {
            s32 key;
            if (si == 5)
                key = (s32)(i % 3u) - 1;  // много дубликатов
            else if (si == 6)
                key = (s32)i - 8;  // возрастающий (хуже всего для убывающей сортировки)
            else if (si == 7)
                key = 100 - (s32)i;  // уже отсортирован
            else
                key = (s32)(rng_u32(&seed) & 0x3FFFFFFFu) - 0x1FFFFFFF;
            u32 p1 = (u32)key ^ 0xA5A5A5A5u;
            u32 p2 = i * 2654435761u;
            u32 p3 = (u32)key + p2;
            wr32(g_mem, base + i * 16u + 0u, (u32)key);
            wr32(g_mem, base + i * 16u + 4u, p1);
            wr32(g_mem, base + i * 16u + 8u, p2);
            wr32(g_mem, base + i * 16u + 12u, p3);
            sum_before += (u64)(u32)key + p1 + p2 + p3;
        }
        call_qsort(base, n, 16u, CMP_FAST_VA);
        u64 sum_after = 0;
        for (u32 i = 0; i < n; i++) {
            s32 key = (s32)rd32(g_mem, base + i * 16u + 0u);
            u32 p1 = rd32(g_mem, base + i * 16u + 4u);
            u32 p2 = rd32(g_mem, base + i * 16u + 8u);
            u32 p3 = rd32(g_mem, base + i * 16u + 12u);
            CHECK(p1 == ((u32)key ^ 0xA5A5A5A5u) && p3 == (u32)key + p2, "fast16 порча полезной нагрузки [%u]", i);
            sum_after += (u64)(u32)key + p1 + p2 + p3;
            if (i > 0) {
                s32 prev = (s32)rd32(g_mem, base + (i - 1u) * 16u);
                CHECK(prev >= key, "fast16 порядок нарушен (n=%u, i=%u: %d < %d)", n, i, prev, key);
            }
        }
        CHECK(sum_before == sum_after, "fast16 контрольная сумма не совпала (n=%u)", n);
    }

    // 2. Общий путь: размер элемента 20 Б, восходящая сортировка через гостевой компаратор
    u32 base2 = ARENA + 0x20000u;
    u32 n2 = 150u;
    g_gen_cmp_calls = 0;
    for (u32 i = 0; i < n2; i++) {
        s32 key = (s32)(rng_u32(&seed) % 200u) - 100;
        wr32(g_mem, base2 + i * 20u + 0u, (u32)key);
        wr32(g_mem, base2 + i * 20u + 4u, (u32)key ^ 0x13579BDFu);
        wr32(g_mem, base2 + i * 20u + 16u, (u32)key + 77u);
    }
    call_qsort(base2, n2, 20u, CMP_GEN_VA);
    CHECK(g_gen_cmp_calls > 0, "общий путь qsort должен звать компаратор");
    for (u32 i = 0; i < n2; i++) {
        s32 key = (s32)rd32(g_mem, base2 + i * 20u + 0u);
        u32 p1 = rd32(g_mem, base2 + i * 20u + 4u);
        u32 p4 = rd32(g_mem, base2 + i * 20u + 16u);
        CHECK(p1 == ((u32)key ^ 0x13579BDFu) && p4 == (u32)key + 77u, "qsort gen порча элемента [%u]", i);
        if (i > 0) {
            s32 prev = (s32)rd32(g_mem, base2 + (i - 1u) * 20u);
            CHECK(prev <= key, "qsort gen порядок нарушен [%u]: %d > %d", i, prev, key);
        }
    }
}

// --- тесты Core.dll (appMemcmp, FBox::+=, SafeNormal, Normalize) -----------------
static s32 call_memcmp(u32 a, u32 b, u32 n)
{
    CPU c = {0};
    c.esp = STACK - 16u;
    wr32(g_mem, c.esp, 0xDEADBEEFu);
    wr32(g_mem, c.esp + 4u, a);
    wr32(g_mem, c.esp + 8u, b);
    wr32(g_mem, c.esp + 12u, n);
    ovr_core_memcmp(&c);
    CHECK(c.esp == STACK - 12u, "appMemcmp esp=%08x", c.esp);
    return (s32)c.eax;
}

static void test_core_leaves(void)
{
    // appMemcmp: строго -1, 0, +1, беззнаковое сравнение байт и n=0
    u32 a = ARENA + 0x30000u, b = ARENA + 0x30100u;
    memcpy(g_mem + a, "\x01\x02\x7F\x04", 4);
    memcpy(g_mem + b, "\x01\x02\x80\x04", 4);
    CHECK(call_memcmp(a, a, 4) == 0, "appMemcmp equal");
    CHECK(call_memcmp(a, b, 4) == -1, "appMemcmp 0x7F < 0x80 -> -1");
    CHECK(call_memcmp(b, a, 4) == 1, "appMemcmp 0x80 > 0x7F -> +1");
    CHECK(call_memcmp(a, b, 2) == 0, "appMemcmp prefix equal");
    CHECK(call_memcmp(0, 0, 0) == 0, "appMemcmp n=0");

    // FBox::operator+=(FVector)
    u32 box = ARENA + 0x30200u, vec = ARENA + 0x30240u;
    memset(g_mem + box, 0, 32);
    float v1[3] = {3.0f, -4.0f, 5.0f};
    memcpy(g_mem + vec, v1, sizeof v1);
    CPU c = {0};
    c.ecx = box;
    c.esp = STACK - 8u;
    wr32(g_mem, c.esp, 0xDEADBEEFu);
    wr32(g_mem, c.esp + 4u, vec);
    ovr_core_box_add_vec(&c);
    CHECK(c.eax == box && c.esp == STACK, "FBox::+= init ret");
    CHECK(g_mem[box + 0x18u] == 1u, "FBox::+= IsValid");
    CHECK(rdf32(g_mem, box) == 3.0f && rdf32(g_mem, box + 0x0Cu) == 3.0f &&
              rdf32(g_mem, box + 4u) == -4.0f && rdf32(g_mem, box + 0x10u) == -4.0f &&
              rdf32(g_mem, box + 8u) == 5.0f && rdf32(g_mem, box + 0x14u) == 5.0f,
          "FBox::+= init values");

    float v2[3] = {-1.0f, 2.0f, 10.0f};
    memcpy(g_mem + vec, v2, sizeof v2);
    c.ecx = box;
    c.esp = STACK - 8u;
    wr32(g_mem, c.esp, 0xDEADBEEFu);
    wr32(g_mem, c.esp + 4u, vec);
    ovr_core_box_add_vec(&c);
    CHECK(c.eax == box && c.esp == STACK, "FBox::+= expand ret");
    CHECK(rdf32(g_mem, box) == -1.0f && rdf32(g_mem, box + 4u) == -4.0f && rdf32(g_mem, box + 8u) == 5.0f &&
              rdf32(g_mem, box + 0x0Cu) == 3.0f && rdf32(g_mem, box + 0x10u) == 2.0f &&
              rdf32(g_mem, box + 0x14u) == 10.0f,
          "FBox::+= expand values");

    // FVector::SafeNormal
    u32 v_in = ARENA + 0x30300u, v_out = ARENA + 0x30320u;
    float vn[3] = {3.0f, 4.0f, 0.0f};
    memcpy(g_mem + v_in, vn, sizeof vn);
    c.ecx = v_in;
    c.esp = STACK - 8u;
    wr32(g_mem, c.esp, 0xDEADBEEFu);
    wr32(g_mem, c.esp + 4u, v_out);
    ovr_core_vec_safe_normal(&c);
    CHECK(c.eax == v_out && c.esp == STACK, "SafeNormal ret");
    CHECK(fabsf(rdf32(g_mem, v_out) - 0.6f) < 1e-6f && fabsf(rdf32(g_mem, v_out + 4u) - 0.8f) < 1e-6f &&
              rdf32(g_mem, v_out + 8u) == 0.0f,
          "SafeNormal (3,4,0)");

    // SafeNormal: малый вектор (sq < 1e-8) -> {0, 0, 0}
    float vtiny[3] = {1e-5f, 0.0f, 0.0f};
    memcpy(g_mem + v_in, vtiny, sizeof vtiny);
    c.ecx = v_in;
    c.esp = STACK - 8u;
    wr32(g_mem, c.esp, 0xDEADBEEFu);
    wr32(g_mem, c.esp + 4u, v_out);
    ovr_core_vec_safe_normal(&c);
    CHECK(rdf32(g_mem, v_out) == 0.0f && rdf32(g_mem, v_out + 4u) == 0.0f && rdf32(g_mem, v_out + 8u) == 0.0f,
          "SafeNormal tiny -> 0");

    // SafeNormal: алиасинг out == this
    memcpy(g_mem + v_in, vn, sizeof vn);
    c.ecx = v_in;
    c.esp = STACK - 8u;
    wr32(g_mem, c.esp, 0xDEADBEEFu);
    wr32(g_mem, c.esp + 4u, v_in);
    ovr_core_vec_safe_normal(&c);
    CHECK(fabsf(rdf32(g_mem, v_in) - 0.6f) < 1e-6f && fabsf(rdf32(g_mem, v_in + 4u) - 0.8f) < 1e-6f,
          "SafeNormal aliasing");

    // FVector::Normalize: нормальный -> 1, малый и NaN -> 0 без изменения
    float v3[3] = {0.0f, 3.0f, 4.0f};
    memcpy(g_mem + v_in, v3, sizeof v3);
    c.ecx = v_in;
    c.esp = STACK - 4u;
    wr32(g_mem, c.esp, 0xDEADBEEFu);
    ovr_core_vec_normalize(&c);
    CHECK(c.eax == 1u && c.esp == STACK, "Normalize normal ret");
    CHECK(rdf32(g_mem, v_in) == 0.0f && fabsf(rdf32(g_mem, v_in + 4u) - 0.6f) < 1e-6f &&
              fabsf(rdf32(g_mem, v_in + 8u) - 0.8f) < 1e-6f,
          "Normalize (0,3,4)");

    memcpy(g_mem + v_in, vtiny, sizeof vtiny);
    c.ecx = v_in;
    c.esp = STACK - 4u;
    wr32(g_mem, c.esp, 0xDEADBEEFu);
    ovr_core_vec_normalize(&c);
    CHECK(c.eax == 0u && c.esp == STACK && rdf32(g_mem, v_in) == 1e-5f, "Normalize tiny -> 0 untouched");

    float vnan[3] = {NAN, 1.0f, 2.0f};
    memcpy(g_mem + v_in, vnan, sizeof vnan);
    c.ecx = v_in;
    c.esp = STACK - 4u;
    wr32(g_mem, c.esp, 0xDEADBEEFu);
    ovr_core_vec_normalize(&c);
    CHECK(c.eax == 0u && c.esp == STACK && isnan(rdf32(g_mem, v_in)), "Normalize NaN -> 0 untouched");
}

// --- тесты скиннинга Engine.dll!0x121ed630 (MeshSkinVerts) -----------------------
static u32 pack_skin_nrm(int x, int y, int z)
{
    u32 ux = (u32)(x + 0x1FF) & 0x3FFu;
    u32 uy = (u32)(y + 0x1FF) & 0x3FFu;
    u32 uz = (u32)(z + 0x1FF) & 0x3FFu;
    return ux | (uy << 10) | (uz << 20);
}

static u32 make_skin_word(u32 tag_nibble, u32 weight_hi16, u32 bone_idx)
{
    return (tag_nibble << 28) | ((weight_hi16 & 0xFFFFu) << 12) | ((bone_idx * 6u) & 0xFFFu);
}

static void call_skin_verts(u32 mesh_va, u32 inst_va, u32 dst_va)
{
    CPU c = {0};
    c.esp = STACK - 16u;
    c.ebx = 0x10203040u;
    c.esi = 0x50607080u;
    c.edi = 0x90A0B0C0u;
    c.ebp = 0xD0E0F000u;
    wr32(g_mem, c.esp, 0xDEADBEEFu);
    wr32(g_mem, c.esp + 4u, mesh_va);
    wr32(g_mem, c.esp + 8u, inst_va);
    wr32(g_mem, c.esp + 12u, dst_va);
    ovr_engine_skin_verts(&c);
    CHECK(c.esp == STACK - 12u, "skin_verts esp=%08x", c.esp);
    CHECK(c.ebx == 0x10203040u && c.esi == 0x50607080u && c.edi == 0x90A0B0C0u && c.ebp == 0xD0E0F000u,
          "skin_verts попортил callee-saved");
}

static void ref_skin_verts(const float space[][12], const float ref[][12], int nbones, const u32 *cmd,
                           const float src_pos[][3], const u32 *src_nrm, float out_v[][6], u32 out_uv[][2],
                           int *out_cnt)
{
    double bm[256][12];
    for (int b = 0; b < nbones; b++) {
        const float *sc = space[b], *rf = ref[b];
        for (int r = 0; r < 3; r++) {
            const float *cr = sc + 3 + r * 3;
            bm[b][r] = (double)sc[r] + cr[0] * (double)rf[0] + cr[1] * (double)rf[1] + cr[2] * (double)rf[2];
            for (int col = 0; col < 3; col++) {
                bm[b][3 + r * 3 + col] = cr[0] * (double)rf[3 + col] + cr[1] * (double)rf[6 + col] +
                                         cr[2] * (double)rf[9 + col];
            }
        }
    }
    const u32 scale_bits = 0x31800080u;
    float w_scale_f;
    memcpy(&w_scale_f, &scale_bits, 4u);
    double w_scale = (double)w_scale_f;
    double cache[512][6];
    int cwr = 0, si = 0, oi = 0, ci = 0;
    while (cmd[ci] != 0xFFFFFFFFu) {
        u32 w0 = cmd[ci];
        if (w0 >= 0xF0000000u) {
            u32 off = (w0 & 0x0FFFFFFFu) / 6u;
            for (int k = 0; k < 6; k++)
                out_v[oi][k] = (float)cache[off][k];
            out_uv[oi][0] = cmd[ci + 1];
            out_uv[oi][1] = cmd[ci + 2];
            oi++;
            ci += 3;
            continue;
        }
        double sp[3] = {src_pos[si][0], src_pos[si][1], src_pos[si][2]};
        u32 pn = src_nrm[si++];
        double sn[3] = {(double)((s32)(pn & 0x3FFu) - 0x1FF), (double)((s32)((pn >> 10) & 0x3FFu) - 0x1FF),
                        (double)((s32)((pn >> 20) & 0x3FFu) - 0x1FF)};
        u32 cnt = ((w0 >> 28) & 7u) + 1u;
        double acc[6] = {0, 0, 0, 0, 0, 0};
        if (cnt == 1u) {
            u32 bi = (w0 & 0xFFFu) / 6u;
            for (int r = 0; r < 3; r++) {
                acc[r] = bm[bi][r] + bm[bi][3 + r * 3] * sp[0] + bm[bi][4 + r * 3] * sp[1] + bm[bi][5 + r * 3] * sp[2];
                acc[3 + r] = bm[bi][3 + r * 3] * sn[0] + bm[bi][4 + r * 3] * sn[1] + bm[bi][5 + r * 3] * sn[2];
            }
        } else {
            for (u32 k = 0; k < cnt; k++) {
                u32 wk = cmd[ci + k];
                u32 bi = (wk & 0xFFFu) / 6u;
                double w = (double)(wk & 0x0FFFFFFFu) * w_scale;
                for (int r = 0; r < 3; r++) {
                    double pr =
                        bm[bi][r] + bm[bi][3 + r * 3] * sp[0] + bm[bi][4 + r * 3] * sp[1] + bm[bi][5 + r * 3] * sp[2];
                    double nr = bm[bi][3 + r * 3] * sn[0] + bm[bi][4 + r * 3] * sn[1] + bm[bi][5 + r * 3] * sn[2];
                    acc[r] += pr * w;
                    acc[3 + r] += nr * w;
                }
            }
        }
        if (w0 & 0x80000000u) {
            for (int k = 0; k < 6; k++)
                cache[cwr][k] = acc[k];
            cwr++;
        }
        for (int k = 0; k < 6; k++)
            out_v[oi][k] = (float)acc[k];
        out_uv[oi][0] = cmd[ci + cnt];
        out_uv[oi][1] = cmd[ci + cnt + 1];
        oi++;
        ci += (int)cnt + 2;
    }
    *out_cnt = oi;
}

static void test_skin_verts(void)
{
    u32 base = ARENA + 0x40000u;
    u32 mesh_va = base;
    u32 inst_va = base + 0x400u;
    u32 lod_va = base + 0x800u;
    u32 space_va = base + 0x1000u;
    u32 ref_va = base + 0x4000u;
    u32 cmd_va = base + 0x7000u;
    u32 src_va = base + 0x9000u;
    u32 dst_va = base + 0xB000u;

    memset(g_mem + base, 0, 0x10000u);
    wr32(g_mem, mesh_va + 0x1f0u, lod_va);
    wr32(g_mem, mesh_va + 0x2d8u, ref_va);
    wr32(g_mem, inst_va + 0xbcu, space_va);
    wr32(g_mem, inst_va + 0xf8u, 0u);   // LOD 0
    wr32(g_mem, inst_va + 0x1a8u, 0u);  // без доп. копии

    // 1. Пустой поток (0 вершин, 0 костей)
    wr32(g_mem, inst_va + 0xc0u, 0u);
    wr32(g_mem, lod_va + 0x00u, cmd_va);
    wr32(g_mem, lod_va + 0x0cu, src_va);
    wr32(g_mem, lod_va + 0x110u, 0u);
    wr32(g_mem, cmd_va, 0xFFFFFFFFu);
    wr32(g_mem, dst_va, 0x5A5A5A5Au);
    call_skin_verts(mesh_va, inst_va, dst_va);
    CHECK(rd32(g_mem, dst_va) == 0x5A5A5A5Au, "skin_verts 0 вершин не должен писать в dst");

    // 2. 135 костей (> 128 -> путь malloc) + 9 вершин (1, 1+кеш, 2, 2+кеш, 3, 5 костей + 3 копии из кеша)
    enum { NB = 135, NSRC = 6 };
    static float space[NB][12], ref[NB][12];
    u32 seed = 0x51A1C0DEu;
    for (int b = 0; b < NB; b++) {
        for (int k = 0; k < 3; k++) {
            space[b][k] = rng_f(&seed, -20.0f, 20.0f);
            ref[b][k] = rng_f(&seed, -10.0f, 10.0f);
        }
        for (int k = 3; k < 12; k++) {
            space[b][k] = rng_f(&seed, -1.5f, 1.5f);
            ref[b][k] = rng_f(&seed, -1.5f, 1.5f);
        }
        memcpy(g_mem + space_va + (u32)b * 48u, space[b], 48u);
        memcpy(g_mem + ref_va + (u32)b * 48u, ref[b], 48u);
    }

    float src_pos[NSRC][3] = {
        {1.5f, -2.25f, 3.75f}, {-4.0f, 0.5f, 8.0f},   {0.0f, 12.0f, -6.5f},
        {7.25f, 3.5f, -1.0f},  {-9.5f, -4.25f, 2.0f}, {2.0f, 2.0f, 2.0f},
    };
    u32 src_nrm[NSRC] = {
        pack_skin_nrm(511, 0, 0),     pack_skin_nrm(0, -511, 0),    pack_skin_nrm(300, 300, -250),
        pack_skin_nrm(-120, 400, 90), pack_skin_nrm(10, -20, 500),  pack_skin_nrm(-500, -100, 100),
    };
    for (int i = 0; i < NSRC; i++) {
        memcpy(g_mem + src_va + (u32)i * 16u, src_pos[i], 12u);
        wr32(g_mem + src_va + (u32)i * 16u, 12u, src_nrm[i]);
    }

    u32 cmds[] = {
        // v0: 1 кость (0), без кеша
        make_skin_word(0u, 0xFFFFu, 0u), 0x3F000000u, 0x3E800000u,
        // v1: 1 кость (134), запись в кеш [0]
        make_skin_word(8u, 0xFFFFu, 134u), 0x3F400000u, 0x3E000000u,
        // v2: 2 кости (1, 2), без кеша
        make_skin_word(1u, 0xA000u, 1u), make_skin_word(0u, 0x6000u, 2u), 0x3F800000u, 0x00000000u,
        // v3: 2 кости (10, 130), запись в кеш [1]
        make_skin_word(9u, 0x8000u, 10u), make_skin_word(0u, 0x8000u, 130u), 0x3EAAAAABu, 0x3F2AAAABu,
        // v4: копия из кеша [0] (off = 0) с новыми UV
        0xF0000000u | 0u, 0x11112222u, 0x33334444u,
        // v5: 3 кости (3, 4, 5), запись в кеш [2]
        make_skin_word(0xAu, 0x5555u, 3u), make_skin_word(0u, 0x5555u, 4u), make_skin_word(0u, 0x5556u, 5u),
        0x55556666u, 0x77778888u,
        // v6: 5 костей (0, 10, 20, 30, 134), без кеша
        make_skin_word(4u, 0x3333u, 0u), make_skin_word(0u, 0x3333u, 10u), make_skin_word(0u, 0x3333u, 20u),
        make_skin_word(0u, 0x3333u, 30u), make_skin_word(0u, 0x3334u, 134u), 0x9999AAAAu, 0xBBBBCCCCu,
        // v7: копия из кеша [2] (off = 12)
        0xF0000000u | 12u, 0x12345678u, 0x9ABCDEF0u,
        // v8: копия из кеша [1] (off = 6)
        0xF0000000u | 6u, 0xCAFEBABEu, 0xFEEDFACEu,
        0xFFFFFFFFu,
    };
    memcpy(g_mem + cmd_va, cmds, sizeof cmds);
    wr32(g_mem, inst_va + 0xc0u, (u32)NB);
    wr32(g_mem, lod_va + 0x110u, 300u);  // > 256 -> проверка пути malloc для кеша

    float ref_v[16][6];
    u32 ref_uv[16][2];
    int ref_cnt = 0;
    ref_skin_verts(space, ref, NB, cmds, src_pos, src_nrm, ref_v, ref_uv, &ref_cnt);
    CHECK(ref_cnt == 9, "skin_verts ref_cnt=%d", ref_cnt);

    call_skin_verts(mesh_va, inst_va, dst_va);
    for (int i = 0; i < ref_cnt; i++) {
        float got[6];
        u32 guv[2];
        memcpy(got, g_mem + dst_va + (u32)i * 32u, 24u);
        memcpy(guv, g_mem + dst_va + (u32)i * 32u + 24u, 8u);
        CHECK(guv[0] == ref_uv[i][0] && guv[1] == ref_uv[i][1], "skin_verts v%d UV mismatch", i);
        for (int k = 0; k < 6; k++) {
            float tol = 1e-3f + 1e-4f * fabsf(ref_v[i][k]);
            CHECK(fabsf(got[k] - ref_v[i][k]) <= tol, "skin_verts v%d[%d]: got %.5f != ref %.5f", i, k, got[k],
                  ref_v[i][k]);
        }
    }
}

// --- тесты спрайтовых частиц USpriteEmitter::FillVertexBuffer --------------------
static void call_sprite_fill_vb(u32 em_va, u32 verts_va, u32 node_va)
{
    CPU c = {0};
    c.ecx = em_va;
    c.esp = STACK - 12u;
    c.ebx = 0x21222324u;
    c.esi = 0x31323334u;
    c.edi = 0x41424344u;
    c.ebp = 0x51525354u;
    wr32(g_mem, c.esp, 0xDEADBEEFu);
    wr32(g_mem, c.esp + 4u, verts_va);
    wr32(g_mem, c.esp + 8u, node_va);
    ovr_engine_sprite_fill_vb(&c);
    CHECK(c.eax == 0u && c.esp == STACK, "sprite_fill_vb ret/esp: eax=%u esp=%08x", c.eax, c.esp);
    CHECK(c.ebx == 0x21222324u && c.esi == 0x31323334u && c.edi == 0x41424344u && c.ebp == 0x51525354u,
          "sprite_fill_vb попортил callee-saved");
}

static void check_sprite_vert(u32 verts_va, u32 idx, const float want_p[3], u32 want_col, const float want_uv[4],
                              const char *tag)
{
    float gp[3], guv[4];
    u32 gc;
    memcpy(gp, g_mem + verts_va + idx * 32u, 12u);
    memcpy(&gc, g_mem + verts_va + idx * 32u + 12u, 4u);
    memcpy(guv, g_mem + verts_va + idx * 32u + 16u, 16u);
    for (int k = 0; k < 3; k++)
        CHECK(fabsf(gp[k] - want_p[k]) <= 2e-3f, "%s v%u pos[%d]: got %.4f != %.4f", tag, idx, k, gp[k], want_p[k]);
    CHECK((gc & 0x00FFFFFFu) == (want_col & 0x00FFFFFFu) && abs((int)(gc >> 24) - (int)(want_col >> 24)) <= 1,
          "%s v%u col: got %08x != %08x", tag, idx, gc, want_col);
    for (int k = 0; k < 4; k++)
        CHECK(fabsf(guv[k] - want_uv[k]) <= 1e-4f, "%s v%u uv[%d]: got %.4f != %.4f", tag, idx, k, guv[k], want_uv[k]);
}

static void test_sprite_fill_vb(void)
{
    u32 base = ARENA + 0x60000u;
    u32 em_va = base;
    u32 node_va = base + 0x800u;
    u32 parts_va = base + 0x1000u;
    u32 verts_va = base + 0x3000u;

    memset(g_mem + base, 0, 0x8000u);
    // ScreenToWorld: row0 = (1,0,0,0) -> Right=(1,0,0); row1 = (0,0,-1,0) -> Up=(0,0,1); Axis = Right x Up?
    // Напоминание: Axis = (Up.z*Right.y - Up.y*Right.z, Up.x*Right.z - Up.z*Right.x, Up.y*Right.x - Up.x*Right.y)
    // Для Up=(0,0,1), Right=(1,0,0) получаем Axis = (0, -1, 0).
    float s2w[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 0.0f, -1.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
    };
    float view_orig[3] = {0.0f, -100.0f, 0.0f};
    memcpy(g_mem + node_va + 0x154u, s2w, sizeof s2w);
    memcpy(g_mem + node_va + 0x194u, view_orig, sizeof view_orig);
    wr32(g_mem, em_va + 0x418u, parts_va);

    // 1. 0 активных частиц
    wr32(g_mem, em_va + 0x428u, 0u);
    wr32(g_mem, verts_va, 0x77777777u);
    call_sprite_fill_vb(em_va, verts_va, node_va);
    CHECK(rd32(g_mem, verts_va) == 0x77777777u, "sprite 0 частиц не должен писать в буфер");

    // 2. 5 слотов, 3 активных (слоты 0, 2, 4), UseDirectionAs=7 (PTDU_Scale), без спина и подразбиений
    wr32(g_mem, em_va + 0x428u, 5u);
    g_mem[em_va + 0x4b0u] = 7u;
    const float locs[3][3] = {{10.0f, 20.0f, 30.0f}, {-5.0f, 0.0f, 2.0f}, {1.0f, 2.0f, 3.0f}};
    const float sz[3][2] = {{2.0f, 4.0f}, {3.0f, 1.5f}, {0.5f, 0.5f}};
    const u32 cols[3] = {0xFF112233u, 0x80445566u, 0x40AABBCCu};
    const int slots[3] = {0, 2, 4};
    for (int k = 0; k < 3; k++) {
        u32 pv = parts_va + (u32)slots[k] * 0xc8u;
        memcpy(g_mem + pv, locs[k], 12u);
        wrf32(g_mem, pv + 0x6cu, sz[k][0]);
        wrf32(g_mem, pv + 0x70u, sz[k][1]);
        wr32(g_mem, pv + 0xa8u, cols[k]);
        g_mem[pv + 0xbcu] = 1u;  // Active
        wr32(g_mem, pv + 0xc0u, 0xFFFFFFFFu);
    }
    call_sprite_fill_vb(em_va, verts_va, node_va);
    const float uv_def[4][4] = {{0, 1, 0, 1}, {1, 1, 1, 1}, {0, 0, 0, 0}, {1, 0, 1, 0}};
    for (int k = 0; k < 3; k++) {
        float lx = locs[k][0], ly = locs[k][1], lz = locs[k][2], sx = sz[k][0], sy = sz[k][1];
        float p0[3] = {lx - sx, ly, lz - sy};
        float p1[3] = {lx + sx, ly, lz - sy};
        float p2[3] = {lx - sx, ly, lz + sy};
        float p3[3] = {lx + sx, ly, lz + sy};
        check_sprite_vert(verts_va, (u32)k * 4u + 0u, p0, cols[k], uv_def[0], "scale");
        check_sprite_vert(verts_va, (u32)k * 4u + 1u, p1, cols[k], uv_def[1], "scale");
        check_sprite_vert(verts_va, (u32)k * 4u + 2u, p2, cols[k], uv_def[2], "scale");
        check_sprite_vert(verts_va, (u32)k * 4u + 3u, p3, cols[k], uv_def[3], "scale");
    }

    // 3. SpinParticles=1 (поворот на 90° = 16384 вокруг Axis=(0,-1,0)):
    //    Up=(0,0,2) -> (-2,0,0), Right=(2,0,0) -> (0,0,2) по Rodrigues (Axis x Up = (-2,0,0), Axis x Right = (0,0,2))
    //    + подразбиение 2x2 с BlendBetweenSubdivisions=1 (rel_t = 0.375 -> idx=1, next=2, alpha=0.5*255=127)
    wr32(g_mem, em_va + 0x428u, 1u);
    g_mem[em_va + 0x4b0u] = 0u;  // PTDU_None (Size.X для обеих осей)
    g_mem[em_va + 0x230u] = 1u;  // SpinParticles
    wr32(g_mem, em_va + 0x338u, 2u);
    wr32(g_mem, em_va + 0x33cu, 2u);
    g_mem[em_va + 0x340u] = 1u;  // BlendBetweenSubdivisions
    wrf32(g_mem, parts_va + 0x30u, 8192.0f);   // Spin
    wrf32(g_mem, parts_va + 0x3cu, 8192.0f);   // StartSpin: 8192 + 1.0*8192 = 16384 (90 deg)
    wrf32(g_mem, parts_va + 0xacu, 1.0f);      // Time
    wrf32(g_mem, parts_va + 0xb0u, 1.0f / 0.375f);  // MaxLifetime -> rel_t = 0.375
    call_sprite_fill_vb(em_va, verts_va, node_va);
    // idx=1 -> u_idx=0, v_idx=1 -> U0=0, U1=0.5, V0=0.5, V1=1.0; next=2 -> u2=1, v2=0 -> U2=0.5..1, V2=0..0.5
    u32 want_blend_col = (cols[0] & 0x00FFFFFFu) | (127u << 24);
    float sp0[3] = {10.0f - (-2.0f) - 0.0f, 20.0f, 30.0f - 0.0f - 2.0f};
    float sp3[3] = {10.0f + (-2.0f) + 0.0f, 20.0f, 30.0f + 0.0f + 2.0f};
    float uv_b0[4] = {0.0f, 1.0f, 0.5f, 0.5f};
    float uv_b3[4] = {0.5f, 0.5f, 1.0f, 0.0f};
    check_sprite_vert(verts_va, 0u, sp0, want_blend_col, uv_b0, "spin+blend");
    check_sprite_vert(verts_va, 3u, sp3, want_blend_col, uv_b3, "spin+blend");

    // 4. UseDirectionAs=5 (PTDU_UpAndNormal) + отрицательный Spin (-16384)
    g_mem[em_va + 0x4b0u] = 5u;
    g_mem[em_va + 0x230u] = 0u;
    wr32(g_mem, em_va + 0x338u, 0u);
    float pn[3] = {0.0f, 1.0f, 0.0f};
    memcpy(g_mem + em_va + 0x4c0u, pn, 12u);
    // Loc=(10,20,30), OldLoc=(10,20,25) -> Dir=(0,0,1); Dir x PN = (-1,0,0)
    // UpVec = Dir*Size.Y = (0,0,4), RightVec = (-1,0,0)*Size.X = (-2,0,0)
    float old_l[3] = {10.0f, 20.0f, 25.0f};
    memcpy(g_mem + parts_va + 0x0cu, old_l, 12u);
    call_sprite_fill_vb(em_va, verts_va, node_va);
    float dp0[3] = {10.0f - 0.0f - (-2.0f), 20.0f, 30.0f - 4.0f - 0.0f};
    float dp3[3] = {10.0f + 0.0f + (-2.0f), 20.0f, 30.0f + 4.0f + 0.0f};
    check_sprite_vert(verts_va, 0u, dp0, cols[0], uv_def[0], "dir5");
    check_sprite_vert(verts_va, 3u, dp3, cols[0], uv_def[3], "dir5");
}

// --- моки d3d8_direct.h для автономной линковки perf_ovr_test (без snx_hle) ------
struct d3ddev {
    u32 guest_ptr;
};

static struct d3ddev g_mock_dev;
static u32 g_mock_dev_va;

typedef struct {
    u32 kind;  // 1=rs, 2=tss, 3=xf, 4=tex, 5=light, 6=light_en, 7=vs, 8=ps, 9=stream, 10=ib, 11=blur
    u32 a, b, c;
    float f;
} d3d_call_t;

static d3d_call_t g_d3d_log[128];
static int g_d3d_log_n;
static int g_mock_blur_rc = 1;

static void mock_d3d_reset(void)
{
    g_d3d_log_n = 0;
}

static void mock_d3d_push(u32 kind, u32 a, u32 b, u32 c, float f)
{
    if (g_d3d_log_n < 128)
        g_d3d_log[g_d3d_log_n++] = (d3d_call_t){kind, a, b, c, f};
}

struct d3ddev *d3d8_dev_from_guest(u32 guest_ptr)
{
    return (guest_ptr && guest_ptr == g_mock_dev_va) ? &g_mock_dev : NULL;
}
void d3d8_direct_set_rs(struct d3ddev *d, u32 state, u32 val)
{
    (void)d;
    mock_d3d_push(1u, state, val, 0u, 0.0f);
}
void d3d8_direct_set_tss(struct d3ddev *d, u32 stage, u32 type, u32 val)
{
    (void)d;
    mock_d3d_push(2u, stage, type, val, 0.0f);
}
void d3d8_direct_set_transform(struct d3ddev *d, u32 type, const float *mat16)
{
    (void)d;
    mock_d3d_push(3u, type, 0u, 0u, mat16 ? mat16[0] : 0.0f);
}
void d3d8_direct_set_material(struct d3ddev *d, const void *mat68) { (void)d; (void)mat68; }
void d3d8_direct_set_light(struct d3ddev *d, u32 idx, const void *light104)
{
    (void)d;
    u32 first = 0u;
    if (light104)
        memcpy(&first, light104, 4u);
    mock_d3d_push(5u, idx, first, 0u, 0.0f);
}
void d3d8_direct_light_enable(struct d3ddev *d, u32 idx, u32 enable)
{
    (void)d;
    mock_d3d_push(6u, idx, enable, 0u, 0.0f);
}
void d3d8_direct_set_texture(struct d3ddev *d, u32 stage, u32 tex_guest_ptr)
{
    (void)d;
    mock_d3d_push(4u, stage, tex_guest_ptr, 0u, 0.0f);
}
void d3d8_direct_set_stream(struct d3ddev *d, u32 idx, u32 vb_guest_ptr, u32 stride)
{
    (void)d;
    mock_d3d_push(9u, idx, vb_guest_ptr, stride, 0.0f);
}
void d3d8_direct_set_indices(struct d3ddev *d, u32 ib_guest_ptr, u32 base_vertex)
{
    (void)d;
    mock_d3d_push(10u, ib_guest_ptr, base_vertex, 0u, 0.0f);
}
void d3d8_direct_set_vs(struct d3ddev *d, u32 handle)
{
    (void)d;
    mock_d3d_push(7u, handle, 0u, 0u, 0.0f);
}
void d3d8_direct_set_ps(struct d3ddev *d, u32 handle)
{
    (void)d;
    mock_d3d_push(8u, handle, 0u, 0u, 0.0f);
}
void d3d8_direct_set_vs_const(struct d3ddev *d, u32 reg, const float *data, u32 count)
{
    (void)d; (void)reg; (void)data; (void)count;
}
void d3d8_direct_set_ps_const(struct d3ddev *d, u32 reg, const float *data, u32 count)
{
    (void)d; (void)reg; (void)data; (void)count;
}
void d3d8_direct_set_viewport(struct d3ddev *d, const void *vp24) { (void)d; (void)vp24; }
void d3d8_direct_set_clip_plane(struct d3ddev *d, u32 idx, const float *plane4)
{
    (void)d; (void)idx; (void)plane4;
}
int d3d8_direct_shadow_blur(struct d3ddev *d, u32 src_tex_guest, u32 dst_surf_guest, float step_px)
{
    (void)d;
    mock_d3d_push(11u, src_tex_guest, dst_surf_guest, 0u, step_px);
    return g_mock_blur_rc;
}

// --- тесты D3DDrv: __ehvec_ctor, SetMaterialPass, CommitState --------------------
#ifdef SNX_PIN
// В сборке с SNX_PIN (linux-arm64 / Switch) оверрайды вызывают ovr_orig_* через rt_pin_enter (runtime/rt.c),
// который грузит w19..w26 из CPU *c перед вызовом и записывает w19..w26 обратно в CPU *c после возврата.
// Обычный C-мок меняет поля c->esp/c->eax в памяти и сохраняет старые w19..w26, поэтому нужен переходник.
#define DEFINE_PIN_MOCK(pin_fn, body_fn)                                                                              \
    extern void pin_fn(CPU *c);                                                                                       \
    __asm__(".text\n"                                                                                                 \
            ".global " #pin_fn "\n"                                                                                   \
            ".type " #pin_fn ", %function\n"                                                                          \
            ".p2align 2\n"                                                                                            \
            #pin_fn ":\n"                                                                                             \
            "    stp x29, x30, [sp, #-32]!\n"                                                                         \
            "    mov x29, sp\n"                                                                                       \
            "    str x0, [sp, #16]\n"                                                                                 \
            "    stp w19, w20, [x0]\n"                                                                                \
            "    stp w21, w22, [x0, #8]\n"                                                                            \
            "    stp w23, w24, [x0, #16]\n"                                                                           \
            "    stp w25, w26, [x0, #24]\n"                                                                           \
            "    bl " #body_fn "\n"                                                                                   \
            "    ldr x0, [sp, #16]\n"                                                                                 \
            "    ldp w19, w20, [x0]\n"                                                                                \
            "    ldp w21, w22, [x0, #8]\n"                                                                            \
            "    ldp w23, w24, [x0, #16]\n"                                                                           \
            "    ldp w25, w26, [x0, #24]\n"                                                                           \
            "    ldp x29, x30, [sp], #32\n"                                                                           \
            "    ret\n"                                                                                               \
            ".size " #pin_fn ", .-" #pin_fn "\n")
#define PIN_MOCK_SYM(pin_fn, body_fn) pin_fn
#else
#define DEFINE_PIN_MOCK(pin_fn, body_fn) extern void body_fn(CPU *c)
#define PIN_MOCK_SYM(pin_fn, body_fn) body_fn
#endif

static int g_ehvec_orig_calls;
void mock_ehvec_orig_body(CPU *c)
{
    g_ehvec_orig_calls++;
    c->esp += 20u;
}
DEFINE_PIN_MOCK(mock_ehvec_orig_pin, mock_ehvec_orig_body);
void (*const ovr_orig_ovr_d3ddrv_ehvec_ctor)(CPU *c) = PIN_MOCK_SYM(mock_ehvec_orig_pin, mock_ehvec_orig_body);

static void test_d3ddrv_ehvec_ctor(void)
{
    // Гостевые адреса кода >= 0x10000000: прямой no-op, переходник JMP -> no-op, и непустой конструктор
    u32 noop_va = 0x12819150u;
    u32 thunk_va = 0x12801140u;
    u32 real_va = 0x12819200u;
    g_mem[noop_va + 0u] = 0x8Bu;
    g_mem[noop_va + 1u] = 0xC1u;
    g_mem[noop_va + 2u] = 0xC3u;
    g_mem[thunk_va] = 0xE9u;
    wr32(g_mem, thunk_va + 1u, noop_va - (thunk_va + 5u));
    g_mem[real_va + 0u] = 0x55u;  // push ebp — реальный конструктор
    g_mem[real_va + 1u] = 0x8Bu;
    g_mem[real_va + 2u] = 0xECu;

    g_ehvec_orig_calls = 0;
    const u32 ctors[3] = {noop_va, thunk_va, real_va};
    for (int i = 0; i < 3; i++) {
        CPU c = {0};
        c.esp = STACK - 20u;
        wr32(g_mem, c.esp, 0xDEADBEEFu);
        wr32(g_mem, c.esp + 4u, ARENA);
        wr32(g_mem, c.esp + 8u, 8u);
        wr32(g_mem, c.esp + 12u, 8u);
        wr32(g_mem, c.esp + 16u, ctors[i]);
        ovr_d3ddrv_ehvec_ctor(&c);
        CHECK(c.esp == STACK, "ehvec_ctor[%d] esp=%08x", i, c.esp);
    }
    CHECK(g_ehvec_orig_calls == 1, "ehvec_ctor: no-op и JMP->no-op должны пропускаться, real=%d", g_ehvec_orig_calls);
}

static void test_d3ddrv_material_and_commit(void)
{
    u32 base = ARENA + 0x80000u;
    u32 ri_va = base;                 // FD3DRenderInterface (нужен +0x4 и +0x2aa0c)
    u32 cur_st_va = base + 0x2B000u;  // CurrentState (нужен +0x550)
    u32 rendev_va = base + 0x2C000u;  // UD3DRenderDevice (нужны +0x58, +0x4114, +0x4150, +0x41f8, +0x4204, +0x4220, +0x46cc, +0x2f6b0)
    u32 sc_va = rendev_va + 0x2f6b0u; // FD3DStateCache (+0..+0x124c)
    u32 pass_va = base + 0x60000u;    // FD3DMaterialState
    u32 tex_obj_va = base + 0x61000u; // FD3DTexture
    u32 d3d_dev_va = base + 0x62000u;
    u32 tex2d_va = 0x20001000u;
    u32 vb_va = 0x20002000u;
    u32 ib_va = 0x20003000u;

    memset(g_mem + base, 0, 0x64000u);
    g_mock_dev_va = d3d_dev_va;
    g_mock_dev.guest_ptr = d3d_dev_va;

    wr32(g_mem, ri_va + 4u, rendev_va);
    wr32(g_mem, ri_va + 0x2aa0cu, cur_st_va);
    wr32(g_mem, rendev_va + 0x4150u, 0xBF000000u);  // default LOD bias = -0.5f
    wr32(g_mem, rendev_va + 0x41f8u, 2u);           // 2 texture stages
    wr32(g_mem, rendev_va + 0x4204u, 2u);           // 2 lights
    wr32(g_mem, rendev_va + 0x4220u, 2u);           // 2 vertex streams
    wr32(g_mem, rendev_va + 0x46ccu, d3d_dev_va);
    wr32(g_mem, sc_va, rendev_va);

    // Заполняем FD3DMaterialState: 1 активная стадия (стадия 1 должна сброситься в DISABLE=1)
    wr32(g_mem, tex_obj_va + 0x2cu, tex2d_va);  // Texture2D (CubeTexture +0x30 == 0)
    wr32(g_mem, pass_va + 0x00u, 0u);           // PixelShader = 0
    wr32(g_mem, pass_va + 0x04u, 1u);           // AlphaBlendEnable = 1
    wr32(g_mem, pass_va + 0x08u, 1u);           // AlphaTestEnable = 1
    g_mem[pass_va + 0x0cu] = 0x7Fu;             // AlphaRef = 127
    wr32(g_mem, pass_va + 0x10u, 1u);           // ZTest = 1 -> ZFunc = 4 (LESSEQUAL)
    wr32(g_mem, pass_va + 0x14u, 0u);           // ZWrite = 0
    wr32(g_mem, pass_va + 0x18u, 0u);           // TwoSided = 0 -> использовать cull_mode arg
    wr32(g_mem, pass_va + 0x1cu, 2u);           // FillMode = 2 -> RS[0]=3 (SOLID)
    wr32(g_mem, pass_va + 0x20u, 0xAABBCCDDu);  // TextureFactor
    wr32(g_mem, pass_va + 0x24u, 5u);           // SrcBlend = SRCALPHA
    wr32(g_mem, pass_va + 0x28u, 6u);           // DestBlend = INVSRCALPHA
    wr32(g_mem, pass_va + 0x34u, 1u);           // NumStages = 1
    // Стадия 0
    wr32(g_mem, pass_va + 0x38u, tex_obj_va);
    wr32(g_mem, pass_va + 0x3cu, 3u);           // AddressU = CLAMP
    wr32(g_mem, pass_va + 0x40u, 3u);           // AddressV = CLAMP
    wr32(g_mem, pass_va + 0x44u, 1u);           // AddressW = WRAP
    wrf32(g_mem, pass_va + 0x48u, -200.0f);     // < -100 -> заменится на default (-0.5f)
    wr32(g_mem, pass_va + 0x4cu, 4u);           // ColorOp = MODULATE
    wr32(g_mem, pass_va + 0x50u, 2u);           // AlphaOp = SELECTARG1
    wr32(g_mem, pass_va + 0x58u, 2u);           // ColorArg1 = TEXTURE
    wr32(g_mem, pass_va + 0x5cu, 0u);           // ColorArg2 = DIFFUSE
    wr32(g_mem, pass_va + 0x74u, 2u);           // TexTransformFlags = COUNT2
    wr32(g_mem, pass_va + 0x78u, 1u);           // UseTexTransform = 1
    wrf32(g_mem, pass_va + 0x7cu, 2.5f);        // M[0][0] = 2.5f

    CPU c = {0};
    c.ecx = ri_va;
    c.esp = STACK - 12u;
    wr32(g_mem, c.esp, 0xDEADBEEFu);
    wr32(g_mem, c.esp + 4u, pass_va);
    wr32(g_mem, c.esp + 8u, 3u);  // CullMode = CCW
    ovr_d3ddrv_set_mat_pass(&c);
    CHECK(c.esp == STACK, "SetMaterialPass esp=%08x", c.esp);
    CHECK(rd32(g_mem, cur_st_va + 0x550u) == pass_va, "SetMaterialPass current pass");
    CHECK(rd32(g_mem, sc_va + 4u + 0u * 4u) == 3u && rd32(g_mem, sc_va + 4u + 5u * 4u) == 3u &&
              rd32(g_mem, sc_va + 4u + 6u * 4u) == 4u && rd32(g_mem, sc_va + 4u + 7u * 4u) == 0x7Fu &&
              rd32(g_mem, sc_va + 4u + 9u * 4u) == 1u && rd32(g_mem, sc_va + 4u + 0x17u * 4u) == 0xAABBCCDDu,
          "SetMaterialPass RS");
    CHECK(rd32(g_mem, sc_va + 0x5a8u) == tex2d_va && rd32(g_mem, sc_va + 0x5acu) == 0u, "SetMaterialPass textures");
    CHECK(rd32(g_mem, pass_va + 0x48u) == 0xBF000000u && rd32(g_mem, sc_va + 0x74u + 14u * 4u) == 0xBF000000u,
          "SetMaterialPass default LOD bias");
    CHECK((rd32(g_mem, sc_va + 0x514u) & 1u) != 0u && rdf32(g_mem, sc_va + 0x254u) == 2.5f, "SetMaterialPass tex xf");
    CHECK(rd32(g_mem, sc_va + 0x74u + 0x3cu) == 1u && rd32(g_mem, sc_va + 0x74u + 0x3cu + 12u) == 1u,
          "SetMaterialPass stage1 disabled");

    // Добавляем VB/IB/VS/Light0 и вызываем CommitState
    wr32(g_mem, sc_va + 0x518u, 0x142u);     // VS
    wr32(g_mem, sc_va + 0x520u, vb_va);      // Stream 0 VB
    wr32(g_mem, sc_va + 0x524u, 32u);        // Stream 0 stride
    wr32(g_mem, sc_va + 0x5a0u, ib_va);      // IB
    wr32(g_mem, sc_va + 0x5a4u, 100u);       // BaseVertex
    wr32(g_mem, sc_va + 0x908u, 1u);         // Light 0 enabled
    wr32(g_mem, sc_va + 0x5c8u, 3u);         // Light 0 Type = DIRECTIONAL

    mock_d3d_reset();
    c.ecx = sc_va;
    c.esp = STACK - 4u;
    wr32(g_mem, c.esp, 0xDEADBEEFu);
    ovr_d3ddrv_commit_state(&c);
    CHECK(c.esp == STACK, "CommitState esp=%08x", c.esp);
    CHECK(rd32(g_mem, sc_va + 0x514u) == 0u, "CommitState очищает dirty-маску матриц");
    CHECK(g_d3d_log_n > 10, "CommitState должен отправить дельты (%d)", g_d3d_log_n);

    int seen_tex = 0, seen_xf = 0, seen_vs = 0, seen_vb = 0, seen_ib = 0, seen_light = 0, seen_len = 0;
    for (int i = 0; i < g_d3d_log_n; i++) {
        d3d_call_t e = g_d3d_log[i];
        if (e.kind == 4u && e.a == 0u && e.b == tex2d_va)
            seen_tex = 1;
        if (e.kind == 3u && e.a == 0x10u && e.f == 2.5f)
            seen_xf = 1;
        if (e.kind == 7u && e.a == 0x142u)
            seen_vs = 1;
        if (e.kind == 9u && e.a == 0u && e.b == vb_va && e.c == 32u)
            seen_vb = 1;
        if (e.kind == 10u && e.a == ib_va && e.b == 100u)
            seen_ib = 1;
        if (e.kind == 5u && e.a == 0u && e.b == 3u)
            seen_light = 1;
        if (e.kind == 6u && e.a == 0u && e.b == 1u)
            seen_len = 1;
    }
    CHECK(seen_tex && seen_xf && seen_vs && seen_vb && seen_ib && seen_light && seen_len,
          "CommitState вызовы: tex=%d xf=%d vs=%d vb=%d ib=%d light=%d len=%d", seen_tex, seen_xf, seen_vs, seen_vb,
          seen_ib, seen_light, seen_len);

    // Повторный CommitState без изменений: 0 вызовов RS/TSS/XF/Tex/VS/VB/IB (только активный Light 0, как в оригинале!)
    mock_d3d_reset();
    c.ecx = sc_va;
    c.esp = STACK - 4u;
    wr32(g_mem, c.esp, 0xDEADBEEFu);
    ovr_d3ddrv_commit_state(&c);
    CHECK(g_d3d_log_n == 1 && g_d3d_log[0].kind == 5u, "CommitState повторный: ожидался 1 SetLight, получили %d",
          g_d3d_log_n);
}

// --- тесты перехвата 9-проходного размытия теней UShadowBitmapMaterial::Get ------
static int g_draw_tile_orig_calls;
static u32 g_shadow_blur_mat_va, g_shadow_other_mat_va;

void mock_draw_tile_orig_body(CPU *c)
{
    g_draw_tile_orig_calls++;
    c->esp += 0x30u;  // ret 0x2c
}
DEFINE_PIN_MOCK(mock_draw_tile_orig_pin, mock_draw_tile_orig_body);
void (*const ovr_orig_ovr_engine_shadow_draw_tile)(CPU *c) =
    PIN_MOCK_SYM(mock_draw_tile_orig_pin, mock_draw_tile_orig_body);

void mock_shadow_get_orig_body(CPU *c)
{
    u32 saved_esp = c->esp;
    // Имитируем 9 вызовов FCanvasUtil::DrawTile для размытия + 1 посторонний DrawTile
    for (int i = 0; i < 9; i++) {
        CPU tc = {0};
        tc.esp = STACK - 0x400u - 0x30u;
        wr32(g_mem, tc.esp, 0xDEADBEEFu);
        wr32(g_mem, tc.esp + 0x28u, g_shadow_blur_mat_va);
        ovr_engine_shadow_draw_tile(&tc);
        CHECK(tc.esp == STACK - 0x400u, "shadow blur DrawTile[%d] esp=%08x", i, tc.esp);
    }
    CPU oc = {0};
    oc.esp = STACK - 0x400u - 0x30u;
    wr32(g_mem, oc.esp, 0xDEADBEEFu);
    wr32(g_mem, oc.esp + 0x28u, g_shadow_other_mat_va);
    ovr_engine_shadow_draw_tile(&oc);
    CHECK(oc.esp == STACK - 0x400u, "non-blur DrawTile esp=%08x", oc.esp);

    c->eax = c->ecx;
    c->esp = saved_esp + 16u;  // ret 0xc
}
DEFINE_PIN_MOCK(mock_shadow_get_orig_pin, mock_shadow_get_orig_body);
void (*const ovr_orig_ovr_engine_shadow_get)(CPU *c) =
    PIN_MOCK_SYM(mock_shadow_get_orig_pin, mock_shadow_get_orig_body);

static void test_shadow_blur_intercept(void)
{
    u32 base = ARENA + 0x100000u;
    u32 smat_va = base;              // UShadowBitmapMaterial
    u32 vp_va = base + 0x200u;       // UViewport
    u32 ri_va = base + 0x600u;       // FD3DRenderInterface
    u32 rendev_va = base + 0x1000u;  // UD3DRenderDevice (с хеш-таблицей +0xc8)
    u32 src_rt_va = base + 0x8000u;  // FRenderTarget (ShadowTexture)
    u32 res_va = base + 0x8100u;     // FD3DTexture
    u32 fb_va = base + 0x8200u;      // UFinalBlend (mode=8)
    u32 proxy_va = base + 0x8300u;   // UProxyBitmapMaterial
    u32 other_va = base + 0x8400u;   // другой материал
    u32 d3d_dev_va = base + 0x8500u;
    u32 src_tex_va = 0x20009000u;

    memset(g_mem + base, 0, 0x9000u);
    g_mock_dev_va = d3d_dev_va;
    g_mock_dev.guest_ptr = d3d_dev_va;
    g_mock_blur_rc = 1;
    g_shadow_blur_mat_va = fb_va;
    g_shadow_other_mat_va = other_va;

    u32 cache_lo = 0x123456E0u, cache_hi = 0x00000007u;
    u32 h = (((cache_lo >> 4) & 0xFF0u) + ((cache_lo >> 16) & 0xFu)) ^ (cache_lo & 0xFFFu);
    wr32(g_mem, src_rt_va + 4u, cache_lo);
    wr32(g_mem, src_rt_va + 8u, cache_hi);
    wr32(g_mem, res_va + 8u, cache_lo);
    wr32(g_mem, res_va + 0xcu, cache_hi);
    wr32(g_mem, res_va + 0x2cu, src_tex_va);
    wr32(g_mem, rendev_va + 0xc8u + h * 4u, res_va);
    wr32(g_mem, rendev_va + 0x46ccu, d3d_dev_va);
    wr32(g_mem, ri_va + 4u, rendev_va);
    wr32(g_mem, vp_va + 0x14cu, ri_va);
    wr32(g_mem, smat_va + 0x58u, src_rt_va);
    g_mem[fb_va + 0x40u] = 8u;
    wr32(g_mem, fb_va + 0x3cu, proxy_va);
    wr32(g_mem, proxy_va + 0x54u, src_rt_va);
    g_mem[other_va + 0x40u] = 2u;  // не FrameBufferBlending=8

    mock_d3d_reset();
    g_draw_tile_orig_calls = 0;
    CPU c = {0};
    c.ecx = smat_va;
    c.esp = STACK - 16u;
    wr32(g_mem, c.esp, 0xDEADBEEFu);
    wr32(g_mem, c.esp + 12u, vp_va);
    ovr_engine_shadow_get(&c);
    CHECK(c.esp == STACK && c.eax == smat_va, "shadow_get esp/eax");
    CHECK(g_d3d_log_n == 1 && g_d3d_log[0].kind == 11u && g_d3d_log[0].a == src_tex_va && g_d3d_log[0].b == 0u &&
              g_d3d_log[0].f == 1.5f,
          "shadow_blur вызван 1 раз с верным src_tex (n=%d)", g_d3d_log_n);
    CHECK(g_draw_tile_orig_calls == 1, "shadow_get: 9 тайлов размытия заменены, посторонний прошёл (%d)",
          g_draw_tile_orig_calls);

    // Вне UShadowBitmapMaterial::Get вызов DrawTile всегда уходит в оригинал
    CPU tc = {0};
    tc.esp = STACK - 0x30u;
    wr32(g_mem, tc.esp, 0xDEADBEEFu);
    wr32(g_mem, tc.esp + 0x28u, fb_va);
    ovr_engine_shadow_draw_tile(&tc);
    CHECK(g_draw_tile_orig_calls == 2, "DrawTile вне shadow_get идёт в оригинал (%d)", g_draw_tile_orig_calls);
}

int main(void)
{
    if (rt_mem_init() != 0)
        return 2;
    test_convex_volume();
    test_qsort();
    test_core_leaves();
    test_skin_verts();
    test_sprite_fill_vb();
    test_d3ddrv_ehvec_ctor();
    test_d3ddrv_material_and_commit();
    test_shadow_blur_intercept();
    printf("perf_ovr_test: %d failed\n", g_fails);
    return g_fails != 0;
}
