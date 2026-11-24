// Синтетический тест проекции камеры (runtime/overrides_engine.c: ovr_engine_camera_matrices) для Hor+ (16:9)
// и режима 4:3 для страниц меню (rt_menu_aspect_4x3), а также значения по умолчанию d3d8_menu_aspect().
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "runtime/abi.h"
#include "runtime/hle/d3d8.h"
#include "runtime/rt.h"

extern void ovr_engine_camera_matrices(CPU *c);

static int g_fails;
static int g_orig_calls;
static float g_last_fov;
static float g_mock_aspect = 16.0f / 9.0f;

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

#define PROJ 0x94u
#define SCREEN_TO_CAM 0xD4u
#define WORLD_TO_SCREEN 0x114u
#define SCREEN_TO_WORLD 0x154u
#define NODE_DET 0x1B8u
#define NODE_FOV 0x1E0u

static void wr_mat(u32 base, const float m[16])
{
    for (u32 i = 0; i < 16; i++)
        wrf32(g_mem, base + i * 4u, m[i]);
}

static void rd_mat(u32 base, float m[16])
{
    for (u32 i = 0; i < 16; i++)
        m[i] = rdf32(g_mem, base + i * 4u);
}

static void mul_mat(float out[16], const float a[16], const float b[16])
{
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            float s = 0.0f;
            for (int k = 0; k < 4; k++)
                s += a[r * 4 + k] * b[k * 4 + c];
            out[r * 4 + c] = s;
        }
}

// Заглушка FCameraSceneNode::UpdateMatrices (thiscall без аргументов: ret 0)
void mock_update_matrices_body(CPU *c)
{
    u32 node = c->ecx;
    g_orig_calls++;
    float fov = rdf32(g_mem, node + NODE_FOV);
    g_last_fov = fov;
    float m00 = 1.0f / tanf(fov * (float)M_PI / 360.0f);
    float m11 = m00 * g_mock_aspect;

    // Невырожденная матрица вида (поворот + перенос) и обратная к ней
    const float v[16] = {
        0.0f, 1.0f, 0.0f, 0.0f,
        -1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        3.0f, -2.0f, 5.0f, 1.0f,
    };
    const float vi[16] = {
        0.0f, -1.0f, 0.0f, 0.0f,
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        2.0f, 3.0f, -5.0f, 1.0f,
    };
    float p[16] = {0}, pi[16] = {0};
    p[0] = m00;
    p[5] = m11;
    p[10] = 1.0f;
    p[11] = 1.0f;  // m23 = 1.0 (перспектива)
    p[14] = -1.0f;

    pi[0] = 1.0f / m00;
    pi[5] = 1.0f / m11;
    pi[11] = -1.0f;
    pi[14] = 1.0f;
    pi[15] = 1.0f;

    float w2s[16], s2w[16];
    mul_mat(w2s, v, p);
    mul_mat(s2w, pi, vi);

    wr_mat(node + PROJ, p);
    wr_mat(node + SCREEN_TO_CAM, pi);
    wr_mat(node + WORLD_TO_SCREEN, w2s);
    wr_mat(node + SCREEN_TO_WORLD, s2w);
    wrf32(g_mem, node + NODE_DET, m00 * m11);  // det(V)=1, det(P)=m00*m11
    c->esp += 4;
}

#ifdef SNX_PIN
// В сборке с SNX_PIN ovr_engine_camera_matrices вызывает оригинал через rt_pin_enter (runtime/rt.c):
// на входе и выходе регистры гостя лежат в w19..w26, а после возврата rt_pin_enter переписывает c->eax..edi
// из них. Переходник сбрасывает w19..w26 в CPU, зовёт C-заглушку и загружает обновлённые регистры обратно.
extern void mock_update_matrices_pin(CPU *c);
__asm__(".text\n"
        ".global mock_update_matrices_pin\n"
        ".type mock_update_matrices_pin, %function\n"
        ".p2align 2\n"
        "mock_update_matrices_pin:\n"
        "    stp x29, x30, [sp, #-32]!\n"
        "    mov x29, sp\n"
        "    str x0, [sp, #16]\n"
        "    stp w19, w20, [x0]\n"
        "    stp w21, w22, [x0, #8]\n"
        "    stp w23, w24, [x0, #16]\n"
        "    stp w25, w26, [x0, #24]\n"
        "    bl mock_update_matrices_body\n"
        "    ldr x0, [sp, #16]\n"
        "    ldp w19, w20, [x0]\n"
        "    ldp w21, w22, [x0, #8]\n"
        "    ldp w23, w24, [x0, #16]\n"
        "    ldp w25, w26, [x0, #24]\n"
        "    ldp x29, x30, [sp], #32\n"
        "    ret\n"
        ".size mock_update_matrices_pin, .-mock_update_matrices_pin\n");
#define MOCK_ORIG mock_update_matrices_pin
#else
#define MOCK_ORIG mock_update_matrices_body
#endif

void (*const ovr_orig_ovr_engine_camera_matrices)(CPU *c) = MOCK_ORIG;
void (*const ovr_orig_ovr_engine_save_game)(CPU *c) = MOCK_ORIG;
void (*const ovr_orig_ovr_engine_flush)(CPU *c) = MOCK_ORIG;

static void call_cam(u32 node)
{
    CPU c;
    memset(&c, 0, sizeof c);
    c.ecx = node;
    c.esp = 0x00028000u;
    wr32(g_mem, c.esp, 0x12345678u);
    ovr_engine_camera_matrices(&c);
    CHECK(c.esp == 0x00028004u, "esp после вызова: %08x", c.esp);
}

int main(void)
{
    CHECK(rt_mem_init() == 0, "rt_mem_init");
    CHECK(rt_gc_epoch == 1u, "начальная эпоха GC: %u", rt_gc_epoch);
    CHECK(d3d8_menu_aspect() == 0, "MenuAspect4x3 по умолчанию должен быть выключен");
    d3d8_set_menu_aspect(1);
    CHECK(d3d8_menu_aspect() == 1, "MenuAspect4x3=1");
    d3d8_set_menu_aspect(0);
    CHECK(d3d8_menu_aspect() == 0, "MenuAspect4x3=0");

    u32 node = 0x00030000u;
    memset(g_mem + node, 0, 0x200u);

    // 1. Обычный широкий экран (Hor+): повторный вызов с расширенным FOV, исходный FOV восстановлен
    g_mock_aspect = 16.0f / 9.0f;
    rt_menu_aspect_4x3 = 0;
    wrf32(g_mem, node + NODE_FOV, 90.0f);
    g_orig_calls = 0;
    call_cam(node);
    float want_wide = (float)(2.0 * atan(tan(90.0 * M_PI / 360.0) * (16.0 / 9.0) / (4.0 / 3.0)) * 180.0 / M_PI);
    CHECK(g_orig_calls == 2, "Hor+: ожидалось 2 вызова оригинала, было %d", g_orig_calls);
    CHECK(fabsf(g_last_fov - want_wide) < 1e-3f, "Hor+ FOV=%.4f (ожидалось %.4f)", g_last_fov, want_wide);
    CHECK(fabsf(rdf32(g_mem, node + NODE_FOV) - 90.0f) < 1e-4f, "FOV не восстановлен: %.4f", rdf32(g_mem, node + NODE_FOV));

    // 2. Режим 4:3 для меню на 16:9 буфере: без Hor+, точная 4:3 проекция и обратность матриц
    rt_menu_aspect_4x3 = 1;
    wrf32(g_mem, node + NODE_FOV, 90.0f);
    g_orig_calls = 0;
    call_cam(node);
    CHECK(g_orig_calls == 1, "4:3 меню: ожидался 1 вызов оригинала, было %d", g_orig_calls);
    float m00 = rdf32(g_mem, node + PROJ);
    float m11 = rdf32(g_mem, node + PROJ + 0x14u);
    float pii11 = rdf32(g_mem, node + SCREEN_TO_CAM + 0x14u);
    float det = rdf32(g_mem, node + NODE_DET);
    CHECK(fabsf(m00 - 1.0f) < 1e-5f, "4:3 m00=%.6f", m00);
    CHECK(fabsf(m11 - 4.0f / 3.0f) < 1e-5f, "4:3 m11=%.6f (ожидалось 1.333333)", m11);
    CHECK(fabsf(pii11 - 0.75f) < 1e-5f, "4:3 ScreenToCamera[1][1]=%.6f (ожидалось 0.75)", pii11);
    CHECK(fabsf(det - 4.0f / 3.0f) < 1e-5f, "4:3 det=%.6f (ожидалось 1.333333)", det);

    float w2s[16], s2w[16], id[16];
    rd_mat(node + WORLD_TO_SCREEN, w2s);
    rd_mat(node + SCREEN_TO_WORLD, s2w);
    mul_mat(id, w2s, s2w);
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            float want = r == c ? 1.0f : 0.0f;
            CHECK(fabsf(id[r * 4 + c] - want) < 1e-5f, "W2S*S2W[%d][%d]=%.6f != %.1f", r, c, id[r * 4 + c], want);
        }

    // 3. Буфер уже 4:3: ни Hor+, ни сжатие Y не применяются
    g_mock_aspect = 4.0f / 3.0f;
    g_orig_calls = 0;
    call_cam(node);
    CHECK(g_orig_calls == 1, "на 4:3 буфере ожидался 1 вызов, было %d", g_orig_calls);
    CHECK(fabsf(rdf32(g_mem, node + PROJ + 0x14u) - 4.0f / 3.0f) < 1e-5f, "4:3 буфер m11 изменился");

    printf("camera_aspect_test: %d failed\n", g_fails);
    return g_fails != 0;
}
