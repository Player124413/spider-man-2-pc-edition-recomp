// HLE Direct3D 8: IDirect3DDevice8 — состояние (render/texture-stage states, матрицы, свет, материал,
// viewport), создание ресурсов, вершинные объявления и FVF, вызовы отрисовки (d3d8_ff.c).
// Методы — stdcall с this в ARG(0): RET(n) считает и this.
#include <math.h>
#include <stdlib.h>

#include "platform/window.h"
#include "runtime/hle/d3d8.h"
#include "runtime/hle/d3d8_direct.h"
#include "runtime/hle/d3d8_int.h"
#include "runtime/hle/user32.h"

_Static_assert(sizeof(d3dlight) == 104, "D3DLIGHT8 must be 104 bytes");

static d3ddev *g_dev;
static u32 g_vt_dev;

d3ddev *d3d8_dev(void) { return g_dev; }

static float rdf(u32 a)
{
    float f;
    memcpy(&f, g_mem + a, 4);
    return f;
}

static float u2f(u32 v)
{
    float f;
    memcpy(&f, &v, 4);
    return f;
}

static u32 f2u(float f)
{
    u32 v;
    memcpy(&v, &f, 4);
    return v;
}

static void identity(float *m)
{
    memset(m, 0, 64);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

// ================================================================ состояние по умолчанию (как после CreateDevice/Reset)
enum {
    RS_ZENABLE = 7, RS_FILLMODE = 8, RS_SHADEMODE = 9, RS_ZWRITEENABLE = 14, RS_LASTPIXEL = 16, RS_SRCBLEND = 19,
    RS_DESTBLEND = 20, RS_CULLMODE = 22, RS_ZFUNC = 23, RS_ALPHAFUNC = 25, RS_FOGEND = 37, RS_FOGDENSITY = 38,
    RS_STENCILFAIL = 53, RS_STENCILZFAIL = 54, RS_STENCILPASS = 55, RS_STENCILFUNC = 56, RS_STENCILMASK = 58,
    RS_STENCILWRITEMASK = 59, RS_TEXTUREFACTOR = 60, RS_CLIPPING = 136, RS_LIGHTING = 137, RS_COLORVERTEX = 141,
    RS_LOCALVIEWER = 142, RS_DIFFUSEMATERIALSOURCE = 145, RS_SPECULARMATERIALSOURCE = 146, RS_POINTSIZE = 154,
    RS_POINTSIZE_MIN = 155, RS_MULTISAMPLEANTIALIAS = 161, RS_MULTISAMPLEMASK = 162, RS_PATCHSEGMENTS = 164,
    RS_POINTSIZE_MAX = 166, RS_COLORWRITEENABLE = 168, RS_BLENDOP = 171, RS_POSITIONORDER = 172,
    RS_NORMALORDER = 173,
};

static void set_defaults(d3ddev *d)
{
    memset(d->rs, 0, sizeof d->rs);
    d->rs[RS_ZENABLE] = d->autodepth ? 1 : 0;
    d->rs[RS_FILLMODE] = 3;
    d->rs[RS_SHADEMODE] = 2;
    d->rs[RS_ZWRITEENABLE] = 1;
    d->rs[RS_LASTPIXEL] = 1;
    d->rs[RS_SRCBLEND] = 2;
    d->rs[RS_DESTBLEND] = 1;
    d->rs[RS_CULLMODE] = 3;
    d->rs[RS_ZFUNC] = 4;
    d->rs[RS_ALPHAFUNC] = 8;
    d->rs[RS_FOGEND] = f2u(1.0f);
    d->rs[RS_FOGDENSITY] = f2u(1.0f);
    d->rs[RS_STENCILFAIL] = d->rs[RS_STENCILZFAIL] = d->rs[RS_STENCILPASS] = 1;
    d->rs[RS_STENCILFUNC] = 8;
    d->rs[RS_STENCILMASK] = d->rs[RS_STENCILWRITEMASK] = 0xFFFFFFFFu;
    d->rs[RS_TEXTUREFACTOR] = 0xFFFFFFFFu;
    d->rs[RS_CLIPPING] = 1;
    d->rs[RS_LIGHTING] = 1;
    d->rs[RS_COLORVERTEX] = 1;
    d->rs[RS_LOCALVIEWER] = 1;
    d->rs[RS_DIFFUSEMATERIALSOURCE] = 1;
    d->rs[RS_SPECULARMATERIALSOURCE] = 2;
    d->rs[RS_POINTSIZE] = f2u(1.0f);
    d->rs[RS_POINTSIZE_MIN] = f2u(1.0f);
    d->rs[RS_POINTSIZE_MAX] = f2u(64.0f);
    d->rs[RS_MULTISAMPLEANTIALIAS] = 1;
    d->rs[RS_MULTISAMPLEMASK] = 0xFFFFFFFFu;
    d->rs[RS_PATCHSEGMENTS] = f2u(1.0f);
    d->rs[RS_COLORWRITEENABLE] = 0xF;
    d->rs[RS_BLENDOP] = 1;
    d->rs[RS_POSITIONORDER] = 3;  // D3DORDER_CUBIC
    d->rs[RS_NORMALORDER] = 1;    // D3DORDER_LINEAR
    for (int s = 0; s < MAX_STAGES; s++) {
        u32 *t = d->tss[s];
        memset(t, 0, 32 * 4);
        t[1] = s == 0 ? 4 : 1;   // COLOROP: MODULATE / DISABLE
        t[2] = 2;                // COLORARG1: TEXTURE
        t[3] = 1;                // COLORARG2: CURRENT
        t[4] = s == 0 ? 2 : 1;   // ALPHAOP: SELECTARG1 / DISABLE
        t[5] = 2;
        t[6] = 1;
        t[11] = (u32)s;          // TEXCOORDINDEX
        t[13] = t[14] = t[25] = 1;  // ADDRESSU/V/W: WRAP
        t[16] = t[17] = 1;       // MAG/MINFILTER: POINT
        t[21] = 1;               // MAXANISOTROPY
        t[26] = t[27] = t[28] = 1;  // COLORARG0, ALPHAARG0, RESULTARG: CURRENT
    }
    identity(d->view);
    identity(d->proj);
    identity(d->world);
    for (int s = 0; s < MAX_STAGES; s++)
        identity(d->texm[s]);
    memset(d->material, 0, sizeof d->material);
    memset(d->light_on, 0, sizeof d->light_on);
    d->vp[0] = d->vp[1] = 0;
    d->vp[2] = d->bb_w;
    d->vp[3] = d->bb_h;
    d->vpz[0] = 0.0f;
    d->vpz[1] = 1.0f;
    d3d8_cmd_reset_defaults(d, d->autodepth != NULL, d->bb_w, d->bb_h);
}

// ================================================================ гранулярное изменение состояния и StateBlocks
// Вспомогательные функции изменения состояния устройства (общие для Set* и ApplyStateBlock):
// обновляют теневое состояние Core 1, отбрасывают дубликаты и ставят компактную D3D8-команду в gl_cs.
static void apply_rs(d3ddev *d, u32 state, u32 val)
{
    if (state >= 256 || d->rs[state] == val)
        return;
    d->rs[state] = val;
    d3d8_cmd_rs(d, state, val);
}

static void apply_tss(d3ddev *d, u32 s, u32 t, u32 val)
{
    if (s >= MAX_STAGES || t >= 32 || d->tss[s][t] == val)
        return;
    d->tss[s][t] = val;
    d3d8_cmd_tss(d, s, t, val);
}

static void apply_tex(d3ddev *d, u32 s, d3dtex *t)
{
    if (s >= MAX_STAGES || t == d->tex[s])
        return;
    if (t)
        com_addref(t->obj);
    if (d->tex[s])
        com_release(d->tex[s]->obj);
    d->tex[s] = t;
    d3d8_cmd_tex(d, s, t);
}

static void apply_stream(d3ddev *d, u32 n, d3dbuf *b, u32 stride)
{
    if (n >= MAX_STREAMS)
        return;
    if (b == d->stream[n] && stride == d->stride[n])
        return;
    if (b != d->stream[n]) {
        if (b)
            com_addref(b->obj);
        if (d->stream[n])
            com_release(d->stream[n]->obj);
        d->stream[n] = b;
    }
    d->stride[n] = stride;
    d3d8_cmd_stream(d, n, b, stride);
}

static void apply_indices(d3ddev *d, d3dbuf *b, u32 base_vertex)
{
    if (b == d->indices && base_vertex == d->base_vertex)
        return;
    if (b != d->indices) {
        if (b)
            com_addref(b->obj);
        if (d->indices)
            com_release(d->indices->obj);
        d->indices = b;
    }
    d->base_vertex = base_vertex;
    d3d8_cmd_indices(d, b, base_vertex);
}

static void apply_xf(d3ddev *d, u32 state, const float *mat)
{
    float *slot = NULL;
    if (state == 2)
        slot = d->view;
    else if (state == 3)
        slot = d->proj;
    else if (state == 256)
        slot = d->world;
    else if (state >= 16 && state < 16 + MAX_STAGES)
        slot = d->texm[state - 16];
    if (!slot || memcmp(slot, mat, 64) == 0)
        return;
    memcpy(slot, mat, 64);
    d3d8_cmd_xf(d, state, mat);
}

static void apply_vp(d3ddev *d, const u32 vp[4], const float vpz[2])
{
    if (!memcmp(d->vp, vp, sizeof d->vp) && !memcmp(d->vpz, vpz, sizeof d->vpz))
        return;
    memcpy(d->vp, vp, sizeof d->vp);
    memcpy(d->vpz, vpz, sizeof d->vpz);
    d3d8_cmd_vp(d, vp, vpz);
}

static void apply_mat(d3ddev *d, const float mat[17])
{
    if (!memcmp(d->material, mat, sizeof d->material))
        return;
    memcpy(d->material, mat, sizeof d->material);
    d3d8_cmd_mat(d, mat);
}

static void apply_light(d3ddev *d, u32 i, const d3dlight *l)
{
    if (i >= MAX_LIGHTS)
        return;
    if (d->light_set[i] && !memcmp(&d->lights[i], l, sizeof *l))
        return;
    d->lights[i] = *l;
    d->light_set[i] = 1;
    d3d8_cmd_light(d, i, l);
}

static void apply_light_enable(d3ddev *d, u32 i, u8 on)
{
    if (i >= MAX_LIGHTS)
        return;
    if (!d->light_set[i]) {  // как D3D: включение незаданного источника — белый направленный (0,0,1)
        d3dlight def = {0};
        def.type = 3;
        def.diffuse[0] = def.diffuse[1] = def.diffuse[2] = 1.0f;
        def.dir[2] = 1.0f;
        apply_light(d, i, &def);
    } else if (d->light_on[i] == on) {
        return;
    }
    d->light_on[i] = on;
    d3d8_cmd_light_enable(d, i, on);
}

static void apply_clip(d3ddev *d, u32 i, const float plane[4])
{
    if (i >= 6 || !memcmp(d->clip[i], plane, 16))
        return;
    memcpy(d->clip[i], plane, 16);
    d3d8_cmd_clip(d, i, plane);
}

static void apply_vs(d3ddev *d, u32 vs)
{
    if (d->vs == vs)
        return;
    d->vs = vs;
    d3d8_cmd_vs(d, vs);
}

static void apply_ps(d3ddev *d, u32 ps)
{
    if (d->ps == ps)
        return;
    d->ps = ps;
    d3d8_cmd_ps(d, ps);
}

static void apply_psc(d3ddev *d, u32 reg, u32 cnt, const void *src)
{
    if (reg >= 8 || !cnt)
        return;
    if (reg + cnt > 8)
        cnt = 8 - reg;
    if (!memcmp(d->psc[reg], src, (size_t)cnt * 16u))
        return;
    memcpy(d->psc[reg], src, (size_t)cnt * 16u);
    d3d8_cmd_psc(d, reg, cnt, (const float (*)[4])d->psc[reg]);
}

struct d3d_sb {
    u32 rs_mask[8];
    u32 rs[256];
    u32 tss_mask[MAX_STAGES];
    u32 tss[MAX_STAGES][32];
    u8 tex_mask;
    d3dtex *tex[MAX_STAGES];      // удерживает ссылку COM
    u8 xf_view, xf_proj, xf_world, xf_texm_mask;
    float view[16], proj[16], world[16], texm[MAX_STAGES][16];
    u8 has_vp, has_mat, has_vs, has_ps, has_indices;
    u32 vp[4];
    float vpz[2];
    float material[17];
    u8 light_mask, light_en_mask;
    d3dlight lights[MAX_LIGHTS];
    u8 light_on[MAX_LIGHTS], light_set[MAX_LIGHTS];
    u8 clip_mask;
    float clip[6][4];
    u16 stream_mask;
    d3dbuf *stream[MAX_STREAMS];  // удерживает ссылку COM
    u32 stride[MAX_STREAMS];
    d3dbuf *indices;              // удерживает ссылку COM
    u32 base_vertex;
    u32 vs, ps;
    u32 vsc_mask[3];
    float vsc[96][4];
    u8 psc_mask;
    float psc[8][4];
};

static inline void sb_set_rs_bit(d3d_sb *s, u32 st)
{
    if (st < 256)
        s->rs_mask[st >> 5] |= 1u << (st & 31);
}

static inline int sb_has_rs(const d3d_sb *s, u32 st) { return (s->rs_mask[st >> 5] & (1u << (st & 31))) != 0; }

static void sb_set_tex(d3d_sb *s, u32 stage, d3dtex *t)
{
    if (stage >= MAX_STAGES)
        return;
    if (t)
        com_addref(t->obj);
    if (s->tex[stage])
        com_release(s->tex[stage]->obj);
    s->tex[stage] = t;
    s->tex_mask |= (u8)(1u << stage);
}

static void sb_set_stream(d3d_sb *s, u32 n, d3dbuf *b, u32 stride)
{
    if (n >= MAX_STREAMS)
        return;
    if (b)
        com_addref(b->obj);
    if (s->stream[n])
        com_release(s->stream[n]->obj);
    s->stream[n] = b;
    s->stride[n] = stride;
    s->stream_mask |= (u16)(1u << n);
}

static void sb_set_indices(d3d_sb *s, d3dbuf *b, u32 base_vertex)
{
    if (b)
        com_addref(b->obj);
    if (s->indices)
        com_release(s->indices->obj);
    s->indices = b;
    s->base_vertex = base_vertex;
    s->has_indices = 1;
}

static void sb_free(d3d_sb *s)
{
    if (!s)
        return;
    for (int i = 0; i < MAX_STAGES; i++)
        if (s->tex[i])
            com_release(s->tex[i]->obj);
    for (int i = 0; i < MAX_STREAMS; i++)
        if (s->stream[i])
            com_release(s->stream[i]->obj);
    if (s->indices)
        com_release(s->indices->obj);
    free(s);
}

static void sb_destroy_all(d3ddev *d)
{
    sb_free(d->rec_sb);
    d->rec_sb = NULL;
    for (u32 i = 0; i < d->sb_cap; i++) {
        sb_free(d->sb[i]);
        d->sb[i] = NULL;
    }
    free(d->sb);
    d->sb = NULL;
    d->sb_cap = 0;
}

static void sb_capture(d3ddev *d, d3d_sb *s)
{
    for (u32 st = 0; st < 256; st++)
        if (sb_has_rs(s, st))
            s->rs[st] = d->rs[st];
    for (int stg = 0; stg < MAX_STAGES; stg++) {
        u32 m = s->tss_mask[stg];
        for (u32 t = 0; m && t < 32; t++)
            if (m & (1u << t))
                s->tss[stg][t] = d->tss[stg][t];
        if (s->tex_mask & (1u << stg))
            sb_set_tex(s, (u32)stg, d->tex[stg]);
    }
    if (s->xf_view)
        memcpy(s->view, d->view, 64);
    if (s->xf_proj)
        memcpy(s->proj, d->proj, 64);
    if (s->xf_world)
        memcpy(s->world, d->world, 64);
    for (int stg = 0; stg < MAX_STAGES; stg++)
        if (s->xf_texm_mask & (1u << stg))
            memcpy(s->texm[stg], d->texm[stg], 64);
    if (s->has_vp) {
        memcpy(s->vp, d->vp, sizeof s->vp);
        memcpy(s->vpz, d->vpz, sizeof s->vpz);
    }
    if (s->has_mat)
        memcpy(s->material, d->material, sizeof s->material);
    for (int i = 0; i < MAX_LIGHTS; i++) {
        if (s->light_mask & (1u << i)) {
            s->lights[i] = d->lights[i];
            s->light_set[i] = d->light_set[i];
        }
        if (s->light_en_mask & (1u << i))
            s->light_on[i] = d->light_on[i];
    }
    for (int i = 0; i < 6; i++)
        if (s->clip_mask & (1u << i))
            memcpy(s->clip[i], d->clip[i], 16);
    for (u32 n = 0; n < MAX_STREAMS; n++)
        if (s->stream_mask & (1u << n))
            sb_set_stream(s, n, d->stream[n], d->stride[n]);
    if (s->has_indices)
        sb_set_indices(s, d->indices, d->base_vertex);
    if (s->has_vs)
        s->vs = d->vs;
    if (s->has_ps)
        s->ps = d->ps;
    for (u32 i = 0; i < 96; i++)
        if (s->vsc_mask[i >> 5] & (1u << (i & 31)))
            memcpy(s->vsc[i], d->vsc[i], 16);
    for (u32 i = 0; i < 8; i++)
        if (s->psc_mask & (1u << i))
            memcpy(s->psc[i], d->psc[i], 16);
}

static void sb_apply(d3ddev *d, const d3d_sb *s)
{
    for (u32 st = 0; st < 256; st++)
        if (sb_has_rs(s, st))
            apply_rs(d, st, s->rs[st]);
    for (int stg = 0; stg < MAX_STAGES; stg++) {
        u32 m = s->tss_mask[stg];
        for (u32 t = 0; m && t < 32; t++)
            if (m & (1u << t))
                apply_tss(d, (u32)stg, t, s->tss[stg][t]);
        if (s->tex_mask & (1u << stg))
            apply_tex(d, (u32)stg, s->tex[stg]);
    }
    if (s->xf_view)
        apply_xf(d, 2, s->view);
    if (s->xf_proj)
        apply_xf(d, 3, s->proj);
    if (s->xf_world)
        apply_xf(d, 256, s->world);
    for (int stg = 0; stg < MAX_STAGES; stg++)
        if (s->xf_texm_mask & (1u << stg))
            apply_xf(d, 16u + (u32)stg, s->texm[stg]);
    if (s->has_vp)
        apply_vp(d, s->vp, s->vpz);
    if (s->has_mat)
        apply_mat(d, s->material);
    for (u32 i = 0; i < MAX_LIGHTS; i++) {
        if ((s->light_mask & (1u << i)) && s->light_set[i])
            apply_light(d, i, &s->lights[i]);
        if (s->light_en_mask & (1u << i))
            apply_light_enable(d, i, s->light_on[i]);
    }
    if (s->clip_mask) {
        for (u32 i = 0; i < 6; i++)
            if (s->clip_mask & (1u << i))
                apply_clip(d, i, s->clip[i]);
    }
    for (u32 n = 0; n < MAX_STREAMS; n++)
        if (s->stream_mask & (1u << n))
            apply_stream(d, n, s->stream[n], s->stride[n]);
    if (s->has_indices)
        apply_indices(d, s->indices, s->base_vertex);
    if (s->has_vs)
        apply_vs(d, s->vs);
    if (s->has_ps)
        apply_ps(d, s->ps);
    for (u32 i = 0; i < 96; i++)
        if (s->vsc_mask[i >> 5] & (1u << (i & 31)))
            memcpy(d->vsc[i], s->vsc[i], 16);
    if (s->psc_mask) {
        for (u32 i = 0; i < 8; i++)
            if (s->psc_mask & (1u << i))
                apply_psc(d, i, 1, s->psc[i]);
    }
}

static u32 sb_store(d3ddev *d, d3d_sb *s)
{
    for (u32 i = 0; i < d->sb_cap; i++)
        if (!d->sb[i]) {
            d->sb[i] = s;
            return i + 1u;
        }
    u32 ncap = d->sb_cap ? d->sb_cap * 2u : 64u;
    d3d_sb **nb = realloc(d->sb, (size_t)ncap * sizeof *nb);
    if (!nb)
        return 0;
    memset(nb + d->sb_cap, 0, (size_t)(ncap - d->sb_cap) * sizeof *nb);
    u32 idx = d->sb_cap;
    d->sb = nb;
    d->sb_cap = ncap;
    d->sb[idx] = s;
    return idx + 1u;
}

static d3d_sb *sb_get(d3ddev *d, u32 tok)
{
    return tok && tok <= d->sb_cap ? d->sb[tok - 1u] : NULL;
}

static void sb_init_type(d3d_sb *s, u32 type)
{
    static const u8 pixel_rs[] = {
        7, 8, 9, 10, 14, 15, 16, 19, 20, 23, 24, 25, 26, 27, 28, 29, 30, 34, 35, 36, 37, 38, 40, 47, 52, 53, 54,
        55, 56, 57, 58, 59, 60, 128, 129, 130, 131, 132, 133, 134, 135, 168, 171,
    };
    static const u8 vertex_rs[] = {
        9, 22, 28, 29, 34, 36, 37, 38, 48, 136, 137, 139, 140, 141, 142, 143, 145, 146, 147, 148, 151, 152, 153,
        154, 155, 156, 157, 158, 159, 160, 161, 162, 163, 164, 166, 167, 170, 172, 173,
    };
    if (type == 1) {  // D3DSBT_ALL
        memset(s->rs_mask, 0xFF, sizeof s->rs_mask);
        for (int i = 0; i < MAX_STAGES; i++)
            s->tss_mask[i] = 0xFFFFFFFFu;
        s->tex_mask = 0xFF;
        s->xf_view = s->xf_proj = s->xf_world = 1;
        s->xf_texm_mask = 0xFF;
        s->has_vp = s->has_mat = s->has_vs = s->has_ps = s->has_indices = 1;
        s->light_mask = s->light_en_mask = 0xFF;
        s->clip_mask = 0x3F;
        s->stream_mask = 0xFFFF;
        s->vsc_mask[0] = s->vsc_mask[1] = s->vsc_mask[2] = 0xFFFFFFFFu;
        s->psc_mask = 0xFF;
    } else if (type == 2) {  // D3DSBT_PIXELSTATE
        for (size_t i = 0; i < sizeof pixel_rs; i++)
            sb_set_rs_bit(s, pixel_rs[i]);
        // Все TSS, кроме TEXCOORDINDEX (11) и TEXTURETRANSFORMFLAGS (24)
        u32 ptss = 0xFFFFFFFFu & ~(1u << 11) & ~(1u << 24);
        for (int i = 0; i < MAX_STAGES; i++)
            s->tss_mask[i] = ptss;
        s->tex_mask = 0xFF;
        s->has_ps = 1;
        s->psc_mask = 0xFF;
    } else if (type == 3) {  // D3DSBT_VERTEXSTATE
        for (size_t i = 0; i < sizeof vertex_rs; i++)
            sb_set_rs_bit(s, vertex_rs[i]);
        u32 vtss = (1u << 11) | (1u << 24);
        for (int i = 0; i < MAX_STAGES; i++)
            s->tss_mask[i] = vtss;
        s->xf_view = s->xf_proj = s->xf_world = 1;
        s->xf_texm_mask = 0xFF;
        s->has_vp = s->has_mat = s->has_vs = s->has_indices = 1;
        s->light_mask = s->light_en_mask = 0xFF;
        s->clip_mask = 0x3F;
        s->stream_mask = 0xFFFF;
        s->vsc_mask[0] = s->vsc_mask[1] = s->vsc_mask[2] = 0xFFFFFFFFu;
    }
}

// ================================================================ вершинные объявления
enum { DT_FLOAT1, DT_FLOAT2, DT_FLOAT3, DT_FLOAT4, DT_D3DCOLOR, DT_UBYTE4, DT_SHORT2, DT_SHORT4 };
static const u8 g_dt_size[8] = {4, 8, 12, 16, 4, 4, 4, 8};

#define MAX_VS D3D8_MAX_VS
typedef struct {
    int used;
    vdecl decl;
    u32 *tokens;
    u32 ntok;
} vshader;
static vshader g_vs[MAX_VS];
#define VS_HANDLE_BASE D3D8_VS_HANDLE_BASE

static void add_elem(vdecl *v, u32 stream, u32 reg, u32 type, u32 offset)
{
    if (v->n < 16)
        v->e[v->n++] = (velem){(u8)stream, (u8)reg, (u8)type, (u8)offset};
}

void d3d8_decl_from_fvf(u32 fvf, vdecl *v)
{
    memset(v, 0, sizeof *v);
    u32 off = 0, pos = fvf & 0xE;
    if (pos == 0x4) {  // XYZRHW
        add_elem(v, 0, 0, DT_FLOAT4, off);
        off += 16;
        v->rhw = 1;
    } else if (pos) {
        add_elem(v, 0, 0, DT_FLOAT3, off);
        off += 12;
        u32 betas = pos >= 6 ? (pos - 4) / 2 : 0;
        if (betas) {
            int ub4 = (fvf & 0x1000) != 0;  // D3DFVF_LASTBETA_UBYTE4
            u32 w = ub4 ? betas - 1 : betas;
            if (w)
                add_elem(v, 0, 1, w > 4 ? DT_FLOAT4 : DT_FLOAT1 + (w > 4 ? 3 : w - 1), off);
            off += 4 * w;
            if (ub4) {
                add_elem(v, 0, 2, DT_UBYTE4, off);
                off += 4;
            }
        }
    }
    if (fvf & 0x10) {
        add_elem(v, 0, 3, DT_FLOAT3, off);
        off += 12;
    }
    if (fvf & 0x20) {
        add_elem(v, 0, 4, DT_FLOAT1, off);
        off += 4;
    }
    if (fvf & 0x40) {
        add_elem(v, 0, 5, DT_D3DCOLOR, off);
        off += 4;
    }
    if (fvf & 0x80) {
        add_elem(v, 0, 6, DT_D3DCOLOR, off);
        off += 4;
    }
    u32 ntex = (fvf >> 8) & 0xF;
    for (u32 i = 0; i < ntex && i < 8; i++) {
        static const u8 code_type[4] = {DT_FLOAT2, DT_FLOAT3, DT_FLOAT4, DT_FLOAT1};
        u32 t = code_type[(fvf >> (16 + 2 * i)) & 3];
        add_elem(v, 0, 7 + i, t, off);
        off += g_dt_size[t];
    }
}

// Объявление D3D8 (токены D3DVSD_*): STREAM, STREAMDATA (регистр/пропуск), CONSTMEM, EXT, END
static u32 parse_decl(d3ddev *d, u32 p, vdecl *v)
{
    memset(v, 0, sizeof *v);
    u32 stream = 0, off = 0, n = 0;
    for (;; n++) {
        u32 t = rd32(g_mem, p + 4 * n);
        if (t == 0xFFFFFFFFu || n > 256)
            break;
        switch (t >> 29) {
        case 1:  // STREAM
            if (!(t & 0x10000000u)) {
                stream = t & 0xF;
                off = 0;
            }
            break;
        case 2:  // STREAMDATA
            if (t & 0x10000000u)
                off += ((t >> 16) & 0xF) * 4;
            else {
                u32 type = (t >> 16) & 0xF;
                add_elem(v, stream, t & 0x1F, type & 7, off);
                off += g_dt_size[type & 7];
            }
            break;
        case 4: {  // CONSTMEM: (count) * 4 dword констант с адреса
            u32 cnt = (t >> 25) & 0xF, addr = t & 0x7F;
            for (u32 i = 0; i < cnt * 4; i++)
                if (addr + i / 4 < 96)
                    d->vsc[addr + i / 4][i % 4] = rdf(p + 4 * (n + 1 + i));
            n += cnt * 4;
            break;
        }
        case 5:  // EXT
            n += (t >> 24) & 0x1F;
            break;
        default:
            break;
        }
    }
    return n + 1;
}

// ================================================================ IDirect3DDevice8
#define DEV(c) ((d3ddev *)com_host(ARG(0)))

static void dev_QueryInterface(CPU *c)
{
    if (com_iid_eq(ARG(1), 0x7385e5df, 0x8fe8, 0x41d5, 0x86b6d7b48547b6cfull)  // IID_IDirect3DDevice8
        || com_iid_eq(ARG(1), 0, 0, 0, 0xc000000000000046ull)) {
        com_addref(ARG(0));
        wr32(g_mem, ARG(2), ARG(0));
        RET(3, S_OK);
    }
    wr32(g_mem, ARG(2), 0);
    RET(3, E_NOINTERFACE);
}

static void dev_TestCooperativeLevel(CPU *c) { RET(1, D3D_OK); }
static void dev_GetAvailableTextureMem(CPU *c) { RET(1, d3d8_vidmem_total() - 16u * 1024 * 1024); }
static void dev_ResourceManagerDiscardBytes(CPU *c) { RET(2, D3D_OK); }

static u32 g_d3d8_obj;
static void dev_GetDirect3D(CPU *c)
{
    com_addref(g_d3d8_obj);
    wr32(g_mem, ARG(1), g_d3d8_obj);
    RET(2, D3D_OK);
}

static void dev_GetDeviceCaps(CPU *c)
{
    d3d8_fill_caps(ARG(1));
    RET(2, D3D_OK);
}

static void dev_GetDisplayMode(CPU *c)
{
    d3ddev *d = DEV(c);
    int w, h;
    user32_screen(&w, &h);
    u32 p = ARG(1);
    wr32(g_mem, p, d->windowed ? (u32)w : d->bb_w);
    wr32(g_mem, p + 4, d->windowed ? (u32)h : d->bb_h);
    wr32(g_mem, p + 8, 60);
    wr32(g_mem, p + 12, FMT_X8R8G8B8);
    RET(2, D3D_OK);
}

static void dev_GetCreationParameters(CPU *c)
{
    u32 p = ARG(1);
    wr32(g_mem, p, 0);
    wr32(g_mem, p + 4, 1);  // D3DDEVTYPE_HAL
    wr32(g_mem, p + 8, DEV(c)->hwnd);
    wr32(g_mem, p + 12, 0x40);  // D3DCREATE_HARDWARE_VERTEXPROCESSING
    RET(2, D3D_OK);
}

static void dev_SetCursorProperties(CPU *c) { RET(4, D3D_OK); }
static void dev_SetCursorPosition(CPU *c) { RET(4, 0); }
static void dev_ShowCursor(CPU *c) { RET(2, 0); }

// задний буфер и автоматическая глубина — renderbuffer-ы текущего размера
// Ссылки устройства: одна — на свой задний буфер/глубину (владение), по одной — на текущие цели.
static void surf_ref(d3dsurf *s, int add)
{
    if (!s)
        return;
    u32 obj = s->tex ? s->tex->obj : s->obj;
    if (add)
        com_addref(obj);
    else
        com_release(obj);
}

static void make_backbuffer(d3ddev *d)
{
    surf_ref(d->rt, 0);
    surf_ref(d->ds, 0);
    surf_ref(d->backbuffer, 0);
    surf_ref(d->autodepth, 0);
    d->rt = d->ds = d->backbuffer = d->autodepth = NULL;
    d->backbuffer = d3d8_surf_create(d->bb_w, d->bb_h, FMT_A8R8G8B8, USAGE_RENDERTARGET, POOL_DEFAULT);
    d->backbuffer->fmt = d->bb_fmt;
    d->backbuffer->backbuffer = 1;
    if (d->ds_fmt)
        d->autodepth = d3d8_surf_create(d->bb_w, d->bb_h, FMT_D24S8, USAGE_DEPTHSTENCIL, POOL_DEFAULT);
    d->rt = d->backbuffer;
    d->ds = d->autodepth;
    surf_ref(d->rt, 1);
    surf_ref(d->ds, 1);
}

// D3DPRESENT_PARAMETERS: BackBufferWidth, Height, Format, Count, MultiSampleType, SwapEffect, hDeviceWindow,
// Windowed, EnableAutoDepthStencil, AutoDepthStencilFormat, Flags, RefreshRate, PresentationInterval
static void read_pp(d3ddev *d, u32 pp)
{
    d->bb_w = rd32(g_mem, pp);
    d->bb_h = rd32(g_mem, pp + 4);
    d->bb_fmt = rd32(g_mem, pp + 8);
    if (rd32(g_mem, pp + 24))
        d->hwnd = rd32(g_mem, pp + 24);
    d->windowed = rd32(g_mem, pp + 28);
    d->ds_fmt = rd32(g_mem, pp + 32) ? rd32(g_mem, pp + 36) : 0;
    if (!d->bb_w || !d->bb_h) {
        int w, h;
        user32_client_size(d->hwnd, &w, &h);
        d->bb_w = (u32)w;
        d->bb_h = (u32)h;
        wr32(g_mem, pp, d->bb_w);
        wr32(g_mem, pp + 4, d->bb_h);
    }
    if (d->bb_fmt == FMT_UNKNOWN) {
        d->bb_fmt = FMT_X8R8G8B8;
        wr32(g_mem, pp + 8, d->bb_fmt);
    }
    fprintf(stderr, "[d3d8] back buffer %ux%u fmt %u %s, depth fmt %u\n", d->bb_w, d->bb_h, d->bb_fmt,
            d->windowed ? "windowed" : "fullscreen", d->ds_fmt);
}

static void dev_CreateAdditionalSwapChain(CPU *c)
{
    wr32(g_mem, ARG(2), 0);
    RET(3, D3DERR_NOTAVAILABLE);
}

// новые параметры показа (Reset и повторный CreateDevice): окно, задний буфер, состояние по умолчанию
static void apply_pp(d3ddev *d, u32 pp)
{
    read_pp(d, pp);
    cs_finish();  // окно меняется, пока поток рендера ничего не рисует
    plat_window_open((int)d->bb_w, (int)d->bb_h, !d->windowed, "Spider-Man 2");
    make_backbuffer(d);
    set_defaults(d);
    d3d8_cmd_set_rt(d, d->rt, d->ds, 1);
}

static void dev_Reset(CPU *c)
{
    apply_pp(DEV(c), ARG(1));
    RET(2, D3D_OK);
}

static void dev_Present(CPU *c)
{
    d3d8_present(DEV(c));
    RET(5, D3D_OK);
}

static void dev_GetBackBuffer(CPU *c)  // (BackBuffer, Type, pp)
{
    d3ddev *d = DEV(c);
    com_addref(d->backbuffer->obj);
    wr32(g_mem, ARG(3), d->backbuffer->obj);
    RET(4, D3D_OK);
}

static void dev_GetRasterStatus(CPU *c)
{
    wr32(g_mem, ARG(1), 0);
    wr32(g_mem, ARG(1) + 4, 0);
    RET(2, D3D_OK);
}

static void dev_SetGammaRamp(CPU *c)  // (Flags, pRamp) — возвращает void
{
    d3ddev *d = DEV(c);
    u32 p = ARG(2);
    if (p >= 0x1000u && p <= RT_WINDOW - 1536u) {
        u16 ramp[3][256];
        for (u32 ch = 0; ch < 3; ch++)
            for (u32 i = 0; i < 256; i++)
                ramp[ch][i] = rd16(g_mem, p + ch * 512u + i * 2u);
        if (memcmp(d->gamma, ramp, sizeof ramp) != 0) {
            memcpy(d->gamma, ramp, sizeof ramp);
            d->gamma_dirty = 1;
        }
    }
    RET(3, 0);
}

static void dev_GetGammaRamp(CPU *c)  // (pRamp) — возвращает void
{
    d3ddev *d = DEV(c);
    u32 p = ARG(1);
    if (p >= 0x1000u && p <= RT_WINDOW - 1536u) {
        for (u32 ch = 0; ch < 3; ch++)
            for (u32 i = 0; i < 256; i++)
                wr16(g_mem, p + ch * 512u + i * 2u, d->gamma[ch][i]);
    }
    RET(2, 0);
}

static u32 tex_result(d3dtex *t, u32 pp)
{
    if (!t) {
        wr32(g_mem, pp, 0);
        return D3DERR_INVALIDCALL;
    }
    wr32(g_mem, pp, t->obj);
    return D3D_OK;
}

// CreateTexture(Width, Height, Levels, Usage, Format, Pool, ppTexture)
static void dev_CreateTexture(CPU *c)
{
    d3dtex *t = d3d8_tex_create(ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), ARG(6), 0);
    if (!t)
        fprintf(stderr, "[d3d8] CreateTexture %ux%u fmt %08x failed\n", ARG(1), ARG(2), ARG(5));
    RET(8, tex_result(t, ARG(7)));
}

// CreateCubeTexture(EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture)
static void dev_CreateCubeTexture(CPU *c)
{
    d3dtex *t = d3d8_tex_create(ARG(1), ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), 1);
    RET(7, tex_result(t, ARG(6)));
}

// CreateVertexBuffer(Length, Usage, FVF, Pool, pp)
static void dev_CreateVertexBuffer(CPU *c)
{
    d3dbuf *b = d3d8_buf_create(0, ARG(1), ARG(2), ARG(3), ARG(4));
    wr32(g_mem, ARG(5), b ? b->obj : 0);
    RET(6, b ? D3D_OK : D3DERR_OUTOFVIDEOMEMORY);
}

// CreateIndexBuffer(Length, Usage, Format, Pool, pp)
static void dev_CreateIndexBuffer(CPU *c)
{
    d3dbuf *b = d3d8_buf_create(1, ARG(1), ARG(2), ARG(3), ARG(4));
    wr32(g_mem, ARG(5), b ? b->obj : 0);
    RET(6, b ? D3D_OK : D3DERR_OUTOFVIDEOMEMORY);
}

// CreateRenderTarget(Width, Height, Format, MultiSample, Lockable, pp)
static void dev_CreateRenderTarget(CPU *c)
{
    d3dsurf *s = d3d8_surf_create(ARG(1), ARG(2), ARG(3), USAGE_RENDERTARGET, POOL_DEFAULT);
    wr32(g_mem, ARG(6), s ? s->obj : 0);
    RET(7, s ? D3D_OK : D3DERR_INVALIDCALL);
}

// CreateDepthStencilSurface(Width, Height, Format, MultiSample, pp)
static void dev_CreateDepthStencilSurface(CPU *c)
{
    d3dsurf *s = d3d8_surf_create(ARG(1), ARG(2), ARG(3), USAGE_DEPTHSTENCIL, POOL_DEFAULT);
    wr32(g_mem, ARG(5), s ? s->obj : 0);
    RET(6, s ? D3D_OK : D3DERR_INVALIDCALL);
}

// CreateImageSurface(Width, Height, Format, pp)
static void dev_CreateImageSurface(CPU *c)
{
    d3dsurf *s = d3d8_surf_create(ARG(1), ARG(2), ARG(3), 0, POOL_SYSTEMMEM);
    wr32(g_mem, ARG(4), s ? s->obj : 0);
    RET(5, s ? D3D_OK : D3DERR_INVALIDCALL);
}

// CopyRects(pSourceSurface, pSourceRectsArray, cRects, pDestinationSurface, pDestPointsArray)
static void dev_CopyRects(CPU *c)
{
    d3dsurf *src = d3d8_surf_from(ARG(1)), *dst = d3d8_surf_from(ARG(4));
    RET(6, d3d8_copy_rects(src, dst, ARG(2), ARG(3), ARG(5)));
}

// UpdateTexture(pSourceTexture, pDestinationTexture)
static void dev_UpdateTexture(CPU *c)
{
    d3dtex *src = d3d8_tex_from(ARG(1)), *dst = d3d8_tex_from(ARG(2));
    RET(3, d3d8_update_texture(src, dst));
}

// GetFrontBuffer(pDestSurface)
static void dev_GetFrontBuffer(CPU *c)
{
    d3ddev *d = DEV(c);
    d3dsurf *dst = d3d8_surf_from(ARG(1));
    RET(2, d3d8_copy_surface(d->backbuffer, dst));
}

// SetRenderTarget(pRenderTarget, pNewZStencil): устройство держит ссылки на цели
static void dev_SetRenderTarget(CPU *c)
{
    d3ddev *d = DEV(c);
    d3dsurf *rt = d3d8_surf_from(ARG(1)), *ds = d3d8_surf_from(ARG(2));
    if (rt && rt != d->rt) {
        surf_ref(rt, 1);
        surf_ref(d->rt, 0);
        d->rt = rt;
    }
    if (ds != d->ds) {
        surf_ref(ds, 1);
        surf_ref(d->ds, 0);
        d->ds = ds;
    }
    // как D3D: viewport — вся новая цель
    d->vp[0] = d->vp[1] = 0;
    d->vp[2] = d->rt->w;
    d->vp[3] = d->rt->h;
    d->vpz[0] = 0.0f;
    d->vpz[1] = 1.0f;
    d3d8_cmd_set_rt(d, d->rt, d->ds, 1);
    RET(3, D3D_OK);
}

static void dev_GetRenderTarget(CPU *c)
{
    d3ddev *d = DEV(c);
    surf_ref(d->rt, 1);
    wr32(g_mem, ARG(1), d->rt->obj);
    RET(2, D3D_OK);
}

static void dev_GetDepthStencilSurface(CPU *c)
{
    d3ddev *d = DEV(c);
    if (d->ds)
        com_addref(d->ds->obj);
    wr32(g_mem, ARG(1), d->ds ? d->ds->obj : 0);
    RET(2, d->ds ? D3D_OK : 0x88760866u);  // D3DERR_NOTFOUND
}

static void dev_BeginScene(CPU *c) { RET(1, D3D_OK); }
static void dev_EndScene(CPU *c) { RET(1, D3D_OK); }

// Clear(Count, pRects, Flags, Color, Z, Stencil): в пределах viewport (и прямоугольников)
static void dev_Clear(CPU *c)
{
    d3ddev *d = DEV(c);
    u32 count = ARG(1), rects = ARG(2), flags = ARG(3), color = ARG(4), stencil = ARG(6);
    float z = u2f(ARG(5));
    s32 sbuf[32];  // до 8 прямоугольников на стеке без аллокаций
    s32 *rbuf = NULL;
    u32 nrect = (count && rects) ? count : 0;
    if (nrect) {
        rbuf = nrect <= 8 ? sbuf : malloc((size_t)nrect * 4u * sizeof(s32));
        if (rbuf) {
            for (u32 i = 0; i < nrect; i++) {
                rbuf[4u * i + 0] = (s32)rd32(g_mem, rects + 16u * i);
                rbuf[4u * i + 1] = (s32)rd32(g_mem, rects + 16u * i + 4u);
                rbuf[4u * i + 2] = (s32)rd32(g_mem, rects + 16u * i + 8u);
                rbuf[4u * i + 3] = (s32)rd32(g_mem, rects + 16u * i + 12u);
            }
        } else {
            nrect = 0;
        }
    }
    d3d8_cmd_clear(d, flags, color, z, stencil, nrect, rbuf);
    if (rbuf && rbuf != sbuf)
        free(rbuf);
    RET(7, D3D_OK);
}

static float *xf_slot(d3ddev *d, u32 state)
{
    if (state == 2)
        return d->view;
    if (state == 3)
        return d->proj;
    if (state >= 16 && state < 16 + MAX_STAGES)
        return d->texm[state - 16];
    if (state == 256)
        return d->world;
    return NULL;
}

// ================================================================ прямой C-интерфейс (d3d8_direct.h)
// Используется нативными оверрайдами D3DDrv/Engine и самими COM-методами dev_Set*:
// минует COM-трамплины и x86-стек, сохраняет полную совместимость с StateBlock (d->rec_sb)
// и отсекает повторную привязку тех же гостевых ресурсов без AddRef/Release.
struct d3ddev *d3d8_dev_from_guest(u32 guest_ptr)
{
    if (!guest_ptr)
        return NULL;
    if (g_dev && guest_ptr == g_dev->obj)
        return g_dev;
    return (d3ddev *)com_host(guest_ptr);
}

void d3d8_direct_set_rs(struct d3ddev *d, u32 state, u32 val)
{
    if (!d)
        return;
    if (d->rec_sb) {
        if (state < 256) {
            d->rec_sb->rs[state] = val;
            sb_set_rs_bit(d->rec_sb, state);
        }
        return;
    }
    apply_rs(d, state, val);
}

void d3d8_direct_set_tss(struct d3ddev *d, u32 stage, u32 type, u32 val)
{
    if (!d)
        return;
    if (d->rec_sb) {
        if (stage < MAX_STAGES && type < 32) {
            d->rec_sb->tss[stage][type] = val;
            d->rec_sb->tss_mask[stage] |= 1u << type;
        }
        return;
    }
    apply_tss(d, stage, type, val);
}

void d3d8_direct_set_transform(struct d3ddev *d, u32 type, const float *mat16)
{
    if (!d || !mat16 || !xf_slot(d, type))
        return;
    float mat[16];
    memcpy(mat, mat16, 64);
    if (d->rec_sb) {
        d3d_sb *sb = d->rec_sb;
        if (type == 2) {
            memcpy(sb->view, mat, 64);
            sb->xf_view = 1;
        } else if (type == 3) {
            memcpy(sb->proj, mat, 64);
            sb->xf_proj = 1;
        } else if (type == 256) {
            memcpy(sb->world, mat, 64);
            sb->xf_world = 1;
        } else if (type >= 16 && type < 16 + MAX_STAGES) {
            memcpy(sb->texm[type - 16], mat, 64);
            sb->xf_texm_mask |= (u8)(1u << (type - 16));
        }
        return;
    }
    apply_xf(d, type, mat);
}

void d3d8_direct_set_material(struct d3ddev *d, const void *mat68)
{
    if (!d || !mat68)
        return;
    if (d->rec_sb) {
        memcpy(d->rec_sb->material, mat68, sizeof d->rec_sb->material);
        d->rec_sb->has_mat = 1;
        return;
    }
    float mat[17];
    memcpy(mat, mat68, sizeof mat);
    apply_mat(d, mat);
}

void d3d8_direct_set_light(struct d3ddev *d, u32 idx, const void *light104)
{
    if (!d || !light104)
        return;
    if (idx < MAX_LIGHTS) {
        d3dlight l;
        memcpy(&l, light104, sizeof l);
        if (d->rec_sb) {
            d->rec_sb->lights[idx] = l;
            d->rec_sb->light_set[idx] = 1;
            d->rec_sb->light_mask |= (u8)(1u << idx);
            return;
        }
        apply_light(d, idx, &l);
    } else {
        hle_todo("d3d8: light index >= 8");
    }
}

void d3d8_direct_light_enable(struct d3ddev *d, u32 idx, u32 enable)
{
    if (!d || idx >= MAX_LIGHTS)
        return;
    u8 on = enable != 0;
    if (d->rec_sb) {
        d->rec_sb->light_on[idx] = on;
        d->rec_sb->light_en_mask |= (u8)(1u << idx);
        return;
    }
    apply_light_enable(d, idx, on);
}

void d3d8_direct_set_texture(struct d3ddev *d, u32 stage, u32 tex_guest_ptr)
{
    if (!d || stage >= MAX_STAGES)
        return;
    if (!d->rec_sb) {
        d3dtex *cur = d->tex[stage];
        if (!tex_guest_ptr) {
            if (!cur)
                return;
        } else if (cur && cur->obj == tex_guest_ptr) {
            return;
        }
    }
    d3dtex *t = d3d8_tex_from(tex_guest_ptr);
    if (d->rec_sb) {
        sb_set_tex(d->rec_sb, stage, t);
        return;
    }
    apply_tex(d, stage, t);
}

void d3d8_direct_set_stream(struct d3ddev *d, u32 idx, u32 vb_guest_ptr, u32 stride)
{
    if (!d || idx >= MAX_STREAMS)
        return;
    if (!d->rec_sb) {
        d3dbuf *cur = d->stream[idx];
        int same = vb_guest_ptr ? (cur && cur->obj == vb_guest_ptr) : !cur;
        if (same && d->stride[idx] == stride)
            return;
    }
    d3dbuf *b = d3d8_buf_from(vb_guest_ptr);
    if (d->rec_sb) {
        sb_set_stream(d->rec_sb, idx, b, stride);
        return;
    }
    apply_stream(d, idx, b, stride);
}

void d3d8_direct_set_indices(struct d3ddev *d, u32 ib_guest_ptr, u32 base_vertex)
{
    if (!d)
        return;
    if (!d->rec_sb) {
        d3dbuf *cur = d->indices;
        int same = ib_guest_ptr ? (cur && cur->obj == ib_guest_ptr) : !cur;
        if (same && d->base_vertex == base_vertex)
            return;
    }
    d3dbuf *b = d3d8_buf_from(ib_guest_ptr);
    if (d->rec_sb) {
        sb_set_indices(d->rec_sb, b, base_vertex);
        return;
    }
    apply_indices(d, b, base_vertex);
}

void d3d8_direct_set_vs(struct d3ddev *d, u32 handle)
{
    if (!d)
        return;
    if (d->rec_sb) {
        d->rec_sb->vs = handle;
        d->rec_sb->has_vs = 1;
        return;
    }
    apply_vs(d, handle);
}

void d3d8_direct_set_ps(struct d3ddev *d, u32 handle)
{
    if (!d)
        return;
    if (d->rec_sb) {
        d->rec_sb->ps = handle;
        d->rec_sb->has_ps = 1;
        return;
    }
    apply_ps(d, handle);
}

void d3d8_direct_set_vs_const(struct d3ddev *d, u32 reg, const float *data, u32 count)
{
    if (!d || !data || !count || reg >= 96)
        return;
    if (reg + count > 96)
        count = 96 - reg;
    if (d->rec_sb) {
        for (u32 i = 0; i < count; i++) {
            u32 r = reg + i;
            memcpy(d->rec_sb->vsc[r], (const u8 *)data + 16u * i, 16);
            d->rec_sb->vsc_mask[r >> 5] |= 1u << (r & 31);
        }
        return;
    }
    memcpy(d->vsc[reg], data, (size_t)count * 16u);
}

void d3d8_direct_set_ps_const(struct d3ddev *d, u32 reg, const float *data, u32 count)
{
    if (!d || !data || !count || reg >= 8)
        return;
    if (reg + count > 8)
        count = 8 - reg;
    if (d->rec_sb) {
        for (u32 i = 0; i < count; i++) {
            u32 r = reg + i;
            memcpy(d->rec_sb->psc[r], (const u8 *)data + 16u * i, 16);
            d->rec_sb->psc_mask |= (u8)(1u << r);
        }
        return;
    }
    apply_psc(d, reg, count, data);
}

void d3d8_direct_set_viewport(struct d3ddev *d, const void *vp24)
{
    if (!d || !vp24)
        return;
    u32 vp[4];
    float vpz[2];
    memcpy(vp, vp24, 16);
    memcpy(vpz, (const u8 *)vp24 + 16, 8);
    if (d->rec_sb) {
        memcpy(d->rec_sb->vp, vp, sizeof vp);
        memcpy(d->rec_sb->vpz, vpz, sizeof vpz);
        d->rec_sb->has_vp = 1;
        return;
    }
    apply_vp(d, vp, vpz);
}

void d3d8_direct_set_clip_plane(struct d3ddev *d, u32 idx, const float *plane4)
{
    if (!d || !plane4 || idx >= 6)
        return;
    float plane[4];
    memcpy(plane, plane4, 16);
    if (d->rec_sb) {
        memcpy(d->rec_sb->clip[idx], plane, 16);
        d->rec_sb->clip_mask |= (u8)(1u << idx);
        return;
    }
    apply_clip(d, idx, plane);
}

int d3d8_direct_shadow_blur(struct d3ddev *d, u32 src_tex_guest, u32 dst_surf_guest, float step_px)
{
    if (!d || d->rec_sb || !src_tex_guest || !(step_px >= 0.0f))
        return 0;
    d3dtex *src_tex = d3d8_tex_from(src_tex_guest);
    d3dsurf *src_surf = src_tex ? NULL : d3d8_surf_from(src_tex_guest);
    if (!src_tex && !src_surf)
        return 0;
    if (src_tex && src_tex->cube)
        return 0;
    if (src_surf && src_surf->tex) {
        src_tex = src_surf->tex;
        src_surf = NULL;
    }
    d3dsurf *dst_surf = NULL;
    if (!dst_surf_guest) {
        dst_surf = d->rt;
    } else {
        dst_surf = d3d8_surf_from(dst_surf_guest);
        if (!dst_surf) {
            d3dtex *dt = d3d8_tex_from(dst_surf_guest);
            if (dt && !dt->cube && dt->surf)
                dst_surf = dt->surf[0];
        }
    }
    if (!dst_surf || !dst_surf->w || !dst_surf->h)
        return 0;
    if (src_tex && dst_surf->tex == src_tex)
        return 0;
    if (src_surf && src_surf == dst_surf)
        return 0;
    if (src_surf && !(src_surf->usage & USAGE_RENDERTARGET) && !src_surf->backbuffer)
        return 0;
    if (!dst_surf->tex && !(dst_surf->usage & USAGE_RENDERTARGET) && !dst_surf->backbuffer)
        return 0;
    d->draws++;
    d3d8_cmd_shadow_blur(d, src_tex, src_surf, dst_surf, step_px);
    return 1;
}

static void dev_SetTransform(CPU *c)
{
    d3d8_direct_set_transform(DEV(c), ARG(1), (const float *)(g_mem + ARG(2)));
    RET(3, D3D_OK);
}

static void dev_GetTransform(CPU *c)
{
    d3ddev *d = DEV(c);
    float *m = xf_slot(d, ARG(1)), id[16];
    if (!m) {
        identity(id);
        m = id;
    }
    memcpy(g_mem + ARG(2), m, 64);
    RET(3, D3D_OK);
}

static void dev_MultiplyTransform(CPU *c)
{
    d3ddev *d = DEV(c);
    u32 st = ARG(1);
    float *m = xf_slot(d, st), b[16], r[16];
    if (m) {
        memcpy(b, g_mem + ARG(2), 64);
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                r[i * 4 + j] = m[i * 4] * b[j] + m[i * 4 + 1] * b[4 + j] + m[i * 4 + 2] * b[8 + j] + m[i * 4 + 3] * b[12 + j];
        apply_xf(d, st, r);
    }
    RET(3, D3D_OK);
}

// D3DVIEWPORT8: X, Y, Width, Height, MinZ, MaxZ
static void dev_SetViewport(CPU *c)
{
    d3d8_direct_set_viewport(DEV(c), g_mem + ARG(1));
    RET(2, D3D_OK);
}

static void dev_GetViewport(CPU *c)
{
    d3ddev *d = DEV(c);
    for (int i = 0; i < 4; i++)
        wr32(g_mem, ARG(1) + 4u * (u32)i, d->vp[i]);
    wr32(g_mem, ARG(1) + 16, f2u(d->vpz[0]));
    wr32(g_mem, ARG(1) + 20, f2u(d->vpz[1]));
    RET(2, D3D_OK);
}

static void dev_SetMaterial(CPU *c)
{
    d3d8_direct_set_material(DEV(c), g_mem + ARG(1));
    RET(2, D3D_OK);
}

static void dev_GetMaterial(CPU *c)
{
    memcpy(g_mem + ARG(1), DEV(c)->material, sizeof DEV(c)->material);
    RET(2, D3D_OK);
}

static void dev_SetLight(CPU *c)
{
    d3d8_direct_set_light(DEV(c), ARG(1), g_mem + ARG(2));
    RET(3, D3D_OK);
}

static void dev_GetLight(CPU *c)
{
    d3ddev *d = DEV(c);
    if (ARG(1) >= MAX_LIGHTS || !d->light_set[ARG(1)])
        RET(3, D3DERR_INVALIDCALL);
    memcpy(g_mem + ARG(2), &d->lights[ARG(1)], sizeof(d3dlight));
    RET(3, D3D_OK);
}

static void dev_LightEnable(CPU *c)
{
    d3d8_direct_light_enable(DEV(c), ARG(1), ARG(2));
    RET(3, D3D_OK);
}

static void dev_GetLightEnable(CPU *c)
{
    d3ddev *d = DEV(c);
    wr32(g_mem, ARG(2), ARG(1) < MAX_LIGHTS ? d->light_on[ARG(1)] : 0);
    RET(3, D3D_OK);
}

static void dev_SetClipPlane(CPU *c)
{
    d3d8_direct_set_clip_plane(DEV(c), ARG(1), (const float *)(g_mem + ARG(2)));
    RET(3, D3D_OK);
}

static void dev_GetClipPlane(CPU *c)
{
    d3ddev *d = DEV(c);
    if (ARG(1) < 6)
        memcpy(g_mem + ARG(2), d->clip[ARG(1)], 16);
    RET(3, D3D_OK);
}

static void dev_SetRenderState(CPU *c)
{
    d3d8_direct_set_rs(DEV(c), ARG(1), ARG(2));
    RET(3, D3D_OK);
}

static void dev_GetRenderState(CPU *c)
{
    wr32(g_mem, ARG(2), ARG(1) < 256 ? DEV(c)->rs[ARG(1)] : 0);
    RET(3, D3D_OK);
}

// ---------------------------------------------------------------- StateBlocks (слоты 52..57)
static void dev_BeginStateBlock(CPU *c)
{
    d3ddev *d = DEV(c);
    if (d->rec_sb)
        RET(1, D3DERR_INVALIDCALL);
    d->rec_sb = calloc(1, sizeof *d->rec_sb);
    RET(1, d->rec_sb ? D3D_OK : E_OUTOFMEMORY);
}

static void dev_EndStateBlock(CPU *c)  // (pToken)
{
    d3ddev *d = DEV(c);
    if (!d->rec_sb || !ARG(1)) {
        sb_free(d->rec_sb);
        d->rec_sb = NULL;
        RET(2, D3DERR_INVALIDCALL);
    }
    u32 tok = sb_store(d, d->rec_sb);
    if (!tok) {
        sb_free(d->rec_sb);
        d->rec_sb = NULL;
        wr32(g_mem, ARG(1), 0);
        RET(2, E_OUTOFMEMORY);
    }
    d->rec_sb = NULL;
    wr32(g_mem, ARG(1), tok);
    RET(2, D3D_OK);
}

static void dev_ApplyStateBlock(CPU *c)  // (Token)
{
    d3ddev *d = DEV(c);
    d3d_sb *s = sb_get(d, ARG(1));
    if (d->rec_sb || !s)
        RET(2, D3DERR_INVALIDCALL);
    sb_apply(d, s);
    RET(2, D3D_OK);
}

static void dev_CaptureStateBlock(CPU *c)  // (Token)
{
    d3ddev *d = DEV(c);
    d3d_sb *s = sb_get(d, ARG(1));
    if (d->rec_sb || !s)
        RET(2, D3DERR_INVALIDCALL);
    sb_capture(d, s);
    RET(2, D3D_OK);
}

static void dev_DeleteStateBlock(CPU *c)  // (Token)
{
    d3ddev *d = DEV(c);
    u32 tok = ARG(1);
    if (!tok || tok > d->sb_cap || !d->sb[tok - 1u])
        RET(2, D3DERR_INVALIDCALL);
    sb_free(d->sb[tok - 1u]);
    d->sb[tok - 1u] = NULL;
    RET(2, D3D_OK);
}

static void dev_CreateStateBlock(CPU *c)  // (Type, pToken)
{
    d3ddev *d = DEV(c);
    u32 type = ARG(1), out = ARG(2);
    if (d->rec_sb || !out || type < 1 || type > 3)
        RET(3, D3DERR_INVALIDCALL);
    d3d_sb *s = calloc(1, sizeof *s);
    if (!s)
        RET(3, E_OUTOFMEMORY);
    sb_init_type(s, type);
    sb_capture(d, s);
    u32 tok = sb_store(d, s);
    if (!tok) {
        sb_free(s);
        wr32(g_mem, out, 0);
        RET(3, E_OUTOFMEMORY);
    }
    wr32(g_mem, out, tok);
    RET(3, D3D_OK);
}

static void dev_SetClipStatus(CPU *c) { RET(2, D3D_OK); }

static void dev_GetClipStatus(CPU *c)
{
    memset(g_mem + ARG(1), 0, 8);
    RET(2, D3D_OK);
}

static void dev_GetTexture(CPU *c)
{
    d3ddev *d = DEV(c);
    d3dtex *t = ARG(1) < MAX_STAGES ? d->tex[ARG(1)] : NULL;
    if (t)
        com_addref(t->obj);
    wr32(g_mem, ARG(2), t ? t->obj : 0);
    RET(3, D3D_OK);
}

static void dev_SetTexture(CPU *c)
{
    d3d8_direct_set_texture(DEV(c), ARG(1), ARG(2));
    RET(3, D3D_OK);
}

static void dev_GetTextureStageState(CPU *c)
{
    d3ddev *d = DEV(c);
    wr32(g_mem, ARG(3), ARG(1) < MAX_STAGES && ARG(2) < 32 ? d->tss[ARG(1)][ARG(2)] : 0);
    RET(4, D3D_OK);
}

static void dev_SetTextureStageState(CPU *c)
{
    d3d8_direct_set_tss(DEV(c), ARG(1), ARG(2), ARG(3));
    RET(4, D3D_OK);
}

static void dev_ValidateDevice(CPU *c)
{
    wr32(g_mem, ARG(1), 1);
    RET(2, D3D_OK);
}

static void dev_GetInfo(CPU *c) { RET(4, 1u); }  // S_FALSE: сведений нет
static void dev_SetPaletteEntries(CPU *c) { RET(3, D3D_OK); }
static void dev_SetCurrentTexturePalette(CPU *c) { RET(2, D3D_OK); }

// ---------------------------------------------------------------- отрисовка
static void dev_DrawPrimitive(CPU *c)  // (Type, StartVertex, PrimitiveCount)
{
    d3ddev *d = DEV(c);
    u32 count = ARG(3);
    if (count) {
        d->draws++;
        d3d8_cmd_draw(d, ARG(1), count, 0, ARG(2), 0, 0);
    }
    RET(4, D3D_OK);
}

static void dev_DrawIndexedPrimitive(CPU *c)  // (Type, MinIndex, NumVertices, StartIndex, PrimCount)
{
    d3ddev *d = DEV(c);
    u32 count = ARG(5);
    if (count) {
        d->draws++;
        d3d8_cmd_draw(d, ARG(1), count, 1, ARG(4), ARG(2), ARG(3));
    }
    RET(6, D3D_OK);
}

static void dev_DrawPrimitiveUP(CPU *c)  // (Type, PrimitiveCount, pVertexStreamZeroData, Stride)
{
    d3ddev *d = DEV(c);
    u32 count = ARG(2);
    if (count) {
        d->draws++;
        d3d8_cmd_draw_up(d, ARG(1), count, 0, 0, 0, g_mem + ARG(3), ARG(4), NULL, 0);
    }
    // как D3D: после *UP поток 0 сброшен (Core 2 сбрасывает d->r.stream[0] внутри x_draw_up)
    if (d->stream[0]) {
        com_release(d->stream[0]->obj);
        d->stream[0] = NULL;
    }
    d->stride[0] = 0;
    if (!count)
        d3d8_cmd_stream(d, 0, NULL, 0);
    RET(5, D3D_OK);
}

// (Type, MinVertexIndex, NumVertexIndices, PrimitiveCount, pIndexData, IndexDataFormat, pVertexData, Stride)
static void dev_DrawIndexedPrimitiveUP(CPU *c)
{
    d3ddev *d = DEV(c);
    u32 count = ARG(4);
    if (count) {
        d->draws++;
        d3d8_cmd_draw_up(d, ARG(1), count, 1, ARG(2), ARG(3), g_mem + ARG(7), ARG(8), g_mem + ARG(5), ARG(6));
    }
    if (d->stream[0]) {
        com_release(d->stream[0]->obj);
        d->stream[0] = NULL;
    }
    if (d->indices) {
        com_release(d->indices->obj);
        d->indices = NULL;
    }
    d->stride[0] = 0;
    if (!count) {
        d3d8_cmd_stream(d, 0, NULL, 0);
        d3d8_cmd_indices(d, NULL, d->base_vertex);
    }
    RET(9, D3D_OK);
}

// ---------------------------------------------------------------- шейдеры
// CreateVertexShader(pDeclaration, pFunction, pHandle, Usage)
// Проверено по D3DDrv.dll: UD3DRenderDevice::GetVertexShader (D3DDrv.dll!0x12811fb0) поддерживает только
// VS_FixedFunction (0) и единственный вызов CreateVertexShader (D3DDrv.dll!0x12826353) всегда передаёт
// pFunction == 0; программируемых vs.1.1 в Shrek 2 нет (CreateVolumeTexture встречается только в недостижимом
// статическом d3dx8.lib).
static void dev_CreateVertexShader(CPU *c)
{
    d3ddev *d = DEV(c);
    if (ARG(2)) {
        hle_todo("d3d8: programmable vertex shaders (vs.1.1) are not translated yet");
        RET(5, D3DERR_INVALIDCALL);
    }
    for (u32 i = 0; i < MAX_VS; i++)
        if (!g_vs[i].used) {
            g_vs[i].used = 1;
            g_vs[i].ntok = parse_decl(d, ARG(1), &g_vs[i].decl);
            g_vs[i].tokens = malloc(4 * g_vs[i].ntok);
            memcpy(g_vs[i].tokens, g_mem + ARG(1), 4 * g_vs[i].ntok);
            d3d8_cmd_vs_set(i, &g_vs[i].decl);
            wr32(g_mem, ARG(3), VS_HANDLE_BASE + i);
            RET(5, D3D_OK);
        }
    RET(5, D3DERR_OUTOFVIDEOMEMORY);
}

static void dev_SetVertexShader(CPU *c)
{
    d3d8_direct_set_vs(DEV(c), ARG(1));
    RET(2, D3D_OK);
}

static void dev_GetVertexShader(CPU *c)
{
    wr32(g_mem, ARG(1), DEV(c)->vs);
    RET(2, D3D_OK);
}

static void dev_DeleteVertexShader(CPU *c)
{
    u32 i = ARG(1) - VS_HANDLE_BASE;
    if (ARG(1) >= VS_HANDLE_BASE && i < MAX_VS && g_vs[i].used) {
        free(g_vs[i].tokens);
        memset(&g_vs[i], 0, sizeof g_vs[i]);
        d3d8_cmd_vs_del(i);
    }
    RET(2, D3D_OK);
}

static void dev_SetVertexShaderConstant(CPU *c)  // (Register, pConstantData, ConstantCount)
{
    d3d8_direct_set_vs_const(DEV(c), ARG(1), (const float *)(g_mem + ARG(2)), ARG(3));
    RET(4, D3D_OK);
}

static void dev_GetVertexShaderConstant(CPU *c)
{
    d3ddev *d = DEV(c);
    for (u32 i = 0; i < ARG(3) && ARG(1) + i < 96; i++)
        memcpy(g_mem + ARG(2) + 16 * i, d->vsc[ARG(1) + i], 16);
    RET(4, D3D_OK);
}

static void dev_SetStreamSource(CPU *c)  // (StreamNumber, pStreamData, Stride)
{
    u32 n = ARG(1);
    if (n >= MAX_STREAMS)
        RET(4, D3DERR_INVALIDCALL);
    d3d8_direct_set_stream(DEV(c), n, ARG(2), ARG(3));
    RET(4, D3D_OK);
}

static void dev_GetStreamSource(CPU *c)
{
    d3ddev *d = DEV(c);
    u32 n = ARG(1);
    d3dbuf *b = n < MAX_STREAMS ? d->stream[n] : NULL;
    if (b)
        com_addref(b->obj);
    wr32(g_mem, ARG(2), b ? b->obj : 0);
    wr32(g_mem, ARG(3), n < MAX_STREAMS ? d->stride[n] : 0);
    RET(4, D3D_OK);
}

static void dev_SetIndices(CPU *c)  // (pIndexData, BaseVertexIndex)
{
    d3d8_direct_set_indices(DEV(c), ARG(1), ARG(2));
    RET(3, D3D_OK);
}

static void dev_GetIndices(CPU *c)
{
    d3ddev *d = DEV(c);
    if (d->indices)
        com_addref(d->indices->obj);
    wr32(g_mem, ARG(1), d->indices ? d->indices->obj : 0);
    wr32(g_mem, ARG(2), d->base_vertex);
    RET(3, D3D_OK);
}

static void dev_CreatePixelShader(CPU *c)  // (pFunction, pHandle)
{
    u32 h = d3d8_ps_create((const u32 *)(g_mem + ARG(1)), (RT_WINDOW - ARG(1)) / 4);
    wr32(g_mem, ARG(2), h);
    RET(3, h ? D3D_OK : D3DERR_INVALIDCALL);
}

static void dev_SetPixelShader(CPU *c)
{
    d3d8_direct_set_ps(DEV(c), ARG(1));
    RET(2, D3D_OK);
}

static void dev_GetPixelShader(CPU *c)
{
    wr32(g_mem, ARG(1), DEV(c)->ps);
    RET(2, D3D_OK);
}

static void dev_DeletePixelShader(CPU *c)
{
    d3d8_ps_delete(ARG(1));
    RET(2, D3D_OK);
}

static void dev_SetPixelShaderConstant(CPU *c)  // (Register, pConstantData, ConstantCount)
{
    d3d8_direct_set_ps_const(DEV(c), ARG(1), (const float *)(g_mem + ARG(2)), ARG(3));
    RET(4, D3D_OK);
}

static void dev_GetPixelShaderConstant(CPU *c)
{
    d3ddev *d = DEV(c);
    for (u32 i = 0; i < ARG(3) && ARG(1) + i < 8; i++)
        memcpy(g_mem + ARG(2) + 16 * i, d->psc[ARG(1) + i], 16);
    RET(4, D3D_OK);
}

// ================================================================ создание
// Игра освобождает устройство при выходе и при смене разрешения (SetRes пересоздаёт устройство). Окно, контекст
// GL и его объекты остаются: следующий CreateDevice берёт их снова с новым COM-объектом. Привязанные текстуры и
// буферы устройство отпускает, как D3D.
static void dev_destroy(void *p)
{
    d3ddev *d = p;
    d->obj = 0;
    sb_destroy_all(d);
    d3d8_cmd_unbind_all(d);
    for (int s = 0; s < MAX_STAGES; s++)
        if (d->tex[s]) {
            com_release(d->tex[s]->obj);
            d->tex[s] = NULL;
        }
    for (int n = 0; n < MAX_STREAMS; n++)
        if (d->stream[n]) {
            com_release(d->stream[n]->obj);
            d->stream[n] = NULL;
        }
    if (d->indices) {
        com_release(d->indices->obj);
        d->indices = NULL;
    }
}

static void init_vtable(void)
{
    if (g_vt_dev)
        return;
    guest_fn m[97] = {0};
    m[0] = dev_QueryInterface;
    m[1] = com_AddRef;
    m[2] = com_Release;
    m[3] = dev_TestCooperativeLevel;
    m[4] = dev_GetAvailableTextureMem;
    m[5] = dev_ResourceManagerDiscardBytes;
    m[6] = dev_GetDirect3D;
    m[7] = dev_GetDeviceCaps;
    m[8] = dev_GetDisplayMode;
    m[9] = dev_GetCreationParameters;
    m[10] = dev_SetCursorProperties;
    m[11] = dev_SetCursorPosition;
    m[12] = dev_ShowCursor;
    m[13] = dev_CreateAdditionalSwapChain;
    m[14] = dev_Reset;
    m[15] = dev_Present;
    m[16] = dev_GetBackBuffer;
    m[17] = dev_GetRasterStatus;
    m[18] = dev_SetGammaRamp;
    m[19] = dev_GetGammaRamp;
    m[20] = dev_CreateTexture;
    // 21 CreateVolumeTexture (в D3DDrv.dll не используется)
    m[22] = dev_CreateCubeTexture;
    m[23] = dev_CreateVertexBuffer;
    m[24] = dev_CreateIndexBuffer;
    m[25] = dev_CreateRenderTarget;
    m[26] = dev_CreateDepthStencilSurface;
    m[27] = dev_CreateImageSurface;
    m[28] = dev_CopyRects;
    m[29] = dev_UpdateTexture;
    m[30] = dev_GetFrontBuffer;
    m[31] = dev_SetRenderTarget;
    m[32] = dev_GetRenderTarget;
    m[33] = dev_GetDepthStencilSurface;
    m[34] = dev_BeginScene;
    m[35] = dev_EndScene;
    m[36] = dev_Clear;
    m[37] = dev_SetTransform;
    m[38] = dev_GetTransform;
    m[39] = dev_MultiplyTransform;
    m[40] = dev_SetViewport;
    m[41] = dev_GetViewport;
    m[42] = dev_SetMaterial;
    m[43] = dev_GetMaterial;
    m[44] = dev_SetLight;
    m[45] = dev_GetLight;
    m[46] = dev_LightEnable;
    m[47] = dev_GetLightEnable;
    m[48] = dev_SetClipPlane;
    m[49] = dev_GetClipPlane;
    m[50] = dev_SetRenderState;
    m[51] = dev_GetRenderState;
    m[52] = dev_BeginStateBlock;
    m[53] = dev_EndStateBlock;
    m[54] = dev_ApplyStateBlock;
    m[55] = dev_CaptureStateBlock;
    m[56] = dev_DeleteStateBlock;
    m[57] = dev_CreateStateBlock;
    m[58] = dev_SetClipStatus;
    m[59] = dev_GetClipStatus;
    m[60] = dev_GetTexture;
    m[61] = dev_SetTexture;
    m[62] = dev_GetTextureStageState;
    m[63] = dev_SetTextureStageState;
    m[64] = dev_ValidateDevice;
    m[65] = dev_GetInfo;
    m[66] = dev_SetPaletteEntries;
    // 67 GetPaletteEntries
    m[68] = dev_SetCurrentTexturePalette;
    // 69 GetCurrentTexturePalette
    m[70] = dev_DrawPrimitive;
    m[71] = dev_DrawIndexedPrimitive;
    m[72] = dev_DrawPrimitiveUP;
    m[73] = dev_DrawIndexedPrimitiveUP;
    // 74 ProcessVertices
    m[75] = dev_CreateVertexShader;
    m[76] = dev_SetVertexShader;
    m[77] = dev_GetVertexShader;
    m[78] = dev_DeleteVertexShader;
    m[79] = dev_SetVertexShaderConstant;
    m[80] = dev_GetVertexShaderConstant;
    // 81 GetVertexShaderDeclaration, 82 GetVertexShaderFunction
    m[83] = dev_SetStreamSource;
    m[84] = dev_GetStreamSource;
    m[85] = dev_SetIndices;
    m[86] = dev_GetIndices;
    m[87] = dev_CreatePixelShader;
    m[88] = dev_SetPixelShader;
    m[89] = dev_GetPixelShader;
    m[90] = dev_DeletePixelShader;
    m[91] = dev_SetPixelShaderConstant;
    m[92] = dev_GetPixelShaderConstant;
    // 93 GetPixelShaderFunction, 94..96 патчи
    g_vt_dev = com_vtable("IDirect3DDevice8", m, 97);
}

// Switch: гость — ядро 1, его фоновые потоки — 0 (kernel32), поток рендера — 2
#define RENDER_CORE 2

static void bind_gl(void *r) { *(int *)r = plat_gl_bind(); }

// окно с контекстом GL и поток рендера (создание устройства; заставка — раньше него)
static int open_window(int w, int h, int fullscreen)
{
    if (plat_window_open(w, h, fullscreen, "Spider-Man 2") != 0)
        return -1;
    // поток рендера (по умолчанию; SNX_RENDER_THREAD=0 — без него): контекст GL — у него, вызовы GL слоя D3D8
    // идут через очередь. С экспериментом SNX_GLTHREAD (glthread Mesa) не совмещается.
    const char *rtv = getenv("SNX_RENDER_THREAD");
    int rt_on = !getenv("SNX_GLTHREAD") && !(rtv && *rtv == '0');
    if (rt_on && cs_start(RENDER_CORE) != 0)
        fprintf(stderr, "[d3d8] cannot start the render thread\n");
    int bound = -1;
    cs_sync(bind_gl, &bound);
    return bound;
}

// ---------------------------------------------------------------- заставка загрузки
typedef struct {
    const u8 *px;  // BGRX, строки снизу вверх
    int w, h;
    u32 stride;
} splash_img;

static void draw_splash(void *arg)
{
    const splash_img *s = arg;
    GLuint tex, fbo;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)(s->stride / 4));
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, s->w, s->h, 0, GL_BGRA, GL_UNSIGNED_BYTE, s->px);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    int W, H;
    plat_drawable_size(&W, &H);
    glViewport(0, 0, W, H);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    // вписать с сохранением пропорций
    int w = W, h = (int)((long long)W * s->h / s->w);
    if (h > H) {
        h = H;
        w = (int)((long long)H * s->w / s->h);
    }
    glBlitFramebuffer(0, 0, s->w, s->h, (W - w) / 2, (H - h) / 2, (W - w) / 2 + w, (H - h) / 2 + h,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
    plat_swap();
}

// game.exe показывает Help\Logo.bmp в диалоге, пока движок грузит пакеты и создаёт устройство (на Switch —
// секунды): окно и поток рендера открываются заранее, картинка — по центру экрана
void d3d8_splash(const u8 *bgrx, int w, int h, u32 stride)
{
    if (g_dev || w <= 0 || h <= 0)
        return;
    int sw = w, sh = h;  // мак: окно размером с картинку, как заставка на ПК
#ifdef __SWITCH__
    plat_screen_size(&sw, &sh);
#endif
    if (open_window(sw, sh, 1) != 0)
        return;
    splash_img s = {bgrx, w, h, stride};
    cs_sync(draw_splash, &s);
}

u32 d3d8_device_create(CPU *c, u32 hwnd, u32 pp)
{
    (void)c;
    if (g_dev) {
        d3ddev *d = g_dev;
        if (d->obj)
            com_addref(d->obj);  // старое ещё не освобождено — то же устройство
        else {
            d->obj = com_create_st(g_vt_dev, d, dev_destroy);
            d3d8_res_init(d->obj);
        }
        d->hwnd = hwnd;
        apply_pp(d, pp);
        return d->obj;
    }
    init_vtable();
    d3ddev *d = calloc(1, sizeof *d);
    d->hwnd = hwnd;
    read_pp(d, pp);
    if (open_window((int)d->bb_w, (int)d->bb_h, !d->windowed) != 0) {
        free(d);
        return 0;
    }
    d->obj = com_create_st(g_vt_dev, d, dev_destroy);
    g_dev = d;
    for (u32 ch = 0; ch < 3; ch++)
        for (u32 i = 0; i < 256; i++)
            d->gamma[ch][i] = (u16)(i * 257u);
    d->gamma_dirty = 1;
    d3d8_res_init(d->obj);
    d3d8_cmd_init_gl(d);
    d3d8_prog_preload();
    make_backbuffer(d);
    set_defaults(d);
    d3d8_cmd_set_rt(d, d->rt, d->ds, 1);
    return d->obj;
}

void d3d8_set_d3d_object(u32 obj) { g_d3d8_obj = obj; }
