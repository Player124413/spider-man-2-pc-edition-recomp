// Синтетический тест транслятора пиксельных шейдеров ps.1.1 / ps.1.3 / ps.1.4 -> GLSL (runtime/hle/d3d8_ps.c):
// def, tex/texld, texcoord/texcrd, texkill, mov, add, sub, mul, mad, dp3, dp4, lrp, cnd, cmp,
// модификаторы источников, сдвиги назначения, насыщение, маски записи, перестановки компонент и битые/усечённые токены.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime/hle/d3d8_int.h"

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

#define PS_VER(maj, min) (0xFFFF0000u | ((u32)(maj) << 8) | (u32)(min))
#define PS_END 0x0000FFFFu
#define PS_PHASE 0x0000FFFDu
#define PS_NOP 0x00000000u
#define PS_COMMENT(nwords) (0x0000FFFEu | (((u32)(nwords) & 0x7FFFu) << 16))

// Типы регистров (совпадают с D3DSHADER_PARAM_REGISTER_TYPE)
enum { REG_TEMP = 0, REG_INPUT = 1, REG_CONST = 2, REG_TEXTURE = 3 };

// Токен назначения: маска записи (0xF = xyzw), sat (1 = _sat), shift (-3..+3 в 4-битном доп. коде)
static u32 dst_tok(u32 type, u32 idx, u32 mask, u32 sat, int shift)
{
    u32 sh = (u32)(shift & 0xF);
    return 0x80000000u | ((type & 7u) << 28) | (sh << 24) | ((sat & 0xFu) << 20) | ((mask & 0xFu) << 16) |
           (idx & 0x7FFu);
}

// Токен источника: перестановка (0xE4 = xyzw), модификатор (0..11)
static u32 src_tok(u32 type, u32 idx, u32 swizzle, u32 mod)
{
    return 0x80000000u | ((type & 7u) << 28) | ((mod & 0xFu) << 24) | ((swizzle & 0xFFu) << 16) | (idx & 0x7FFu);
}

static u32 fbits(float f)
{
    u32 u;
    memcpy(&u, &f, 4);
    return u;
}

static void expect_sub(const char *glsl, const char *sub, const char *tag)
{
    CHECK(glsl != NULL && strstr(glsl, sub) != NULL, "%s: в GLSL нет ожидаемого фрагмента \"%s\"", tag, sub);
}

static void test_ps11(void)
{
    const u32 tokens[] = {
        PS_VER(1, 1),
        PS_COMMENT(2), 0x11223344u, 0x55667788u,
        PS_NOP,
        // def c0, 0.5, 0.25, 0.75, 1.0
        81u, dst_tok(REG_CONST, 0, 0xF, 0, 0), fbits(0.5f), fbits(0.25f), fbits(0.75f), fbits(1.0f),
        // tex t0
        66u, dst_tok(REG_TEXTURE, 0, 0xF, 0, 0),
        // texcoord t1
        64u, dst_tok(REG_TEXTURE, 1, 0xF, 0, 0),
        // mov_sat r0.xyz, v0
        1u, dst_tok(REG_TEMP, 0, 0x7, 1, 0), src_tok(REG_INPUT, 0, 0xE4, 0),
        // add_x2 r1, t0, c0
        2u, dst_tok(REG_TEMP, 1, 0xF, 0, 1), src_tok(REG_TEXTURE, 0, 0xE4, 0), src_tok(REG_CONST, 0, 0xE4, 0),
        // sub_d2 r1.w, r1.w, t1.w
        3u, dst_tok(REG_TEMP, 1, 0x8, 0, -1), src_tok(REG_TEMP, 1, 0xFF, 0), src_tok(REG_TEXTURE, 1, 0xFF, 0),
        // mul r0, r0, r1
        5u, dst_tok(REG_TEMP, 0, 0xF, 0, 0), src_tok(REG_TEMP, 0, 0xE4, 0), src_tok(REG_TEMP, 1, 0xE4, 0),
        // mad r0, r0, c1, v1
        4u, dst_tok(REG_TEMP, 0, 0xF, 0, 0), src_tok(REG_TEMP, 0, 0xE4, 0), src_tok(REG_CONST, 1, 0xE4, 0),
        src_tok(REG_INPUT, 1, 0xE4, 0),
        // dp3 r2.xyz, t0_bx2, v0_bias
        8u, dst_tok(REG_TEMP, 2, 0x7, 0, 0), src_tok(REG_TEXTURE, 0, 0xE4, 4), src_tok(REG_INPUT, 0, 0xE4, 2),
        // dp4 r2.w, r0, c0
        9u, dst_tok(REG_TEMP, 2, 0x8, 0, 0), src_tok(REG_TEMP, 0, 0xE4, 0), src_tok(REG_CONST, 0, 0xE4, 0),
        // lrp_sat r0, c0.a, r1, r2
        18u, dst_tok(REG_TEMP, 0, 0xF, 1, 0), src_tok(REG_CONST, 0, 0xFF, 0), src_tok(REG_TEMP, 1, 0xE4, 0),
        src_tok(REG_TEMP, 2, 0xE4, 0),
        // cnd r0, r0, r1, r2 (ps.1.1 — по альфе .a > 0.5)
        80u, dst_tok(REG_TEMP, 0, 0xF, 0, 0), src_tok(REG_TEMP, 0, 0xE4, 0), src_tok(REG_TEMP, 1, 0xE4, 0),
        src_tok(REG_TEMP, 2, 0xE4, 0),
        PS_END,
    };
    u32 h = d3d8_ps_create(tokens, (u32)(sizeof tokens / sizeof tokens[0]));
    CHECK(h != 0, "ps.1.1: d3d8_ps_create вернул 0");
    const char *s = d3d8_ps_glsl(h);
    expect_sub(s, "// ps.1.1", "ps.1.1");
    expect_sub(s, "c0 = vec4(0.5, 0.25, 0.75, 1.0);", "ps.1.1 def");
    expect_sub(s, "texture(u_tex0, tc0.xy)", "ps.1.1 tex");
    expect_sub(s, "clamp(tc1, 0.0, 1.0)", "ps.1.1 texcoord");
    expect_sub(s, "r0.xyz = clamp(vec4(v0).xyz, 0.0, 1.0);", "ps.1.1 mov_sat");
    expect_sub(s, "(t0 + c0) * 2.0", "ps.1.1 add_x2");
    expect_sub(s, "(r1.wwww - t1.wwww) * 0.5", "ps.1.1 sub_d2");
    expect_sub(s, "r0 * r1", "ps.1.1 mul");
    expect_sub(s, "r0 * c1 + v1", "ps.1.1 mad");
    expect_sub(s, "vec4(dot(vec3((2.0 * (t0 - 0.5))), vec3((v0 - 0.5))))", "ps.1.1 dp3");
    expect_sub(s, "vec4(dot(vec4(r0), vec4(c0)))", "ps.1.1 dp4");
    expect_sub(s, "mix(r2, r1, c0.wwww)", "ps.1.1 lrp");
    expect_sub(s, "(vec4(r0).a > 0.5 ? vec4(r1) : vec4(r2))", "ps.1.1 cnd");
    expect_sub(s, "cur = r0;", "ps.1.1 epilogue");
    u32 ntok = 0;
    CHECK(d3d8_ps_hash(h) != 0, "ps.1.1: хеш байткода не должен быть 0");
    CHECK(d3d8_ps_tokens(h, &ntok) != NULL && ntok == (u32)(sizeof tokens / sizeof tokens[0]),
          "ps.1.1: сохранённые токены (%u)", ntok);
    d3d8_ps_delete(h);
    CHECK(d3d8_ps_glsl(h) == NULL, "ps.1.1: после delete d3d8_ps_glsl должен вернуть NULL");
    CHECK(d3d8_ps_hash(h) == 0, "ps.1.1: после delete хеш должен быть 0");
}

static void test_ps13_mods_shifts_texkill(void)
{
    const u32 tokens[] = {
        PS_VER(1, 3),
        // texkill t0
        65u, dst_tok(REG_TEXTURE, 0, 0xF, 0, 0),
        // Все оставшиеся модификаторы источника и сдвиги назначения (_x4, _x8, _d4, _d8) и произвольные маски/свиззлы
        // mov_x4 r0.xz, -v0.yxzw
        1u, dst_tok(REG_TEMP, 0, 0x5, 0, 2), src_tok(REG_INPUT, 0, 0xE1, 1),
        // add_x8 r1, (0.5 - v0), (-2*(v1-0.5))
        2u, dst_tok(REG_TEMP, 1, 0xF, 0, 3), src_tok(REG_INPUT, 0, 0xE4, 3), src_tok(REG_INPUT, 1, 0xE4, 5),
        // mul_d4 r2, (1 - r0), (2 * r1)
        5u, dst_tok(REG_TEMP, 2, 0xF, 0, -2), src_tok(REG_TEMP, 0, 0xE4, 6), src_tok(REG_TEMP, 1, 0xE4, 7),
        // sub_d8 r0, (-2 * r2), abs(c0.xxxx)
        3u, dst_tok(REG_TEMP, 0, 0xF, 0, -3), src_tok(REG_TEMP, 2, 0xE4, 8), src_tok(REG_CONST, 0, 0x00, 11),
        // cmp r0, r0, r1, r2
        88u, dst_tok(REG_TEMP, 0, 0xF, 0, 0), src_tok(REG_TEMP, 0, 0xE4, 0), src_tok(REG_TEMP, 1, 0xE4, 0),
        src_tok(REG_TEMP, 2, 0xE4, 0),
        PS_END,
    };
    u32 h = d3d8_ps_create(tokens, (u32)(sizeof tokens / sizeof tokens[0]));
    CHECK(h != 0, "ps.1.3: d3d8_ps_create вернул 0");
    const char *s = d3d8_ps_glsl(h);
    expect_sub(s, "// ps.1.3", "ps.1.3");
    expect_sub(s, "if (any(lessThan(t0.xyz, vec3(0.0)))) discard;", "ps.1.3 texkill");
    expect_sub(s, "r0.xz = clamp(vec4(((-v0.yxzw)) * 4.0).xz, -8.0, 8.0);", "ps.1.3 neg/mask/x4");
    expect_sub(s, "(0.5 - v0) + (-2.0 * (v1 - 0.5))", "ps.1.3 mod 3/5");
    expect_sub(s, "* 8.0", "ps.1.3 x8");
    expect_sub(s, "(1.0 - r0) * (2.0 * r1)", "ps.1.3 mod 6/7");
    expect_sub(s, "* 0.25", "ps.1.3 d4");
    expect_sub(s, "(-2.0 * r2) - abs(c0.xxxx)", "ps.1.3 mod 8/11");
    expect_sub(s, "* 0.125", "ps.1.3 d8");
    expect_sub(s, "mix(r2, r1, vec4(greaterThanEqual(vec4(r0), vec4(0.0))))", "ps.1.3 cmp");
    d3d8_ps_delete(h);
}

static void test_ps14(void)
{
    const u32 tokens[] = {
        PS_VER(1, 4),
        // texld r0, t0 (в ps.1.4 регистр типа TEXTURE именуется tc0)
        66u, dst_tok(REG_TEMP, 0, 0xF, 0, 0), src_tok(REG_TEXTURE, 0, 0xE4, 0),
        // texcrd r1.xyz, t1
        64u, dst_tok(REG_TEMP, 1, 0x7, 0, 0), src_tok(REG_TEXTURE, 1, 0xE4, 0),
        PS_PHASE,
        // texld r2, r1 (зависимая выборка текстуры по r1)
        66u, dst_tok(REG_TEMP, 2, 0xF, 0, 0), src_tok(REG_TEMP, 1, 0xE4, 0),
        // cnd r0, r0, r1, r2 (в ps.1.4 — покомпонентное сравнение > 0.5)
        80u, dst_tok(REG_TEMP, 0, 0xF, 0, 0), src_tok(REG_TEMP, 0, 0xE4, 0), src_tok(REG_TEMP, 1, 0xE4, 0),
        src_tok(REG_TEMP, 2, 0xE4, 0),
        PS_END,
    };
    u32 h = d3d8_ps_create(tokens, (u32)(sizeof tokens / sizeof tokens[0]));
    CHECK(h != 0, "ps.1.4: d3d8_ps_create вернул 0");
    const char *s = d3d8_ps_glsl(h);
    expect_sub(s, "// ps.1.4", "ps.1.4");
    expect_sub(s, "texture(u_tex0, tc0.xy)", "ps.1.4 texld tc0");
    expect_sub(s, "r1.xyz = clamp(vec4(tc1).xyz, -8.0, 8.0);", "ps.1.4 texcrd");
    expect_sub(s, "texture(u_tex2, r1.xy)", "ps.1.4 texld r1");
    expect_sub(s, "mix(r2, r1, vec4(greaterThan(vec4(r0), vec4(0.5))))", "ps.1.4 cnd");
    d3d8_ps_delete(h);
}

static void test_malformed_and_limits(void)
{
    const u32 bad_ver1[] = {0x00000000u};
    const u32 bad_ver2[] = {0xFFFE0101u};  // вершинный шейдер vs.1.1 вместо ps
    const u32 bad_op[] = {PS_VER(1, 1), 0x00000077u, PS_END};
    const u32 no_end[] = {PS_VER(1, 1), PS_NOP};
    const u32 trunc_comment[] = {PS_VER(1, 1), PS_COMMENT(500)};

    CHECK(d3d8_ps_create(bad_ver1, 0) == 0, "max=0 должен вернуть 0");
    CHECK(d3d8_ps_create(bad_ver1, 1) == 0, "нулевой токен версии должен вернуть 0");
    CHECK(d3d8_ps_create(bad_ver2, 1) == 0, "токен vs.1.1 должен вернуть 0");
    CHECK(d3d8_ps_create(bad_op, 3) == 0, "неизвестный опкод должен вернуть 0");

    // Усечённый поток без END: не должно быть падения и утечек
    u32 h_noend = d3d8_ps_create(no_end, 2);
    if (h_noend) {
        CHECK(d3d8_ps_glsl(h_noend) != NULL, "поток без END вернул непустой дескриптор без GLSL");
        d3d8_ps_delete(h_noend);
    }
    // Усечённый комментарий и усечённые инструкции с нехваткой операндов должны отклоняться без чтения за границей
    CHECK(d3d8_ps_create(trunc_comment, 2) == 0, "усечённый комментарий должен вернуть 0");
    const u32 trunc_def[] = {PS_VER(1, 1), 81u, dst_tok(REG_CONST, 0, 0xF, 0, 0), fbits(1.0f)};
    const u32 trunc_mov[] = {PS_VER(1, 1), 1u, dst_tok(REG_TEMP, 0, 0xF, 0, 0)};
    const u32 trunc_add[] = {PS_VER(1, 1), 2u, dst_tok(REG_TEMP, 0, 0xF, 0, 0), src_tok(REG_INPUT, 0, 0xE4, 0)};
    const u32 trunc_mad[] = {PS_VER(1, 1), 4u, dst_tok(REG_TEMP, 0, 0xF, 0, 0), src_tok(REG_INPUT, 0, 0xE4, 0),
                             src_tok(REG_CONST, 0, 0xE4, 0)};
    const u32 trunc_texld14[] = {PS_VER(1, 4), 66u, dst_tok(REG_TEMP, 0, 0xF, 0, 0)};
    CHECK(d3d8_ps_create(trunc_def, 4) == 0, "усечённый def должен вернуть 0");
    CHECK(d3d8_ps_create(trunc_mov, 3) == 0, "усечённый mov должен вернуть 0");
    CHECK(d3d8_ps_create(trunc_add, 4) == 0, "усечённый add должен вернуть 0");
    CHECK(d3d8_ps_create(trunc_mad, 5) == 0, "усечённый mad должен вернуть 0");
    CHECK(d3d8_ps_create(trunc_texld14, 3) == 0, "усечённый texld ps.1.4 должен вернуть 0");

    // Неверные дескрипторы
    CHECK(d3d8_ps_glsl(0) == NULL, "glsl(0)");
    CHECK(d3d8_ps_glsl(999) == NULL, "glsl(999)");
    d3d8_ps_delete(0);
    d3d8_ps_delete(999);

    // Проверка заполнения таблицы слотов (MAX_PS = 64) и освобождения слота
    const u32 minimal[] = {PS_VER(1, 1), PS_END};
    u32 hs[64];
    for (int i = 0; i < 64; i++) {
        hs[i] = d3d8_ps_create(minimal, 2);
        CHECK(hs[i] != 0, "слот %d не выделился", i);
    }
    CHECK(d3d8_ps_create(minimal, 2) == 0, "65-й шейдер должен вернуть 0 при полной таблице");
    d3d8_ps_delete(hs[10]);
    hs[10] = d3d8_ps_create(minimal, 2);
    CHECK(hs[10] != 0, "переиспользование освобождённого слота");
    for (int i = 0; i < 64; i++)
        d3d8_ps_delete(hs[i]);
}

int main(void)
{
    test_ps11();
    test_ps13_mods_shifts_texkill();
    test_ps14();
    test_malformed_and_limits();
    printf("d3d8_ps_test: %d failed\n", g_fails);
    return g_fails != 0;
}
