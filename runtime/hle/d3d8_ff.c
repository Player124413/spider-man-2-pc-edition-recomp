// HLE Direct3D 8: отрисовка поверх GL 4.1 core (Android — ES 3.0, platform/gles_shim.h) — цели рендера, состояние GL,
// фиксированный конвейер D3D
// (преобразования, освещение по формулам D3D, туман, генерация/матрицы текстурных координат, текстурные стадии,
// альфа-тест), сгенерированный в GLSL по ключу состояния с динамическим кешем программ, кешем бинарников GL
// и пост-проходом Present (переворот по Y, пропорции / 4:3-pillarbox меню, гамма-LUT 256x1 RGB16).
// Матрицы D3D (строки, вектор-строка) загружаются в GL как есть: GL читает их по столбцам, получается
// транспонированная — ровно то, что нужно для M * v.
// Все вызовы GL в этом файле выполняются исключительно на потоке рендера (Core 2, либо Core 1 в direct-режиме),
// поэтому макросы cs_gl* здесь отключены (GL_CS_NO_WRAP).
#define GL_CS_NO_WRAP
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#ifdef __APPLE__
#include <libproc.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

#include "platform/window.h"
#ifdef __ANDROID__
#include "platform/android/perf.h"
#endif
#include "runtime/hle/bink.h"
#include "runtime/game/menu.h"
#include "runtime/game/rumble.h"
#include "runtime/hle/d3d8.h"
#include "runtime/script_fix.h"
#include "runtime/hle/d3d8_int.h"
#include "runtime/hle/overlay.h"
#include "runtime/hle/vfs.h"

#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#endif

static float u2f(u32 v)
{
    float f;
    memcpy(&f, &v, 4);
    return f;
}

// d3d * d3d (строки): r = a * b
static void mat_mul(float *r, const float *a, const float *b)
{
    float t[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            t[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
    memcpy(r, t, 64);
}

// Нормальная матрица N = transpose(inverse(mat3(u_wv))) в постолбцовом порядке GL.
// В GLSL столбцы mat3(u_wv) — это первые 3 строки wv: c0 = wv[0..2], c1 = wv[4..6], c2 = wv[8..10].
// Столбцы матрицы кофакторов: n0 = c1 x c2, n1 = c2 x c0, n2 = c0 x c1; det = dot(c0, n0).
static void compute_normal_mat(float nm[9], const float wv[16])
{
    const float *c0 = wv, *c1 = wv + 4, *c2 = wv + 8;
    float n0[3] = {c1[1] * c2[2] - c1[2] * c2[1], c1[2] * c2[0] - c1[0] * c2[2], c1[0] * c2[1] - c1[1] * c2[0]};
    float n1[3] = {c2[1] * c0[2] - c2[2] * c0[1], c2[2] * c0[0] - c2[0] * c0[2], c2[0] * c0[1] - c2[1] * c0[0]};
    float n2[3] = {c0[1] * c1[2] - c0[2] * c1[1], c0[2] * c1[0] - c0[0] * c1[2], c0[0] * c1[1] - c0[1] * c1[0]};
    float det = c0[0] * n0[0] + c0[1] * n0[1] + c0[2] * n0[2];
    if (fabsf(det) > 1e-20f) {
        float inv = 1.0f / det;
        nm[0] = n0[0] * inv; nm[1] = n0[1] * inv; nm[2] = n0[2] * inv;
        nm[3] = n1[0] * inv; nm[4] = n1[1] * inv; nm[5] = n1[2] * inv;
        nm[6] = n2[0] * inv; nm[7] = n2[1] * inv; nm[8] = n2[2] * inv;
    } else {
        nm[0] = c0[0]; nm[1] = c0[1]; nm[2] = c0[2];
        nm[3] = c1[0]; nm[4] = c1[1]; nm[5] = c1[2];
        nm[6] = c2[0]; nm[7] = c2[1]; nm[8] = c2[2];
    }
}

// ================================================================ цели рендера (Core 2)
static void dump_target_r(d3d_rstate *r);
static int dumping_frame(int frame);

static void bind_target_r(d3d_rstate *r)
{
    if (!r->fbo)
        glGenFramebuffers(1, &r->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, r->fbo);
    if (r->fbo_rt != r->rt) {
        d3d8_fbo_attach(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, r->rt);
        r->fbo_rt = r->rt;
    }
    if (r->fbo_ds != r->ds) {
        d3dsurf *s = r->ds;
        const d3dfmt *f = s ? d3d8_format(s->fmt) : NULL;
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
        if (s)
            d3d8_fbo_attach(GL_FRAMEBUFFER, f->stencil ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT, s);
        r->fbo_ds = s;
    }
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE)
        fprintf(stderr, "[d3d8] framebuffer incomplete 0x%x (rt %ux%u fmt %u)\n", st, r->rt ? r->rt->w : 0,
                r->rt ? r->rt->h : 0, r->rt ? r->rt->fmt : 0);
    r->gen_vp++;
    r->gl_dirty |= GLD_ALL;
}

// ================================================================ ключ фиксированного конвейера
enum { FOG_NONE, FOG_SPECULAR, FOG_VERTEX, FOG_PIXEL };

typedef struct {
    u8 cop, ca1, ca2, ca0, aop, aa1, aa2, aa0, res, tex;  // tex: 0 нет, 1 2D, 2 куб
    u8 tci, tcgen, ttf_count, ttf_proj;
} stagekey;

typedef struct {
    u64 ps_hash;           // 64-битный хеш байткода ps.1.x (0 — фиксированные текстурные стадии)
    u16 inputs;            // маска регистров D3DVSDE (0..15), есть в вершине
    u8 tcsize[8];          // компонент во входных наборах текстурных координат
    u8 rhw, lighting, normalize, localviewer, specular, colorvertex;
    u8 fog, fogmode, rangefog, alphafunc, clipmask, nstages;
    u8 mat_src[4];         // diffuse, specular, ambient, emissive: 0 материал, 1 color1, 2 color2
    u8 light[MAX_LIGHTS];  // 0 выкл, 1 point, 2 spot, 3 directional
    stagekey st[MAX_STAGES];
#ifdef SNX_GLES
    u16 bgra;              // входы D3DCOLOR (B,G,R,A в памяти): в ES нет размера GL_BGRA — переставляет шейдер
    u8 clipvary;           // плоскости отсечения без GL_EXT_clip_cull_distance: varying и discard
#endif
} ffkey;

// диагностика кадра (ниже): пишем состояние первых отрисовок кадра, показываем поверх картинки
static void diag_record(const d3d_rstate *r, const ffkey *k);
static _Atomic u32 g_diag_clear;  // последний цвет заливки заднего буфера (пишет x_clear)
// Самодиагностика картинки: раз в DIAG_ROT кадров включается один отладочный вариант состояния (0 — как есть,
// 1 — туман розовый, 2 — без texgen, 3 — TextureFactor белый, 4 — стадия 0 только текстура). По строке «[diag]»
// в логе видно, какой вариант меняет цвет неба, — причина находится по одному прогону. Варианты, меняющие только
// uniform'ы (1 и 3), не требуют новых программ; 2 и 4 собирают по одной программе на материал.
#define DIAG_ROT 30          // кадров на один отладочный вариант в «Проверке неба»
#define DIAG_VARIANTS 5
#define DIAG_LOG_EVERY 600   // кадров между строками «[diag] pic» (раз в ~10 с игры)
static _Atomic int g_diag_variant;
static _Atomic int g_diag_cycles;   // сколько кругов «Проверки неба» ещё осталось

void d3d8_sky_test(int cycles)
{
    atomic_store(&g_diag_cycles, cycles > 0 ? cycles : 0);
    atomic_store(&g_diag_variant, 0);
    fprintf(stderr, "[diag] sky test: %d cycle(s) — варианты пойдут по кругу\n", cycles);
}

static void diag_sweep_note(void)
{
    static int done;
    if (done)
        return;
    done = 1;
    fprintf(stderr,
            "[diag] картинка: «[diag] pic» раз в ~10 с игры — полосы кадра и первые отрисовки; «Проверка неба» "
            "в шестерёнке крутит варианты v0 как есть, v1 туман розовый, v2 без texgen, v3 TextureFactor белый, "
            "v4 стадия 0 = только текстура (по %d кадров на вариант)\n",
            DIAG_ROT);
}

typedef struct {
    ffkey key;
    u32 hash;
    GLuint prog;
    GLint u_wv, u_proj, u_world, u_nrm_mat, u_vp, u_depth, u_texm, u_mat, u_power, u_ambient, u_ld, u_ls, u_la,
        u_lpos, u_ldir, u_latt, u_fog, u_fogcolor, u_clip, u_tfactor, u_aref, u_psc, u_bump;
    u32 gen_xf, gen_view, gen_texm, gen_vp, gen_clip, gen_light, gen_mat, gen_const, gen_psc, gen_bump;
    int last_frame;
    _Atomic int ready;  // собрана потоком рендера (программы прошлых запусков собираются в фоне)
} ffprog;

// Динамическая хеш-таблица указателей ffprog*: принадлежит ИСКЛЮЧИТЕЛЬНО потоку рендера (Core 2).
#define PROG_INIT_CAP 4096u
#define PROG_MAX_CAP 65536u
static ffprog **g_progs;
static u32 g_prog_cap;
static _Atomic u32 g_nprogs;
static GLuint g_cur_prog;
static ffprog *g_mru[4];  // MRU-кеш последних программ: убирает хеширование и пробинг при повторяющихся материалах
static ffkey g_cached_key;
static ffprog *g_cached_prog;  // программа для текущего неизменного состояния (key_dirty == 0)

// Копия таблицы объявлений вершинных шейдеров для Core 2 (обновляется через gl_cs)
static struct {
    int used;
    vdecl decl;
} g_r_vs[D3D8_MAX_VS];

static int r_vdecl_for(const d3d_rstate *r, vdecl *out)
{
    if (r->vs < D3D8_VS_HANDLE_BASE) {
        d3d8_decl_from_fvf(r->vs, out);
        return out->n > 0;
    }
    u32 i = r->vs - D3D8_VS_HANDLE_BASE;
    if (i >= D3D8_MAX_VS || !g_r_vs[i].used)
        return 0;
    *out = g_r_vs[i].decl;
    return 1;
}

static u32 fnv(const void *p, size_t n)
{
    const u8 *b = p;
    u64 h = 14695981039346656037ull;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        u64 w;
        memcpy(&w, b + i, 8);
        h = (h ^ w) * 1099511628211ull;
    }
    for (; i < n; i++)
        h = (h ^ b[i]) * 1099511628211ull;
    return (u32)(h ^ (h >> 32));
}

static void make_key(const d3d_rstate *d, const vdecl *decl, ffkey *k)
{
    memset(k, 0, sizeof *k);
    for (int i = 0; i < decl->n; i++) {
        u32 r = decl->e[i].reg;
        if (r < 16)
            k->inputs |= (u16)(1u << r);
        if (r >= 7 && r < 15) {
            static const u8 comps[8] = {1, 2, 3, 4, 4, 4, 2, 4};
            k->tcsize[r - 7] = comps[decl->e[i].type & 7];
        }
#ifdef SNX_GLES
        if (r < 16 && decl->e[i].type == 4)
            k->bgra |= (u16)(1u << r);
#endif
    }
    const u32 *rs = d->rs;
    k->rhw = (u8)decl->rhw;
    k->lighting = !decl->rhw && rs[137];
    k->specular = rs[29] != 0;
    if (k->lighting) {
        k->normalize = rs[143] != 0;
        k->localviewer = rs[142] != 0;
        k->colorvertex = rs[141] != 0;
        static const u32 src_rs[4] = {145, 146, 147, 148};  // DIFFUSE/SPECULAR/AMBIENT/EMISSIVEMATERIALSOURCE
        for (int i = 0; i < 4; i++) {
            u32 s = k->colorvertex ? rs[src_rs[i]] : 0;
            if (s == 1 && !(k->inputs & (1u << 5)))
                s = 0;
            if (s == 2 && !(k->inputs & (1u << 6)))
                s = 0;
            k->mat_src[i] = (u8)(s <= 2 ? s : 0);
        }
        for (int i = 0; i < MAX_LIGHTS; i++)
            k->light[i] = d->light_on[i] ? (u8)(d->lights[i].type <= 3 ? d->lights[i].type : 3) : 0;
    }
    if (rs[28]) {  // FOGENABLE
        u32 table = rs[35], vert = rs[140];
        if (table) {
            k->fog = FOG_PIXEL;
            k->fogmode = (u8)table;
        } else if (vert && !decl->rhw) {
            k->fog = FOG_VERTEX;
            k->fogmode = (u8)vert;
            k->rangefog = rs[48] != 0;
        } else
            k->fog = FOG_SPECULAR;
    }
    k->alphafunc = rs[15] ? (u8)rs[25] : 8;  // ALPHATESTENABLE ? ALPHAFUNC : ALWAYS
    if (k->alphafunc == 8)
        k->alphafunc = 0;
    k->clipmask = decl->rhw ? 0 : (u8)(rs[152] & 0x3F);
#ifdef SNX_GLES
    k->clipvary = k->clipmask && !gles_caps.clip_distance;
#endif
    k->ps_hash = d->ps ? d3d8_r_ps_hash(d->ps) : 0;
    int nst = 0;
    for (int s = 0; s < MAX_STAGES; s++) {
        const u32 *t = d->tss[s];
        if (!k->ps_hash && t[1] == 1)  // COLOROP DISABLE: дальше стадий нет
            break;
        if (k->ps_hash && s >= 6)
            break;
        stagekey *sk = &k->st[s];
        if (!k->ps_hash) {
            sk->cop = (u8)t[1];
            sk->ca1 = (u8)t[2];
            sk->ca2 = (u8)t[3];
            sk->ca0 = (u8)t[26];
            sk->aop = (u8)(t[4] == 1 ? 2 : t[4]);  // ALPHAOP DISABLE при включённом цвете — как SELECTARG1(CURRENT)
            sk->aa1 = (u8)(t[4] == 1 ? 1 : t[5]);
            sk->aa2 = (u8)t[6];
            sk->aa0 = (u8)t[27];
            sk->res = (u8)(t[28] == 5);  // RESULTARG: TEMP
            if (g_diag_variant == 4 && s == 0) {  // вариант самодиагностики: cur = текстура
                sk->cop = 2;  // D3DTOP_SELECTARG1
                sk->ca1 = 2;  // D3DTA_TEXTURE
                sk->aop = 2;
                sk->aa1 = 2;
            }
        }
        sk->tex = d->tex[s] ? (u8)(d->tex[s]->cube ? 2 : 1) : 0;
        sk->tci = (u8)(t[11] & 7);
        sk->tcgen = (u8)((t[11] >> 16) & 7);  // 1..4: NORMAL, POSITION, REFLECTION, SPHEREMAP
        if (g_diag_variant == 2)
            sk->tcgen = 0;  // вариант самодиагностики: координаты только из вершины
        sk->ttf_count = (u8)(t[24] & 7);
        sk->ttf_proj = (t[24] & 0x100) != 0;
        nst = s + 1;
    }
    k->nstages = (u8)nst;
}

// ================================================================ генерация GLSL (GL 4.1 core / GL 4.3)
typedef struct {
    char *p;
    size_t n, cap;
} sbuf;

static void put(sbuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void put(sbuf *b, const char *fmt, ...)
{
    va_list ap;
    for (;;) {
        if (!b->p) {
            b->cap = 8192;
            b->p = malloc(b->cap);
            if (!b->p)
                return;
        }
        va_start(ap, fmt);
        int w = vsnprintf(b->p + b->n, b->cap - b->n, fmt, ap);
        va_end(ap);
        if (w >= 0 && b->n + (size_t)w < b->cap) {
            b->n += (size_t)w;
            return;
        }
        b->cap *= 2;
        char *np = realloc(b->p, b->cap);
        if (!np)
            return;
        b->p = np;
    }
}

// вход D3DCOLOR в ES (ffkey.bgra): атрибут читается как есть под именем <имя>_in, а в начале main — перестановка
static int in_bgra(const ffkey *k, int reg)
{
#ifdef SNX_GLES
    return (k->bgra >> reg) & 1;
#else
    (void)k;
    (void)reg;
    return 0;
#endif
}

static int clip_vary(const ffkey *k)
{
#ifdef SNX_GLES
    return k->clipvary;
#else
    (void)k;
    return 0;
#endif
}

static void gen_vs(const ffkey *k, sbuf *b)
{
    put(b, SNX_GLSL_HEADER);
    if (k->clipmask && !clip_vary(k) && SNX_IS_GLES)
        put(b, "#extension GL_EXT_clip_cull_distance : enable\n");
    put(b, SNX_GLSL_PRECISION);
    put(b, "layout(location=0) in vec4 a_pos;\n");
    if (k->inputs & (1u << 3))
        put(b, "layout(location=3) in vec3 a_nrm;\n");
    if (k->inputs & (1u << 5))
        put(b, "layout(location=5) in vec4 a_col0%s;\n", in_bgra(k, 5) ? "_in" : "");
    if (k->inputs & (1u << 6))
        put(b, "layout(location=6) in vec4 a_col1%s;\n", in_bgra(k, 6) ? "_in" : "");
    for (int i = 0; i < 8; i++)
        if (k->inputs & (1u << (7 + i)))
            put(b, "layout(location=%d) in vec4 a_tc%d%s;\n", 7 + i, i, in_bgra(k, 7 + i) ? "_in" : "");
    put(b, "uniform mat4 u_wv, u_proj, u_world, u_texm[8];\n"
           "uniform mat3 u_nrm_mat;\n"
           "uniform vec4 u_vp; uniform vec2 u_depth;\n"
           "uniform vec4 u_mat[4]; uniform float u_power; uniform vec4 u_ambient;\n"
           "uniform vec4 u_ld[8], u_ls[8], u_la[8], u_lpos[8], u_ldir[8], u_latt[8];\n"
           "uniform vec4 u_fog; uniform vec4 u_clip[6];\n"
           "out vec4 v_col0, v_col1; out float v_fog;\n");
    for (int s = 0; s < k->nstages; s++)
        put(b, "out vec4 v_tc%d;\n", s);
    if (clip_vary(k))
        put(b, "out float v_clip[6];\n");
    else if (k->clipmask && !SNX_IS_GLES)
        put(b, "out float gl_ClipDistance[6];\n");
    put(b, "void main() {\n");
    if (in_bgra(k, 5) && (k->inputs & (1u << 5)))
        put(b, "  vec4 a_col0 = a_col0_in.zyxw;\n");
    if (in_bgra(k, 6) && (k->inputs & (1u << 6)))
        put(b, "  vec4 a_col1 = a_col1_in.zyxw;\n");
    for (int i = 0; i < 8; i++)
        if (in_bgra(k, 7 + i) && (k->inputs & (1u << (7 + i))))
            put(b, "  vec4 a_tc%d = a_tc%d_in.zyxw;\n", i, i);
    if (k->rhw) {
        // экранные координаты D3D -> NDC; центры пикселей D3D целые (GL: +0,5)
        put(b, "  float w = a_pos.w != 0.0 ? 1.0 / a_pos.w : 1.0;\n"
               "  vec2 ndc = (a_pos.xy + 0.5 - u_vp.xy) / u_vp.zw * 2.0 - 1.0;\n"
               "  float z = (a_pos.z - u_depth.x) / max(u_depth.y - u_depth.x, 1e-6) * 2.0 - 1.0;\n"
               "  gl_Position = vec4(ndc * w, z * w, w);\n"
               "  vec4 epos = vec4(0.0, 0.0, w, 1.0);\n");
    } else {
        put(b, "  vec4 pos = vec4(a_pos.xyz, 1.0);\n"
               "  vec4 epos = u_wv * pos;\n"
               "  vec4 cp = u_proj * epos;\n"
               // полпикселя (центры пикселей D3D целые): вправо и вниз по экрану, а ось y клипа D3D — вверх.
               // С неверным знаком по y полноэкранный квад не накрывал нижнюю строку цели (чёрный край
               // размытой текстуры тени при CLAMP затемнял целые треугольники под проектором).
               "  cp.x += cp.w / u_vp.z;\n"
               "  cp.y -= cp.w / u_vp.w;\n"
               "  cp.y = -cp.y;\n"               // кадр хранится сверху вниз
               "  cp.z = 2.0 * cp.z - cp.w;\n"   // глубина D3D [0,w] -> GL [-w,w]
               "  gl_Position = cp;\n");
        for (int i = 0; i < 6; i++)
            if (k->clipmask & (1u << i))
                put(b, "  %s[%d] = dot(u_world * pos, u_clip[%d]);\n", clip_vary(k) ? "v_clip" : "gl_ClipDistance", i, i);
    }
    int need_en = k->lighting;
    for (int s = 0; s < k->nstages; s++)
        need_en |= k->st[s].tcgen == 1 || k->st[s].tcgen == 3 || k->st[s].tcgen == 4;
    if (need_en && !k->rhw)
        put(b, "  vec3 En = %s;\n", (k->inputs & (1u << 3)) ? "u_nrm_mat * a_nrm" : "vec3(0.0)");
    const char *c0 = (k->inputs & (1u << 5)) ? "a_col0" : "vec4(1.0)";
    const char *c1 = (k->inputs & (1u << 6)) ? "a_col1" : "vec4(0.0)";
    if (k->lighting) {
        static const char *src[3] = {NULL, "a_col0", "a_col1"};
        const char *md = k->mat_src[0] ? src[k->mat_src[0]] : "u_mat[0]";
        const char *ms = k->mat_src[1] ? src[k->mat_src[1]] : "u_mat[2]";
        const char *ma = k->mat_src[2] ? src[k->mat_src[2]] : "u_mat[1]";
        const char *me = k->mat_src[3] ? src[k->mat_src[3]] : "u_mat[3]";
        put(b, "  vec3 N = En;\n");
        if (k->normalize)
            put(b, "  if (dot(N, N) > 0.0) N = normalize(N);\n");
        put(b, "  vec3 V = %s;\n",
            k->localviewer ? "(dot(epos.xyz, epos.xyz) > 0.0 ? normalize(-epos.xyz) : vec3(0.0, 0.0, -1.0))"
                           : "vec3(0.0, 0.0, -1.0)");
        put(b, "  vec3 dif = vec3(0.0), amb = vec3(0.0), spe = vec3(0.0);\n");
        for (int i = 0; i < MAX_LIGHTS; i++) {
            if (!k->light[i])
                continue;
            put(b, "  {\n");
            if (k->light[i] == 3)
                put(b, "    vec3 L = -u_ldir[%d].xyz; float att = 1.0;\n", i);
            else {
                put(b, "    vec3 Lv = u_lpos[%d].xyz - epos.xyz; float dist = length(Lv); vec3 L = Lv / max(dist, 1e-6);\n"
                       "    float att = dist > u_lpos[%d].w ? 0.0 : 1.0 / max(u_latt[%d].x + u_latt[%d].y * dist + u_latt[%d].z * dist * dist, 1e-6);\n",
                    i, i, i, i, i);
                if (k->light[i] == 2)  // spot: u_ldir.w = falloff, u_ld.w/u_ls.w — cos(theta/2), cos(phi/2)
                    put(b, "    float rho = dot(-L, u_ldir[%d].xyz);\n"
                           "    att *= rho > u_ld[%d].w ? 1.0 : rho <= u_ls[%d].w ? 0.0 : pow((rho - u_ls[%d].w) / max(u_ld[%d].w - u_ls[%d].w, 1e-6), u_ldir[%d].w);\n",
                        i, i, i, i, i, i, i);
            }
            put(b, "    float nl = max(dot(N, L), 0.0);\n"
                   "    dif += u_ld[%d].rgb * nl * att; amb += u_la[%d].rgb * att;\n"
                   "    if (nl > 0.0 && u_power > 0.0) { vec3 H = L + V; if (dot(H, H) > 0.0) spe += u_ls[%d].rgb * pow(max(dot(N, normalize(H)), 0.0), u_power) * att; }\n"
                   "  }\n",
                i, i, i);
        }
        put(b, "  v_col0 = vec4(clamp(%s.rgb + %s.rgb * (u_ambient.rgb + amb) + %s.rgb * dif, 0.0, 1.0), %s.a);\n",
            me, ma, md, md);
        put(b, "  v_col1 = vec4(clamp(%s.rgb * spe, 0.0, 1.0), %s.a);\n", ms, c1);
    } else {
        put(b, "  v_col0 = %s; v_col1 = %s;\n", c0, c1);
    }
    // туман: v_fog — коэффициент (1 — без тумана) или глубина для попиксельного
    switch (k->fog) {
    case FOG_SPECULAR:
        // D3D8: FOGENABLE без табличного и вершинного режимов — коэффициент тумана в альфе specular-цвета.
        // Если specular в формате вершины нет, аппаратура берёт 1.0 (тумана нет): раньше здесь всегда было
        // v_col1.a = 0, и такие отрисовки (небо рисуется именно так — без вершинных цветов) заливались
        // сплошным цветом тумана вместо своей текстуры.
        put(b, "  v_fog = %s;\n", (k->inputs & (1u << 6)) ? "v_col1.a" : "1.0");
        break;
    case FOG_VERTEX:
        put(b, "  float fd = %s;\n", k->rangefog ? "length(epos.xyz)" : "abs(epos.z)");
        if (k->fogmode == 3)
            put(b, "  v_fog = (u_fog.y - fd) * u_fog.w;\n");
        else if (k->fogmode == 1)
            put(b, "  v_fog = exp(-u_fog.z * fd);\n");
        else
            put(b, "  v_fog = exp(-(u_fog.z * fd) * (u_fog.z * fd));\n");
        break;
    case FOG_PIXEL:
        put(b, "  v_fog = abs(epos.z);\n");
        break;
    default:
        put(b, "  v_fog = 1.0;\n");
    }
    // текстурные координаты стадий
    for (int s = 0; s < k->nstages; s++) {
        const stagekey *sk = &k->st[s];
        int n;  // компонент исходного вектора
        switch (sk->tcgen) {
        case 1:
            put(b, "  vec4 tc%d = vec4(%s, 1.0);\n", s,
                k->rhw ? "vec3(0.0)" : "(dot(En, En) > 0.0 ? normalize(En) : vec3(0.0))");
            n = 3;
            break;
        case 2:
            put(b, "  vec4 tc%d = vec4(epos.xyz, 1.0);\n", s);
            n = 3;
            break;
        case 3:
            put(b, "  vec4 tc%d = vec4(%s, 1.0);\n", s,
                k->rhw ? "vec3(0.0)"
                       : "reflect(dot(epos.xyz, epos.xyz) > 0.0 ? normalize(epos.xyz) : vec3(0.0, 0.0, 1.0), "
                         "dot(En, En) > 0.0 ? normalize(En) : vec3(0.0, 0.0, 1.0))");
            n = 3;
            break;
        case 4:  // SPHEREMAP: u,v = 0.5 + 0.5 * n.xy / sqrt(nx^2 + ny^2 + (nz + 1)^2)
            put(b, "  vec3 sm = %s;\n",
                k->rhw ? "vec3(0.0, 0.0, 1.0)"
                       : "(dot(En, En) > 0.0 ? normalize(En) : vec3(0.0, 0.0, 1.0))");
            put(b, "  float sd = sqrt(sm.x * sm.x + sm.y * sm.y + (sm.z + 1.0) * (sm.z + 1.0));\n"
                   "  float si = sd > 1e-6 ? 0.5 / sd : 0.0;\n");
            put(b, "  vec4 tc%d = vec4(sm.x * si + 0.5, sm.y * si + 0.5, 0.0, 1.0);\n", s);
            n = 2;
            break;
        default:
            n = k->tcsize[sk->tci];
            if (!(k->inputs & (1u << (7 + sk->tci))))
                put(b, "  vec4 tc%d = vec4(0.0, 0.0, 0.0, 1.0);\n", s), n = 4;
            else if (n == 1)
                put(b, "  vec4 tc%d = vec4(a_tc%d.x, 1.0, 0.0, 0.0);\n", s, sk->tci);
            else if (n == 2)
                put(b, "  vec4 tc%d = vec4(a_tc%d.xy, 1.0, 0.0);\n", s, sk->tci);
            else if (n == 3)
                put(b, "  vec4 tc%d = vec4(a_tc%d.xyz, 1.0);\n", s, sk->tci);
            else
                put(b, "  vec4 tc%d = a_tc%d;\n", s, sk->tci);
        }
        (void)n;
        if (sk->ttf_count)
            put(b, "  tc%d = u_texm[%d] * tc%d;\n", s, s, s);
        put(b, "  v_tc%d = tc%d;\n", s, s);
    }
    put(b, "}\n");
}

static const char *stage_arg(const ffkey *k, int s, u32 a, char *buf, size_t cap, int alpha)
{
    (void)k;
    const char *base;
    switch (a & 7) {
    case 0: base = "v_col0"; break;
    case 1: base = s == 0 ? "v_col0" : "cur"; break;
    case 2: base = "tex"; break;
    case 3: base = "u_tfactor"; break;
    case 4: base = "v_col1"; break;
    case 5: base = "tmp"; break;
    default: base = "cur";
    }
    const char *comp = alpha || (a & 0x20) ? ".aaaa" : "";
    if (a & 0x10)
        snprintf(buf, cap, "(vec4(1.0) - %s%s)", base, comp);
    else
        snprintf(buf, cap, "%s%s", base, comp);
    return buf;
}

// Операция стадии над 4-компонентными аргументами (результат обрезается)
static void stage_op(sbuf *b, const char *dst, u32 op, const char *a1, const char *a2, const char *a0, int s)
{
    switch (op) {
    case 2: put(b, "%s = %s;\n", dst, a1); break;
    case 3: put(b, "%s = %s;\n", dst, a2); break;
    case 4: put(b, "%s = %s * %s;\n", dst, a1, a2); break;
    case 5: put(b, "%s = 2.0 * %s * %s;\n", dst, a1, a2); break;
    case 6: put(b, "%s = 4.0 * %s * %s;\n", dst, a1, a2); break;
    case 7: put(b, "%s = %s + %s;\n", dst, a1, a2); break;
    case 8: put(b, "%s = %s + %s - 0.5;\n", dst, a1, a2); break;
    case 9: put(b, "%s = 2.0 * (%s + %s - 0.5);\n", dst, a1, a2); break;
    case 10: put(b, "%s = %s - %s;\n", dst, a1, a2); break;
    case 11: put(b, "%s = %s + %s - %s * %s;\n", dst, a1, a2, a1, a2); break;
    case 12: put(b, "%s = mix(%s, %s, v_col0.a);\n", dst, a2, a1); break;
    case 13: put(b, "%s = mix(%s, %s, tex.a);\n", dst, a2, a1); break;
    case 14: put(b, "%s = mix(%s, %s, u_tfactor.a);\n", dst, a2, a1); break;
    case 15: put(b, "%s = %s + %s * (1.0 - tex.a);\n", dst, a1, a2); break;
    case 16: put(b, "%s = mix(%s, %s, %s.a);\n", dst, a2, a1, s == 0 ? "v_col0" : "cur"); break;
    case 17: put(b, "%s = %s;\n", dst, a1); break;  // PREMODULATE: модуляция следующей стадией не делается
    case 18: put(b, "%s = %s + %s.a * %s;\n", dst, a1, a1, a2); break;
    case 19: put(b, "%s = %s * %s + %s.a;\n", dst, a1, a2, a1); break;
    case 20: put(b, "%s = (1.0 - %s.a) * %s + %s;\n", dst, a1, a2, a1); break;
    case 21: put(b, "%s = (vec4(1.0) - %s) * %s + %s.a;\n", dst, a1, a2, a1); break;
    case 22: case 23: put(b, "%s = %s;\n", dst, a2); break;  // BUMPENVMAP*: смещение считает следующая стадия
    case 24: put(b, "%s = vec4(clamp(4.0 * dot(%s.rgb - 0.5, %s.rgb - 0.5), 0.0, 1.0));\n", dst, a1, a2); break;
    case 25: put(b, "%s = %s + %s * %s;\n", dst, a0, a1, a2); break;
    case 26: put(b, "%s = mix(%s, %s, %s);\n", dst, a2, a1, a0); break;
    default: put(b, "%s = %s;\n", dst, a1);
    }
}

static void gen_fs(const ffkey *k, const char *ps_body, sbuf *b)
{
    put(b, SNX_GLSL_HEADER);
    put(b, SNX_GLSL_PRECISION);
    put(b, "in vec4 v_col0, v_col1; in float v_fog;\n");
    if (clip_vary(k))
        put(b, "in float v_clip[6];\n");
    for (int s = 0; s < k->nstages; s++)
        put(b, "in vec4 v_tc%d;\n", s);
    for (int s = 0; s < k->nstages; s++)
        if (k->st[s].tex || k->ps_hash)
            put(b, "uniform %s u_tex%d;\n", (k->st[s].tex == 2 && !k->ps_hash) ? "samplerCube" : "sampler2D", s);
    put(b, "uniform vec4 u_tfactor, u_fogcolor, u_fog, u_psc[8], u_bump[8]; uniform float u_aref;\n"
           "out vec4 o_color;\n"
           "void main() {\n"
           "  vec4 cur = v_col0, tmp = vec4(0.0), tex = vec4(1.0), bump = vec4(0.0);\n");
    if (clip_vary(k))  // плоскости отсечения D3D (ES без GL_EXT_clip_cull_distance)
        for (int i = 0; i < 6; i++)
            if (k->clipmask & (1u << i))
                put(b, "  if (v_clip[%d] < 0.0) discard;\n", i);
    if (k->ps_hash) {
        // интерфейс тела ps.1.x (d3d8_ps.c): v0, v1, tc0..tc5, u_tex0..5, u_psc[]; результат — в cur
        put(b, "  vec4 v0 = v_col0, v1 = v_col1;\n");
        for (int s = 0; s < 6; s++) {
            if (s < k->nstages)
                put(b, "  vec4 tc%d = v_tc%d;\n", s, s);
            else
                put(b, "  vec4 tc%d = vec4(0.0);\n", s);
        }
        put(b, "%s\n", ps_body ? ps_body : "  cur = v_col0;\n");
    } else {
        for (int s = 0; s < k->nstages; s++) {
            const stagekey *sk = &k->st[s];
            put(b, "  // stage %d\n", s);
            int uses_tex = (sk->ca1 & 7) == 2 || (sk->ca2 & 7) == 2 || (sk->ca0 & 7) == 2 || (sk->aa1 & 7) == 2 ||
                           (sk->aa2 & 7) == 2 || (sk->aa0 & 7) == 2 || sk->cop == 13 || sk->cop == 15 ||
                           sk->aop == 13 || sk->aop == 15 || sk->cop == 22 || sk->cop == 23;
            if (uses_tex && sk->tex) {
                char uv[96];
                int n = sk->ttf_count ? sk->ttf_count : 2;
                if (sk->tex == 2)
                    snprintf(uv, sizeof uv, "v_tc%d.xyz", s);
                else if (sk->ttf_proj && n >= 2)
                    snprintf(uv, sizeof uv, "v_tc%d.xy / v_tc%d.%c", s, s, "xyzw"[n - 1]);
                else
                    snprintf(uv, sizeof uv, "v_tc%d.xy", s);
                // BUMPENVMAP на предыдущей стадии: смещение (du, dv) её текстуры через матрицу BUMPENVMAT
                if (s > 0 && (k->st[s - 1].cop == 22 || k->st[s - 1].cop == 23) && sk->tex == 1)
                    put(b, "  tex = texture(u_tex%d, %s + vec2(dot(bump.xy, u_bump[%d].xz), dot(bump.xy, u_bump[%d].yw)));\n",
                        s, uv, s - 1, s - 1);
                else
                    put(b, "  tex = texture(u_tex%d, %s);\n", s, uv);
            } else if (uses_tex)
                put(b, "  tex = vec4(1.0);\n");
            if (sk->cop == 22 || sk->cop == 23)
                put(b, "  bump = tex;\n");
            char a1[64], a2[64], a0[64];
            put(b, "  { vec4 rc, ra;\n    ");
            stage_op(b, "rc", sk->cop, stage_arg(k, s, sk->ca1, a1, sizeof a1, 0),
                     stage_arg(k, s, sk->ca2, a2, sizeof a2, 0), stage_arg(k, s, sk->ca0, a0, sizeof a0, 0), s);
            if (sk->cop == 24)  // DOTPRODUCT3 — во все каналы, и в альфу
                put(b, "    ra = rc;\n");
            else {
                put(b, "    ");
                stage_op(b, "ra", sk->aop, stage_arg(k, s, sk->aa1, a1, sizeof a1, 1),
                         stage_arg(k, s, sk->aa2, a2, sizeof a2, 1), stage_arg(k, s, sk->aa0, a0, sizeof a0, 1), s);
            }
            put(b, "    %s = clamp(vec4(rc.rgb, ra.a), 0.0, 1.0); }\n", sk->res ? "tmp" : "cur");
        }
    }
    if (k->specular)
        put(b, "  cur.rgb = clamp(cur.rgb + v_col1.rgb, 0.0, 1.0);\n");
    if (k->fog) {
        if (k->fog == FOG_PIXEL) {
            if (k->fogmode == 3)
                put(b, "  float f = (u_fog.y - v_fog) * u_fog.w;\n");
            else if (k->fogmode == 1)
                put(b, "  float f = exp(-u_fog.z * v_fog);\n");
            else
                put(b, "  float f = exp(-(u_fog.z * v_fog) * (u_fog.z * v_fog));\n");
        } else
            put(b, "  float f = v_fog;\n");
        put(b, "  cur.rgb = mix(u_fogcolor.rgb, cur.rgb, clamp(f, 0.0, 1.0));\n");
    }
    if (k->alphafunc) {
        static const char *cmp[9] = {"", "false", "<", "==", "<=", ">", "!=", ">=", "true"};
        if (k->alphafunc == 1)
            put(b, "  discard;\n");
        else if (k->alphafunc <= 7)
            put(b, "  if (!(floor(cur.a * 255.0 + 0.5) %s u_aref)) discard;\n", cmp[k->alphafunc]);
    }
    put(b, "  o_color = cur;\n}\n");
}

// ================================================================ кеш программ и бинарников GL
#define PROG_FILE "System/SnxPrograms.bin"
#define PROG_MAGIC 0x50584E53u  // "SNXP"
#define PROG_VERSION 2u
#define PROG_GEN_VERSION 2u     // версия генератора GLSL (меняется при правке шейдеров)

#define BIN_FILE "System/SnxProgramBins.bin"
#define BIN_MAGIC 0x42584E53u   // "SNXB"
#define BIN_VERSION 1u
#define MAX_BIN_BYTES (16u << 20)
#define MAX_PS_SAVED 4096u

typedef struct {
    u32 magic;
    u32 version;
    u32 gen_version;
    u32 key_size;
    char gl_version[128];
    char gl_renderer[128];
} bin_hdr;

typedef struct {
    ffkey key;
    u32 format;
    u32 size;
} bin_rec_hdr;

typedef struct {
    ffkey key;
    u32 hash, format, size;
    u8 *data;
    u8 state;  // 0 — пусто, 1 — занято, 2 — забрано (tombstone)
} bin_entry;

static int g_bin_ok;            // драйвер поддерживает glGetProgramBinary / glProgramBinary
static bin_hdr g_bin_hdr;       // эталонный заголовок текущего драйвера и генератора
static char g_prog_path[1100];
static char g_bin_path[1100];
static bin_entry *g_bins;
static u32 g_bin_cap, g_nbins;

static void bin_map_insert(const ffkey *k, u32 fmt, u8 *data, u32 size)
{
    if ((g_nbins + 1u) * 4u >= g_bin_cap * 3u) {
        u32 ncap = g_bin_cap ? g_bin_cap * 2u : 512u;
        bin_entry *nb = calloc(ncap, sizeof *nb);
        if (!nb) {
            free(data);
            return;
        }
        for (u32 i = 0; i < g_bin_cap; i++) {
            if (g_bins[i].state != 1)
                continue;
            u32 j = g_bins[i].hash & (ncap - 1u);
            while (nb[j].state == 1)
                j = (j + 1u) & (ncap - 1u);
            nb[j] = g_bins[i];
        }
        free(g_bins);
        g_bins = nb;
        g_bin_cap = ncap;
    }
    u32 h = fnv(k, sizeof *k);
    u32 i = h & (g_bin_cap - 1u);
    while (g_bins[i].state == 1) {
        if (g_bins[i].hash == h && !memcmp(&g_bins[i].key, k, sizeof *k)) {
            free(g_bins[i].data);
            g_bins[i].format = fmt;
            g_bins[i].size = size;
            g_bins[i].data = data;
            return;
        }
        i = (i + 1u) & (g_bin_cap - 1u);
    }
    g_bins[i].key = *k;
    g_bins[i].hash = h;
    g_bins[i].format = fmt;
    g_bins[i].size = size;
    g_bins[i].data = data;
    g_bins[i].state = 1;
    g_nbins++;
}

// Забрать владение загруженным бинарником программы (освобождает поток рендера после загрузки)
static u8 *bin_map_take(const ffkey *k, u32 h, u32 *out_fmt, u32 *out_size)
{
    if (!g_bin_cap)
        return NULL;
    u32 i = h & (g_bin_cap - 1u);
    while (g_bins[i].state) {
        if (g_bins[i].state == 1 && g_bins[i].hash == h && !memcmp(&g_bins[i].key, k, sizeof *k)) {
            u8 *data = g_bins[i].data;
            *out_fmt = g_bins[i].format;
            *out_size = g_bins[i].size;
            g_bins[i].data = NULL;
            g_bins[i].state = 2;
            return data;
        }
        i = (i + 1u) & (g_bin_cap - 1u);
    }
    return NULL;
}

// Вызывается на Core 2: пути уже разрешены на Core 1 через VFS (без обращений к vfs_* из потока рендера)
static void bin_cache_init_r(const char *bin_in, const char *bin_out)
{
    if (!g_bin_ok)
        return;
    int valid = 0;
    u32 loaded = 0;
    if (bin_in[0]) {
        FILE *f = fopen(bin_in, "rb");
        if (f) {
            bin_hdr h;
            if (fread(&h, sizeof h, 1, f) == 1 && !memcmp(&h, &g_bin_hdr, sizeof h)) {
                valid = 1;
                bin_rec_hdr rh;
                while (fread(&rh, sizeof rh, 1, f) == 1) {
                    if (!rh.size || rh.size > MAX_BIN_BYTES)
                        break;
                    u8 *buf = malloc(rh.size);
                    if (!buf)
                        break;
                    if (fread(buf, 1, rh.size, f) != rh.size) {
                        free(buf);
                        break;
                    }
                    bin_map_insert(&rh.key, rh.format, buf, rh.size);
                    loaded++;
                }
            }
            fclose(f);
        }
    }
    snprintf(g_bin_path, sizeof g_bin_path, "%s", bin_out);
    if (!g_bin_path[0])
        return;
    if (!valid) {
        FILE *f = fopen(g_bin_path, "wb");
        if (!f || fwrite(&g_bin_hdr, sizeof g_bin_hdr, 1, f) != 1)
            g_bin_path[0] = 0;
        if (f)
            fclose(f);
    }
    if (loaded)
        fprintf(stderr, "[d3d8] loaded %u cached GL program binaries\n", loaded);
}

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "[d3d8] shader compile failed:\n%s\n--- source ---\n%s\n", log, src);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

typedef struct {
    ffprog *p;
    const char *vs, *fs;
    u8 *bin_data;
    u32 bin_format, bin_size;
} link_args;

// Программа и расположение униформ (исполняется в потоке рендера); prog = 0 — ошибка
static void link_body(link_args *a)
{
    ffprog *p = a->p;
    int from_bin = 0;
#ifdef GL_PROGRAM_BINARY_LENGTH
    if (g_bin_ok && a->bin_data && a->bin_size) {
        GLuint prog = glCreateProgram();
        glProgramBinary(prog, (GLenum)a->bin_format, a->bin_data, (GLsizei)a->bin_size);
        GLint ok = 0;
        glGetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (ok) {
            p->prog = prog;
            from_bin = 1;
        } else {
            glDeleteProgram(prog);
            glGetError();  // сбросить возможную ошибку несовместимого бинарника перед фоллбэком на GLSL
        }
    }
#endif
    if (!from_bin) {
        GLuint v = compile(GL_VERTEX_SHADER, a->vs), f = compile(GL_FRAGMENT_SHADER, a->fs);
        if (!v || !f) {
            if (v)
                glDeleteShader(v);
            if (f)
                glDeleteShader(f);
            return;
        }
        p->prog = glCreateProgram();
        glAttachShader(p->prog, v);
        glAttachShader(p->prog, f);
#ifdef GL_PROGRAM_BINARY_RETRIEVABLE_HINT
        if (g_bin_ok)
            glProgramParameteri(p->prog, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
#endif
        glLinkProgram(p->prog);
        glDeleteShader(v);
        glDeleteShader(f);
        GLint ok = 0;
        glGetProgramiv(p->prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[2048];
            glGetProgramInfoLog(p->prog, sizeof log, NULL, log);
            fprintf(stderr, "[d3d8] program link failed: %s\n", log);
            glDeleteProgram(p->prog);
            p->prog = 0;
            return;
        }
#ifdef GL_PROGRAM_BINARY_LENGTH
        if (g_bin_ok && g_bin_path[0]) {
            GLint blen = 0;
            glGetProgramiv(p->prog, GL_PROGRAM_BINARY_LENGTH, &blen);
            if (blen > 0 && (u32)blen <= MAX_BIN_BYTES) {
                u8 *buf = malloc((size_t)blen);
                if (buf) {
                    GLsizei out_len = 0;
                    GLenum out_fmt = 0;
                    glGetProgramBinary(p->prog, blen, &out_len, &out_fmt, buf);
                    if (out_len > 0 && out_len <= blen) {
                        FILE *bf = fopen(g_bin_path, "ab");
                        if (bf) {
                            bin_rec_hdr rh = {p->key, (u32)out_fmt, (u32)out_len};
                            fwrite(&rh, sizeof rh, 1, bf);
                            fwrite(buf, 1, (size_t)out_len, bf);
                            fclose(bf);
                        }
                    }
                    free(buf);
                }
            }
        }
#endif
    }
    glUseProgram(p->prog);
#define LOC(n) p->n = glGetUniformLocation(p->prog, #n)
    LOC(u_wv); LOC(u_proj); LOC(u_world); LOC(u_nrm_mat); LOC(u_vp); LOC(u_depth); LOC(u_mat); LOC(u_power);
    LOC(u_ambient); LOC(u_ld); LOC(u_ls); LOC(u_la); LOC(u_lpos); LOC(u_ldir); LOC(u_latt); LOC(u_fog);
    LOC(u_fogcolor); LOC(u_clip); LOC(u_tfactor); LOC(u_aref); LOC(u_psc); LOC(u_bump);
#undef LOC
    p->u_texm = glGetUniformLocation(p->prog, "u_texm");
    for (int s = 0; s < MAX_STAGES; s++) {
        char name[16];
        snprintf(name, sizeof name, "u_tex%d", s);
        GLint l = glGetUniformLocation(p->prog, name);
        if (l >= 0)
            glUniform1i(l, s);
    }
}

static void link_program(link_args *a)
{
    u64 t0 = rt_ticks_ns();
    link_body(a);
    g_hitch.links++;  // строка [hitch] (d3d8_present)
    g_hitch.link_ns += rt_ticks_ns() - t0;
    atomic_store_explicit(&a->p->ready, 1, memory_order_release);
}

static int prog_rehash(u32 new_cap)
{
    ffprog **nt = calloc(new_cap, sizeof *nt);
    if (!nt)
        return 0;
    for (u32 i = 0; i < g_prog_cap; i++) {
        ffprog *p = g_progs[i];
        if (!p)
            continue;
        u32 j = p->hash & (new_cap - 1u);
        while (nt[j])
            j = (j + 1u) & (new_cap - 1u);
        nt[j] = p;
    }
    free(g_progs);
    g_progs = nt;
    g_prog_cap = new_cap;
    return 1;
}

// Удаление слота с сохранением цепочек линейного пробирования (backward-shift, без аллокаций и OOM)
static void prog_erase_at(u32 slot)
{
    u32 mask = g_prog_cap - 1u;
    u32 hole = slot, cur = (slot + 1u) & mask;
    g_progs[hole] = NULL;
    while (g_progs[cur]) {
        u32 home = g_progs[cur]->hash & mask;
        if (((hole - home) & mask) < ((cur - home) & mask)) {
            g_progs[hole] = g_progs[cur];
            g_progs[cur] = NULL;
            hole = cur;
        }
        cur = (cur + 1u) & mask;
    }
}

static void prog_evict_old(int cur_frame)
{
    // Вызывается исключительно на Core 2: никаких cs_finish() не требуется
    g_cur_prog = 0;
    g_cached_prog = NULL;
    memset(g_mru, 0, sizeof g_mru);
    u32 removed = 0;
    static const int ages[] = {120, 60, 30, 15, 7, 3, 1, 0, -1};
    for (size_t k = 0; k < sizeof ages / sizeof ages[0] && g_nprogs >= g_prog_cap / 2u; k++) {
        int age = ages[k];
        for (u32 i = 0; i < g_prog_cap;) {
            ffprog *p = g_progs[i];
            if (p && (age < 0 || cur_frame - p->last_frame > age)) {
                if (p->prog)
                    glDeleteProgram(p->prog);
                free(p);
                prog_erase_at(i);
                atomic_fetch_sub_explicit(&g_nprogs, 1, memory_order_relaxed);
                removed++;
                if (age < 0 && g_nprogs < g_prog_cap / 2u)
                    break;
                continue;  // проверить элемент, сдвинувшийся в слот i
            }
            i++;
        }
    }
    fprintf(stderr, "[d3d8] program cache evicted %u unused programs (%u remain)\n", removed,
            atomic_load_explicit(&g_nprogs, memory_order_relaxed));
}

static ffprog *prog_find(const ffkey *k, u32 *h)
{
    *h = fnv(k, sizeof *k);
    if (!g_prog_cap)
        return NULL;
    u32 i = *h & (g_prog_cap - 1u);
    while (g_progs[i]) {
        if (g_progs[i]->hash == *h && !memcmp(&g_progs[i]->key, k, sizeof *k))
            return g_progs[i];
        i = (i + 1u) & (g_prog_cap - 1u);
    }
    return NULL;
}

static ffprog *prog_insert(const ffkey *k, u32 h, int cur_frame)
{
    if (!g_prog_cap) {
        if (!prog_rehash(PROG_INIT_CAP))
            return NULL;
    } else if ((g_nprogs + 1u) * 4u >= g_prog_cap * 3u) {
        if (g_prog_cap < PROG_MAX_CAP) {
            if (!prog_rehash(g_prog_cap * 2u))
                prog_evict_old(cur_frame);
        } else {
            prog_evict_old(cur_frame);
        }
    }
    if (g_nprogs >= g_prog_cap)
        return NULL;
    ffprog *p = calloc(1, sizeof *p);
    if (!p)
        return NULL;
    p->key = *k;
    p->hash = h;
    p->gen_xf = p->gen_view = p->gen_texm = p->gen_vp = p->gen_clip = 0xFFFFFFFFu;
    p->gen_light = p->gen_mat = p->gen_const = p->gen_psc = p->gen_bump = 0xFFFFFFFFu;
    p->last_frame = cur_frame;
    u32 i = h & (g_prog_cap - 1u);
    while (g_progs[i])
        i = (i + 1u) & (g_prog_cap - 1u);
    g_progs[i] = p;
    atomic_fetch_add_explicit(&g_nprogs, 1, memory_order_relaxed);
    return p;
}

typedef struct {
    char bin_in[1100], bin_out[1100];
    char prog_in[1100], prog_out[1100];
} preload_args;

static void x_prog_preload(void *arg)
{
    const preload_args *pa = arg;
    bin_cache_init_r(pa->bin_in, pa->bin_out);
    u32 hdr[3] = {0}, n = 0, n_ps = 0, n_bin = 0;
    if (pa->prog_in[0]) {
        FILE *f = fopen(pa->prog_in, "rb");
        if (f && fread(hdr, 4, 3, f) == 3 && hdr[0] == PROG_MAGIC && hdr[1] == PROG_VERSION &&
            hdr[2] == sizeof(ffkey)) {
            ffkey k;
            u32 ps_ntok;
            while (fread(&k, sizeof k, 1, f) == 1 && fread(&ps_ntok, 4, 1, f) == 1) {
                char *ps_body = NULL;
                if (ps_ntok > MAX_PS_SAVED)
                    break;
                if (ps_ntok > 0) {
                    u32 *toks = malloc((size_t)ps_ntok * sizeof(u32));
                    if (!toks || fread(toks, 4, ps_ntok, f) != ps_ntok) {
                        free(toks);
                        break;
                    }
                    u64 h64 = 0;
                    ps_body = d3d8_ps_translate(toks, ps_ntok, NULL, &h64);
                    free(toks);
                    if (!ps_body || h64 != k.ps_hash) {
                        free(ps_body);
                        continue;
                    }
                } else if (k.ps_hash)
                    continue;
                u32 h;
                if (prog_find(&k, &h)) {
                    free(ps_body);
                    continue;
                }
                ffprog *p = prog_insert(&k, h, 0);
                if (!p) {
                    free(ps_body);
                    break;
                }
                u32 bfmt = 0, bsize = 0;
                u8 *bdata = bin_map_take(&k, h, &bfmt, &bsize);
                int had_bin = bdata != NULL;
                sbuf vs = {0}, fs = {0};
                gen_vs(&k, &vs);
                gen_fs(&k, ps_body, &fs);
                free(ps_body);
                link_args a = {p, vs.p, fs.p, bdata, bfmt, bsize};
                link_program(&a);
                free(vs.p);
                free(fs.p);
                free(bdata);
                n++;
                if (k.ps_hash)
                    n_ps++;
                if (had_bin)
                    n_bin++;
            }
        } else if (f) {
            hdr[0] = 0;  // старый формат ключа — пересоздать файл
        }
        if (f)
            fclose(f);
    }
    snprintf(g_prog_path, sizeof g_prog_path, "%s", pa->prog_out);
    if (g_prog_path[0] && hdr[0] != PROG_MAGIC) {
        FILE *f = fopen(g_prog_path, "wb");
        u32 h[3] = {PROG_MAGIC, PROG_VERSION, (u32)sizeof(ffkey)};
        if (!f || fwrite(h, 4, 3, f) != 3)
            g_prog_path[0] = 0;
        if (f)
            fclose(f);
    }
    g_cur_prog = 0;
    fprintf(stderr, "[d3d8] %u programs (%u ps, %u binaries) preloaded on render thread\n", n, n_ps, n_bin);
}

// Ключи программ между запусками: <user>/System/SnxPrograms.bin (v2) и кеш бинарников SnxProgramBins.bin.
// Пути VFS разрешаются на Core 1, а разбор и компиляция шейдеров выполняются асинхронно на Core 2.
void d3d8_prog_preload(void)
{
    preload_args pa;
    memset(&pa, 0, sizeof pa);
    if (g_bin_ok) {
        if (!vfs_find(BIN_FILE, pa.bin_in, sizeof pa.bin_in))
            pa.bin_in[0] = 0;
        if (!vfs_for_write(BIN_FILE, pa.bin_out, sizeof pa.bin_out, 0))
            pa.bin_out[0] = 0;
        else if (!pa.bin_in[0])
            vfs_changed();
    }
    if (!vfs_find(PROG_FILE, pa.prog_in, sizeof pa.prog_in))
        pa.prog_in[0] = 0;
    if (!vfs_for_write(PROG_FILE, pa.prog_out, sizeof pa.prog_out, 0))
        pa.prog_out[0] = 0;
    else if (!pa.prog_in[0])
        vfs_changed();
    cs_async(x_prog_preload, &pa, sizeof pa);
}

static void prog_mru_put(ffprog *p)
{
    for (int i = 0; i < 4; i++) {
        if (g_mru[i] == p) {
            for (int j = i; j > 0; j--)
                g_mru[j] = g_mru[j - 1];
            g_mru[0] = p;
            return;
        }
    }
    g_mru[3] = g_mru[2];
    g_mru[2] = g_mru[1];
    g_mru[1] = g_mru[0];
    g_mru[0] = p;
}

// Исполняется исключительно на Core 2: при промахе кеша шейдер компилируется и сохраняется прямо здесь,
// без cs_sync и остановки потока гостя (Core 1)!
static ffprog *program_for(d3d_rstate *r, const ffkey *k)
{
    for (int i = 0; i < 4; i++) {
        ffprog *p = g_mru[i];
        if (p && !memcmp(&p->key, k, sizeof *k)) {
            if (i > 0) {
                for (int j = i; j > 0; j--)
                    g_mru[j] = g_mru[j - 1];
                g_mru[0] = p;
            }
            p->last_frame = r->frame;
            return p;
        }
    }
    u32 h;
    ffprog *p = prog_find(k, &h);
    if (p) {
        p->last_frame = r->frame;
        prog_mru_put(p);
        return p;
    }
    p = prog_insert(k, h, r->frame);
    if (!p)
        return NULL;
    const char *ps_body = k->ps_hash ? d3d8_r_ps_glsl(r->ps) : NULL;
    u32 bfmt = 0, bsize = 0;
    u8 *bdata = bin_map_take(k, h, &bfmt, &bsize);
    sbuf vs = {0}, fs = {0};
    gen_vs(k, &vs);
    gen_fs(k, ps_body, &fs);
    link_args a = {p, vs.p, fs.p, bdata, bfmt, bsize};
    link_program(&a);
    g_cur_prog = 0;
    free(vs.p);
    free(fs.p);
    free(bdata);
    if (g_prog_path[0]) {
        u32 ps_ntok = 0;
        const u32 *ps_toks = k->ps_hash ? d3d8_r_ps_tokens(r->ps, &ps_ntok) : NULL;
        if (!k->ps_hash || (ps_toks && ps_ntok > 0 && ps_ntok <= MAX_PS_SAVED)) {
            FILE *f = fopen(g_prog_path, "ab");
            if (f) {
                fwrite(k, sizeof *k, 1, f);
                fwrite(&ps_ntok, 4, 1, f);
                if (ps_ntok)
                    fwrite(ps_toks, 4, ps_ntok, f);
                fclose(f);
            }
        }
    }
    prog_mru_put(p);
    return p;
}

static void color4(float *o, u32 c)
{
    o[0] = (float)((c >> 16) & 0xFF) / 255.0f;
    o[1] = (float)((c >> 8) & 0xFF) / 255.0f;
    o[2] = (float)(c & 0xFF) / 255.0f;
    o[3] = (float)(c >> 24) / 255.0f;
}

static void upload_uniforms(d3d_rstate *d, ffprog *p)
{
    if (p->gen_xf != d->gen_xf) {
        float wv[16];
        if (p->u_wv >= 0 || p->u_nrm_mat >= 0)
            mat_mul(wv, d->world, d->view);
        if (p->u_wv >= 0)
            glUniformMatrix4fv(p->u_wv, 1, GL_FALSE, wv);
        if (p->u_proj >= 0)
            glUniformMatrix4fv(p->u_proj, 1, GL_FALSE, d->proj);
        if (p->u_world >= 0)
            glUniformMatrix4fv(p->u_world, 1, GL_FALSE, d->world);
        if (p->u_nrm_mat >= 0) {
            float nm[9];
            compute_normal_mat(nm, wv);
            glUniformMatrix3fv(p->u_nrm_mat, 1, GL_FALSE, nm);
        }
        p->gen_xf = d->gen_xf;
    }
    if (p->gen_texm != d->gen_texm) {
        if (p->u_texm >= 0)
            glUniformMatrix4fv(p->u_texm, MAX_STAGES, GL_FALSE, &d->texm[0][0]);
        p->gen_texm = d->gen_texm;
    }
    if (p->gen_vp != d->gen_vp) {
        if (p->u_vp >= 0)
            glUniform4f(p->u_vp, (float)d->vp[0], (float)d->vp[1], (float)d->vp[2], (float)d->vp[3]);
        if (p->u_depth >= 0)
            glUniform2f(p->u_depth, d->vpz[0], d->vpz[1]);
        p->gen_vp = d->gen_vp;
    }
    if (p->gen_clip != d->gen_clip) {
        if (p->u_clip >= 0)
            glUniform4fv(p->u_clip, 6, &d->clip[0][0]);
        p->gen_clip = d->gen_clip;
    }
    if (p->key.lighting && (p->gen_light != d->gen_light || p->gen_view != d->gen_view)) {
        float ld[MAX_LIGHTS][4], ls[MAX_LIGHTS][4], la[MAX_LIGHTS][4], lpos[MAX_LIGHTS][4], ldir[MAX_LIGHTS][4],
            latt[MAX_LIGHTS][4];
        const float *v = d->view;
        for (int i = 0; i < MAX_LIGHTS; i++) {
            const d3dlight *l = &d->lights[i];
            memcpy(ld[i], l->diffuse, 12);
            memcpy(ls[i], l->specular, 12);
            memcpy(la[i], l->ambient, 12);
            ld[i][3] = cosf(l->theta * 0.5f);
            ls[i][3] = cosf(l->phi * 0.5f);
            la[i][3] = 0.0f;
            for (int j = 0; j < 3; j++) {
                lpos[i][j] = l->pos[0] * v[j] + l->pos[1] * v[4 + j] + l->pos[2] * v[8 + j] + v[12 + j];
                ldir[i][j] = l->dir[0] * v[j] + l->dir[1] * v[4 + j] + l->dir[2] * v[8 + j];
            }
            float n = sqrtf(ldir[i][0] * ldir[i][0] + ldir[i][1] * ldir[i][1] + ldir[i][2] * ldir[i][2]);
            if (n > 0)
                for (int j = 0; j < 3; j++)
                    ldir[i][j] /= n;
            lpos[i][3] = l->range > 0 ? l->range : 1e30f;
            ldir[i][3] = l->falloff;
            latt[i][0] = l->att0;
            latt[i][1] = l->att1;
            latt[i][2] = l->att2;
            latt[i][3] = 0.0f;
        }
        if (p->u_ld >= 0)
            glUniform4fv(p->u_ld, MAX_LIGHTS, &ld[0][0]);
        if (p->u_ls >= 0)
            glUniform4fv(p->u_ls, MAX_LIGHTS, &ls[0][0]);
        if (p->u_la >= 0)
            glUniform4fv(p->u_la, MAX_LIGHTS, &la[0][0]);
        if (p->u_lpos >= 0)
            glUniform4fv(p->u_lpos, MAX_LIGHTS, &lpos[0][0]);
        if (p->u_ldir >= 0)
            glUniform4fv(p->u_ldir, MAX_LIGHTS, &ldir[0][0]);
        if (p->u_latt >= 0)
            glUniform4fv(p->u_latt, MAX_LIGHTS, &latt[0][0]);
        p->gen_light = d->gen_light;
        p->gen_view = d->gen_view;
    }
    if (p->key.lighting && p->gen_mat != d->gen_mat) {
        if (p->u_mat >= 0)
            glUniform4fv(p->u_mat, 4, d->material);
        if (p->u_power >= 0)
            glUniform1f(p->u_power, d->material[16]);
        p->gen_mat = d->gen_mat;
    }
    if (p->gen_const != d->gen_const) {
        float c[4];
        if (p->u_tfactor >= 0) {
            color4(c, d->rs[60]);
            if (g_diag_variant == 3)  // вариант самодиагностики: TextureFactor белый
                c[0] = c[1] = c[2] = c[3] = 1.0f;
            glUniform4fv(p->u_tfactor, 1, c);
        }
        if (p->u_fogcolor >= 0) {
            color4(c, d->rs[34]);
            if (g_diag_variant == 1) {  // вариант самодиагностики: туман розовый — видно, что закрашено туманом
                c[0] = 1.0f;
                c[1] = 0.0f;
                c[2] = 1.0f;
            }
            glUniform4fv(p->u_fogcolor, 1, c);
        }
        if (p->u_ambient >= 0) {
            color4(c, d->rs[139]);
            glUniform4fv(p->u_ambient, 1, c);
        }
        if (p->u_fog >= 0) {
            float fs = u2f(d->rs[36]), fe = u2f(d->rs[37]);
            glUniform4f(p->u_fog, fs, fe, u2f(d->rs[38]), fe != fs ? 1.0f / (fe - fs) : 0.0f);
        }
        if (p->u_aref >= 0)
            glUniform1f(p->u_aref, (float)(d->rs[24] & 0xFF));
        p->gen_const = d->gen_const;
    }
    if (p->gen_psc != d->gen_psc) {
        if (p->u_psc >= 0)
            glUniform4fv(p->u_psc, 8, &d->psc[0][0]);
        p->gen_psc = d->gen_psc;
    }
    if (p->gen_bump != d->gen_bump) {
        if (p->u_bump >= 0) {
            float bm[MAX_STAGES][4];
            for (int s = 0; s < MAX_STAGES; s++)
                for (int j = 0; j < 4; j++)
                    bm[s][j] = u2f(d->tss[s][7 + j]);
            glUniform4fv(p->u_bump, MAX_STAGES, &bm[0][0]);
        }
        p->gen_bump = d->gen_bump;
    }
}

// ================================================================ состояние GL (гранулярно по d->gl_dirty)
static GLenum gl_blend(u32 b)
{
    static const GLenum t[14] = {GL_ONE, GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA,
                                 GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_DST_COLOR,
                                 GL_ONE_MINUS_DST_COLOR, GL_SRC_ALPHA_SATURATE, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA};
    return b < 14 ? t[b] : GL_ONE;
}

static GLenum gl_cmp(u32 f)
{
    static const GLenum t[9] = {GL_ALWAYS, GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL,
                                GL_ALWAYS};
    return f < 9 ? t[f] : GL_ALWAYS;
}

static GLenum gl_stencil_op(u32 o)
{
    static const GLenum t[9] = {GL_KEEP, GL_KEEP, GL_ZERO, GL_REPLACE, GL_INCR, GL_DECR, GL_INVERT, GL_INCR_WRAP,
                                GL_DECR_WRAP};
    return o < 9 ? t[o] : GL_KEEP;
}

static void apply_state(d3d_rstate *d)
{
    u32 dirty = d->gl_dirty;
    if (!dirty)
        return;
    d->gl_dirty = 0;
    const u32 *rs = d->rs;
    if (dirty & GLD_VP) {
        glViewport((GLint)d->vp[0], (GLint)d->vp[1], (GLsizei)d->vp[2], (GLsizei)d->vp[3]);
        glDepthRange(d->vpz[0], d->vpz[1]);
    }
    if (dirty & GLD_SCISSOR)
        glDisable(GL_SCISSOR_TEST);
    if (dirty & GLD_DEPTH) {
        if (rs[7] && d->ds) {
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(gl_cmp(rs[23]));
        } else
            glDisable(GL_DEPTH_TEST);
        glDepthMask(rs[14] && d->ds ? GL_TRUE : GL_FALSE);
    }
    if (dirty & GLD_BLEND) {
        if (rs[27]) {
            glEnable(GL_BLEND);
            u32 src = rs[19], dst = rs[20];
            if (src == 12) {  // BOTHSRCALPHA
                src = 5;
                dst = 6;
            } else if (src == 13) {  // BOTHINVSRCALPHA
                src = 6;
                dst = 5;
            }
            glBlendFunc(gl_blend(src), gl_blend(dst));
            static const GLenum ops[6] = {GL_FUNC_ADD, GL_FUNC_ADD, GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT,
                                          GL_MIN, GL_MAX};
            glBlendEquation(rs[171] < 6 ? ops[rs[171]] : GL_FUNC_ADD);
        } else
            glDisable(GL_BLEND);
    }
    if (dirty & GLD_CULL) {
        // D3D «по часовой» = GL «против часовой» (кадр хранится сверху вниз)
        glFrontFace(GL_CCW);
        if (rs[22] == 2) {
            glEnable(GL_CULL_FACE);
            glCullFace(GL_FRONT);
        } else if (rs[22] == 3) {
            glEnable(GL_CULL_FACE);
            glCullFace(GL_BACK);
        } else
            glDisable(GL_CULL_FACE);
    }
    if (dirty & GLD_STENCIL) {
        if (rs[52] && d->ds) {
            glEnable(GL_STENCIL_TEST);
            glStencilFunc(gl_cmp(rs[56]), (GLint)rs[57], rs[58]);
            glStencilOp(gl_stencil_op(rs[53]), gl_stencil_op(rs[54]), gl_stencil_op(rs[55]));
            glStencilMask(rs[59]);
        } else
            glDisable(GL_STENCIL_TEST);
    }
    if (dirty & GLD_COLORMASK) {
        u32 cw = rs[168];
        glColorMask(cw & 1, (cw >> 1) & 1, (cw >> 2) & 1, (cw >> 3) & 1);
    }
    if (dirty & GLD_POLY) {
        glPolygonMode(GL_FRONT_AND_BACK, rs[8] == 1 ? GL_POINT : rs[8] == 2 ? GL_LINE : GL_FILL);
        if (rs[47]) {  // ZBIAS 0..16: больше — ближе
            glEnable(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(-1.0f, -(float)rs[47]);
        } else
            glDisable(GL_POLYGON_OFFSET_FILL);
    }
    if (dirty & GLD_CLIP) {
        for (int i = 0; i < 6; i++) {
            if (rs[152] & (1u << i))
                glEnable(GL_CLIP_DISTANCE0 + i);
            else
                glDisable(GL_CLIP_DISTANCE0 + i);
        }
    }
}

static GLint gl_address(u32 a)
{
    switch (a) {
    case 2: case 5: return GL_MIRRORED_REPEAT;
    case 3: return GL_CLAMP_TO_EDGE;
    case 4: return GL_CLAMP_TO_BORDER;
    default: return GL_REPEAT;
    }
}

// Возможности GL (анизотропия и кеш бинарников программ) — проверяются один раз при создании устройства
static int g_aniso;

static void x_init_gl(void *arg)
{
    d3ddev *d = arg;
    d3d_rstate *r = &d->r;
    glGenVertexArrays(1, &r->vao);
    glBindVertexArray(r->vao);
    glGenBuffers(1, &r->up_vbo);
    glGenBuffers(1, &r->up_ibo);
    glGenSamplers(MAX_STAGES, r->samplers);
    glGetError();
    glSamplerParameterf(r->samplers[0], GL_TEXTURE_MAX_ANISOTROPY_EXT, 1.0f);
    g_aniso = glGetError() == GL_NO_ERROR;
#ifdef GL_NUM_PROGRAM_BINARY_FORMATS
    GLint nfmts = 0;
    glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &nfmts);
    g_bin_ok = nfmts > 0;
#if SNX_GL_NEEDS_LOADER
    if (!glGetProgramBinary || !glProgramBinary || !glProgramParameteri)
        g_bin_ok = 0;
#endif
#ifdef SNX_GLES
    {
        const char *e = getenv("SNX_SHADER_CACHE");  // SNX_SHADER_CACHE=0 — без кэша бинарников (docs/ANDROID.md)
        if (e && !strcmp(e, "0"))
            g_bin_ok = 0;
    }
#endif
    if (g_bin_ok) {
        const char *ver = (const char *)glGetString(GL_VERSION);
        const char *ren = (const char *)glGetString(GL_RENDERER);
        memset(&g_bin_hdr, 0, sizeof g_bin_hdr);
        g_bin_hdr.magic = BIN_MAGIC;
        g_bin_hdr.version = BIN_VERSION;
        g_bin_hdr.gen_version = PROG_GEN_VERSION;
        g_bin_hdr.key_size = (u32)sizeof(ffkey);
        if (ver)
            snprintf(g_bin_hdr.gl_version, sizeof g_bin_hdr.gl_version, "%s", ver);
#ifdef SNX_GLES
        // генератор GLSL сборки ES отличается от настольного (точность, BGRA, v_clip): бинарники — только своей сборки
        size_t vl = strlen(g_bin_hdr.gl_version);
        snprintf(g_bin_hdr.gl_version + vl, sizeof g_bin_hdr.gl_version - vl, "|es " __DATE__ " " __TIME__);
#endif
        if (ren)
            snprintf(g_bin_hdr.gl_renderer, sizeof g_bin_hdr.gl_renderer, "%s", ren);
    }
    glGetError();
#endif
}

void d3d8_cmd_init_gl(d3ddev *d) { cs_sync(x_init_gl, d); }

static void update_sampler(d3d_rstate *d, int s)
{
    const u32 *t = d->tss[s];
    GLuint sm = d->samplers[s];
    glSamplerParameteri(sm, GL_TEXTURE_WRAP_S, gl_address(t[13]));
    glSamplerParameteri(sm, GL_TEXTURE_WRAP_T, gl_address(t[14]));
    glSamplerParameteri(sm, GL_TEXTURE_WRAP_R, gl_address(t[25]));
    float bc[4];
    color4(bc, t[15]);
    glSamplerParameterfv(sm, GL_TEXTURE_BORDER_COLOR, bc);
    u32 mag = t[16], min = t[17], mipf = t[18];
    glSamplerParameteri(sm, GL_TEXTURE_MAG_FILTER, mag == 1 ? GL_NEAREST : GL_LINEAR);
    GLint mf;
    int lin = min != 1;
    if (mipf == 0)
        mf = lin ? GL_LINEAR : GL_NEAREST;
    else if (mipf == 1)
        mf = lin ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST;
    else
        mf = lin ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR;
    glSamplerParameteri(sm, GL_TEXTURE_MIN_FILTER, mf);
    float aniso = (min == 3 || mag == 3) && t[21] > 1 ? (float)t[21] : 1.0f;
    if (g_aniso)
        glSamplerParameterf(sm, GL_TEXTURE_MAX_ANISOTROPY_EXT, aniso);
    glSamplerParameterf(sm, GL_TEXTURE_LOD_BIAS, u2f(t[19]));
    glSamplerParameterf(sm, GL_TEXTURE_MIN_LOD, (float)t[20]);
}

// ================================================================ вершины и отрисовка
static void vertex_count(u32 prim, u32 count, GLenum *mode, u32 *n)
{
    switch (prim) {
    case 1: *mode = GL_POINTS; *n = count; break;
    case 2: *mode = GL_LINES; *n = count * 2; break;
    case 3: *mode = GL_LINE_STRIP; *n = count + 1; break;
    case 5: *mode = GL_TRIANGLE_STRIP; *n = count + 2; break;
    case 6: *mode = GL_TRIANGLE_FAN; *n = count + 2; break;
    default: *mode = GL_TRIANGLES; *n = count * 3;
    }
}

static u32 g_attr_enabled;

// ES без glDrawElementsBaseVertex: базовая вершина — сдвигом указателей атрибутов (вершин от начала буфера)
static u32 g_attr_base;

static void setup_attribs(d3d_rstate *d, const vdecl *decl, const void *up, u32 up_stride, u32 up_bytes)
{
    u32 want = 0;
    if (up) {
        glBindBuffer(GL_ARRAY_BUFFER, d->up_vbo);
        glBufferData(GL_ARRAY_BUFFER, up_bytes, up, GL_STREAM_DRAW);
    }
    for (int i = 0; i < decl->n; i++) {
        const velem *e = &decl->e[i];
        if (e->reg >= 16)
            continue;
        u32 stride;
        if (up) {
            if (e->stream != 0)
                continue;
            stride = up_stride;
        } else {
            d3dbuf *b = d->stream[e->stream];
            if (!b)
                continue;
            glBindBuffer(GL_ARRAY_BUFFER, b->gl);
            stride = d->stride[e->stream];
        }
        const void *off = (const void *)(uintptr_t)(e->offset + (up ? 0 : g_attr_base * stride));
        switch (e->type) {
        case 0: case 1: case 2: case 3:
            glVertexAttribPointer(e->reg, e->type + 1, GL_FLOAT, GL_FALSE, (GLsizei)stride, off);
            break;
        case 4:
            glVertexAttribPointer(e->reg, GL_BGRA, GL_UNSIGNED_BYTE, GL_TRUE, (GLsizei)stride, off);
            break;
        case 5:
            glVertexAttribPointer(e->reg, 4, GL_UNSIGNED_BYTE, GL_FALSE, (GLsizei)stride, off);
            break;
        case 6:
            glVertexAttribPointer(e->reg, 2, GL_SHORT, GL_FALSE, (GLsizei)stride, off);
            break;
        default:
            glVertexAttribPointer(e->reg, 4, GL_SHORT, GL_FALSE, (GLsizei)stride, off);
        }
        want |= 1u << e->reg;
    }
    for (u32 r = 0; r < 16; r++) {
        if ((want ^ g_attr_enabled) & (1u << r)) {
            if (want & (1u << r))
                glEnableVertexAttribArray(r);
            else
                glDisableVertexAttribArray(r);
        }
    }
    g_attr_enabled = want;
}

// SNX_DUMP_DRAWS=N: состояние каждой отрисовки кадра N (стадии, смешивание, матрицы текстур) — в stderr
static int dumping_frame(int cur_frame)
{
    static int frame = -2;
    if (frame == -2) {
        const char *e = getenv("SNX_DUMP_DRAWS");
        frame = e ? atoi(e) : -1;
    }
    return cur_frame == frame;
}

int d3d8_dumping(const d3ddev *d) { return d && dumping_frame(d->frame); }

static void dump_draw(const d3d_rstate *d, const ffkey *k, u32 prim, u32 count, int indexed)
{
    const u32 *rs = d->rs;
    fprintf(stderr, "[dump] draw %u: prim %u n %u %s rhw %u light %u | blend %u %u/%u z %u/%u/%u atest %u/%u/%u "
                    "cull %u clip %02x fog %u\n",
            d->draws, prim, count, indexed ? "idx" : "arr", k->rhw, k->lighting, rs[27], rs[19], rs[20], rs[7],
            rs[14], rs[23], rs[15], rs[25], rs[24], rs[22], k->clipmask, k->fog);
    for (int s = 0; s < k->nstages; s++) {
        const stagekey *sk = &k->st[s];
        const u32 *t = d->tss[s];
        d3dtex *tx = d->tex[s];
        fprintf(stderr, "[dump]   st%d: c %u(%x,%x) a %u(%x,%x) tex %ux%u fmt %x tci %u gen %u ttf %u%s addr %u/%u "
                        "border %08x\n",
                s, sk->cop, sk->ca1, sk->ca2, sk->aop, sk->aa1, sk->aa2, tx ? tx->w : 0, tx ? tx->h : 0,
                tx ? tx->fmt : 0, sk->tci, sk->tcgen, sk->ttf_count, sk->ttf_proj ? "P" : "", t[13], t[14], t[15]);
        if (sk->ttf_count) {
            const float *m = d->texm[s];
            fprintf(stderr, "[dump]     texm");
            for (int i = 0; i < 16; i++)
                fprintf(stderr, " %.3g", m[i]);
            fprintf(stderr, "\n");
        }
    }
}

// ================================================================ команды D3D8 (Core 1 -> gl_cs -> Core 2)
static u32 f2u_r(float f)
{
    u32 v;
    memcpy(&v, &f, 4);
    return v;
}

static void identity_r(float *m)
{
    memset(m, 0, 64);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

// Чистые функции обновления d3d_rstate (выполняются на Core 2 и проверяются в d3d8_cs_test)
void d3d8_r_apply_rs(d3d_rstate *r, u32 state, u32 val)
{
    if (state >= 256 || r->rs[state] == val)
        return;
    r->rs[state] = val;
    switch (state) {
    case 7: case 14: case 23:
        r->gl_dirty |= GLD_DEPTH;
        break;
    case 8: case 47:
        r->gl_dirty |= GLD_POLY;
        break;
    case 15: case 25: case 28: case 29: case 35: case 48:
    case 137: case 140: case 141: case 142: case 143:
    case 145: case 146: case 147: case 148:
        r->key_dirty = 1;
        break;
    case 19: case 20: case 27: case 171:
        r->gl_dirty |= GLD_BLEND;
        break;
    case 22:
        r->gl_dirty |= GLD_CULL;
        break;
    case 24: case 34: case 36: case 37: case 38: case 60: case 139:
        r->gen_const++;
        break;
    case 52: case 53: case 54: case 55: case 56: case 57: case 58: case 59:
        r->gl_dirty |= GLD_STENCIL;
        break;
    case 152:
        r->gl_dirty |= GLD_CLIP;
        r->key_dirty = 1;
        break;
    case 168:
        r->gl_dirty |= GLD_COLORMASK;
        break;
    default:
        break;
    }
}

void d3d8_r_apply_tss(d3d_rstate *r, u32 s, u32 t, u32 val)
{
    if (s >= MAX_STAGES || t >= 32 || r->tss[s][t] == val)
        return;
    r->tss[s][t] = val;
    if (t >= 7 && t <= 10)
        r->gen_bump++;
    else if ((t >= 13 && t <= 21) || t == 25)
        r->sampler_dirty |= 1u << s;
    else if ((t >= 1 && t <= 6) || t == 11 || t == 24 || (t >= 26 && t <= 28))
        r->key_dirty = 1;
}

void d3d8_r_apply_xf(d3d_rstate *r, u32 state, const float mat[16])
{
    if (state == 2) {
        memcpy(r->view, mat, 64);
        r->gen_xf++;
        r->gen_view++;
    } else if (state == 3) {
        memcpy(r->proj, mat, 64);
        r->gen_xf++;
    } else if (state == 256) {
        memcpy(r->world, mat, 64);
        r->gen_xf++;
    } else if (state >= 16 && state < 16 + MAX_STAGES) {
        memcpy(r->texm[state - 16], mat, 64);
        r->gen_texm++;
    }
}

void d3d8_r_apply_tex(d3d_rstate *r, u32 s, d3dtex *t)
{
    if (s >= MAX_STAGES)
        return;
    d3dtex *old = r->tex[s];
    u8 old_k = old ? (u8)(old->cube ? 2 : 1) : 0;
    u8 new_k = t ? (u8)(t->cube ? 2 : 1) : 0;
    r->tex[s] = t;
    if (old_k != new_k)
        r->key_dirty = 1;
}

typedef struct { d3ddev *d; int has_autodepth; u32 bb_w, bb_h; } cmd_reset_t;
static void x_reset_defaults(void *p)
{
    const cmd_reset_t *q = p;
    d3d_rstate *r = &q->d->r;
    memset(r->rs, 0, sizeof r->rs);
    r->rs[7] = q->has_autodepth ? 1 : 0;
    r->rs[8] = 3;
    r->rs[9] = 2;
    r->rs[14] = 1;
    r->rs[16] = 1;
    r->rs[19] = 2;
    r->rs[20] = 1;
    r->rs[22] = 3;
    r->rs[23] = 4;
    r->rs[25] = 8;
    r->rs[37] = f2u_r(1.0f);
    r->rs[38] = f2u_r(1.0f);
    r->rs[53] = r->rs[54] = r->rs[55] = 1;
    r->rs[56] = 8;
    r->rs[58] = r->rs[59] = 0xFFFFFFFFu;
    r->rs[60] = 0xFFFFFFFFu;
    r->rs[136] = 1;
    r->rs[137] = 1;
    r->rs[141] = 1;
    r->rs[142] = 1;
    r->rs[145] = 1;
    r->rs[146] = 2;
    r->rs[154] = f2u_r(1.0f);
    r->rs[155] = f2u_r(1.0f);
    r->rs[166] = f2u_r(64.0f);
    r->rs[161] = 1;
    r->rs[162] = 0xFFFFFFFFu;
    r->rs[164] = f2u_r(1.0f);
    r->rs[168] = 0xF;
    r->rs[171] = 1;
    r->rs[172] = 3;
    r->rs[173] = 1;
    for (int s = 0; s < MAX_STAGES; s++) {
        u32 *t = r->tss[s];
        memset(t, 0, 32 * 4);
        t[1] = s == 0 ? 4 : 1;
        t[2] = 2;
        t[3] = 1;
        t[4] = s == 0 ? 2 : 1;
        t[5] = 2;
        t[6] = 1;
        t[11] = (u32)s;
        t[13] = t[14] = t[25] = 1;
        t[16] = t[17] = 1;
        t[21] = 1;
        t[26] = t[27] = t[28] = 1;
    }
    identity_r(r->view);
    identity_r(r->proj);
    identity_r(r->world);
    for (int s = 0; s < MAX_STAGES; s++)
        identity_r(r->texm[s]);
    memset(r->material, 0, sizeof r->material);
    memset(r->light_on, 0, sizeof r->light_on);
    r->vp[0] = r->vp[1] = 0;
    r->vp[2] = q->bb_w;
    r->vp[3] = q->bb_h;
    r->vpz[0] = 0.0f;
    r->vpz[1] = 1.0f;
    r->sampler_dirty = 0xFF;
    r->gen_xf++;
    r->gen_view++;
    r->gen_texm++;
    r->gen_vp++;
    r->gen_clip++;
    r->gen_light++;
    r->gen_mat++;
    r->gen_const++;
    r->gen_psc++;
    r->gen_bump++;
    r->gl_dirty = GLD_ALL;
    r->key_dirty = 1;
    r->decl_dirty = 1;
    g_cached_prog = NULL;
}

void d3d8_cmd_reset_defaults(d3ddev *d, int has_autodepth, u32 bb_w, u32 bb_h)
{
    cmd_reset_t cmd = {d, has_autodepth, bb_w, bb_h};
    cs_cmd_push(x_reset_defaults, &cmd, sizeof cmd);
}

static void x_unbind_all(void *p)
{
    d3ddev *d = *(d3ddev **)p;
    d3d_rstate *r = &d->r;
    memset(r->tex, 0, sizeof r->tex);
    memset(r->stream, 0, sizeof r->stream);
    r->indices = NULL;
    r->gl_dirty = GLD_ALL;
    r->key_dirty = 1;
    g_cached_prog = NULL;
}

void d3d8_cmd_unbind_all(d3ddev *d) { cs_cmd_push(x_unbind_all, &d, sizeof d); }

typedef struct { d3ddev *d; u32 state, val; } cmd_rs_t;
static void x_rs(void *p) { const cmd_rs_t *q = p; d3d8_r_apply_rs(&q->d->r, q->state, q->val); }
void d3d8_cmd_rs(d3ddev *d, u32 state, u32 val)
{
    cmd_rs_t cmd = {d, state, val};
    cs_cmd_push(x_rs, &cmd, sizeof cmd);
}

typedef struct { d3ddev *d; u32 stage, type, val; } cmd_tss_t;
static void x_tss(void *p) { const cmd_tss_t *q = p; d3d8_r_apply_tss(&q->d->r, q->stage, q->type, q->val); }
void d3d8_cmd_tss(d3ddev *d, u32 stage, u32 type, u32 val)
{
    cmd_tss_t cmd = {d, stage, type, val};
    cs_cmd_push(x_tss, &cmd, sizeof cmd);
}

typedef struct { d3ddev *d; u32 state; float mat[16]; } cmd_xf_t;
static void x_xf(void *p) { const cmd_xf_t *q = p; d3d8_r_apply_xf(&q->d->r, q->state, q->mat); }
void d3d8_cmd_xf(d3ddev *d, u32 state, const float mat[16])
{
    cmd_xf_t cmd;
    cmd.d = d;
    cmd.state = state;
    memcpy(cmd.mat, mat, 64);
    cs_cmd_push(x_xf, &cmd, sizeof cmd);
}

typedef struct { d3ddev *d; u32 vp[4]; float vpz[2]; } cmd_vp_t;
static void x_vp(void *p)
{
    const cmd_vp_t *q = p;
    d3d_rstate *r = &q->d->r;
    memcpy(r->vp, q->vp, sizeof r->vp);
    memcpy(r->vpz, q->vpz, sizeof r->vpz);
    if (dumping_frame(r->frame))
        fprintf(stderr, "[dump] SetViewport %u,%u %ux%u\n", r->vp[0], r->vp[1], r->vp[2], r->vp[3]);
    r->gen_vp++;
    r->gl_dirty |= GLD_VP;
}
void d3d8_cmd_vp(d3ddev *d, const u32 vp[4], const float vpz[2])
{
    cmd_vp_t cmd;
    cmd.d = d;
    memcpy(cmd.vp, vp, sizeof cmd.vp);
    memcpy(cmd.vpz, vpz, sizeof cmd.vpz);
    cs_cmd_push(x_vp, &cmd, sizeof cmd);
}

typedef struct { d3ddev *d; float mat[17]; } cmd_mat_t;
static void x_mat(void *p)
{
    const cmd_mat_t *q = p;
    memcpy(q->d->r.material, q->mat, sizeof q->d->r.material);
    q->d->r.gen_mat++;
}
void d3d8_cmd_mat(d3ddev *d, const float mat[17])
{
    cmd_mat_t cmd;
    cmd.d = d;
    memcpy(cmd.mat, mat, sizeof cmd.mat);
    cs_cmd_push(x_mat, &cmd, sizeof cmd);
}

typedef struct { d3ddev *d; u32 idx; d3dlight l; } cmd_light_t;
static void x_light(void *p)
{
    const cmd_light_t *q = p;
    d3d_rstate *r = &q->d->r;
    if (r->lights[q->idx].type != q->l.type)
        r->key_dirty = 1;
    r->lights[q->idx] = q->l;
    r->gen_light++;
}
void d3d8_cmd_light(d3ddev *d, u32 idx, const d3dlight *l)
{
    cmd_light_t cmd = {d, idx, *l};
    cs_cmd_push(x_light, &cmd, sizeof cmd);
}

typedef struct { d3ddev *d; u32 idx; u8 on; } cmd_light_en_t;
static void x_light_en(void *p)
{
    const cmd_light_en_t *q = p;
    d3d_rstate *r = &q->d->r;
    if (r->light_on[q->idx] != q->on)
        r->key_dirty = 1;
    r->light_on[q->idx] = q->on;
    r->gen_light++;
}
void d3d8_cmd_light_enable(d3ddev *d, u32 idx, u8 on)
{
    cmd_light_en_t cmd = {d, idx, on};
    cs_cmd_push(x_light_en, &cmd, sizeof cmd);
}

typedef struct { d3ddev *d; u32 idx; float plane[4]; } cmd_clip_t;
static void x_clip(void *p)
{
    const cmd_clip_t *q = p;
    memcpy(q->d->r.clip[q->idx], q->plane, 16);
    q->d->r.gen_clip++;
}
void d3d8_cmd_clip(d3ddev *d, u32 idx, const float plane[4])
{
    cmd_clip_t cmd = {d, idx, {plane[0], plane[1], plane[2], plane[3]}};
    cs_cmd_push(x_clip, &cmd, sizeof cmd);
}

typedef struct { d3ddev *d; d3dtex *t; u32 stage; } cmd_tex_t;
static void x_tex(void *p) { const cmd_tex_t *q = p; d3d8_r_apply_tex(&q->d->r, q->stage, q->t); }
void d3d8_cmd_tex(d3ddev *d, u32 stage, d3dtex *t)
{
    cmd_tex_t cmd = {d, t, stage};
    cs_cmd_push(x_tex, &cmd, sizeof cmd);
}

typedef struct { d3ddev *d; d3dbuf *b; u32 n, stride; } cmd_stream_t;
static void x_stream(void *p)
{
    const cmd_stream_t *q = p;
    q->d->r.stream[q->n] = q->b;
    q->d->r.stride[q->n] = q->stride;
}
void d3d8_cmd_stream(d3ddev *d, u32 n, d3dbuf *b, u32 stride)
{
    cmd_stream_t cmd = {d, b, n, stride};
    cs_cmd_push(x_stream, &cmd, sizeof cmd);
}

typedef struct { d3ddev *d; d3dbuf *b; u32 base_vertex; } cmd_indices_t;
static void x_indices(void *p)
{
    const cmd_indices_t *q = p;
    q->d->r.indices = q->b;
    q->d->r.base_vertex = q->base_vertex;
}
void d3d8_cmd_indices(d3ddev *d, d3dbuf *b, u32 base_vertex)
{
    cmd_indices_t cmd = {d, b, base_vertex};
    cs_cmd_push(x_indices, &cmd, sizeof cmd);
}

typedef struct { d3ddev *d; u32 val; } cmd_u32_t;
static void x_vs(void *p)
{
    const cmd_u32_t *q = p;
    q->d->r.vs = q->val;
    q->d->r.decl_dirty = 1;
    q->d->r.key_dirty = 1;
}
void d3d8_cmd_vs(d3ddev *d, u32 vs)
{
    cmd_u32_t cmd = {d, vs};
    cs_cmd_push(x_vs, &cmd, sizeof cmd);
}

typedef struct { u32 idx; vdecl decl; } cmd_vs_set_t;
static void x_vs_set(void *p)
{
    const cmd_vs_set_t *q = p;
    if (q->idx < D3D8_MAX_VS) {
        g_r_vs[q->idx].used = 1;
        g_r_vs[q->idx].decl = q->decl;
    }
}
void d3d8_cmd_vs_set(u32 idx, const vdecl *decl)
{
    cmd_vs_set_t cmd = {idx, *decl};
    cs_cmd_push(x_vs_set, &cmd, sizeof cmd);
}

static void x_vs_del(void *p)
{
    u32 idx = *(const u32 *)p;
    if (idx < D3D8_MAX_VS) {
        g_r_vs[idx].used = 0;
        memset(&g_r_vs[idx].decl, 0, sizeof g_r_vs[idx].decl);
    }
}
void d3d8_cmd_vs_del(u32 idx) { cs_cmd_push(x_vs_del, &idx, sizeof idx); }

static void x_ps(void *p)
{
    const cmd_u32_t *q = p;
    q->d->r.ps = q->val;
    q->d->r.key_dirty = 1;
}
void d3d8_cmd_ps(d3ddev *d, u32 ps)
{
    cmd_u32_t cmd = {d, ps};
    cs_cmd_push(x_ps, &cmd, sizeof cmd);
}

typedef struct { d3ddev *d; u32 reg, count; float data[8][4]; } cmd_psc_t;
static void x_psc(void *p)
{
    const cmd_psc_t *q = p;
    memcpy(q->d->r.psc[q->reg], q->data, (size_t)q->count * 16u);
    q->d->r.gen_psc++;
}
void d3d8_cmd_psc(d3ddev *d, u32 reg, u32 count, const float data[][4])
{
    cmd_psc_t cmd;
    cmd.d = d;
    cmd.reg = reg;
    cmd.count = count;
    memcpy(cmd.data, data, (size_t)count * 16u);
    cs_cmd_push(x_psc, &cmd, sizeof cmd);
}

typedef struct { d3ddev *d; d3dsurf *rt, *ds; int reset_vp; } cmd_rt_t;
static void x_set_rt(void *p)
{
    const cmd_rt_t *q = p;
    d3d_rstate *r = &q->d->r;
    if (q->reset_vp && dumping_frame(r->frame) && r->rt && r->rt->tex)
        dump_target_r(r);
    r->rt = q->rt;
    r->ds = q->ds;
    if (q->reset_vp && r->rt) {
        if (dumping_frame(r->frame))
            fprintf(stderr, "[dump] SetRenderTarget %ux%u fmt %x%s ds %ux%u\n", r->rt->w, r->rt->h, r->rt->fmt,
                    r->rt->tex ? " (texture)" : "", r->ds ? r->ds->w : 0, r->ds ? r->ds->h : 0);
        r->vp[0] = r->vp[1] = 0;
        r->vp[2] = r->rt->w;
        r->vp[3] = r->rt->h;
        r->vpz[0] = 0.0f;
        r->vpz[1] = 1.0f;
    }
    bind_target_r(r);
}
void d3d8_cmd_set_rt(d3ddev *d, d3dsurf *rt, d3dsurf *ds, int reset_vp)
{
    cmd_rt_t cmd = {d, rt, ds, reset_vp};
    cs_cmd_push(x_set_rt, &cmd, sizeof cmd);
}

void d3d8_bind_target(d3ddev *d)
{
    if (d)
        d3d8_cmd_set_rt(d, d->rt, d->ds, 0);
}

typedef struct {
    d3ddev *d;
    u32 flags, color, stencil, count;
    float z;
    const s32 *rects;
    void *heap;
} cmd_clear_t;

static void x_clear(void *p)
{
    const cmd_clear_t *q = p;
    d3d_rstate *r = &q->d->r;
    u32 flags = q->flags, color = q->color, count = q->count;
    GLbitfield mask = 0;
    if (flags & 1) {
        mask |= GL_COLOR_BUFFER_BIT;
        if (!r->rt)
            atomic_store_explicit(&g_diag_clear, color, memory_order_relaxed);  // цвет фона кадра — для диагностики
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor((float)((color >> 16) & 0xFF) / 255.0f, (float)((color >> 8) & 0xFF) / 255.0f,
                     (float)(color & 0xFF) / 255.0f, (float)(color >> 24) / 255.0f);
    }
    if ((flags & 2) && r->ds) {
        mask |= GL_DEPTH_BUFFER_BIT;
        glDepthMask(GL_TRUE);
        glClearDepth(q->z);
    }
    if ((flags & 4) && r->ds) {
        mask |= GL_STENCIL_BUFFER_BIT;
        glStencilMask(0xFF);
        glClearStencil((GLint)q->stencil);
    }
    if (dumping_frame(r->frame))
        fprintf(stderr, "[dump] Clear flags %x color %08x z %g rects %u vp %u,%u %ux%u\n", flags, color, q->z, count,
                r->vp[0], r->vp[1], r->vp[2], r->vp[3]);
    if (mask) {
        glEnable(GL_SCISSOR_TEST);
        if (!count || !q->rects) {
            glScissor((GLint)r->vp[0], (GLint)r->vp[1], (GLsizei)r->vp[2], (GLsizei)r->vp[3]);
            glClear(mask);
        }
        for (u32 i = 0; q->rects && i < count; i++) {
            s32 x1 = q->rects[4u * i + 0], y1 = q->rects[4u * i + 1];
            s32 x2 = q->rects[4u * i + 2], y2 = q->rects[4u * i + 3];
            if (x2 > x1 && y2 > y1) {
                glScissor(x1, y1, x2 - x1, y2 - y1);
                glClear(mask);
            }
        }
        r->gl_dirty |= GLD_SCISSOR;
        if (flags & 1)
            r->gl_dirty |= GLD_COLORMASK;
        if ((flags & 2) && r->ds)
            r->gl_dirty |= GLD_DEPTH;
        if ((flags & 4) && r->ds)
            r->gl_dirty |= GLD_STENCIL;
    }
    free(q->heap);
}

void d3d8_cmd_clear(d3ddev *d, u32 flags, u32 color, float z, u32 stencil, u32 count, const s32 *rects)
{
    cmd_clear_t cmd = {d, flags, color, stencil, count, z, rects, NULL};
    cs_cmd_push_var(x_clear, &cmd, sizeof cmd, rects, (size_t)count * 16u,
                    offsetof(cmd_clear_t, rects), offsetof(cmd_clear_t, heap));
}

// Исполнение отрисовки на Core 2: vdecl/ffkey/program кешируются между вызовами по флагам decl_dirty/key_dirty
static void exec_draw(d3d_rstate *r, u32 prim, u32 count, int indexed, u32 first, u32 min_index, u32 num_vertices,
                      const void *up_vertices, u32 up_stride, const void *up_indices, u32 up_index_fmt)
{
    if (!count)
        return;
    r->draws++;
    if (r->decl_dirty) {
        r->decl_ok = r_vdecl_for(r, &r->cached_decl);
        r->decl_dirty = 0;
    }
    if (!r->decl_ok) {
        hle_todo("d3d8: draw with an unknown vertex format");
        return;
    }
    GLenum mode;
    u32 n;
    vertex_count(prim, count, &mode, &n);
    if (r->key_dirty || !g_cached_prog) {
        make_key(r, &r->cached_decl, &g_cached_key);
        g_cached_prog = program_for(r, &g_cached_key);
        r->key_dirty = 0;
    }
    if (dumping_frame(r->frame))
        dump_draw(r, &g_cached_key, prim, count, indexed);
    diag_record(r, &g_cached_key);
    ffprog *p = g_cached_prog;
    if (!p || !p->prog)
        return;
    p->last_frame = r->frame;
    apply_state(r);
    if (g_cur_prog != p->prog) {
        glUseProgram(p->prog);
        g_cur_prog = p->prog;
    }
    upload_uniforms(r, p);
    for (int s = 0; s < g_cached_key.nstages; s++) {
        d3dtex *t = r->tex[s];
        if (!t)
            continue;
        glActiveTexture(GL_TEXTURE0 + (GLenum)s);
        glBindTexture(t->cube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D, t->gl);
        if (r->sampler_dirty & (1u << s)) {
            update_sampler(r, s);
            r->sampler_dirty &= ~(1u << s);
        }
        glBindSampler((GLuint)s, r->samplers[s]);
    }
    glBindVertexArray(r->vao);
    if (!indexed) {
        if (up_vertices) {
            setup_attribs(r, &r->cached_decl, up_vertices, up_stride, n * up_stride);
            glDrawArrays(mode, 0, (GLsizei)n);
        } else {
            setup_attribs(r, &r->cached_decl, NULL, 0, 0);
            glDrawArrays(mode, (GLint)first, (GLsizei)n);
        }
    } else if (up_vertices) {
        u32 isize = up_index_fmt == FMT_INDEX32 ? 4 : 2;
        setup_attribs(r, &r->cached_decl, up_vertices, up_stride, (min_index + num_vertices) * up_stride);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, r->up_ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, n * isize, up_indices, GL_STREAM_DRAW);
        glDrawElements(mode, (GLsizei)n, isize == 4 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT, 0);
    } else {
        d3dbuf *ib = r->indices;
        if (!ib)
            return;
        u32 isize = ib->fmt == FMT_INDEX32 ? 4 : 2;
#ifdef SNX_GLES
        if (!gles_caps.base_vertex && r->base_vertex) {
            g_attr_base = r->base_vertex;
            setup_attribs(r, &r->cached_decl, NULL, 0, 0);
            g_attr_base = 0;  // указатели атрибутов ставятся заново при каждой отрисовке
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib->gl);
            glDrawElements(mode, (GLsizei)n, isize == 4 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT,
                           (const void *)(uintptr_t)(first * isize));
            return;
        }
#endif
        setup_attribs(r, &r->cached_decl, NULL, 0, 0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib->gl);
        glDrawElementsBaseVertex(mode, (GLsizei)n, isize == 4 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT,
                                 (const void *)(uintptr_t)(first * isize), (GLint)r->base_vertex);
    }
}

typedef struct {
    d3ddev *d;
    u32 prim, count, first, min_index, num_vertices;
    int indexed;
} cmd_draw_t;

static void x_draw(void *p)
{
    const cmd_draw_t *q = p;
    exec_draw(&q->d->r, q->prim, q->count, q->indexed, q->first, q->min_index, q->num_vertices, NULL, 0, NULL, 0);
}

void d3d8_cmd_draw(d3ddev *d, u32 prim, u32 count, int indexed, u32 first, u32 min_index, u32 num_vertices)
{
    cmd_draw_t cmd = {d, prim, count, first, min_index, num_vertices, indexed};
    cs_cmd_push(x_draw, &cmd, sizeof cmd);
}

typedef struct {
    d3ddev *d;
    u32 prim, count, min_index, num_vertices, up_stride, up_index_fmt, idx_off;
    int indexed;
    const void *data;  // вершины в [0..vbytes), индексы в [idx_off..idx_off+ibytes) или up_indices в direct
    const void *direct_idx;
    void *heap;
} cmd_draw_up_t;

static void x_draw_up(void *p)
{
    const cmd_draw_up_t *q = p;
    const u8 *base = q->data;
    const void *idx = q->indexed ? (q->direct_idx ? q->direct_idx : (base ? base + q->idx_off : NULL)) : NULL;
    exec_draw(&q->d->r, q->prim, q->count, q->indexed, 0, q->min_index, q->num_vertices, base, q->up_stride, idx,
              q->up_index_fmt);
    q->d->r.stream[0] = NULL;
    q->d->r.stride[0] = 0;
    if (q->indexed)
        q->d->r.indices = NULL;
    free(q->heap);
}

void d3d8_cmd_draw_up(d3ddev *d, u32 prim, u32 count, int indexed, u32 min_index, u32 num_vertices,
                      const void *up_vertices, u32 up_stride, const void *up_indices, u32 up_index_fmt)
{
    if (!cs_enabled()) {
        cmd_draw_up_t cmd = {d, prim, count, min_index, num_vertices, up_stride, up_index_fmt, 0, indexed,
                             up_vertices, up_indices, NULL};
        x_draw_up(&cmd);
        return;
    }
    GLenum mode;
    u32 n;
    vertex_count(prim, count, &mode, &n);
    u32 vbytes = indexed ? (min_index + num_vertices) * up_stride : n * up_stride;
    u32 ibytes = indexed ? n * (up_index_fmt == FMT_INDEX32 ? 4u : 2u) : 0u;
    if (!ibytes) {
        cmd_draw_up_t cmd = {d, prim, count, min_index, num_vertices, up_stride, up_index_fmt, 0, indexed,
                             up_vertices, NULL, NULL};
        cs_cmd_push_var(x_draw_up, &cmd, sizeof cmd, up_vertices, vbytes,
                        offsetof(cmd_draw_up_t, data), offsetof(cmd_draw_up_t, heap));
        return;
    }
    u32 idx_off = (vbytes + 15u) & ~15u;
    size_t total = (size_t)idx_off + ibytes;
    u8 sbuf[4096];
    u8 *pack = total <= sizeof sbuf ? sbuf : malloc(total);
    if (!pack)
        return;
    if (vbytes)
        memcpy(pack, up_vertices, vbytes);
    memcpy(pack + idx_off, up_indices, ibytes);
    cmd_draw_up_t cmd = {d, prim, count, min_index, num_vertices, up_stride, up_index_fmt, idx_off, indexed,
                         pack, NULL, NULL};
    cs_cmd_push_var(x_draw_up, &cmd, sizeof cmd, pack, total,
                    offsetof(cmd_draw_up_t, data), offsetof(cmd_draw_up_t, heap));
    if (pack != sbuf)
        free(pack);
}

// ================================================================ снимки кадров (отладка)
// SNX_SHOT=N[,K]: снимок заднего буфера в кадре N (и далее каждые K кадров) -> shot_<кадр>.png в текущем каталоге
// или в SNX_SHOT_DIR. PNG без сжатия (deflate stored), чтобы не тащить zlib.
static u32 crc_tab[256];
static u32 png_crc(u32 c, const u8 *p, size_t n)
{
    if (!crc_tab[1])
        for (u32 i = 0; i < 256; i++) {
            u32 x = i;
            for (int k = 0; k < 8; k++)
                x = (x & 1) ? 0xEDB88320u ^ (x >> 1) : x >> 1;
            crc_tab[i] = x;
        }
    for (size_t i = 0; i < n; i++)
        c = crc_tab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c;
}

static void be32(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
}

static void png_chunk(FILE *f, const char *type, const u8 *data, u32 n)
{
    u8 hdr[8];
    be32(hdr, n);
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    if (n)
        fwrite(data, 1, n, f);
    u32 c = png_crc(0xFFFFFFFFu, hdr + 4, 4);
    c = png_crc(c, data, n) ^ 0xFFFFFFFFu;
    be32(hdr, c);
    fwrite(hdr, 1, 4, f);
}

static void write_png(const char *path, const u8 *rgba, u32 w, u32 h)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    u8 ihdr[13];
    be32(ihdr, w);
    be32(ihdr + 4, h);
    ihdr[8] = 8;
    ihdr[9] = 2;  // RGB
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    png_chunk(f, "IHDR", ihdr, 13);
    size_t row = 1 + (size_t)w * 3, raw_n = row * h;
    u8 *raw = malloc(raw_n);
    for (u32 y = 0; y < h; y++) {
        raw[y * row] = 0;
        for (u32 x = 0; x < w; x++)
            memcpy(raw + y * row + 1 + x * 3, rgba + ((size_t)y * w + x) * 4, 3);
    }
    size_t nblk = (raw_n + 65534) / 65535, zn = 2 + raw_n + nblk * 5 + 4;
    u8 *z = malloc(zn), *q = z;
    *q++ = 0x78;
    *q++ = 0x01;
    u32 a = 1, b2 = 0;
    for (size_t off = 0; off < raw_n;) {
        u32 len = raw_n - off > 65535 ? 65535 : (u32)(raw_n - off);
        *q++ = off + len == raw_n;
        *q++ = (u8)len;
        *q++ = (u8)(len >> 8);
        *q++ = (u8)~len;
        *q++ = (u8)(~len >> 8);
        memcpy(q, raw + off, len);
        for (u32 i = 0; i < len; i++) {
            a = (a + raw[off + i]) % 65521;
            b2 = (b2 + a) % 65521;
        }
        q += len;
        off += len;
    }
    be32(q, b2 << 16 | a);
    q += 4;
    png_chunk(f, "IDAT", z, (u32)(q - z));
    png_chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw);
    free(z);
}

// кадр SNX_DUMP_DRAWS: содержимое текущей цели рендера (из её FBO на Core 2) -> <SNX_SHOT_DIR>/dump_<n>.png
static void dump_target_r(d3d_rstate *r)
{
    static int n;
    if (!r->rt)
        return;
    u32 w = r->rt->w, h = r->rt->h;
    u8 *px = malloc((size_t)w * h * 4);
    if (!px)
        return;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, r->fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glBindFramebuffer(GL_FRAMEBUFFER, r->fbo);
    char path[512];
    const char *dir = getenv("SNX_SHOT_DIR");
    snprintf(path, sizeof path, "%s%sdump_%02d.png", dir ? dir : "", dir ? "/" : "", n++);
    write_png(path, px, w, h);
    free(px);
    fprintf(stderr, "[dump] target %ux%u -> %s\n", w, h, path);
}

typedef struct {
    d3ddev *d;
    int from_win;
    u32 w, h;
    u8 *px;
} shot_read_args;

static void x_read_shot(void *arg)
{
    const shot_read_args *a = arg;
    d3d_rstate *r = &a->d->r;
    if (!a->from_win) {
        if (!r->blit_fbo)
            glGenFramebuffers(1, &r->blit_fbo);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, r->blit_fbo);
        d3d8_fbo_attach(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, a->d->backbuffer);
    } else {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    }
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glReadPixels(0, 0, (GLsizei)a->w, (GLsizei)a->h, GL_RGBA, GL_UNSIGNED_BYTE, a->px);
}

static void maybe_shot(d3ddev *d, int win_w, int win_h)
{
    static int first = -2, step;
    if (first == -2) {
        const char *e = getenv("SNX_SHOT");
        first = e ? atoi(e) : -1;
        const char *c = e ? strchr(e, ',') : NULL;
        step = c ? atoi(c + 1) : 0;
    }
    if (first < 0 || d->frame < first || (d->frame != first && (!step || (d->frame - first) % step)))
        return;
    // Читаем итоговый кадр окна после пост-прохода Present (видны 4:3 pillarbox, гамма-LUT и оверлей); если окна
    // нет — резервный снимок из заднего буфера blit_fbo.
    int from_win = win_w > 0 && win_h > 0;
    u32 w = from_win ? (u32)win_w : d->bb_w, h = from_win ? (u32)win_h : d->bb_h;
    u8 *px = malloc((size_t)w * h * 4);
    if (!px)
        return;
    shot_read_args sa = {d, from_win, w, h, px};
    cs_sync(x_read_shot, &sa);
    if (from_win) {  // окно GL — снизу вверх; переворачиваем строки для PNG
        size_t row = (size_t)w * 4;
        u8 *tmp = malloc(row);
        if (tmp) {
            for (u32 y = 0; y < h / 2; y++) {
                memcpy(tmp, px + (size_t)y * row, row);
                memcpy(px + (size_t)y * row, px + (size_t)(h - 1 - y) * row, row);
                memcpy(px + (size_t)(h - 1 - y) * row, tmp, row);
            }
            free(tmp);
        }
    }
    char path[512];
    const char *dir = getenv("SNX_SHOT_DIR");
    snprintf(path, sizeof path, "%s%sshot_%05d.png", dir ? dir : "", dir ? "/" : "", d->frame);
    write_png(path, px, w, h);
    free(px);
    fprintf(stderr, "[d3d8] frame %d: %u draws -> %s\n", d->frame, d->draws, path);
}

// ================================================================ Present: пост-проход, гамма-LUT и 4:3 меню
// Ограничение частоты кадров (SNX_FPS_CAP=30 и т. п.; лаунчер Android — «Ограничение FPS»: меньше нагрев и
// троттлинг телефона; если частота экрана равна ему — только vsync, platform/window.c plat_set_display) и подсказки
// производительности Android (ADPF): длительность работы кадра потоком гостя — без ожиданий потока рендера и
// ограничителя.
static void frame_pace(void)
{
    static int cap = -1;
    static u64 next, last;
#ifdef __ANDROID__
    static u64 last_wait;
#endif
    if (cap < 0) {
        const char *e = getenv("SNX_FPS_CAP");
        cap = e ? atoi(e) : 0;
        cap = cap < 0 ? 0 : cap;
#ifdef __ANDROID__
        android_perf_target(cap > 0 ? 1000000000ull / (u64)cap : 16666667ull);
#endif
    }
    u64 now = rt_ticks_ns();
#ifdef __ANDROID__
    u64 wait = cs_wait_total_ns();
    if (last) {
        u64 dt = now - last, idle = wait - last_wait;
        android_perf_frame(dt > idle ? dt - idle : 0);
    }
    last_wait = wait;
#endif
    if (cap > 0 && !plat_vsync_paced()) {  // частота экрана равна ограничению — кадры отмеряет vsync
        u64 period = 1000000000ull / (u64)cap;
        next = next && now < next + period ? next + period : now + period;
        if (next > now) {
            struct timespec ts = {(time_t)((next - now) / 1000000000ull), (long)((next - now) % 1000000000ull)};
            nanosleep(&ts, NULL);
        }
    }
    last = rt_ticks_ns();
}

static _Atomic int g_fps_overlay;
void d3d8_set_fps_overlay(int on) { g_fps_overlay = on; }
int d3d8_fps_overlay(void) { return g_fps_overlay; }

// ================================================================ диагностика кадра
// Включается на ходу: «Настройки» → «Диагностика кадра». Нужна, чтобы по одному скриншоту с телефона понять,
// откуда взялся цвет на экране: фон заливки Clear, туман, TextureFactor/материал или стадии текстуры. Сверху —
// доминирующий цвет двух полос заднего буфера ДО гаммы Present (совпадает ли он с Clear/туманом), ниже —
// состояние первых четырёх отрисовок кадра (стадии, texgen, texture transform, туман, смешивание, z-тест).
#define DIAG_DRAWS 4
static _Atomic int g_diag = -1;  // -1 — ещё не читали SNX_DIAG (лаунчер: «Диагностика кадра»)

void d3d8_set_diag(int on) { atomic_store_explicit(&g_diag, on != 0, memory_order_relaxed); }

int d3d8_diag(void)
{
    int v = atomic_load_explicit(&g_diag, memory_order_relaxed);
    if (v < 0) {  // первый опрос: значение из окружения (лаунчер передаёт SNX_DIAG)
        const char *e = getenv("SNX_DIAG");
        v = e && *e && *e != '0';
        atomic_store_explicit(&g_diag, v, memory_order_relaxed);
    }
    return v;
}

typedef struct {
    u32 nstages, tex, tci, gen, ttf, proj, tf, fogc, flags;
    u32 tw[2], th[2], tfm[2];  // текстуры стадий 0 и 1: размер и формат D3D
    u8 cop, ca1, ca2;          // операция стадии 0 (D3DTOP и аргументы)
} diag_rec;

static diag_rec g_diag_rec[DIAG_DRAWS];  // пишет поток рендера, читает поток гостя (только для показа)
static _Atomic int g_diag_n;             // сколько отрисовок кадра записано (0..DIAG_DRAWS)

static void diag_record(const d3d_rstate *r, const ffkey *k)
{
    int i = atomic_load_explicit(&g_diag_n, memory_order_relaxed);
    if (i >= DIAG_DRAWS)
        return;
    diag_rec q;
    memset(&q, 0, sizeof q);
    q.nstages = k->nstages;
    for (int s = 0; s < k->nstages && s < 8; s++) {
        q.tex |= (u32)k->st[s].tex << (2 * s);
        if (k->st[s].tci)
            q.tci |= 1u << s;
        q.gen |= (u32)k->st[s].tcgen << (2 * s);
        q.ttf |= (u32)k->st[s].ttf_count << (2 * s);
        if (k->st[s].ttf_proj)
            q.proj |= 1u << s;
    }
    for (int s = 0; s < 2 && s < k->nstages; s++) {
        const d3dtex *t = r->tex[s];
        if (t) {
            q.tw[s] = t->w;
            q.th[s] = t->h;
            q.tfm[s] = t->fmt;
        }
    }
    q.cop = k->st[0].cop;
    q.ca1 = k->st[0].ca1;
    q.ca2 = k->st[0].ca2;
    q.tf = r->rs[60];   // D3DRS_TEXTUREFACTOR
    q.fogc = r->rs[34]; // D3DRS_FOGCOLOR
    q.flags = (u32)(k->fog & 3) | (r->rs[28] ? 4u : 0u) | (r->rs[27] ? 8u : 0u) | (k->lighting ? 16u : 0u) |
              (r->rs[7] ? 32u : 0u) | (r->rs[14] ? 64u : 0u);
    g_diag_rec[i] = q;
    atomic_store_explicit(&g_diag_n, i + 1, memory_order_relaxed);
}

typedef struct {
    d3ddev *d;
    float v0;      // низ полосы в координатах заднего буфера (0 — низ кадра, 1 — верх, как в GL)
    u32 rgb;       // доминирующий цвет полосы
    int pct, w, h; // его доля в процентах и размер кадра
} diag_strip_args;

// Полоса заднего буфера: доминирующий цвет грубой сеткой 4 бита на канал (4096 ячеек), затем среднее по ней
// Смена отладочного варианта: ключи программ кешируются, поэтому просим поток рендера собрать их заново
static void x_diag_invalidate(void *arg)
{
    d3d_rstate *r = &(*(d3ddev **)arg)->r;
    r->key_dirty = 1;
    r->gen_const++;
}

static void x_diag_strip(void *arg)
{
    diag_strip_args *a = arg;
    d3d_rstate *r = &a->d->r;
    d3dsurf *bb = a->d->backbuffer;
    a->rgb = 0;
    a->pct = 0;
    a->w = bb ? (int)bb->w : 0;
    a->h = bb ? (int)bb->h : 0;
    if (!bb || !bb->w || !bb->h)
        return;
    u32 w = bb->w, h = bb->h, rows = 8;
    int y0 = (int)(a->v0 * (float)h);
    if (y0 < 0)
        y0 = 0;
    if ((u32)y0 + rows > h)
        y0 = h > rows ? (int)(h - rows) : 0;
    u8 *px = malloc((size_t)w * rows * 4);
    if (!px)
        return;
    if (!r->blit_fbo)
        glGenFramebuffers(1, &r->blit_fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, r->blit_fbo);
    d3d8_fbo_attach(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, bb);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glReadPixels(0, y0, (GLsizei)w, (GLsizei)rows, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glBindFramebuffer(GL_FRAMEBUFFER, r->fbo);
    static u16 cnt[4096];
    static u32 sum[4096][3];
    memset(cnt, 0, sizeof cnt);
    memset(sum, 0, sizeof sum);
    u32 n = w * rows;
    for (u32 i = 0; i < n; i++) {
        const u8 *p = px + 4u * i;
        u32 c = (((u32)p[0] >> 4) << 8) | (((u32)p[1] >> 4) << 4) | ((u32)p[2] >> 4);
        if (!cnt[c]) {
            sum[c][0] = sum[c][1] = sum[c][2] = 0;
        }
        cnt[c]++;
        sum[c][0] += p[0];
        sum[c][1] += p[1];
        sum[c][2] += p[2];
    }
    int best = 0;
    for (int c = 1; c < 4096; c++)
        if (cnt[c] > cnt[best])
            best = c;
    if (cnt[best]) {
        a->rgb = ((sum[best][0] / cnt[best]) << 16) | ((sum[best][1] / cnt[best]) << 8) | (sum[best][2] / cnt[best]);
        a->pct = (int)((u64)cnt[best] * 100u / (n ? n : 1u));
    }
    free(px);
}

static void diag_text(int w, int h, const diag_strip_args *sa, const diag_strip_args *sb)
{
    char line[12][192];
    int nl = 0;
    u32 clr = atomic_load_explicit(&g_diag_clear, memory_order_relaxed);
    u32 fogc = g_diag_n ? g_diag_rec[0].fogc : 0;
    snprintf(line[nl++], sizeof line[0], "clear=%08X fog=%08X", clr, fogc);
    snprintf(line[nl++], sizeof line[0], "top=%d%%:%02X%02X%02X mid=%d%%:%02X%02X%02X", sa->pct, (sa->rgb >> 16) & 0xFF,
             (sa->rgb >> 8) & 0xFF, sa->rgb & 0xFF, sb->pct, (sb->rgb >> 16) & 0xFF, (sb->rgb >> 8) & 0xFF,
             sb->rgb & 0xFF);
    int nd = atomic_load_explicit(&g_diag_n, memory_order_relaxed);
    for (int i = 0; i < nd && nl < 12; i++) {
        const diag_rec *q = &g_diag_rec[i];
        snprintf(line[nl++], sizeof line[0], "d%d st%u tex=%u/%u gen=%u/%u tci=%X ttf=%u/%u pj=%X tf=%08X", i,
                 (unsigned)q->nstages, q->tex & 3u, (q->tex >> 2) & 3u, q->gen & 3u, (q->gen >> 2) & 3u,
                 (unsigned)q->tci, q->ttf & 3u, (q->ttf >> 2) & 3u, (unsigned)q->proj, q->tf);
        snprintf(line[nl++], sizeof line[0], "   fm%u fog=%u ab=%u lit=%u z=%u/%u", q->flags & 3u,
                 (q->flags >> 2) & 1u, (q->flags >> 3) & 1u, (q->flags >> 4) & 1u, (q->flags >> 5) & 1u,
                 (q->flags >> 6) & 1u);
    }
    float fh = (float)h * 0.026f;
    if (fh < 11.0f)
        fh = 11.0f;
    if (fh > 40.0f)
        fh = 40.0f;
    float step = fh * 1.2f, pad = fh * 0.35f, x = fh * 0.5f, y = fh * 0.5f, bw = 0.0f;
    for (int i = 0; i < nl; i++) {
        float tw = ov_text_width(fh, line[i]);
        if (tw > bw)
            bw = tw;
    }
    ov_rect(x - pad, y - pad, bw + 2.0f * pad, (float)nl * step + 2.0f * pad, OV_RGBA(0, 0, 0, 190));
    for (int i = 0; i < nl; i++)
        ov_text(x, y + (float)i * step, fh, OV_RGBA(235, 240, 220, 255), line[i]);
    (void)w;
}

// Режим 4:3-pillarbox для страниц меню (выключен по умолчанию: у страниц книги Shrek 2 есть собственные
// кинематографические чёрные полосы 12.5% сверху и снизу; включается в Quick Menu или Snx.ini)
static _Atomic int g_menu_aspect;

static void sync_menu_aspect_flag(const d3ddev *d)
{
    int active = d && d->bb_h && atomic_load_explicit(&g_menu_aspect, memory_order_relaxed) && game_menu_open(NULL) &&
                 (float)d->bb_w / (float)d->bb_h > 4.0f / 3.0f + 0.01f;
    __atomic_store_n(&rt_menu_aspect_4x3, active, __ATOMIC_RELAXED);
}

void d3d8_set_menu_aspect(int on)
{
    atomic_store_explicit(&g_menu_aspect, on != 0, memory_order_relaxed);
    sync_menu_aspect_flag(d3d8_dev());
}

int d3d8_menu_aspect(void) { return atomic_load_explicit(&g_menu_aspect, memory_order_relaxed); }

// Единый расчёт прямоугольника вывода кадра в окне w x h (используется и в Present, и в пересчёте координат ввода)
static void present_rect(const d3ddev *d, int w, int h, float *dx, float *dy, float *dw, float *dh)
{
    float disp_aspect = (float)d->bb_w / (float)d->bb_h;
    if (atomic_load_explicit(&g_menu_aspect, memory_order_relaxed) && game_menu_open(NULL) &&
        disp_aspect > 4.0f / 3.0f + 0.01f)
        disp_aspect = 4.0f / 3.0f;
    float win_aspect = (float)w / (float)h;
    if (win_aspect >= disp_aspect) {
        *dh = (float)h;
        *dw = (float)h * disp_aspect;
    } else {
        *dw = (float)w;
        *dh = (float)w / disp_aspect;
    }
    *dx = ((float)w - *dw) * 0.5f;
    *dy = ((float)h - *dh) * 0.5f;
}

typedef struct {
    GLuint bb_tex;
    int win_w, win_h;
    int dx, dy, dw, dh;
    int update_gamma;
    u16 gamma_rgb[256][3];  // 256x1 RGB16 LUT (0..65535 -> [0, 1])
} present_pass_args;

static GLuint g_post_prog, g_post_vao, g_gamma_tex;

// ================================================================ 1-проходное GPU-размытие теней-проекторов
// Эквивалент 9 проходов FCanvasUtil::DrawTile (3x3, смещение ±step_px, SrcAlpha=28/255, Dest=One)
// в Engine.dll!0x121e0050 (UShadowBitmapMaterial::Get). Обе поверхности — D3D8 RTT («сверху вниз»),
// поэтому UV без Y-переворота; GL_BLEND выключен, альфа результата = 1.0.
static GLuint g_blur_prog, g_blur_vao, g_blur_sampler, g_blur_fbo;
static GLint g_blur_u_step = -1;

static void blur_init_gl(void)
{
    static const char vs_src[] =
        SNX_GLSL_HEADER
        "out vec2 v_uv;\n"
        "void main() {\n"
        "  vec2 p = vec2((gl_VertexID & 1) * 2, (gl_VertexID & 2));\n"
        "  v_uv = p;\n"
        "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
        "}\n";
    static const char fs_src[] =
        SNX_GLSL_HEADER
        SNX_GLSL_PRECISION
        "in vec2 v_uv;\n"
        "uniform sampler2D u_src;\n"
        "uniform vec2 u_step;\n"
        "out vec4 o_color;\n"
        "void main() {\n"
        "  vec3 s = vec3(0.0);\n"
        "  s += texture(u_src, v_uv + vec2(-u_step.x, -u_step.y)).rgb;\n"
        "  s += texture(u_src, v_uv + vec2(      0.0, -u_step.y)).rgb;\n"
        "  s += texture(u_src, v_uv + vec2( u_step.x, -u_step.y)).rgb;\n"
        "  s += texture(u_src, v_uv + vec2(-u_step.x,       0.0)).rgb;\n"
        "  s += texture(u_src, v_uv                             ).rgb;\n"
        "  s += texture(u_src, v_uv + vec2( u_step.x,       0.0)).rgb;\n"
        "  s += texture(u_src, v_uv + vec2(-u_step.x,  u_step.y)).rgb;\n"
        "  s += texture(u_src, v_uv + vec2(      0.0,  u_step.y)).rgb;\n"
        "  s += texture(u_src, v_uv + vec2( u_step.x,  u_step.y)).rgb;\n"
        "  o_color = vec4(s * (28.0 / 255.0), 1.0);\n"
        "}\n";
    GLuint v = compile(GL_VERTEX_SHADER, vs_src), f = compile(GL_FRAGMENT_SHADER, fs_src);
    if (v && f) {
        g_blur_prog = glCreateProgram();
        glAttachShader(g_blur_prog, v);
        glAttachShader(g_blur_prog, f);
        glLinkProgram(g_blur_prog);
        GLint ok = 0;
        glGetProgramiv(g_blur_prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            glDeleteProgram(g_blur_prog);
            g_blur_prog = 0;
        } else {
            glUseProgram(g_blur_prog);
            glUniform1i(glGetUniformLocation(g_blur_prog, "u_src"), 0);
            g_blur_u_step = glGetUniformLocation(g_blur_prog, "u_step");
            g_cur_prog = 0;
        }
    }
    if (v)
        glDeleteShader(v);
    if (f)
        glDeleteShader(f);
    glGenVertexArrays(1, &g_blur_vao);
    glGenFramebuffers(1, &g_blur_fbo);
    glGenSamplers(1, &g_blur_sampler);
    glSamplerParameteri(g_blur_sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glSamplerParameteri(g_blur_sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glSamplerParameteri(g_blur_sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(g_blur_sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

typedef struct {
    d3ddev *d;
    d3dtex *src_tex;
    d3dsurf *src_surf;
    d3dsurf *dst_surf;
    float step_px;
} cmd_shadow_blur_t;

static void x_shadow_blur(void *p)
{
    const cmd_shadow_blur_t *q = p;
    d3d_rstate *r = &q->d->r;
    GLuint src_gl = q->src_tex ? q->src_tex->gl : (q->src_surf ? q->src_surf->color_tex : 0);
    u32 src_w = q->src_tex ? q->src_tex->w : (q->src_surf ? q->src_surf->w : 0);
    u32 src_h = q->src_tex ? q->src_tex->h : (q->src_surf ? q->src_surf->h : 0);
    d3dsurf *dst = q->dst_surf;
    if (!src_gl || !src_w || !src_h || !dst || !dst->w || !dst->h)
        return;
    if (!g_blur_prog)
        blur_init_gl();
    if (!g_blur_prog)
        return;

    int own_fbo = !(r->fbo && r->fbo_rt == dst);
    if (own_fbo) {
        glBindFramebuffer(GL_FRAMEBUFFER, g_blur_fbo);
        d3d8_fbo_attach(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, dst);
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, r->fbo);
    }

    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_POLYGON_OFFSET_FILL);
    for (int i = 0; i < 6; i++)
        glDisable(GL_CLIP_DISTANCE0 + i);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, (GLsizei)dst->w, (GLsizei)dst->h);

    glUseProgram(g_blur_prog);
    glUniform2f(g_blur_u_step, q->step_px / (float)src_w, q->step_px / (float)src_h);
    glBindVertexArray(g_blur_vao);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, src_gl);
    glBindSampler(0, g_blur_sampler);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // Восстановить FBO, VAO, сэмплер/текстуру 0 и пометить фиксированное состояние GL грязным
    if (own_fbo) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, r->fbo);
    }
    glBindVertexArray(r->vao);
    glBindSampler(0, r->samplers[0]);
    glBindTexture(GL_TEXTURE_2D, (r->tex[0] && !r->tex[0]->cube) ? r->tex[0]->gl : 0);
    r->gl_dirty = GLD_ALL;
    r->sampler_dirty |= 1u;
    g_cur_prog = 0;
    r->draws++;
}

void d3d8_cmd_shadow_blur(d3ddev *d, d3dtex *src_tex, d3dsurf *src_surf, d3dsurf *dst_surf, float step_px)
{
    cmd_shadow_blur_t cmd = {d, src_tex, src_surf, dst_surf, step_px};
    cs_cmd_push(x_shadow_blur, &cmd, sizeof cmd);
}

static void post_init_gl(void)
{
    static const char vs_src[] =
        SNX_GLSL_HEADER
        "out vec2 v_uv;\n"
        "void main() {\n"
        "  vec2 p = vec2((gl_VertexID & 1) * 2, (gl_VertexID & 2));\n"
        "  v_uv = vec2(p.x, 1.0 - p.y);\n"  // переворот по Y: кадр D3D хранится сверху вниз
        "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
        "}\n";
    static const char fs_src[] =
        SNX_GLSL_HEADER
        SNX_GLSL_PRECISION
        "in vec2 v_uv;\n"
        "uniform sampler2D u_frame;\n"
        "uniform sampler2D u_gamma;\n"
        "out vec4 o_color;\n"
        "void main() {\n"
        "  vec3 c = clamp(texture(u_frame, v_uv).rgb, 0.0, 1.0);\n"
        "  vec3 u = (c * 255.0 + 0.5) / 256.0;\n"
        "  o_color = vec4(texture(u_gamma, vec2(u.r, 0.5)).r,\n"
        "                 texture(u_gamma, vec2(u.g, 0.5)).g,\n"
        "                 texture(u_gamma, vec2(u.b, 0.5)).b, 1.0);\n"
        "}\n";
    GLuint v = compile(GL_VERTEX_SHADER, vs_src), f = compile(GL_FRAGMENT_SHADER, fs_src);
    if (v && f) {
        g_post_prog = glCreateProgram();
        glAttachShader(g_post_prog, v);
        glAttachShader(g_post_prog, f);
        glLinkProgram(g_post_prog);
        GLint ok = 0;
        glGetProgramiv(g_post_prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            glDeleteProgram(g_post_prog);
            g_post_prog = 0;
        } else {
            glUseProgram(g_post_prog);
            glUniform1i(glGetUniformLocation(g_post_prog, "u_frame"), 0);
            glUniform1i(glGetUniformLocation(g_post_prog, "u_gamma"), 1);
        }
    }
    if (v)
        glDeleteShader(v);
    if (f)
        glDeleteShader(f);
    glGenVertexArrays(1, &g_post_vao);
    glGenTextures(1, &g_gamma_tex);
    glBindTexture(GL_TEXTURE_2D, g_gamma_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

static void x_present_pass(void *arg)
{
    const present_pass_args *a = arg;
    int first = !g_post_prog;
    if (first)
        post_init_gl();
    if (a->update_gamma || first) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, g_gamma_tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
#ifdef SNX_GLES
        // в ES 3.0 нет нормализованных 16-битных текстур (GL_RGB16): LUT в 8 битах на канал
        u8 lut[256][4];
        for (int i = 0; i < 256; i++) {
            lut[i][0] = (u8)(a->gamma_rgb[i][0] >> 8);
            lut[i][1] = (u8)(a->gamma_rgb[i][1] >> 8);
            lut[i][2] = (u8)(a->gamma_rgb[i][2] >> 8);
            lut[i][3] = 255;
        }
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, lut);
#else
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB16, 256, 1, 0, GL_RGB, GL_UNSIGNED_SHORT, a->gamma_rgb);
#endif
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_POLYGON_OFFSET_FILL);
    for (int i = 0; i < 6; i++)
        glDisable(GL_CLIP_DISTANCE0 + i);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, a->win_w, a->win_h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!g_post_prog || !a->bb_tex)
        return;
    glViewport(a->dx, a->dy, a->dw, a->dh);
    glUseProgram(g_post_prog);
    glBindVertexArray(g_post_vao);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, a->bb_tex);
    glBindSampler(0, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, g_gamma_tex);
    glBindSampler(1, 0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

// счётчик кадров: семисегментные цифры прямоугольниками glClear с ножницами (без шейдеров и текстур)
static void fill(int x, int y, int w, int h)
{
    glScissor(x, y, w, h);
    glClear(GL_COLOR_BUFFER_BIT);
}

// число n в левом верхнем углу окна w x h (у GL начало — внизу)
static void draw_fps(int n, int w, int h)
{
    (void)w;
    static const u8 seg[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};  // биты: a b c d e f g
    char s[8];
    int len = snprintf(s, sizeof s, "%d", n < 0 ? 0 : n > 999 ? 999 : n);
    int dh = h / 24, dw = dh * 11 / 20, t = dh / 7 > 1 ? dh / 7 : 1, gap = dh / 3, m = h / 60;
    int x0 = m + gap / 2, top = h - m - gap / 2;
    glEnable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
    fill(m, top - dh - gap / 2, len * (dw + gap) + gap / 2, dh + gap);
    glClearColor(1.0f, 0.85f, 0.1f, 1);
    for (int i = 0; i < len; i++) {
        int x = x0 + i * (dw + gap), b = seg[s[i] - '0'], mid = top - dh / 2 - t / 2;
        if (b & 0x01) fill(x, top - t, dw, t);                    // a
        if (b & 0x02) fill(x + dw - t, mid, t, top - mid);        // b
        if (b & 0x04) fill(x + dw - t, top - dh, t, mid - (top - dh) + t);  // c
        if (b & 0x08) fill(x, top - dh, dw, t);                   // d
        if (b & 0x10) fill(x, top - dh, t, mid - (top - dh) + t); // e
        if (b & 0x20) fill(x, mid, t, top - mid);                 // f
        if (b & 0x40) fill(x, mid, dw, t);                        // g
    }
    glDisable(GL_SCISSOR_TEST);
}

typedef struct { int n, w, h; } draw_fps_args;

static void x_draw_fps(void *arg)
{
    const draw_fps_args *a = arg;
    draw_fps(a->n, a->w, a->h);
}

static void x_post_present(void *arg)
{
    d3ddev *d = *(d3ddev **)arg;
    d3d_rstate *r = &d->r;
    glBindFramebuffer(GL_FRAMEBUFFER, r->fbo);
    glBindVertexArray(r->vao);
    r->gl_dirty = GLD_ALL;
    g_cur_prog = 0;
    r->draws = 0;
    g_diag_n = 0;  // диагностика: следующий кадр пишется с чистого листа
    r->frame++;
    if (r->frame < 5 || r->frame % 300 == 0) {
        GLenum err = glGetError();
        if (err)
            fprintf(stderr, "[d3d8] GL error 0x%x by frame %d\n", err, r->frame);
    }
}

// Долгий кадр (дольше 40 мс и в полтора раза дольше обычного): что в нём было — сборка программ GL, чтения файлов
// с диска, заливка текстур, ожидание гостем потока рендера (фризы на новых эффектах и в отдельных местах карты)
static void hitch_check(d3ddev *d)
{
    typedef struct {
        unsigned long long reads, read_bytes, read_ns, links, link_ns, uploads, upload_bytes, wait_ns;
    } snap;
    static snap prev;
    static u64 t_prev, usual;
    snap cur = {g_hitch.reads, g_hitch.read_bytes, g_hitch.read_ns, g_hitch.links, g_hitch.link_ns,
                g_hitch.uploads, g_hitch.upload_bytes, cs_wait_total_ns()};
    u64 now = rt_ticks_ns(), dt = t_prev ? now - t_prev : 0;
    t_prev = now;
    if (dt && usual && dt > 40000000ull && dt * 2 > usual * 3)
        fprintf(stderr, "[hitch] frame %d: %.0f ms (usual %.0f), %u draws: programs %llu (%.0f ms), disk reads %llu "
                        "(%.0f KB, %.0f ms), texture uploads %llu (%.0f KB), waited for render %.0f ms\n",
                d->frame, (double)dt / 1e6, (double)usual / 1e6, d->draws, cur.links - prev.links,
                (double)(cur.link_ns - prev.link_ns) / 1e6, cur.reads - prev.reads,
                (double)(cur.read_bytes - prev.read_bytes) / 1024, (double)(cur.read_ns - prev.read_ns) / 1e6,
                cur.uploads - prev.uploads, (double)(cur.upload_bytes - prev.upload_bytes) / 1024,
                (double)(cur.wait_ns - prev.wait_ns) / 1e6);
    else if (dt)
        usual = usual ? (usual * 15 + dt) / 16 : dt;  // среднее без самих долгих кадров
    prev = cur;
}

// Ролик, кадр которого игра не вывела в задний буфер (больше кадра: 800x600 в 1200x540 и т. п. — D3DDrv тогда
// кадр не копирует, остаются звук и чёрный экран): порт рисует текущий кадр ролика сам, растянутым на весь задний
// буфер, как write_movie (d3d8_res.c). Если игра сама выводит кадры ролика (write_movie за последние
// 250 мс) — ничего. Кадр берётся в потоке гостя (bink.c), рисуется командой в потоке рендера (копия пикселей).
typedef struct {
    d3ddev *d;
    u32 w, h;
    const u8 *px;
    void *heap;
} cmd_movie_overlay_t;

static void x_movie_overlay(void *p)
{
    const cmd_movie_overlay_t *q = p;
    d3ddev *d = q->d;
    d3d_rstate *r = &d->r;
    d3dsurf *bb = d->backbuffer;
    static GLuint tex, fb[2];
    static u32 tw, th;
    if (bb && q->px) {
        if (!fb[0])
            glGenFramebuffers(2, fb);
        glActiveTexture(GL_TEXTURE0);
        if (!tex || tw != q->w || th != q->h) {
            if (!tex)
                glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)q->w, (GLsizei)q->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            tw = q->w;
            th = q->h;
        } else
            glBindTexture(GL_TEXTURE_2D, tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)q->w, (GLsizei)q->h, GL_RGBA, GL_UNSIGNED_BYTE, q->px);
        glBindTexture(GL_TEXTURE_2D, (r->tex[0] && !r->tex[0]->cube) ? r->tex[0]->gl : 0);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fb[0]);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb[1]);
        d3d8_fbo_attach(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, bb);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        // Растягиваем ролик на весь кадр: 4:3-видео на широком экране без чёрных полей (на 16:9 — ровно 16:9).
        GLint W = (GLint)bb->w, H = (GLint)bb->h;
        glBlitFramebuffer(0, 0, (GLint)q->w, (GLint)q->h, 0, 0, W, H, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, r->fbo);
        r->gl_dirty = GLD_ALL;
    }
    free(q->heap);
}

static void movie_overlay(d3ddev *d)
{
    const u8 *px;
    u32 mw, mh;
    if (rt_ticks_ns() - g_movie_drawn_ns < 250000000ull || !d->backbuffer || !bink_movie_overlay(&px, &mw, &mh))
        return;
    static int logged;
    if (!logged++)
        fprintf(stderr, "[bink] movie %ux%u is not in the %ux%u frame: drawn by the port\n", mw, mh, d->bb_w, d->bb_h);
    cmd_movie_overlay_t cmd = {d, mw, mh, px, NULL};
    if (!cs_enabled())
        x_movie_overlay(&cmd);
    else
        cs_cmd_push_var(x_movie_overlay, &cmd, sizeof cmd, px, (size_t)mw * mh * 4u, offsetof(cmd_movie_overlay_t, px),
                        offsetof(cmd_movie_overlay_t, heap));
    g_movie_overlay_ns = rt_ticks_ns();
}

static u32 g_last_frame_draws;  // сохраняем перед сбросом в Present: хост читает значение в input_tick

void d3d8_present(d3ddev *d)
{
    game_menu_frame(d->bb_w, d->bb_h);  // состояние меню игры — для ввода хоста и 4:3-режима
    game_rumble_frame();                // вибрация по урону игрока
    sync_menu_aspect_flag(d);
    script_fix_scan(1);                 // запасной поиск неисправленного байткода (runtime/script_fix.c)
    movie_overlay(d);
    int w, h;
    plat_drawable_size(&w, &h);
    float fdx = 0, fdy = 0, fdw = (float)w, fdh = (float)h;
    if (w > 0 && h > 0 && d->bb_w && d->bb_h)
        present_rect(d, w, h, &fdx, &fdy, &fdw, &fdh);
    present_pass_args pa;
    memset(&pa, 0, sizeof pa);
    pa.bb_tex = d->backbuffer ? d->backbuffer->color_tex : 0;
    pa.win_w = w;
    pa.win_h = h;
    pa.dx = (int)(fdx + 0.5f);
    pa.dy = (int)(fdy + 0.5f);
    pa.dw = (int)(fdw + 0.5f);
    pa.dh = (int)(fdh + 0.5f);
    if (d->gamma_dirty || d->frame == 0) {
        pa.update_gamma = 1;
        for (int i = 0; i < 256; i++) {
            pa.gamma_rgb[i][0] = d->gamma[0][i];
            pa.gamma_rgb[i][1] = d->gamma[1][i];
            pa.gamma_rgb[i][2] = d->gamma[2][i];
        }
        d->gamma_dirty = 0;
    }
    cs_async(x_present_pass, &pa, sizeof pa);
    // ------------------------------------------------------------------ диагностика картинки в лог
    // Раз в DIAG_LOG_EVERY кадров (только в игре) — строка «[diag] pic …»: доминирующий цвет двух полос кадра до
    // гаммы, цвет заливки, цвет тумана и состояние первых отрисовок кадра (небо в игре рисуется первой). Ничего
    // не меняет в отрисовке — можно просто играть и прислать лог.
    static int diag_log_on = -1;
    if (diag_log_on < 0) {
        const char *e = getenv("SNX_DIAG_AUTO");
        diag_log_on = !(e && *e == '0');
        if (diag_log_on)
            diag_sweep_note();
    }
    int diag_meas = diag_log_on && w > 0 && h > 0 && d->draws >= 8;
    int want_pic = diag_meas && d->frame % DIAG_LOG_EVERY == DIAG_LOG_EVERY - 1;
    // Проверка неба по кнопке («Проверка неба» в шестерёнке): один-два круга отладочных вариантов на месте,
    // где небо видно. Крутится, пока g_diag_cycles > 0, потом картинка обычная.
    int sweep_run = diag_meas && atomic_load(&g_diag_cycles) > 0 && d->frame % DIAG_ROT == DIAG_ROT - 1;
    if (want_pic || sweep_run) {
        diag_strip_args s1 = {d, 0.86f, 0, 0, 0, 0}, s2 = {d, 0.68f, 0, 0, 0, 0};
        cs_sync(x_diag_strip, &s1);
        cs_sync(x_diag_strip, &s2);
        int v = (int)g_diag_variant;
        fprintf(stderr, "[diag] %s v=%d top=%02X%02X%02X %d%% mid=%02X%02X%02X %d%% clear=%08X fog=%08X frame=%d draws=%u\n",
                sweep_run ? "sweep" : "pic", v, (s1.rgb >> 16) & 0xFF, (s1.rgb >> 8) & 0xFF, s1.rgb & 0xFF, s1.pct,
                (s2.rgb >> 16) & 0xFF, (s2.rgb >> 8) & 0xFF, s2.rgb & 0xFF, s2.pct,
                atomic_load_explicit(&g_diag_clear, memory_order_relaxed), d->r.rs[34], d->frame, d->draws);
        // состояние первых отрисовок кадра: по нему видно, чем нарисовано небо
        int nd = atomic_load_explicit(&g_diag_n, memory_order_relaxed);
        char buf[768];
        size_t off = 0;
        for (int i = 0; i < nd && off < sizeof buf - 200; i++) {
            const diag_rec *q = &g_diag_rec[i];
            off += (size_t)snprintf(buf + off, sizeof buf - off,
                                    "\n[diag] d%d{st%u tex%u:%ux%u/f%u tci%X gen%u/%u ttf%u/%u tf=%08X "
                                    "op%u/%u/%u fm%u ab%u lit%u}",
                                    i, (unsigned)q->nstages, q->tex & 3u, q->tw[0], q->th[0], q->tfm[0],
                                    (unsigned)q->tci, q->gen & 3u, (q->gen >> 2) & 3u, q->ttf & 3u, (q->ttf >> 2) & 3u,
                                    q->tf, q->cop, q->ca1, q->ca2, q->flags & 3u, (q->flags >> 3) & 1u,
                                    (q->flags >> 4) & 1u);
        }
        fprintf(stderr, "%s\n", buf);
    }
    if (sweep_run) {
        int v = (int)g_diag_variant, nv = v + 1;
        if (nv >= DIAG_VARIANTS) {
            nv = 0;
            int left = atomic_load(&g_diag_cycles) - 1;
            atomic_store(&g_diag_cycles, left);
            if (left <= 0)
                fprintf(stderr, "[diag] sweep done\n");
        }
        atomic_store(&g_diag_variant, nv);
        cs_cmd_push(x_diag_invalidate, &d, sizeof d);  // команды идут по порядку: следующий кадр соберёт ключи заново
    }
    // Диагностика кадра («Настройки» → «Диагностика кадра»): две полосы заднего буфера читаем на Core 2 до гаммы
    // Present, текст показываем в слое порта ниже — по скриншоту видно, откуда цвет: Clear, туман, TextureFactor
    // или стадии текстуры (d0 — первая отрисовка кадра, обычно небо).
    int diag_on = d3d8_diag() && w > 0 && h > 0;
    diag_strip_args dsa = {d, 0.86f, 0, 0, 0, 0}, dsb = {d, 0.68f, 0, 0, 0, 0};
    if (diag_on) {
        cs_sync(x_diag_strip, &dsa);
        cs_sync(x_diag_strip, &dsb);
    }
    // Слой интерфейса порта (значки Switch, легенда меню, Quick Menu) — прямо в окно поверх пост-прохода:
    // квадратные пиксели 1:1 даже в режиме 4:3 и без искажения гаммой игры
    if (w > 0 && h > 0) {
        ov_begin();
        game_menu_draw_overlay(w, h, fdx, fdy, fdw, fdh);
        if (diag_on)
            diag_text(w, h, &dsa, &dsb);
        ov_flush(0, w, h);
    }
    // счётчик на экране: кадров за последние полсекунды
    static u64 ov_t0;
    static int ov_frame0, ov_fps;
    u64 ov_now = rt_ticks_ns();
    if (!ov_t0 || ov_now - ov_t0 >= 500000000ull) {
        if (ov_t0)
            ov_fps = (int)((double)(d->frame - ov_frame0) * 1e9 / (double)(ov_now - ov_t0) + 0.5);
        ov_t0 = ov_now;
        ov_frame0 = d->frame;
    }
    if (g_fps_overlay) {
        draw_fps_args fa = {ov_fps, w, h};
        cs_cmd_push(x_draw_fps, &fa, sizeof fa);
    }
    maybe_shot(d, w, h);
    hitch_check(d);
    if (hle_frame_hook)
        hle_frame_hook(d->frame);
    if (d->frame < 5 || d->frame % 600 == 0)
        fprintf(stderr, "[d3d8] frame %d: %u draws, %u programs\n", d->frame, d->draws,
                atomic_load_explicit(&g_nprogs, memory_order_relaxed));
    // частота кадров раз в 5 с (на Switch — в терминал nxlink)
    static u64 fps_t0;
    static int fps_frame0;
    u64 now = rt_ticks_ns();
    if (!fps_t0 || now - fps_t0 >= 5000000000ull) {
        cs_stats st;
        cs_stats_take(&st);
        if (fps_t0) {
            double dt = (double)(now - fps_t0), frames = d->frame - fps_frame0 ? d->frame - fps_frame0 : 1;
            fprintf(stderr, "[d3d8] %.1f fps (%u draws in the last frame)\n", frames * 1e9 / dt, d->draws);
#ifdef __APPLE__
            // SNX_PERF=1: инструкции и такты процесса на кадр (на маке без вентилятора частота плавает — для
            // сравнения сборок это надёжнее кадров в секунду)
            static int perf = -1;
            static u64 ins0, cyc0;
            if (perf < 0)
                perf = getenv("SNX_PERF") != NULL;
            struct rusage_info_v4 ri;
            if (perf && proc_pid_rusage(getpid(), RUSAGE_INFO_V4, (rusage_info_t *)&ri) == 0) {
                if (ins0)
                    fprintf(stderr, "[perf] %.2f M instructions, %.2f M cycles per frame\n",
                            (double)(ri.ri_instructions - ins0) / frames / 1e6, (double)(ri.ri_cycles - cyc0) / frames / 1e6);
                ins0 = ri.ri_instructions;
                cyc0 = ri.ri_cycles;
                // сборка с SNX_TRACE: входов в функции гостя на кадр (rt_trace_pos считает ENTER)
                extern u32 rt_trace_pos;
                static u32 ent0;
                if (rt_trace_pos != ent0 && ent0)
                    fprintf(stderr, "[perf] %.0f K guest function entries per frame\n",
                            (double)(u32)(rt_trace_pos - ent0) / frames / 1e3);
                ent0 = rt_trace_pos;
            }
#endif
            // поток рендера: команд/КБ/ожиданий cs_sync на кадр; доля времени: гость ждёт, рендер занят, из них показ
            if (cs_enabled())
                fprintf(stderr, "[cs] %.0f cmds %.0f KB %.1f syncs per frame; guest waits %.0f%%, render busy %.0f%% "
                                "(swap %.0f%%)\n",
                        (double)st.recs / frames, (double)st.bytes / 1024 / frames, (double)st.syncs / frames,
                        100 * (double)st.wait_ns / dt, 100 * (double)st.busy_ns / dt, 100 * (double)st.swap_ns / dt);
        }
        fps_t0 = now;
        fps_frame0 = d->frame;
    }
    g_last_frame_draws = d->draws;
    d->draws = 0;
    cs_swap();
    frame_pace();
    cs_cmd_push(x_post_present, &d, sizeof d);
    d->frame++;
}


int d3d8_frame_draws(void)
{
    return (int)g_last_frame_draws;
}

int d3d8_window_to_target(float u, float v, int *x, int *y)
{
    d3ddev *d = d3d8_dev();
    int w, h;
    plat_drawable_size(&w, &h);
    if (!d || !w || !h || !d->bb_w || !d->bb_h)
        return 0;
    float dx, dy, dw, dh;
    present_rect(d, w, h, &dx, &dy, &dw, &dh);
    *x = (int)floorf((u * (float)w - dx) / dw * (float)d->bb_w);
    *y = (int)floorf((v * (float)h - dy) / dh * (float)d->bb_h);
    return 1;
}

int d3d8_target_to_window(float x, float y, float *u, float *v)
{
    d3ddev *d = d3d8_dev();
    int w, h;
    plat_drawable_size(&w, &h);
    if (!d || !w || !h || !d->bb_w || !d->bb_h)
        return 0;
    float dx, dy, dw, dh;
    present_rect(d, w, h, &dx, &dy, &dw, &dh);
    *u = (x / (float)d->bb_w * dw + dx) / (float)w;
    *v = (y / (float)d->bb_h * dh + dy) / (float)h;
    return 1;
}
