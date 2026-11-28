// Нативные обёртки функций Engine.dll (таблица — recomp/overrides.py, WRAPS).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#include "platform/platform.h"
#include "runtime/abi.h"
#include "runtime/hle/d3d8_direct.h"
#include "runtime/rt.h"
#include "runtime/script_fix.h"

// Weak-заглушки прямого интерфейса d3d8_direct.h для таргетов без snx_hle (difftest_*):
// в snx_boot и тестах с snx_hle сильные реализации из runtime/hle/d3d8_dev.c их перекрывают,
// а в perf_ovr_test.c — сильные моки. Возврат NULL/0 безопасно откатывает оверрайды на ovr_orig_*.
__attribute__((weak)) struct d3ddev *d3d8_dev_from_guest(u32 guest_ptr) { (void)guest_ptr; return NULL; }
__attribute__((weak)) void d3d8_direct_set_rs(struct d3ddev *d, u32 state, u32 val) { (void)d; (void)state; (void)val; }
__attribute__((weak)) void d3d8_direct_set_tss(struct d3ddev *d, u32 stage, u32 type, u32 val)
{
    (void)d; (void)stage; (void)type; (void)val;
}
__attribute__((weak)) void d3d8_direct_set_transform(struct d3ddev *d, u32 type, const float *mat16)
{
    (void)d; (void)type; (void)mat16;
}
__attribute__((weak)) void d3d8_direct_set_light(struct d3ddev *d, u32 idx, const void *light104)
{
    (void)d; (void)idx; (void)light104;
}
__attribute__((weak)) void d3d8_direct_light_enable(struct d3ddev *d, u32 idx, u32 enable)
{
    (void)d; (void)idx; (void)enable;
}
__attribute__((weak)) void d3d8_direct_set_texture(struct d3ddev *d, u32 stage, u32 tex_guest_ptr)
{
    (void)d; (void)stage; (void)tex_guest_ptr;
}
__attribute__((weak)) void d3d8_direct_set_stream(struct d3ddev *d, u32 idx, u32 vb_guest_ptr, u32 stride)
{
    (void)d; (void)idx; (void)vb_guest_ptr; (void)stride;
}
__attribute__((weak)) void d3d8_direct_set_indices(struct d3ddev *d, u32 ib_guest_ptr, u32 base_vertex)
{
    (void)d; (void)ib_guest_ptr; (void)base_vertex;
}
__attribute__((weak)) void d3d8_direct_set_vs(struct d3ddev *d, u32 handle) { (void)d; (void)handle; }
__attribute__((weak)) void d3d8_direct_set_ps(struct d3ddev *d, u32 handle) { (void)d; (void)handle; }
__attribute__((weak)) int d3d8_direct_shadow_blur(struct d3ddev *d, u32 src_tex_guest, u32 dst_surf_guest,
                                                  float step_px)
{
    (void)d; (void)src_tex_guest; (void)dst_surf_guest; (void)step_px;
    return 0;
}

// ---------------------------------------------------------------- Hor+ на широком экране
// FCameraSceneNode::UpdateMatrices строит проекцию FPerspectiveMatrix(FOV/2, FOV/2, 1, W/H): горизонтальный
// угол FOV (градусы, [node+0x1E0]) постоянен, и на 16:9 картинка обрезается по вертикали (vert-). Если кадр
// шире 4:3, матрицы строятся заново с горизонтальным углом, при котором вертикальный — как в 4:3 (Hor+).
// Отсечение по пирамиде видимости движок считает из этих же матриц. FOV узла после вызова — прежний.
// Узел: +0x94 ProjectionMatrix (M[0][0] +0x94, M[1][1] +0xA8, M[2][3] +0xC0, M[3][3] +0xD0).
extern void (*const ovr_orig_ovr_engine_camera_matrices)(CPU *c);

#define NODE_FOV 0x1E0u
#define PROJ 0x94u
#define SCREEN_TO_CAM 0xD4u
#define WORLD_TO_SCREEN 0x114u
#define SCREEN_TO_WORLD 0x154u
#define NODE_DET 0x1B8u

void ovr_engine_camera_matrices(CPU *c)
{
    u32 node = c->ecx, ret = rd32(g_mem, c->esp);
    RT_ENTER_GEN(c, ovr_orig_ovr_engine_camera_matrices);  // оригинал — код гостя
    float m00 = rdf32(g_mem, node + PROJ), m11 = rdf32(g_mem, node + PROJ + 0x14);
    float m23 = rdf32(g_mem, node + PROJ + 0x2C), m33 = rdf32(g_mem, node + PROJ + 0x3C);
    if (m23 != 1.0f || m33 != 0.0f || !(m00 > 0.0f))
        return;  // не перспектива (виды редактора, ортогональные)
    double aspect = (double)m11 / (double)m00, k = aspect / (4.0 / 3.0);
    float fov = rdf32(g_mem, node + NODE_FOV);
    if (k < 1.01 || !(fov > 1.0f && fov < 170.0f))
        return;
    // В режиме 4:3 для страниц меню (MenuAspect4x3) задний буфер 16:9 при Present сжимается по ширине до 4:3:
    // вместо Hor+ приводим вертикальный масштаб проекции к 4:3 (P' = P * diag(1, 1/k, 1, 1)), чтобы 3D-книга
    // сохранила исходные пропорции и границы без пустой области справа и без сжатия по горизонтали.
    if (__atomic_load_n(&rt_menu_aspect_4x3, __ATOMIC_RELAXED)) {
        float inv_k = (float)(1.0 / k), fk = (float)k;
        wrf32(g_mem, node + PROJ + 0x14u, m11 * inv_k);
        wrf32(g_mem, node + SCREEN_TO_CAM + 0x14u, rdf32(g_mem, node + SCREEN_TO_CAM + 0x14u) * fk);
        for (u32 r = 0; r < 4; r++) {
            u32 off = node + WORLD_TO_SCREEN + (r * 4u + 1u) * 4u;
            wrf32(g_mem, off, rdf32(g_mem, off) * inv_k);
        }
        for (u32 col = 0; col < 4; col++) {
            u32 off = node + SCREEN_TO_WORLD + (4u + col) * 4u;
            wrf32(g_mem, off, rdf32(g_mem, off) * fk);
        }
        wrf32(g_mem, node + NODE_DET, rdf32(g_mem, node + NODE_DET) * inv_k);
        return;
    }
    double wide = 2.0 * atan(tan((double)fov * M_PI / 360.0) * k) * 180.0 / M_PI;
    wrf32(g_mem, node + NODE_FOV, (float)wide);
    c->esp -= 4;  // повторный вызов тем же адресом возврата (thiscall без аргументов)
    wr32(g_mem, c->esp, ret);
    c->ecx = node;
    RT_ENTER_GEN(c, ovr_orig_ovr_engine_camera_matrices);  // оригинал — код гостя
    wrf32(g_mem, node + NODE_FOV, fov);
}

// Примечание по пропорциям 2D HUD на 16:9:
// UCanvas::Update (Engine.dll!0x120f95e0) переносит Viewport->SizeX/SizeY в Canvas->ClipX/ClipY (+0x40/+0x44)
// и SizeX/SizeY (+0x64/+0x68). Скрипты HUD (SHHud, HudItemHealthBar.RenderHud, HudItemCoins в SHGame.u)
// вычисляют X/ширину отдельно от Canvas.SizeX, а Y/высоту — от Canvas.SizeY, рисуя через общий UCanvas::DrawTile
// (Engine.dll!0x120f88a0), который также используется меню GUI, шрифтами, затемнениями экрана и экранами загрузки.
// Подмена координат в UCanvas::DrawTile без контекста виджета нарушит выравнивание текста, клик-области GUI и
// полноэкранные затемнения, поэтому страницы меню исправляются 4:3-pillarbox в d3d8_present (d3d8_set_menu_aspect),
// а адаптация внутриигрового HUD требует скоординированного перехвата в recomp/overrides.py + проверки в игре.

// ---------------------------------------------------------------- без сброса рендера после сохранения
// UGameEngine::Tick после отложенного сохранения (автосохранение, точки сохранения) зовёт SaveGame, затем
// Exec("RestartSounds") и Flush(1): все ресурсы рендера (текстуры, буферы мешей) выбрасываются, и следующий кадр
// прогревает уровень целиком — 36 МБ текстур и 14 тыс. вызовов отрисовки на болоте, на Switch ≈1,3 с из
// двухсекундного фриза сохранения. Уровень после сохранения тот же, и этот сброс пропускается: Flush с той же
// глубины стека гостя, что и SaveGame, и вскоре за местом его вызова (тот же Tick), ничего не делает. Консольный
// SAVEGAME сброса не зовёт. Каждый Flush — строка в лог (откуда и с каким аргументом), SaveGame — с его временем.
extern void (*const ovr_orig_ovr_engine_save_game)(CPU *c);
extern void (*const ovr_orig_ovr_engine_flush)(CPU *c);

static u32 g_save_esp, g_save_ret;  // стек и адрес возврата последнего SaveGame (0 — нет)

void ovr_engine_save_game(CPU *c)
{
    u32 esp = c->esp, ret = rd32(g_mem, esp), slot = rd32(g_mem, esp + 4);
    u64 t0 = rt_ticks_ns();
    // На Switch временно поднимаем частоту CPU (FastLoad, 1785 МГц) на время сериализации сохранения.
    // UGameEngine::LoadMap намеренно не оборачивается здесь: у неё есть C++ EH-кадр с повторным выбросом
    // исключений (3_The_Hunt_Part4), и longjmp через обёртку оставил бы boost включённым.
    rt_platform_cpu_boost(1);
    RT_ENTER_GEN(c, ovr_orig_ovr_engine_save_game);  // оригинал — код гостя
    rt_platform_cpu_boost(0);
    fprintf(stderr, "[engine] SaveGame(%d): %.0f ms\n", (int)slot, (double)(rt_ticks_ns() - t0) / 1e6);
    g_save_esp = esp;
    g_save_ret = ret;
}

void ovr_engine_flush(CPU *c)
{
    u32 ret = rd32(g_mem, c->esp), precache = rd32(g_mem, c->esp + 4);
    int skip = g_save_ret && c->esp == g_save_esp && ret - g_save_ret < 0x100u;
    g_save_ret = 0;
    fprintf(stderr, "[engine] Flush(%u) from %08x%s\n", precache, ret, skip ? ", right after SaveGame: skipped" : "");
    if (skip) {
        c->esp += 8;  // thiscall с одним аргументом: ret 4
        return;
    }
    // shrekNX: script_fix_scan() здесь правил байткод SHGame; у Webhead такого скрипта нет.
    RT_ENTER_GEN(c, ovr_orig_ovr_engine_flush);  // оригинал — код гостя
}

// ---------------------------------------------------------------- отсечение по пирамиде видимости (NEON)
// FConvexVolume::BoxCheck (Engine.dll!0x121b3530, экспорт 0x12001eba) и SphereCheck (0x121b3230, 0x12002900):
// в оригинале есть SSE-путь (проверка 4 плоскостей разом), но CPUID рекомпилятора сообщает «без SSE» (GIsSSE = 0),
// поэтому движок гоняет скалярный x87-цикл в double с двумя fcomp/fnstsw на каждую плоскость.
// Раскладка FConvexVolume: +0x000 FPlane Planes[32] (float X, Y, Z, W — по 16 Б), +0x200 INT NumPlanes.
// Возврат (FOutcode в al): бит 0 — Inside (1), бит 1 — Outside (2):
//   1 — полностью внутри всех плоскостей, 3 — пересекает хотя бы одну, 2 — полностью снаружи хотя бы одной.
// Слабые определения NULL для этих указателей вынесены в runtime/ovr_orig_weak.c (иначе GCC с
// -fvisibility=hidden сворачивает const-переменную с инициализатором в NULL в этом же TU).
extern void (*const ovr_orig_ovr_engine_box_check)(CPU *c);
extern void (*const ovr_orig_ovr_engine_sphere_check)(CPU *c);

#define CV_NUM_PLANES 0x200u

static inline int engine_math_check(void)  // SNX_MATH_CHECK=1: сверка с оригиналом
{
    static int v = -1;
    if (v < 0)
        v = getenv("SNX_MATH_CHECK") != NULL;
    return v;
}

static u8 box_check_impl(const u8 *planes, s32 n, float ox, float oy, float oz, float ex, float ey, float ez)
{
    u8 res = 1u;
    s32 i = 0;
#if defined(__aarch64__)
    if (n >= 4) {
        float32x4_t vox = vdupq_n_f32(ox), voy = vdupq_n_f32(oy), voz = vdupq_n_f32(oz);
        float32x4_t vex = vdupq_n_f32(ex), vey = vdupq_n_f32(ey), vez = vdupq_n_f32(ez);
        for (; i + 4 <= n; i += 4) {
            float buf[16];
            memcpy(buf, planes + (u32)i * 16u, sizeof buf);
            float32x4x4_t p = vld4q_f32(buf);
            float32x4_t dist = vsubq_f32(
                vaddq_f32(vaddq_f32(vmulq_f32(vox, p.val[0]), vmulq_f32(voy, p.val[1])), vmulq_f32(voz, p.val[2])),
                p.val[3]);
            float32x4_t push = vaddq_f32(
                vaddq_f32(vabsq_f32(vmulq_f32(vex, p.val[0])), vabsq_f32(vmulq_f32(vey, p.val[1]))),
                vabsq_f32(vmulq_f32(vez, p.val[2])));
            // NaN даёт 0 в vcgtq_f32 — совпадает с test ah,0x41 в оригинале
            if (vmaxvq_u32(vcgtq_f32(dist, push)))
                return 2u;
            if (vmaxvq_u32(vcgtq_f32(dist, vnegq_f32(push))))
                res = 3u;
        }
    }
#endif
    for (; i < n; i++) {
        u32 off = (u32)i * 16u;
        float px = rdf32(planes, off);
        float py = rdf32(planes, off + 4u);
        float pz = rdf32(planes, off + 8u);
        float pw = rdf32(planes, off + 12u);
        float dist = ox * px + oy * py + oz * pz - pw;
        float push = fabsf(ex * px) + fabsf(ey * py) + fabsf(ez * pz);
        if (dist > -push) {
            res = 3u;
            if (dist > push)
                return 2u;
        }
    }
    return res;
}

static u8 sphere_check_impl(const u8 *planes, s32 n, float sx, float sy, float sz, float rad)
{
    u8 res = 1u;
    s32 i = 0;
#if defined(__aarch64__)
    if (n >= 4) {
        float32x4_t vsx = vdupq_n_f32(sx), vsy = vdupq_n_f32(sy), vsz = vdupq_n_f32(sz);
        float32x4_t vrad = vdupq_n_f32(rad), vnrad = vdupq_n_f32(-rad);
        for (; i + 4 <= n; i += 4) {
            float buf[16];
            memcpy(buf, planes + (u32)i * 16u, sizeof buf);
            float32x4x4_t p = vld4q_f32(buf);
            float32x4_t dist = vsubq_f32(
                vaddq_f32(vaddq_f32(vmulq_f32(vsx, p.val[0]), vmulq_f32(vsy, p.val[1])), vmulq_f32(vsz, p.val[2])),
                p.val[3]);
            if (vmaxvq_u32(vcgtq_f32(dist, vrad)))
                return 2u;
            if (vmaxvq_u32(vcgtq_f32(dist, vnrad)))
                res = 3u;
        }
    }
#endif
    for (; i < n; i++) {
        u32 off = (u32)i * 16u;
        float px = rdf32(planes, off);
        float py = rdf32(planes, off + 4u);
        float pz = rdf32(planes, off + 8u);
        float pw = rdf32(planes, off + 12u);
        float dist = sx * px + sy * py + sz * pz - pw;
        if (dist > -rad) {
            res = 3u;
            if (dist > rad)
                return 2u;
        }
    }
    return res;
}

void ovr_engine_box_check(CPU *c)
{
    const u8 *m = g_mem;
    u32 vol = c->ecx;
    u32 orig_va = rd32(m, c->esp + 4u), ext_va = rd32(m, c->esp + 8u);
    s32 n = (s32)rd32(m, vol + CV_NUM_PLANES);
    u8 res = n > 0 ? box_check_impl(m + vol, n, rdf32(m, orig_va), rdf32(m, orig_va + 4u), rdf32(m, orig_va + 8u),
                                    rdf32(m, ext_va), rdf32(m, ext_va + 4u), rdf32(m, ext_va + 8u))
                   : 1u;
    if (engine_math_check() && ovr_orig_ovr_engine_box_check) {
        RT_ENTER_GEN(c, ovr_orig_ovr_engine_box_check);
        u8 want = (u8)(c->eax & 0xFFu);
        if (want != res)
            fprintf(stderr, "[engine] SNX_MATH_CHECK: BoxCheck(n=%d) got %u, orig %u\n", n, res, want);
        return;
    }
    c->eax = res;
    c->esp += 12u;  // thiscall, два аргумента: ret 8
}

void ovr_engine_sphere_check(CPU *c)
{
    const u8 *m = g_mem;
    u32 vol = c->ecx;
    u32 sph_va = rd32(m, c->esp + 4u);
    s32 n = (s32)rd32(m, vol + CV_NUM_PLANES);
    u8 res = n > 0 ? sphere_check_impl(m + vol, n, rdf32(m, sph_va), rdf32(m, sph_va + 4u), rdf32(m, sph_va + 8u),
                                       rdf32(m, sph_va + 12u))
                   : 1u;
    if (engine_math_check() && ovr_orig_ovr_engine_sphere_check) {
        RT_ENTER_GEN(c, ovr_orig_ovr_engine_sphere_check);
        u8 want = (u8)(c->eax & 0xFFu);
        if (want != res)
            fprintf(stderr, "[engine] SNX_MATH_CHECK: SphereCheck(n=%d) got %u, orig %u\n", n, res, want);
        return;
    }
    c->eax = res;
    c->esp += 8u;  // thiscall, один аргумент: ret 4
}

// ---------------------------------------------------------------- CPU-скиннинг скелетных мешей (NEON)
// Engine.dll!0x121ed630 (неэкспортированная MeshSkinVerts, cdecl):
//   void MeshSkinVerts(USkeletalMesh *Mesh, USkeletalMeshInstance *Inst, FSkinVertex *DestVerts)
// Раскладка структур (EN и RUS совпадают):
//   USkeletalMesh: +0x1f0 TArray<FStaticLODModel>::Data (шаг 0x120), +0x2d8 TArray<FCoords>::Data (RefBasesInvMatrix)
//   USkeletalMeshInstance: +0xbc SpaceBases.Data (FCoords*, шаг 0x30), +0xc0 NumBones, +0xf8 LastLODLevel,
//                          +0x1a8 флаг копирования доп. буфера (если != 0 — уходим в оригинал)
//   FStaticLODModel: +0x00 поток команд скиннинга (u32[], терминатор 0xFFFFFFFF), +0x0c исходные вершины (шаг 0x10:
//                    float x,y,z + u32 packed_nrm по 10 бит со смещением 0x1FF), +0x10 NumVerts, +0x110 CacheCount.
//   Выход FSkinVertex (32 Б): float px,py,pz, nx,ny,nz; u32 u, v.
//   Масштаб весов в оригинале: Engine.dll!0x1237a6d8 = 0x31800080 (≈ 1 / 0x0FFFF000).
extern void (*const ovr_orig_ovr_engine_skin_verts)(CPU *c);

static inline int engine_skin_check(void)  // SNX_SKIN_CHECK=1: сверка вершин с оригиналом
{
    static int v = -1;
    if (v < 0)
        v = getenv("SNX_SKIN_CHECK") != NULL;
    return v;
}

typedef struct {
#if defined(__aarch64__)
    float32x4_t c0, c1, c2, tr;  // столбцы 3x3 + перенос (lane 3 = 0)
#else
    float c0[3], c1[3], c2[3], tr[3];
#endif
} skin_mat_t;

static inline float u32_as_f32(u32 bits)
{
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}

static inline void skin_build_mats(const u8 *m, u32 space_va, u32 ref_va, s32 num_bones, skin_mat_t *out)
{
    for (s32 b = 0; b < num_bones; b++) {
        u32 boff = (u32)b * 48u;
        float sc[12], rf[12];
        memcpy(sc, m + space_va + boff, sizeof sc);
        memcpy(rf, m + ref_va + boff, sizeof rf);
        // FCoords: [0..2] Origin, [3..5] XAxis (row 0), [6..8] YAxis (row 1), [9..11] ZAxis (row 2)
#if defined(__aarch64__)
        float32x4_t ctr = {sc[0], sc[1], sc[2], 0.0f};
        float32x4_t cc0 = {sc[3], sc[6], sc[9], 0.0f};
        float32x4_t cc1 = {sc[4], sc[7], sc[10], 0.0f};
        float32x4_t cc2 = {sc[5], sc[8], sc[11], 0.0f};
        out[b].tr = vfmaq_n_f32(vfmaq_n_f32(vfmaq_n_f32(ctr, cc0, rf[0]), cc1, rf[1]), cc2, rf[2]);
        out[b].c0 = vfmaq_n_f32(vfmaq_n_f32(vmulq_n_f32(cc0, rf[3]), cc1, rf[6]), cc2, rf[9]);
        out[b].c1 = vfmaq_n_f32(vfmaq_n_f32(vmulq_n_f32(cc0, rf[4]), cc1, rf[7]), cc2, rf[10]);
        out[b].c2 = vfmaq_n_f32(vfmaq_n_f32(vmulq_n_f32(cc0, rf[5]), cc1, rf[8]), cc2, rf[11]);
#else
        float cc0[3] = {sc[3], sc[6], sc[9]};
        float cc1[3] = {sc[4], sc[7], sc[10]};
        float cc2[3] = {sc[5], sc[8], sc[11]};
        for (int r = 0; r < 3; r++) {
            out[b].tr[r] = sc[r] + cc0[r] * rf[0] + cc1[r] * rf[1] + cc2[r] * rf[2];
            out[b].c0[r] = cc0[r] * rf[3] + cc1[r] * rf[6] + cc2[r] * rf[9];
            out[b].c1[r] = cc0[r] * rf[4] + cc1[r] * rf[7] + cc2[r] * rf[10];
            out[b].c2[r] = cc0[r] * rf[5] + cc1[r] * rf[8] + cc2[r] * rf[11];
        }
#endif
    }
}

static inline void skin_store_vert(u8 *dst, const float p[3], const float n[3], u32 u_bits, u32 v_bits)
{
    u32 tmp[8];
    memcpy(&tmp[0], p, 12u);
    memcpy(&tmp[3], n, 12u);
    tmp[6] = u_bits;
    tmp[7] = v_bits;
    memcpy(dst, tmp, sizeof tmp);
}

static inline int skin_decode_bone(u32 word, s32 num_bones, u32 *out_idx)
{
    u32 raw = word & 0xFFFu;
    // Индекс в потоке предварительно умножен на 6 (смещение в FCoords: raw * 8 == idx * 48)
    u32 idx = raw / 6u;
    if (idx * 6u != raw || (s32)idx >= num_bones)
        return 0;
    *out_idx = idx;
    return 1;
}

static int skin_verts_impl(u8 *m, u32 mesh_va, u32 inst_va, u32 dst_va, u32 *out_verts)
{
    if (!mesh_va || !inst_va || !dst_va)
        return 0;
    if (rd32(m, inst_va + 0x1a8u) != 0u)
        return 0;  // доп. копия с ленивыми массивами и виртуальными вызовами — редкий путь

    s32 lod_idx = (s32)rd32(m, inst_va + 0xf8u);
    u32 lod_arr = rd32(m, mesh_va + 0x1f0u);
    s32 num_bones = (s32)rd32(m, inst_va + 0xc0u);
    if (lod_idx < 0 || !lod_arr || num_bones < 0 || num_bones > 4096)
        return 0;

    u32 lod_va = lod_arr + (u32)lod_idx * 0x120u;
    u32 cmd_va = rd32(m, lod_va + 0x00u);
    u32 src_va = rd32(m, lod_va + 0x0cu);
    s32 cache_count = (s32)rd32(m, lod_va + 0x110u);
    u32 space_va = rd32(m, inst_va + 0xbcu);
    u32 ref_va = rd32(m, mesh_va + 0x2d8u);
    if (!cmd_va || !src_va || cache_count < 0 || cache_count > 65536 || (num_bones > 0 && (!space_va || !ref_va)))
        return 0;

    skin_mat_t mats_stack[128];
    skin_mat_t *mats = mats_stack;
    if (num_bones > 128) {
        mats = (skin_mat_t *)malloc((size_t)num_bones * sizeof(skin_mat_t));
        if (!mats)
            return 0;
    }
    if (num_bones > 0)
        skin_build_mats(m, space_va, ref_va, num_bones, mats);

    u32 cache_cap = (u32)cache_count * 6u;
    float cache_stack[256 * 6];
    float *cache = cache_stack;
    if (cache_count > 256) {
        cache = (float *)malloc((size_t)cache_cap * sizeof(float));
        if (!cache) {
            if (mats != mats_stack)
                free(mats);
            return 0;
        }
    }

    const float w_scale = u32_as_f32(0x31800080u);  // Engine.dll!0x1237a6d8
    const u8 *cmd = m + cmd_va;
    const u8 *src = m + src_va;
    u8 *dst = m + dst_va;
    u32 cache_wr = 0u, n_written = 0u;
    int ok = 1;

    for (;;) {
        u32 w0;
        memcpy(&w0, cmd, 4u);
        if (w0 == 0xFFFFFFFFu)
            break;

        if (w0 >= 0xF0000000u) {
            // Копия позиции и нормали из кеша швов UV без сдвига src
            u32 off = w0 & 0x0FFFFFFFu;
            if (off + 6u > cache_cap) {
                ok = 0;
                break;
            }
            u32 uv[2];
            memcpy(uv, cmd + 4u, 8u);
            skin_store_vert(dst, cache + off, cache + off + 3u, uv[0], uv[1]);
            dst += 32u;
            cmd += 12u;
            n_written++;
            continue;
        }

        float sp[3];
        u32 pn;
        memcpy(sp, src, 12u);
        memcpy(&pn, src + 12u, 4u);
        float sn0 = (float)((s32)(pn & 0x3FFu) - 0x1FF);
        float sn1 = (float)((s32)((pn >> 10) & 0x3FFu) - 0x1FF);
        float sn2 = (float)((s32)((pn >> 20) & 0x3FFu) - 0x1FF);
        src += 16u;

        u32 count = ((w0 >> 28) & 7u) + 1u;
        int store_cache = (w0 & 0x80000000u) != 0u;
        float out_p[3], out_n[3];

        if (count == 1u) {
            u32 bi;
            if (!skin_decode_bone(w0, num_bones, &bi)) {
                ok = 0;
                break;
            }
            const skin_mat_t *bm = &mats[bi];
#if defined(__aarch64__)
            float32x4_t vp = vfmaq_n_f32(vfmaq_n_f32(vfmaq_n_f32(bm->tr, bm->c0, sp[0]), bm->c1, sp[1]), bm->c2, sp[2]);
            float32x4_t vn = vfmaq_n_f32(vfmaq_n_f32(vmulq_n_f32(bm->c0, sn0), bm->c1, sn1), bm->c2, sn2);
            float pf[4], nf[4];
            vst1q_f32(pf, vp);
            vst1q_f32(nf, vn);
            out_p[0] = pf[0];
            out_p[1] = pf[1];
            out_p[2] = pf[2];
            out_n[0] = nf[0];
            out_n[1] = nf[1];
            out_n[2] = nf[2];
#else
            for (int r = 0; r < 3; r++) {
                out_p[r] = bm->tr[r] + bm->c0[r] * sp[0] + bm->c1[r] * sp[1] + bm->c2[r] * sp[2];
                out_n[r] = bm->c0[r] * sn0 + bm->c1[r] * sn1 + bm->c2[r] * sn2;
            }
#endif
        } else {
#if defined(__aarch64__)
            float32x4_t acc_p = vdupq_n_f32(0.0f);
            float32x4_t acc_n = vdupq_n_f32(0.0f);
            for (u32 k = 0; k < count; k++) {
                u32 wk;
                memcpy(&wk, cmd + k * 4u, 4u);
                u32 bi;
                if (!skin_decode_bone(wk, num_bones, &bi)) {
                    ok = 0;
                    break;
                }
                float w = (float)(wk & 0x0FFFFFFFu) * w_scale;
                const skin_mat_t *bm = &mats[bi];
                float32x4_t vp =
                    vfmaq_n_f32(vfmaq_n_f32(vfmaq_n_f32(bm->tr, bm->c0, sp[0]), bm->c1, sp[1]), bm->c2, sp[2]);
                float32x4_t vn = vfmaq_n_f32(vfmaq_n_f32(vmulq_n_f32(bm->c0, sn0), bm->c1, sn1), bm->c2, sn2);
                acc_p = vfmaq_n_f32(acc_p, vp, w);
                acc_n = vfmaq_n_f32(acc_n, vn, w);
            }
            if (!ok)
                break;
            float pf[4], nf[4];
            vst1q_f32(pf, acc_p);
            vst1q_f32(nf, acc_n);
            out_p[0] = pf[0];
            out_p[1] = pf[1];
            out_p[2] = pf[2];
            out_n[0] = nf[0];
            out_n[1] = nf[1];
            out_n[2] = nf[2];
#else
            out_p[0] = out_p[1] = out_p[2] = 0.0f;
            out_n[0] = out_n[1] = out_n[2] = 0.0f;
            for (u32 k = 0; k < count; k++) {
                u32 wk;
                memcpy(&wk, cmd + k * 4u, 4u);
                u32 bi;
                if (!skin_decode_bone(wk, num_bones, &bi)) {
                    ok = 0;
                    break;
                }
                float w = (float)(wk & 0x0FFFFFFFu) * w_scale;
                const skin_mat_t *bm = &mats[bi];
                for (int r = 0; r < 3; r++) {
                    float pr = bm->tr[r] + bm->c0[r] * sp[0] + bm->c1[r] * sp[1] + bm->c2[r] * sp[2];
                    float nr = bm->c0[r] * sn0 + bm->c1[r] * sn1 + bm->c2[r] * sn2;
                    out_p[r] += pr * w;
                    out_n[r] += nr * w;
                }
            }
            if (!ok)
                break;
#endif
        }

        if (store_cache) {
            if (cache_wr + 6u > cache_cap) {
                ok = 0;
                break;
            }
            memcpy(cache + cache_wr, out_p, 12u);
            memcpy(cache + cache_wr + 3u, out_n, 12u);
            cache_wr += 6u;
        }

        u32 uv[2];
        memcpy(uv, cmd + count * 4u, 8u);
        skin_store_vert(dst, out_p, out_n, uv[0], uv[1]);
        dst += 32u;
        cmd += (count + 2u) * 4u;
        n_written++;
    }

    if (cache != cache_stack)
        free(cache);
    if (mats != mats_stack)
        free(mats);
    if (ok && out_verts)
        *out_verts = n_written;
    return ok;
}

static inline int float_near(float a, float b, float abs_tol, float rel_tol)
{
    if (isnan(a) && isnan(b))
        return 1;
    float d = fabsf(a - b);
    return d <= abs_tol + rel_tol * fabsf(b);
}

void ovr_engine_skin_verts(CPU *c)
{
    u8 *m = g_mem;
    u32 mesh_va = rd32(m, c->esp + 4u);
    u32 inst_va = rd32(m, c->esp + 8u);
    u32 dst_va = rd32(m, c->esp + 12u);
    u32 n_verts = 0u;

    if (!skin_verts_impl(m, mesh_va, inst_va, dst_va, &n_verts)) {
        if (ovr_orig_ovr_engine_skin_verts)
            RT_ENTER_GEN(c, ovr_orig_ovr_engine_skin_verts);
        else
            c->esp += 4u;
        return;
    }

    if (engine_skin_check() && ovr_orig_ovr_engine_skin_verts) {
        size_t bytes = (size_t)n_verts * 32u;
        u8 *got = bytes ? (u8 *)malloc(bytes) : NULL;
        if (got)
            memcpy(got, m + dst_va, bytes);
        RT_ENTER_GEN(c, ovr_orig_ovr_engine_skin_verts);
        if (got) {
            static int logged = 0;
            for (u32 i = 0; i < n_verts; i++) {
                float gp[6], wp[6];
                u32 guv[2], wuv[2];
                memcpy(gp, got + (size_t)i * 32u, 24u);
                memcpy(wp, m + dst_va + (size_t)i * 32u, 24u);
                memcpy(guv, got + (size_t)i * 32u + 24u, 8u);
                memcpy(wuv, m + dst_va + (size_t)i * 32u + 24u, 8u);
                int bad = (guv[0] != wuv[0] || guv[1] != wuv[1]);
                for (int k = 0; k < 6 && !bad; k++) {
                    if (!float_near(gp[k], wp[k], 2e-3f, 2e-4f))
                        bad = 1;
                }
                if (bad && logged++ < 32) {
                    fprintf(stderr,
                            "[engine] SNX_SKIN_CHECK: vert %u/%u pos=(%.4f,%.4f,%.4f) vs (%.4f,%.4f,%.4f) "
                            "nrm=(%.3f,%.3f,%.3f) vs (%.3f,%.3f,%.3f) uv=(%08x,%08x) vs (%08x,%08x)\n",
                            i, n_verts, gp[0], gp[1], gp[2], wp[0], wp[1], wp[2], gp[3], gp[4], gp[5], wp[3], wp[4],
                            wp[5], guv[0], guv[1], wuv[0], wuv[1]);
                    break;
                }
            }
            free(got);
        }
        return;
    }

    c->esp += 4u;  // cdecl: ret
}

// ---------------------------------------------------------------- спрайтовые частицы (NEON)
// USpriteEmitter::FillVertexBuffer (Engine.dll!0x122039d0, экспорт 0x1200c44b, __thiscall):
//   int USpriteEmitter::FillVertexBuffer(FSpriteParticleVertex *Vertices, FLevelSceneNode *SceneNode)
// Возвращает 0; для редких режимов (PTCS_Relative, UseRotationFrom==1 с Owner, UseDirectionAs 3/4)
// безопасно передаёт управление оригиналу ovr_orig_ovr_engine_sprite_fill_vb.
// Раскладка USpriteEmitter / UParticleEmitter (EN и RUS совпадают):
//   +0x10c u8 CoordinateSystem (1 = PTCS_Relative), +0x22c u8 UseRotationFrom, +0x230 u8 SpinParticles (бит 0)
//   +0x338 s32 TextureUSubdivisions, +0x33c s32 TextureVSubdivisions
//   +0x340 u8 флаги подразбиений (бит 0 = BlendBetweenSubdivisions, бит 1 = UseSubdivisionScale)
//   +0x344/+0x348 TArray<float> SubdivisionScale, +0x350/+0x354 s32 SubdivisionStart/End
//   +0x40c u32 Owner, +0x418 u32 Particles.Data (шаг 0xc8), +0x428 s32 ActiveParticles
//   +0x4b0 u8 UseDirectionAs (0..7), +0x4c0 FVector ProjectionNormal
// Раскладка FParticle (0xc8 Б):
//   +0x00 Location, +0x0c OldLocation, +0x30 Spin, +0x3c StartSpin, +0x6c Size.X, +0x70 Size.Y,
//   +0xa8 Color (u32), +0xac Time, +0xb0 MaxLifetime, +0xbc Flags (бит 0 = Active), +0xc0 s32 Subdivision
// Раскладка FSpriteParticleVertex (32 Б × 4 на частицу): float x,y,z; u32 Color; float u,v,u2,v2.
extern void (*const ovr_orig_ovr_engine_sprite_fill_vb)(CPU *c);

static inline int engine_particle_check(void)  // SNX_PARTICLE_CHECK=1: сверка буфера частиц с оригиналом
{
    static int v = -1;
    if (v < 0)
        v = getenv("SNX_PARTICLE_CHECK") != NULL;
    return v;
}

static inline void vec3_normalize(float v[3])
{
    float sq = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    if (sq >= 1e-8f) {
        float inv = 1.0f / sqrtf(sq);
        v[0] *= inv;
        v[1] *= inv;
        v[2] *= inv;
    }
}

// Совпадает с FVector::RotateAngleAxis (Core.dll!0x1010ffd0): Rodrigues вокруг единичной оси Axis
static inline void vec3_rotate_axis(float v[3], const float ax[3], float s, float co)
{
    float omc = 1.0f - co;
    float d = (ax[0] * v[0] + ax[1] * v[1] + ax[2] * v[2]) * omc;
    float cx = ax[1] * v[2] - ax[2] * v[1];
    float cy = ax[2] * v[0] - ax[0] * v[2];
    float cz = ax[0] * v[1] - ax[1] * v[0];
    v[0] = v[0] * co + ax[0] * d + cx * s;
    v[1] = v[1] * co + ax[1] * d + cy * s;
    v[2] = v[2] * co + ax[2] * d + cz * s;
}

static inline void sprite_emit_quad(u8 *dst, const float loc[3], const float up[3], const float right[3], u32 col,
                                    float u0, float u1, float v0, float v1, float u2_0, float u2_1, float v2_0,
                                    float v2_1)
{
#if defined(__aarch64__)
    float32x4_t vloc = {loc[0], loc[1], loc[2], 0.0f};
    float32x4_t vup = {up[0], up[1], up[2], 0.0f};
    float32x4_t vrg = {right[0], right[1], right[2], 0.0f};
    float32x4_t bot = vsubq_f32(vloc, vup);
    float32x4_t top = vaddq_f32(vloc, vup);

    uint32x4_t p0 = vsetq_lane_u32(col, vreinterpretq_u32_f32(vsubq_f32(bot, vrg)), 3);
    uint32x4_t p1 = vsetq_lane_u32(col, vreinterpretq_u32_f32(vaddq_f32(bot, vrg)), 3);
    uint32x4_t p2 = vsetq_lane_u32(col, vreinterpretq_u32_f32(vsubq_f32(top, vrg)), 3);
    uint32x4_t p3 = vsetq_lane_u32(col, vreinterpretq_u32_f32(vaddq_f32(top, vrg)), 3);

    float32x4_t t0 = {u0, v1, u2_0, v2_1};
    float32x4_t t1 = {u1, v1, u2_1, v2_1};
    float32x4_t t2 = {u0, v0, u2_0, v2_0};
    float32x4_t t3 = {u1, v0, u2_1, v2_0};

    memcpy(dst + 0u, &p0, 16u);
    memcpy(dst + 16u, &t0, 16u);
    memcpy(dst + 32u, &p1, 16u);
    memcpy(dst + 48u, &t1, 16u);
    memcpy(dst + 64u, &p2, 16u);
    memcpy(dst + 80u, &t2, 16u);
    memcpy(dst + 96u, &p3, 16u);
    memcpy(dst + 112u, &t3, 16u);
#else
    float bx = loc[0] - up[0], by = loc[1] - up[1], bz = loc[2] - up[2];
    float tx = loc[0] + up[0], ty = loc[1] + up[1], tz = loc[2] + up[2];
    float vtx[4][3] = {
        {bx - right[0], by - right[1], bz - right[2]},
        {bx + right[0], by + right[1], bz + right[2]},
        {tx - right[0], ty - right[1], tz - right[2]},
        {tx + right[0], ty + right[1], tz + right[2]},
    };
    float uvs[4][4] = {
        {u0, v1, u2_0, v2_1},
        {u1, v1, u2_1, v2_1},
        {u0, v0, u2_0, v2_0},
        {u1, v0, u2_1, v2_0},
    };
    for (int i = 0; i < 4; i++) {
        u8 *vd = dst + (u32)i * 32u;
        memcpy(vd, vtx[i], 12u);
        memcpy(vd + 12u, &col, 4u);
        memcpy(vd + 16u, uvs[i], 16u);
    }
#endif
}

static int sprite_fill_vb_impl(u8 *m, u32 this_va, u32 verts_va, u32 node_va, u32 *out_verts)
{
    if (!this_va || !verts_va || !node_va)
        return 0;
    if (m[this_va + 0x10cu] == 1u)
        return 0;  // PTCS_Relative — редкий режим с трансформацией базиса через Owner
    if (m[this_va + 0x22cu] == 1u && rd32(m, this_va + 0x40cu) != 0u)
        return 0;  // UseRotationFrom == 1 с Owner

    u8 use_dir = m[this_va + 0x4b0u];
    if (use_dir == 3u || use_dir == 4u || use_dir > 7u)
        return 0;  // нормальные режимы (GetNonParallel + SafeNormal) — передаём оригиналу

    s32 active_cnt = (s32)rd32(m, this_va + 0x428u);
    if (active_cnt <= 0) {
        if (out_verts)
            *out_verts = 0u;
        return 1;
    }

    u32 parts_va = rd32(m, this_va + 0x418u);
    if (!parts_va)
        return 0;

    // Базис камеры из ScreenToWorld (SceneNode + 0x154, матрица 4x4 по строкам) и ViewOrigin (+0x194).
    // Deproject(0,-1000,0,1) - Deproject(0,0,0,1) = -1000 * row1, Deproject(1000,0,0,1) - P0 = 1000 * row0.
    float view_orig[3];
    memcpy(view_orig, m + node_va + 0x194u, 12u);
    float s2w[16];
    memcpy(s2w, m + node_va + 0x154u, sizeof s2w);
    float cam_up[3] = {-1000.0f * s2w[4], -1000.0f * s2w[5], -1000.0f * s2w[6]};
    float cam_right[3] = {1000.0f * s2w[0], 1000.0f * s2w[1], 1000.0f * s2w[2]};
    // Axis = (Up.z*Right.y - Up.y*Right.z, Up.x*Right.z - Up.z*Right.x, Up.y*Right.x - Up.x*Right.y)
    float cam_axis[3] = {
        cam_up[2] * cam_right[1] - cam_up[1] * cam_right[2],
        cam_up[0] * cam_right[2] - cam_up[2] * cam_right[0],
        cam_up[1] * cam_right[0] - cam_up[0] * cam_right[1],
    };
    vec3_normalize(cam_up);
    vec3_normalize(cam_right);
    vec3_normalize(cam_axis);

    int spin_on = (m[this_va + 0x230u] & 1u) != 0u;
    s32 u_sub = (s32)rd32(m, this_va + 0x338u);
    s32 v_sub = (s32)rd32(m, this_va + 0x33cu);
    int has_sub = (u_sub != 0 && v_sub != 0);
    float du = has_sub ? (1.0f / (float)u_sub) : 0.0f;
    float dv = has_sub ? (1.0f / (float)v_sub) : 0.0f;
    u8 sub_flags = m[this_va + 0x340u];
    u32 sub_scale_va = rd32(m, this_va + 0x344u);
    s32 sub_scale_num = (s32)rd32(m, this_va + 0x348u);
    s32 sub_start = (s32)rd32(m, this_va + 0x350u);
    s32 sub_end = (s32)rd32(m, this_va + 0x354u);
    float proj_nrm[3];
    memcpy(proj_nrm, m + this_va + 0x4c0u, 12u);

    u8 *dst = m + verts_va;
    u32 emitted = 0u;

    for (s32 i = 0; i < active_cnt; i++) {
        u32 p_va = parts_va + (u32)i * 0xc8u;
        if ((m[p_va + 0xbcu] & 1u) == 0u)
            continue;

        float loc[3];
        memcpy(loc, m + p_va, 12u);
        u32 col = rd32(m, p_va + 0xa8u);
        float p_time = rdf32(m, p_va + 0xacu);
        float max_life = rdf32(m, p_va + 0xb0u);
        float rel_t = 0.0f;
        if (max_life != 0.0f) {
            float t = p_time / max_life;
            rel_t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        }

        float sx = rdf32(m, p_va + 0x6cu);
        float sy = rdf32(m, p_va + 0x70u);
        float up_v[3], rg_v[3];

        if (use_dir == 0u) {
            up_v[0] = cam_up[0] * sx;
            up_v[1] = cam_up[1] * sx;
            up_v[2] = cam_up[2] * sx;
            rg_v[0] = cam_right[0] * sx;
            rg_v[1] = cam_right[1] * sx;
            rg_v[2] = cam_right[2] * sx;
        } else if (use_dir == 7u) {
            up_v[0] = cam_up[0] * sy;
            up_v[1] = cam_up[1] * sy;
            up_v[2] = cam_up[2] * sy;
            rg_v[0] = cam_right[0] * sx;
            rg_v[1] = cam_right[1] * sx;
            rg_v[2] = cam_right[2] * sx;
        } else {
            float old_loc[3];
            memcpy(old_loc, m + p_va + 0x0cu, 12u);
            float dir[3] = {loc[0] - old_loc[0], loc[1] - old_loc[1], loc[2] - old_loc[2]};
            vec3_normalize(dir);
            float rg2[3];
            if (use_dir == 5u || use_dir == 6u) {
                rg2[0] = dir[1] * proj_nrm[2] - dir[2] * proj_nrm[1];
                rg2[1] = dir[2] * proj_nrm[0] - dir[0] * proj_nrm[2];
                rg2[2] = dir[0] * proj_nrm[1] - dir[1] * proj_nrm[0];
            } else {
                float to_p[3] = {loc[0] - view_orig[0], loc[1] - view_orig[1], loc[2] - view_orig[2]};
                rg2[0] = dir[1] * to_p[2] - dir[2] * to_p[1];
                rg2[1] = dir[2] * to_p[0] - dir[0] * to_p[2];
                rg2[2] = dir[0] * to_p[1] - dir[1] * to_p[0];
                vec3_normalize(rg2);
            }
            if (use_dir == 1u || use_dir == 5u) {
                up_v[0] = dir[0] * sy;
                up_v[1] = dir[1] * sy;
                up_v[2] = dir[2] * sy;
                rg_v[0] = rg2[0] * sx;
                rg_v[1] = rg2[1] * sx;
                rg_v[2] = rg2[2] * sx;
            } else {
                up_v[0] = rg2[0] * sy;
                up_v[1] = rg2[1] * sy;
                up_v[2] = rg2[2] * sy;
                rg_v[0] = dir[0] * sx;
                rg_v[1] = dir[1] * sx;
                rg_v[2] = dir[2] * sx;
            }
        }

        if (spin_on) {
            float spin = rdf32(m, p_va + 0x30u);
            float start_spin = rdf32(m, p_va + 0x3cu);
            u32 angle;
            if (spin < 0.0f) {
                s32 r = (s32)lrintf(start_spin - p_time * spin);
                angle = 0xFFFFu - ((u32)r & 0xFFFFu);
            } else {
                s32 r = (s32)lrintf(start_spin + p_time * spin);
                angle = (u32)r & 0xFFFFu;
            }
            u32 idx = (angle >> 2) & 0x3FFFu;
            float rad = (float)idx * (float)(2.0 * M_PI / 16384.0);
            float s = sinf(rad), co = cosf(rad);
            vec3_rotate_axis(up_v, cam_axis, s, co);
            vec3_rotate_axis(rg_v, cam_axis, s, co);
        }

        float u0 = 0.0f, u1 = 1.0f, v0 = 0.0f, v1 = 1.0f;
        float u2_0 = 0.0f, u2_1 = 1.0f, v2_0 = 0.0f, v2_1 = 1.0f;
        if (has_sub) {
            s32 sub = (s32)rd32(m, p_va + 0xc0u);
            if (sub != -1) {
                s32 u_idx = sub / v_sub;
                s32 v_idx = sub % v_sub;
                v0 = (float)v_idx * dv;
                v1 = v0 + dv;
                if (v1 > 1.0f)
                    v1 = 1.0f;
                u0 = (float)u_idx * du;
                u1 = u0 + du;
                if (u1 > 1.0f)
                    u1 = 1.0f;
            } else if (max_life != 0.0f) {
                s32 total = u_sub * v_sub;
                s32 idx = 0;
                if (sub_flags & 2u) {
                    for (; idx < sub_scale_num; idx++) {
                        if (rel_t <= rdf32(m, sub_scale_va + (u32)idx * 4u))
                            break;
                    }
                } else if (sub_end != 0) {
                    s32 span = sub_end - sub_start;
                    if (span < 1)
                        span = 1;
                    idx = (s32)floorf(rel_t * (float)span) + sub_start;
                } else {
                    idx = (s32)floorf(rel_t * (float)total);
                }
                if (idx < 0)
                    idx = 0;
                else if (idx >= total)
                    idx = total - 1;

                s32 u_idx = idx / v_sub;
                s32 v_idx = idx % v_sub;
                v0 = (float)v_idx * dv;
                v1 = v0 + dv;
                if (v1 > 1.0f)
                    v1 = 1.0f;
                u0 = (float)u_idx * du;
                u1 = u0 + du;
                if (u1 > 1.0f)
                    u1 = 1.0f;

                if (sub_flags & 1u) {
                    s32 next = idx + 1;
                    if (next == total) {
                        u2_0 = u0;
                        u2_1 = u1;
                        v2_0 = v0;
                        v2_1 = v1;
                    } else {
                        s32 u2 = next / v_sub;
                        s32 v2 = next % v_sub;
                        v2_0 = (float)v2 * dv;
                        v2_1 = v2_0 + dv;
                        u2_0 = (float)u2 * du;
                        u2_1 = u2_0 + du;
                    }
                    float alpha_f;
                    if ((sub_flags & 2u) && sub_scale_num > total) {
                        float lo = idx == 0 ? 0.0f : rdf32(m, sub_scale_va + (u32)(idx - 1) * 4u);
                        float hi = rdf32(m, sub_scale_va + (u32)idx * 4u);
                        if (hi > 1.0f)
                            hi = 1.0f;
                        alpha_f = (rel_t - lo) / (hi - lo);
                    } else {
                        float x = rel_t * (float)total;
                        alpha_f = x - floorf(x);
                    }
                    u8 a_byte = (u8)(s32)(alpha_f * 255.0f);
                    col = (col & 0x00FFFFFFu) | ((u32)a_byte << 24);
                }
            }
        }

        sprite_emit_quad(dst, loc, up_v, rg_v, col, u0, u1, v0, v1, u2_0, u2_1, v2_0, v2_1);
        dst += 128u;
        emitted += 4u;
    }

    if (out_verts)
        *out_verts = emitted;
    return 1;
}

void ovr_engine_sprite_fill_vb(CPU *c)
{
    u8 *m = g_mem;
    u32 this_va = c->ecx;
    u32 verts_va = rd32(m, c->esp + 4u);
    u32 node_va = rd32(m, c->esp + 8u);
    u32 n_verts = 0u;

    if (!sprite_fill_vb_impl(m, this_va, verts_va, node_va, &n_verts)) {
        if (ovr_orig_ovr_engine_sprite_fill_vb)
            RT_ENTER_GEN(c, ovr_orig_ovr_engine_sprite_fill_vb);
        else {
            c->eax = 0u;
            c->esp += 12u;
        }
        return;
    }

    if (engine_particle_check() && ovr_orig_ovr_engine_sprite_fill_vb) {
        size_t bytes = (size_t)n_verts * 32u;
        u8 *got = bytes ? (u8 *)malloc(bytes) : NULL;
        if (got)
            memcpy(got, m + verts_va, bytes);
        RT_ENTER_GEN(c, ovr_orig_ovr_engine_sprite_fill_vb);
        if (got) {
            static int logged = 0;
            for (u32 i = 0; i < n_verts; i++) {
                float gp[3], wp[3], guv[4], wuv[4];
                u32 gc, wc;
                memcpy(gp, got + (size_t)i * 32u, 12u);
                memcpy(wp, m + verts_va + (size_t)i * 32u, 12u);
                memcpy(&gc, got + (size_t)i * 32u + 12u, 4u);
                memcpy(&wc, m + verts_va + (size_t)i * 32u + 12u, 4u);
                memcpy(guv, got + (size_t)i * 32u + 16u, 16u);
                memcpy(wuv, m + verts_va + (size_t)i * 32u + 16u, 16u);
                int c_bad = ((gc & 0x00FFFFFFu) != (wc & 0x00FFFFFFu)) ||
                            (abs((int)(gc >> 24) - (int)(wc >> 24)) > 1);
                int bad = c_bad;
                for (int k = 0; k < 3 && !bad; k++) {
                    if (!float_near(gp[k], wp[k], 2e-3f, 2e-4f))
                        bad = 1;
                }
                for (int k = 0; k < 4 && !bad; k++) {
                    if (!float_near(guv[k], wuv[k], 1e-4f, 1e-4f))
                        bad = 1;
                }
                if (bad && logged++ < 32) {
                    fprintf(stderr,
                            "[engine] SNX_PARTICLE_CHECK: v %u/%u dir=%u pos=(%.3f,%.3f,%.3f) vs (%.3f,%.3f,%.3f) "
                            "col=%08x vs %08x uv=(%.3f,%.3f,%.3f,%.3f) vs (%.3f,%.3f,%.3f,%.3f)\n",
                            i, n_verts, m[this_va + 0x4b0u], gp[0], gp[1], gp[2], wp[0], wp[1], wp[2], gc, wc, guv[0],
                            guv[1], guv[2], guv[3], wuv[0], wuv[1], wuv[2], wuv[3]);
                    break;
                }
            }
            free(got);
        }
        return;
    }

    c->eax = 0u;
    c->esp += 12u;  // thiscall, два аргумента: ret 8
}

// ---------------------------------------------------------------- D3DDrv: пропуск пустых __ehvec_ctor
// D3DDrv.dll!0x12802100 (__ehvec_ctor, __stdcall: ptr, stride, count, ctor -> ret 0x10):
// SetVertexStreams (0x1281bdd0) и SetDynamicStream (0x1281c140) на каждом вызове крутят 16×8 = 128 косвенных
// вызовов конструктора D3DDrv.dll!0x12819150 (через переходник 0x12801140), тело которого — пустой no-op
// `mov eax, ecx; ret` (8B C1 C3). То же относится к 0x12833116. Пропуск пустого конструктора убирает ~1,7% Core 1.
extern void (*const ovr_orig_ovr_d3ddrv_ehvec_ctor)(CPU *c);

void ovr_d3ddrv_ehvec_ctor(CPU *c)
{
    const u8 *m = g_mem;
    s32 count = (s32)rd32(m, c->esp + 12u);
    if (count <= 0) {
        c->esp += 20u;  // __stdcall, 4 аргумента: ret 0x10
        return;
    }
    u32 ctor = rd32(m, c->esp + 16u);
    if (ctor >= 0x10000000u && ctor + 5u < RT_WINDOW) {
        if (m[ctor] == 0xE9u) {
            s32 rel = (s32)rd32(m, ctor + 1u);
            ctor = ctor + 5u + (u32)rel;
        }
        if (ctor >= 0x10000000u && ctor + 3u < RT_WINDOW &&
            m[ctor] == 0x8Bu && m[ctor + 1u] == 0xC1u && m[ctor + 2u] == 0xC3u) {
            c->esp += 20u;
            return;
        }
    }
    if (ovr_orig_ovr_d3ddrv_ehvec_ctor)
        RT_ENTER_GEN(c, ovr_orig_ovr_d3ddrv_ehvec_ctor);
    else
        c->esp += 20u;
}

// ---------------------------------------------------------------- D3DDrv: SetMaterialPass + CommitState
// Обе функции побайтно совпадают в EN и RUS (0 отличий).
// D3DDrv.dll!0x12819eb0 (FD3DRenderInterface::SetMaterialPass, __thiscall, ret 8):
//   обновляет теневой FD3DStateCache (UD3DRenderDevice + 0x2f6b0) из FD3DMaterialState *Pass.
// D3DDrv.dll!0x1281fe80 (FD3DStateCache::Commit, __thiscall, ret):
//   сверяет CurrentState (sc + 0x000..0x924) с HardwareState (sc + 0x924..0x124c) и шлёт дельты напрямую
//   в d3d8_direct_set_* (минуя ~70 COM-трамплинов, x86-стек и вызовы appMemcmp/memcpy/rdtsc).
extern void (*const ovr_orig_ovr_d3ddrv_set_mat_pass)(CPU *c);
extern void (*const ovr_orig_ovr_d3ddrv_commit_state)(CPU *c);

static inline int engine_d3d_commit_check(void)  // SNX_D3D_COMMIT_CHECK=1: побайтная сверка с оригиналом
{
    static int v = -1;
    if (v < 0)
        v = getenv("SNX_D3D_COMMIT_CHECK") != NULL;
    return v;
}

static int d3ddrv_set_mat_pass_impl(u8 *m, u32 ri, u32 pass, u32 cull_mode)
{
    if (!ri || !pass)
        return 0;
    u32 cur_state = rd32(m, ri + 0x2aa0cu);
    if (!cur_state)
        return 0;
    if (rd32(m, cur_state + 0x550u) == pass)
        return 1;  // этот же материал уже установлен

    u32 rendev = rd32(m, ri + 4u);
    if (!rendev)
        return 0;
    if (rd32(m, pass) != 0u)
        return 0;  // редкий случай с пиксельным шейдером (вызов GetPixelShader) — отдаём оригиналу

    s32 num_stages = (s32)rd32(m, pass + 0x34u);
    s32 max_stages = (s32)rd32(m, rendev + 0x41f8u);
    if (num_stages < 0 || num_stages > 8 || max_stages < 0 || max_stages > 8)
        return 0;

    u32 sc = rendev + 0x2f6b0u;
    wr32(m, sc + 0x51cu, 0u);  // PixelShader = 0

    // RenderStates: sc + 4 + idx*4
    wr32(m, sc + 4u + 9u * 4u, rd32(m, pass + 0x04u) != 0u ? 1u : 0u);   // AlphaBlendEnable
    wr32(m, sc + 4u + 7u * 4u, (u32)m[pass + 0x0cu]);                    // AlphaRef
    wr32(m, sc + 4u + 8u * 4u, 5u);                                      // AlphaFunc = GREATER
    wr32(m, sc + 4u + 2u * 4u, rd32(m, pass + 0x08u) != 0u ? 1u : 0u);   // AlphaTestEnable

    u32 fill_mode = rd32(m, pass + 0x1cu);
    if (fill_mode == 0u || fill_mode == 2u) {
        wr32(m, sc + 4u + 0u * 4u, fill_mode == 0u ? 2u : 3u);           // FillMode: WIREFRAME / SOLID
        wr32(m, sc + 0x74u, 4u);                                         // TSS[0][0] = MODULATE
    } else if (fill_mode == 1u) {
        wr32(m, sc + 0x74u, 3u);                                         // TSS[0][0]
        wr32(m, sc + 4u + 0u * 4u, 3u);                                  // FillMode = SOLID
    }

    wr32(m, sc + 4u + 6u * 4u, rd32(m, pass + 0x10u) == 0u ? 8u : 4u);   // ZFunc: ALWAYS / LESSEQUAL
    wr32(m, sc + 4u + 1u * 4u, rd32(m, pass + 0x14u) != 0u ? 1u : 0u);   // ZWriteEnable
    wr32(m, sc + 4u + 5u * 4u, rd32(m, pass + 0x18u) != 0u ? 1u : cull_mode);  // CullMode
    wr32(m, sc + 4u + 3u * 4u, rd32(m, pass + 0x24u));                   // SrcBlend
    wr32(m, sc + 4u + 4u * 4u, rd32(m, pass + 0x28u));                   // DestBlend
    wr32(m, sc + 4u + 0x17u * 4u, rd32(m, pass + 0x20u));                // TextureFactor

    u32 dirty_xf = rd32(m, sc + 0x514u);
    for (s32 s = 0; s < num_stages; s++) {
        u32 st = pass + (u32)s * 0x84u;
        u32 tex_obj = rd32(m, st + 0x38u);
        u32 tex_ptr = 0u;
        if (tex_obj != 0u) {
            tex_ptr = rd32(m, tex_obj + 0x30u);  // CubeTexture
            if (!tex_ptr)
                tex_ptr = rd32(m, tex_obj + 0x2cu);  // Texture2D
        }
        wr32(m, sc + 0x5a8u + (u32)s * 4u, tex_ptr);

        u32 tss = sc + 0x74u + (u32)s * 0x3cu;
        wr32(m, tss + 7u * 4u, rd32(m, st + 0x3cu));   // AddressU
        wr32(m, tss + 8u * 4u, rd32(m, st + 0x40u));   // AddressV
        wr32(m, tss + 10u * 4u, rd32(m, st + 0x44u));  // AddressW

        float lod_bias = rdf32(m, st + 0x48u);
        if (lod_bias < -100.0f) {
            u32 def_bias = rd32(m, rendev + 0x4150u);
            wr32(m, st + 0x48u, def_bias);
            wr32(m, tss + 14u * 4u, def_bias);
        } else {
            wr32(m, tss + 14u * 4u, rd32(m, st + 0x48u));
        }

        wr32(m, tss + 0u * 4u, rd32(m, st + 0x4cu));   // ColorOp
        wr32(m, tss + 3u * 4u, rd32(m, st + 0x50u));   // AlphaOp
        wr32(m, tss + 11u * 4u, rd32(m, st + 0x54u));  // ResultArg
        wr32(m, tss + 1u * 4u, rd32(m, st + 0x58u));   // ColorArg1
        wr32(m, tss + 2u * 4u, rd32(m, st + 0x5cu));   // ColorArg2
        wr32(m, tss + 12u * 4u, rd32(m, st + 0x60u));  // ColorArg0
        wr32(m, tss + 4u * 4u, rd32(m, st + 0x64u));   // AlphaArg1
        wr32(m, tss + 5u * 4u, rd32(m, st + 0x68u));   // AlphaArg2
        wr32(m, tss + 13u * 4u, rd32(m, st + 0x6cu));  // AlphaArg0
        wr32(m, tss + 6u * 4u, rd32(m, st + 0x70u));   // TexCoordIndex

        u32 use_xf = rd32(m, st + 0x78u);
        wr32(m, tss + 9u * 4u, use_xf ? rd32(m, st + 0x74u) : 0u);  // TextureTransformFlags
        if (use_xf) {
            memcpy(m + sc + 0x254u + (u32)s * 0x40u, m + st + 0x7cu, 0x40u);
            dirty_xf |= (1u << (u32)s);
        }
    }
    wr32(m, sc + 0x514u, dirty_xf);

    for (s32 s = num_stages; s < max_stages; s++) {
        u32 st = pass + (u32)s * 0x84u;
        wr32(m, st + 0x4cu, 1u);
        wr32(m, st + 0x50u, 1u);
        wr32(m, st + 0x38u, 0u);
        u32 tss = sc + 0x74u + (u32)s * 0x3cu;
        wr32(m, tss + 0u * 4u, 1u);
        wr32(m, tss + 3u * 4u, 1u);
        wr32(m, sc + 0x5a8u + (u32)s * 4u, 0u);
    }

    wr32(m, cur_state + 0x550u, pass);
    return 1;
}

void ovr_d3ddrv_set_mat_pass(CPU *c)
{
    u8 *m = g_mem;
    u32 ri = c->ecx;
    u32 pass = rd32(m, c->esp + 4u);
    u32 cull_mode = rd32(m, c->esp + 8u);

    if (engine_d3d_commit_check() && ovr_orig_ovr_d3ddrv_set_mat_pass && ri && pass) {
        u32 cur_state = rd32(m, ri + 0x2aa0cu);
        u32 rendev = rd32(m, ri + 4u);
        if (cur_state && rendev && rd32(m, pass) == 0u) {
            u32 sc = rendev + 0x2f6b0u;
            u8 sc_orig[0x5c8], sc_got[0x5c8];
            u8 pass_orig[0x38 + 8 * 0x84];
            u32 cur_pass_orig = rd32(m, cur_state + 0x550u);
            memcpy(sc_orig, m + sc, sizeof sc_orig);
            memcpy(pass_orig, m + pass, sizeof pass_orig);
            if (d3ddrv_set_mat_pass_impl(m, ri, pass, cull_mode)) {
                u32 cur_pass_got = rd32(m, cur_state + 0x550u);
                memcpy(sc_got, m + sc, sizeof sc_got);
                memcpy(m + sc, sc_orig, sizeof sc_orig);
                memcpy(m + pass, pass_orig, sizeof pass_orig);
                wr32(m, cur_state + 0x550u, cur_pass_orig);
                RT_ENTER_GEN(c, ovr_orig_ovr_d3ddrv_set_mat_pass);
                static int logged = 0;
                if ((cur_pass_got != rd32(m, cur_state + 0x550u) || memcmp(sc_got, m + sc, sizeof sc_got) != 0) &&
                    logged++ < 32) {
                    fprintf(stderr, "[d3ddrv] SNX_D3D_COMMIT_CHECK: SetMaterialPass mismatch (pass=%08x)\n", pass);
                    for (u32 o = 0; o < sizeof sc_got; o += 4u) {
                        u32 g, w;
                        memcpy(&g, sc_got + o, 4u);
                        memcpy(&w, m + sc + o, 4u);
                        if (g != w) {
                            fprintf(stderr, "  sc+0x%03x: got %08x != orig %08x\n", o, g, w);
                            break;
                        }
                    }
                }
                return;
            }
        }
    }

    if (!d3ddrv_set_mat_pass_impl(m, ri, pass, cull_mode)) {
        if (ovr_orig_ovr_d3ddrv_set_mat_pass)
            RT_ENTER_GEN(c, ovr_orig_ovr_d3ddrv_set_mat_pass);
        else
            c->esp += 12u;
        return;
    }
    c->esp += 12u;  // thiscall, два аргумента: ret 8
}

#define D3D_HW_OFF 0x924u

static inline void commit_rs_slot(u8 *m, struct d3ddev *dev, u32 sc, u32 off, u32 d3d_rs)
{
    u32 v = rd32(m, sc + off);
    if (v != rd32(m, sc + off + D3D_HW_OFF)) {
        wr32(m, sc + off + D3D_HW_OFF, v);
        d3d8_direct_set_rs(dev, d3d_rs, v);
    }
}

static int d3ddrv_commit_state_impl(u8 *m, u32 sc)
{
    if (!sc)
        return 0;
    u32 rendev = rd32(m, sc);
    if (!rendev)
        return 0;
    u32 d3d_dev_guest = rd32(m, rendev + 0x46ccu);
    struct d3ddev *dev = d3d8_dev_from_guest(d3d_dev_guest);
    if (!dev)
        return 0;

    if (rd32(m, rendev + 0x4114u) != 0u && rd32(m, sc + 0x20u) == 0u)
        wr32(m, sc + 0x0cu, 0u);  // без AlphaRef принудительно гасим AlphaTest

    // 1. RenderStates (28 слотов: sc+0x04..0x70 против HardwareState sc+0x928..0x994)
    if (memcmp(m + sc + 0x04u, m + sc + 0x04u + D3D_HW_OFF, 0x70u) != 0) {
        commit_rs_slot(m, dev, sc, 0x04u, 0x08u);  // FILLMODE
        commit_rs_slot(m, dev, sc, 0x08u, 0x0eu);  // ZWRITEENABLE
        commit_rs_slot(m, dev, sc, 0x0cu, 0x0fu);  // ALPHATESTENABLE
        commit_rs_slot(m, dev, sc, 0x10u, 0x13u);  // SRCBLEND
        commit_rs_slot(m, dev, sc, 0x14u, 0x14u);  // DESTBLEND
        commit_rs_slot(m, dev, sc, 0x18u, 0x16u);  // CULLMODE
        commit_rs_slot(m, dev, sc, 0x1cu, 0x17u);  // ZFUNC
        commit_rs_slot(m, dev, sc, 0x20u, 0x18u);  // ALPHAREF
        commit_rs_slot(m, dev, sc, 0x24u, 0x19u);  // ALPHAFUNC
        commit_rs_slot(m, dev, sc, 0x28u, 0x1bu);  // ALPHABLENDENABLE
        commit_rs_slot(m, dev, sc, 0x2cu, 0x1cu);  // FOGENABLE
        if (rd32(m, sc + 0x2cu) != 0u) {
            commit_rs_slot(m, dev, sc, 0x30u, 0x22u);  // FOGCOLOR
            commit_rs_slot(m, dev, sc, 0x34u, 0x24u);  // FOGSTART
            commit_rs_slot(m, dev, sc, 0x38u, 0x25u);  // FOGEND
        }
        commit_rs_slot(m, dev, sc, 0x3cu, 0x2fu);  // ZBIAS
        if (rd32(m, rendev + 0x58u) != 0u) {       // UseStencil (GIsEditor == 0 в игре)
            commit_rs_slot(m, dev, sc, 0x40u, 0x34u);
            commit_rs_slot(m, dev, sc, 0x44u, 0x35u);
            commit_rs_slot(m, dev, sc, 0x48u, 0x36u);
            commit_rs_slot(m, dev, sc, 0x4cu, 0x37u);
            commit_rs_slot(m, dev, sc, 0x50u, 0x38u);
            commit_rs_slot(m, dev, sc, 0x54u, 0x39u);
            commit_rs_slot(m, dev, sc, 0x58u, 0x3au);
            commit_rs_slot(m, dev, sc, 0x5cu, 0x3bu);
        }
        commit_rs_slot(m, dev, sc, 0x60u, 0x3cu);  // TEXTUREFACTOR
        commit_rs_slot(m, dev, sc, 0x64u, 0x89u);  // LIGHTING
        commit_rs_slot(m, dev, sc, 0x68u, 0x8bu);  // AMBIENT
        commit_rs_slot(m, dev, sc, 0x6cu, 0x8du);  // COLORVERTEX
        commit_rs_slot(m, dev, sc, 0x70u, 0xa4u);  // PATCHSEGMENTS / MISC
    }

    // 2. TextureStageStates (до 8 стадий × 15 слотов = 60 Б на стадию)
    static const u32 k_tss_map[15] = {
        1u, 2u, 3u, 4u, 5u, 6u, 0x0bu, 0x0du, 0x0eu, 0x18u, 0x19u, 0x1au, 0x1bu, 0x1cu, 0x13u,
    };
    s32 num_stages = (s32)rd32(m, rendev + 0x41f8u);
    if (num_stages > 8)
        num_stages = 8;
    for (s32 s = 0; s < num_stages; s++) {
        u32 base = sc + 0x74u + (u32)s * 0x3cu;
        u32 hw = base + D3D_HW_OFF;
        if (memcmp(m + base, m + hw, 0x3cu) == 0)
            continue;
        for (u32 k = 0; k < 15u; k++) {
            u32 v = rd32(m, base + k * 4u);
            if (v != rd32(m, hw + k * 4u)) {
                wr32(m, hw + k * 4u, v);
                d3d8_direct_set_tss(dev, (u32)s, k_tss_map[k], v);
            }
        }
    }

    // 3. Матрицы трансформации (dirty-маска sc+0x514, биты 0..7 Texture0..7, 8 View, 9 Projection, 10 World)
    u32 dirty_xf = rd32(m, sc + 0x514u);
    if (dirty_xf & 0x7FFu) {
        static const u32 k_xf_type[11] = {
            0x10u, 0x11u, 0x12u, 0x13u, 0x14u, 0x15u, 0x16u, 0x17u, 2u, 3u, 0x100u,
        };
        for (u32 i = 0; i < 11u; i++) {
            if ((dirty_xf & (1u << i)) == 0u)
                continue;
            u32 cur_off = sc + 0x254u + i * 0x40u;
            u32 hw_off = cur_off + D3D_HW_OFF;
            if (memcmp(m + cur_off, m + hw_off, 0x40u) != 0) {
                float mat[16];
                memcpy(mat, m + cur_off, sizeof mat);
                memcpy(m + hw_off, mat, sizeof mat);
                d3d8_direct_set_transform(dev, k_xf_type[i], mat);
            }
        }
    }

    // 4. Текстуры (до 8 стадий: sc+0x5a8 против sc+0xecc)
    for (s32 s = 0; s < num_stages; s++) {
        u32 t = rd32(m, sc + 0x5a8u + (u32)s * 4u);
        if (t != rd32(m, sc + 0xeccu + (u32)s * 4u)) {
            wr32(m, sc + 0xeccu + (u32)s * 4u, t);
            d3d8_direct_set_texture(dev, (u32)s, t);
        }
    }

    // 5. Источники света (до 8: sc+0x5c8 по 0x68 Б, флаги sc+0x908 против sc+0x122c)
    s32 max_lights = (s32)rd32(m, rendev + 0x4204u);
    if (max_lights > 8)
        max_lights = 8;
    for (s32 i = 0; i < max_lights; i++) {
        u32 en = rd32(m, sc + 0x908u + (u32)i * 4u);
        if (en != 0u) {
            u32 l_cur = sc + 0x5c8u + (u32)i * 0x68u;
            u32 l_hw = sc + 0xeecu + (u32)i * 0x68u;
            memcpy(m + l_hw, m + l_cur, 0x68u);
            d3d8_direct_set_light(dev, (u32)i, m + l_cur);
        }
        if (en != rd32(m, sc + 0x122cu + (u32)i * 4u)) {
            wr32(m, sc + 0x122cu + (u32)i * 4u, en);
            d3d8_direct_light_enable(dev, (u32)i, en);
        }
    }

    // 6. VertexShader (sc+0x518 против sc+0xe3c)
    u32 vs = rd32(m, sc + 0x518u);
    if (vs != rd32(m, sc + 0xe3cu)) {
        wr32(m, sc + 0xe3cu, vs);
        d3d8_direct_set_vs(dev, vs);
    }

    // 7. Вершинные потоки (до 16: sc+0x520 по 8 Б {vb, stride} против sc+0xe44)
    s32 max_streams = (s32)rd32(m, rendev + 0x4220u);
    if (max_streams > 16)
        max_streams = 16;
    for (s32 i = 0; i < max_streams; i++) {
        u32 vb = rd32(m, sc + 0x520u + (u32)i * 8u);
        u32 stride = rd32(m, sc + 0x524u + (u32)i * 8u);
        if (vb != rd32(m, sc + 0xe44u + (u32)i * 8u) || stride != rd32(m, sc + 0xe48u + (u32)i * 8u)) {
            wr32(m, sc + 0xe44u + (u32)i * 8u, vb);
            wr32(m, sc + 0xe48u + (u32)i * 8u, stride);
            d3d8_direct_set_stream(dev, (u32)i, vb, stride);
        }
    }

    // 8. PixelShader (sc+0x51c против sc+0xe40)
    u32 ps = rd32(m, sc + 0x51cu);
    if (ps != rd32(m, sc + 0xe40u)) {
        wr32(m, sc + 0xe40u, ps);
        d3d8_direct_set_ps(dev, ps);
    }

    // 9. Индексный буфер (sc+0x5a0 {ib, base} против sc+0xec4)
    u32 ib = rd32(m, sc + 0x5a0u);
    u32 base_v = rd32(m, sc + 0x5a4u);
    if (ib != rd32(m, sc + 0xec4u) || base_v != rd32(m, sc + 0xec8u)) {
        wr32(m, sc + 0xec4u, ib);
        wr32(m, sc + 0xec8u, base_v);
        d3d8_direct_set_indices(dev, ib, base_v);
    }

    wr32(m, sc + 0x514u, 0u);  // сброс dirty-маски матриц
    return 1;
}

void ovr_d3ddrv_commit_state(CPU *c)
{
    u8 *m = g_mem;
    u32 sc = c->ecx;

    if (engine_d3d_commit_check() && ovr_orig_ovr_d3ddrv_commit_state && sc) {
        u8 sc_orig[0x124c], sc_got[0x124c];
        memcpy(sc_orig, m + sc, sizeof sc_orig);
        if (d3ddrv_commit_state_impl(m, sc)) {
            memcpy(sc_got, m + sc, sizeof sc_got);
            memcpy(m + sc, sc_orig, sizeof sc_orig);
            RT_ENTER_GEN(c, ovr_orig_ovr_d3ddrv_commit_state);
            static int logged = 0;
            if (memcmp(sc_got, m + sc, sizeof sc_got) != 0 && logged++ < 32) {
                fprintf(stderr, "[d3ddrv] SNX_D3D_COMMIT_CHECK: CommitState HardwareState mismatch (sc=%08x)\n", sc);
                for (u32 o = 0; o < sizeof sc_got; o += 4u) {
                    u32 g, w;
                    memcpy(&g, sc_got + o, 4u);
                    memcpy(&w, m + sc + o, 4u);
                    if (g != w) {
                        fprintf(stderr, "  sc+0x%04x: got %08x != orig %08x\n", o, g, w);
                        break;
                    }
                }
            }
            return;
        }
    }

    if (!d3ddrv_commit_state_impl(m, sc)) {
        if (ovr_orig_ovr_d3ddrv_commit_state)
            RT_ENTER_GEN(c, ovr_orig_ovr_d3ddrv_commit_state);
        else
            c->esp += 4u;
        return;
    }
    c->esp += 4u;  // thiscall без аргументов: ret
}

// ---------------------------------------------------------------- тени-проекторы: 1-проходное GPU-размытие
// Engine.dll!0x121e0050 (UShadowBitmapMaterial::Get, экспорт 0x120027ed) после рендера силуэта в ShadowTexture
// (+0x58) привязывает RenderTarget (+0x54) и делает 9 проходов FCanvasUtil::DrawTile (Engine.dll!0x121b6760)
// с 3×3 смещениями {-1.5, 0, +1.5} и UFinalBlend (FrameBufferBlending=8, SRCALPHA/ONE, alpha=28/255).
// Мы оборачиваем UShadowBitmapMaterial::Get (ставя флаговый контекст) и FCanvasUtil::DrawTile: на первом из 9
// тайлов, опознанном структурно по UFinalBlend(mode=8) -> UProxyBitmapMaterial(+0x54 == ShadowTexture), зовём
// d3d8_direct_shadow_blur (1 проход на GPU/Core 2), а тайлы 2..9 мгновенно пропускаем.
// Отключение для A/B-сверки: SNX_GPU_SHADOW_BLUR=0.
extern void (*const ovr_orig_ovr_engine_shadow_get)(CPU *c);
extern void (*const ovr_orig_ovr_engine_shadow_draw_tile)(CPU *c);

static inline int engine_gpu_shadow_blur_on(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("SNX_GPU_SHADOW_BLUR");
        v = !(e && e[0] == '0' && e[1] == '\0');
    }
    return v;
}

typedef struct {
    int active;
    int done;
    u32 mat_va;
    u32 viewport_va;
} shadow_blur_ctx_t;

static _Thread_local shadow_blur_ctx_t g_shadow_ctx;

// Поиск IDirect3DTexture8* (FD3DTexture+0x2c) в хеш-таблице UD3DRenderDevice (+0xc8), 1-в-1 с D3DDrv!0x12810e70
static u32 d3ddrv_lookup_rt_tex(const u8 *m, u32 rendev, u32 rt_va)
{
    if (!rendev || !rt_va)
        return 0u;
    u32 cache_lo = rd32(m, rt_va + 4u);
    u32 cache_hi = rd32(m, rt_va + 8u);
    u32 h = (((cache_lo >> 4) & 0xFF0u) + ((cache_lo >> 16) & 0xFu)) ^ (cache_lo & 0xFFFu);
    if (rendev + 0xc8u + h * 4u + 4u >= RT_WINDOW)
        return 0u;
    for (u32 res = rd32(m, rendev + 0xc8u + h * 4u), guard = 0u; res && guard < 1024u; guard++) {
        if (res + 0x34u >= RT_WINDOW)
            break;
        if (rd32(m, res + 8u) == cache_lo && rd32(m, res + 0xcu) == cache_hi)
            return rd32(m, res + 0x2cu);  // FD3DTexture::Direct3DTexture8
        res = rd32(m, res + 0x1cu);
    }
    return 0u;
}

void ovr_engine_shadow_get(CPU *c)
{
    if (!engine_gpu_shadow_blur_on() || !ovr_orig_ovr_engine_shadow_get) {
        if (ovr_orig_ovr_engine_shadow_get)
            RT_ENTER_GEN(c, ovr_orig_ovr_engine_shadow_get);
        else
            c->esp += 16u;
        return;
    }
    shadow_blur_ctx_t prev = g_shadow_ctx;
    g_shadow_ctx.active = 1;
    g_shadow_ctx.done = 0;
    g_shadow_ctx.mat_va = c->ecx;
    g_shadow_ctx.viewport_va = rd32(g_mem, c->esp + 12u);  // double Time (+4..+11), UViewport* (+12)
    RT_ENTER_GEN(c, ovr_orig_ovr_engine_shadow_get);
    g_shadow_ctx = prev;
}

void ovr_engine_shadow_draw_tile(CPU *c)
{
    if (!g_shadow_ctx.active) {
        if (ovr_orig_ovr_engine_shadow_draw_tile)
            RT_ENTER_GEN(c, ovr_orig_ovr_engine_shadow_draw_tile);
        else
            c->esp += 0x30u;
        return;
    }

    const u8 *m = g_mem;
    // Структурная проверка: это именно тайл размытия UShadowBitmapMaterial (UFinalBlend mode=8 -> ShadowTexture)
    u32 mat_arg = rd32(m, c->esp + 0x28u);  // 9 float (36 Б) + 4 Б ret = +0x28
    u32 src_rt = rd32(m, g_shadow_ctx.mat_va + 0x58u);
    int is_blur_tile = 0;
    if (mat_arg && src_rt && mat_arg + 0x44u < RT_WINDOW && m[mat_arg + 0x40u] == 8u) {
        u32 proxy = rd32(m, mat_arg + 0x3cu);
        if (proxy && proxy + 0x58u < RT_WINDOW && rd32(m, proxy + 0x54u) == src_rt)
            is_blur_tile = 1;
    }

    if (is_blur_tile) {
        if (g_shadow_ctx.done) {
            c->esp += 0x30u;  // проходы 2..9 пропущены: ret 0x2c
            return;
        }
        u32 vp = g_shadow_ctx.viewport_va;
        u32 ri = vp ? rd32(m, vp + 0x14cu) : 0u;
        u32 rendev = (ri && ri + 8u < RT_WINDOW) ? rd32(m, ri + 4u) : 0u;
        u32 d3d_dev_guest = (rendev && rendev + 0x46d0u < RT_WINDOW) ? rd32(m, rendev + 0x46ccu) : 0u;
        struct d3ddev *dev = d3d_dev_guest ? d3d8_dev_from_guest(d3d_dev_guest) : NULL;
        u32 src_tex_guest = dev ? d3ddrv_lookup_rt_tex(m, rendev, src_rt) : 0u;
        if (dev && src_tex_guest && d3d8_direct_shadow_blur(dev, src_tex_guest, 0u, 1.5f)) {
            g_shadow_ctx.done = 1;
            c->esp += 0x30u;  // ret 0x2c
            return;
        }
    }

    if (ovr_orig_ovr_engine_shadow_draw_tile)
        RT_ENTER_GEN(c, ovr_orig_ovr_engine_shadow_draw_tile);
    else
        c->esp += 0x30u;
}
