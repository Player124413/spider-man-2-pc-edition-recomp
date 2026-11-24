// HLE Direct3D 8: ресурсы — текстуры (2D, кубические), поверхности, вершинные и индексные буферы.
// Текстура — объект GL; блокировка уровня выделяет гостевую память (с прежним содержимым, если уровень уже
// загружен и блокировка не DISCARD), разблокировка загружает уровень целиком. Буферы держат постоянную
// гостевую копию: Lock отдаёт указатель в неё, изменённый диапазон догружается в GL перед отрисовкой.
#include <math.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>

#include "runtime/heap.h"
#include "runtime/rt.h"
#include "runtime/hle/d3d8_int.h"

#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif

// ================================================================ форматы
#define SW(r, g, b, a) {GL_##r, GL_##g, GL_##b, GL_##a}
static const d3dfmt g_fmts[] = {
    {FMT_A8R8G8B8, GL_RGBA8, GL_BGRA, GL_UNSIGNED_BYTE, 4, 0, 0, {0}},
    {FMT_X8R8G8B8, GL_RGBA8, GL_BGRA, GL_UNSIGNED_BYTE, 4, 0, 0, SW(RED, GREEN, BLUE, ONE)},
    {FMT_R8G8B8, GL_RGB8, GL_BGR, GL_UNSIGNED_BYTE, 3, 0, 0, {0}},
    {FMT_R5G6B5, GL_RGB565, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, 2, 0, 0, {0}},
    {FMT_X1R5G5B5, GL_RGB5_A1, GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV, 2, 0, 0, SW(RED, GREEN, BLUE, ONE)},
    {FMT_A1R5G5B5, GL_RGB5_A1, GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV, 2, 0, 0, {0}},
    {FMT_A4R4G4B4, GL_RGBA4, GL_BGRA, GL_UNSIGNED_SHORT_4_4_4_4_REV, 2, 0, 0, {0}},
    {FMT_X4R4G4B4, GL_RGBA4, GL_BGRA, GL_UNSIGNED_SHORT_4_4_4_4_REV, 2, 0, 0, SW(RED, GREEN, BLUE, ONE)},
    {FMT_A8, GL_R8, GL_RED, GL_UNSIGNED_BYTE, 1, 0, 0, SW(ZERO, ZERO, ZERO, RED)},
    {FMT_L8, GL_R8, GL_RED, GL_UNSIGNED_BYTE, 1, 0, 0, SW(RED, RED, RED, ONE)},
    {FMT_A8L8, GL_RG8, GL_RG, GL_UNSIGNED_BYTE, 2, 0, 0, SW(RED, RED, RED, GREEN)},
    {FMT_V8U8, GL_RG8_SNORM, GL_RG, GL_BYTE, 2, 0, 0, SW(RED, GREEN, ONE, ONE)},
    {FMT_DXT1, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, 0, 0, 8, 0, 0, {0}},
    {FMT_DXT2, GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, 0, 0, 16, 0, 0, {0}},
    {FMT_DXT3, GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, 0, 0, 16, 0, 0, {0}},
    {FMT_DXT4, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, 0, 0, 16, 0, 0, {0}},
    {FMT_DXT5, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, 0, 0, 16, 0, 0, {0}},
    {FMT_D16, GL_DEPTH_COMPONENT16, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, 2, 1, 0, {0}},
    {FMT_D16_LOCKABLE, GL_DEPTH_COMPONENT16, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, 2, 1, 0, {0}},
    {FMT_D24X8, GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, 4, 1, 0, {0}},
    {FMT_D32, GL_DEPTH_COMPONENT32, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, 4, 1, 0, {0}},
    {FMT_D24S8, GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, 4, 1, 1, {0}},
    {FMT_D24X4S4, GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, 4, 1, 1, {0}},
    {FMT_D15S1, GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, 4, 1, 1, {0}},
};
#undef SW

const d3dfmt *d3d8_format(u32 fmt)
{
    for (size_t i = 0; i < sizeof g_fmts / sizeof g_fmts[0]; i++)
        if (g_fmts[i].fmt == fmt)
            return &g_fmts[i];
    return NULL;
}

u32 d3d8_pitch(const d3dfmt *f, u32 w) { return f->format ? w * f->bpp : ((w + 3) / 4) * f->bpp; }
u32 d3d8_rows(const d3dfmt *f, u32 h) { return f->format ? h : (h + 3) / 4; }

static u32 mip(u32 v, u32 level)
{
    v >>= level;
    return v ? v : 1;
}

static u32 g_dev_obj;
static u32 g_vt_tex, g_vt_cube, g_vt_surf, g_vt_vb, g_vt_ib;

static u32 galloc(u32 size) { return heap_alloc(heap_process(), 0, size ? size : 1); }
static void gfree(u32 p)
{
    if (p)
        heap_free(heap_process(), p);
}

// ================================================================ текстуры: GL
static GLenum face_target(const d3dtex *t, int face)
{
    return t->cube ? (GLenum)(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face) : GL_TEXTURE_2D;
}

static GLenum tex_target(const d3dtex *t) { return t->cube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D; }

static void upload_level(d3dtex *t, int face, u32 level, const void *data)
{
    const d3dfmt *f = d3d8_format(t->fmt);
    u32 w = mip(t->w, level), h = mip(t->h, level);
    g_hitch.uploads++;  // строка [hitch] (d3d8_present)
    g_hitch.upload_bytes += (u64)d3d8_pitch(f, w) * d3d8_rows(f, h);
    glBindTexture(tex_target(t), t->gl);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    if (f->format)  // размер известен — с потоком рендера данные копируются в очередь
        cs_TexImage2D(face_target(t, face), (GLint)level, (GLint)f->internal, (GLsizei)w, (GLsizei)h, f->format,
                      f->type, data, (size_t)d3d8_pitch(f, w) * d3d8_rows(f, h));
    else
        glCompressedTexImage2D(face_target(t, face), (GLint)level, f->internal, (GLsizei)w, (GLsizei)h, 0,
                               (GLsizei)(d3d8_pitch(f, w) * d3d8_rows(f, h)), data);
}

static void read_level(d3dtex *t, int face, u32 level, void *out)
{
    const d3dfmt *f = d3d8_format(t->fmt);
    glBindTexture(tex_target(t), t->gl);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    if (f->format)
        glGetTexImage(face_target(t, face), (GLint)level, f->format, f->type, out);
    else
        glGetCompressedTexImage(face_target(t, face), (GLint)level, out);
}

static void tex_destroy(void *p);

d3dtex *d3d8_tex_create(u32 w, u32 h, u32 levels, u32 usage, u32 fmt, u32 pool, int cube)
{
    const d3dfmt *f = d3d8_format(fmt);
    if (!f || !w || !h)
        return NULL;
    u32 full = 1;
    for (u32 m = w > h ? w : h; m > 1; m >>= 1)
        full++;
    if (!levels || levels > full)
        levels = full;
    d3dtex *t = calloc(1, sizeof *t);
    t->cube = cube;
    t->w = w;
    t->h = h;
    t->levels = levels;
    t->fmt = fmt;
    t->usage = usage;
    t->pool = pool;
    t->surf = calloc((size_t)(cube ? 6 : 1) * levels, sizeof *t->surf);
    glGenTextures(1, &t->gl);
    GLenum tg = tex_target(t);
    glBindTexture(tg, t->gl);
    glTexParameteri(tg, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(tg, GL_TEXTURE_MAX_LEVEL, (GLint)levels - 1);
    if (f->swizzle[0]) {
        glTexParameteri(tg, GL_TEXTURE_SWIZZLE_R, f->swizzle[0]);
        glTexParameteri(tg, GL_TEXTURE_SWIZZLE_G, f->swizzle[1]);
        glTexParameteri(tg, GL_TEXTURE_SWIZZLE_B, f->swizzle[2]);
        glTexParameteri(tg, GL_TEXTURE_SWIZZLE_A, f->swizzle[3]);
    }
    for (int face = 0; face < (cube ? 6 : 1); face++)
        for (u32 l = 0; l < levels; l++)
            upload_level(t, face, l, NULL);
    t->obj = com_create_st(cube ? g_vt_cube : g_vt_tex, t, tex_destroy);
    return t;
}

// ================================================================ поверхности
void d3d8_fbo_attach(GLenum fb_target, GLenum attachment, const d3dsurf *s)
{
    if (!s)
        glFramebufferRenderbuffer(fb_target, attachment, GL_RENDERBUFFER, 0);
    else if (s->tex)
        glFramebufferTexture2D(fb_target, attachment, face_target(s->tex, s->face), s->tex->gl, s->level);
    else if (s->color_tex)
        glFramebufferTexture2D(fb_target, attachment, GL_TEXTURE_2D, s->color_tex, 0);
    else
        glFramebufferRenderbuffer(fb_target, attachment, GL_RENDERBUFFER, s->rb);
}

static void x_surf_destroy(void *p)
{
    d3dsurf *s = *(d3dsurf **)p;
    d3ddev *d = d3d8_dev();
    if (d) {
        if (d->r.rt == s)
            d->r.rt = NULL;
        if (d->r.ds == s)
            d->r.ds = NULL;
        if (d->r.fbo_rt == s)
            d->r.fbo_rt = NULL;
        if (d->r.fbo_ds == s)
            d->r.fbo_ds = NULL;
    }
    if (s->rb)
        glDeleteRenderbuffers(1, &s->rb);
    if (s->color_tex)
        glDeleteTextures(1, &s->color_tex);
    if (s->movie_tex)
        glDeleteTextures(1, &s->movie_tex);
    free(s);
}

static void surf_destroy(void *p)
{
    d3dsurf *s = p;
    if (s->own_mem)
        gfree(s->mem);
    gfree(s->rb_mem);
    s->mem = s->rb_mem = 0;
    // Отложенное удаление GL-объектов и free(s) на Core 2 после всех предшествующих Draw/FBO-команд
    cs_cmd_push(x_surf_destroy, &s, sizeof s);
}

static d3dsurf *surf_new(u32 w, u32 h, u32 fmt, u32 usage, u32 pool)
{
    d3dsurf *s = calloc(1, sizeof *s);
    s->w = w;
    s->h = h;
    s->fmt = fmt;
    s->usage = usage;
    s->pool = pool;
    s->obj = com_create_st(g_vt_surf, s, surf_destroy);
    return s;
}

d3dsurf *d3d8_surf_create(u32 w, u32 h, u32 fmt, u32 usage, u32 pool)
{
    const d3dfmt *f = d3d8_format(fmt);
    if (!f)
        return NULL;
    d3dsurf *s = surf_new(w, h, fmt, usage, pool);
    if ((usage & USAGE_RENDERTARGET) && !f->depth && f->format) {
        // Цветовые цели рендера и задний буфер — 2D-текстуры (чтобы Present читал их в шейдере без лишнего blit)
        glGenTextures(1, &s->color_tex);
        glBindTexture(GL_TEXTURE_2D, s->color_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        if (f->swizzle[0]) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, f->swizzle[0]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, f->swizzle[1]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, f->swizzle[2]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, f->swizzle[3]);
        }
        cs_TexImage2D(GL_TEXTURE_2D, 0, (GLint)f->internal, (GLsizei)w, (GLsizei)h, f->format, f->type, NULL, 0);
    } else if (usage & (USAGE_RENDERTARGET | USAGE_DEPTHSTENCIL)) {
        glGenRenderbuffers(1, &s->rb);
        glBindRenderbuffer(GL_RENDERBUFFER, s->rb);
        glRenderbufferStorage(GL_RENDERBUFFER, f->internal, (GLsizei)w, (GLsizei)h);
    } else {  // поверхность в памяти (CreateImageSurface): только гостевая копия
        s->pitch = d3d8_pitch(f, w);
        s->mem = galloc(s->pitch * d3d8_rows(f, h));
        s->own_mem = 1;
        memset(g_mem + s->mem, 0, s->pitch * d3d8_rows(f, h));
    }
    return s;
}

static void x_tex_destroy(void *p)
{
    d3dtex *t = *(d3dtex **)p;
    d3ddev *d = d3d8_dev();
    if (d) {
        for (int s = 0; s < MAX_STAGES; s++)
            if (d->r.tex[s] == t) {
                d->r.tex[s] = NULL;
                d->r.key_dirty = 1;
            }
    }
    if (t->gl)
        glDeleteTextures(1, &t->gl);
    free(t);
}

static void tex_destroy(void *p)
{
    d3dtex *t = p;
    for (u32 i = 0; i < (t->cube ? 6u : 1u) * t->levels; i++)
        if (t->surf[i]) {
            gfree(t->surf[i]->locked ? t->surf[i]->mem : 0);
            com_release(t->surf[i]->obj);  // собственный счётчик поверхности уровня всегда 1
        }
    free(t->surf);
    t->surf = NULL;
    // Отложенное удаление GL-текстуры и free(t) на Core 2 после всех предшествующих Draw
    cs_cmd_push(x_tex_destroy, &t, sizeof t);
}

static d3dsurf *tex_surface(d3dtex *t, int face, u32 level)
{
    d3dsurf **sp = &t->surf[(u32)face * t->levels + level];
    if (!*sp) {
        d3dsurf *s = surf_new(mip(t->w, level), mip(t->h, level), t->fmt, t->usage, t->pool);
        s->tex = t;
        s->face = face;
        s->level = (int)level;
        *sp = s;
    }
    return *sp;
}

d3dtex *d3d8_tex_from(u32 obj)
{
    if (!obj)
        return NULL;
    u32 vt = rd32(g_mem, obj);
    return vt == g_vt_tex || vt == g_vt_cube ? com_host(obj) : NULL;
}

d3dsurf *d3d8_surf_from(u32 obj) { return obj && rd32(g_mem, obj) == g_vt_surf ? com_host(obj) : NULL; }

d3dbuf *d3d8_buf_from(u32 obj)
{
    if (!obj)
        return NULL;
    u32 vt = rd32(g_mem, obj);
    return vt == g_vt_vb || vt == g_vt_ib ? com_host(obj) : NULL;
}

// Ролик рисует порт (movie_overlay: кадр больше заднего буфера): игра всё равно блокирует задний буфер на каждом
// кадре ролика (копия Bink мимо), и чтение FBO при Lock (glReadPixels — ожидание всего ГП) и заливка при Unlock
// стоили бы десятков мс на кадр. Задний буфер в это время целиком перекрывает кадр ролика — не читаем и не заливаем.
static int overlay_target(const d3dsurf *s)
{
    d3ddev *d = d3d8_dev();
    return d && s == d->backbuffer && g_movie_overlay_ns && rt_ticks_ns() - g_movie_overlay_ns < 250000000ull;
}

// Блокировка поверхности: уровень текстуры, самостоятельная поверхность в памяти или цель рендера
static u32 surf_lock(d3dsurf *s, u32 locked_rect, u32 rect, u32 flags)
{
    const d3dfmt *f = d3d8_format(s->fmt);
    if (s->locked)
        return D3DERR_INVALIDCALL;
    u32 pitch = d3d8_pitch(f, s->w), size = pitch * d3d8_rows(f, s->h);
    if (rect) {
        for (int i = 0; i < 4; i++)
            s->lock_rect[i] = rd32(g_mem, rect + 4u * (u32)i);
    } else {
        s->lock_rect[0] = s->lock_rect[1] = 0;
        s->lock_rect[2] = s->w;
        s->lock_rect[3] = s->h;
    }
    int is_rt = s->rb != 0 || s->color_tex != 0;
    if (!s->own_mem) {
        if (is_rt && !s->rb_mem)
            s->rb_mem = galloc(size);
        s->mem = is_rt ? s->rb_mem : galloc(size);
        s->pitch = pitch;
        if (!s->mem)
            return D3DERR_OUTOFVIDEOMEMORY;
        int keep = !(flags & LOCK_DISCARD);
        if (s->tex && keep && (s->tex->filled & (1u << s->level)))
            read_level(s->tex, s->face, (u32)s->level, g_mem + s->mem);
        else if (is_rt && keep && !s->movie_mode && !overlay_target(s)) {
            // цель рендера: прочитать заблокированный прямоугольник из её FBO (снимок экрана; ReduceMouseLag
            // блокирует 1x1 каждый кадр — ради ожидания GPU). Во время ролика не читаем: заливается только
            // кадр ролика (write_movie), остальное в памяти блокировки не нужно
            GLint l = (GLint)s->lock_rect[0], t = (GLint)s->lock_rect[1];
            GLuint fb;
            glGenFramebuffers(1, &fb);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, fb);
            d3d8_fbo_attach(GL_READ_FRAMEBUFFER, f->depth ? GL_DEPTH_ATTACHMENT : GL_COLOR_ATTACHMENT0, s);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glPixelStorei(GL_PACK_ROW_LENGTH, (GLint)s->w);
            glReadPixels(l, t, (GLsizei)s->lock_rect[2] - l, (GLsizei)s->lock_rect[3] - t, f->format, f->type,
                         g_mem + s->mem + (u32)t * pitch + (u32)l * f->bpp);
            glPixelStorei(GL_PACK_ROW_LENGTH, 0);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
            glDeleteFramebuffers(1, &fb);
            d3d8_bind_target(d3d8_dev());
        } else if (!is_rt)
            memset(g_mem + s->mem, 0, size);
    }
    u32 off = 0;
    if (rect) {
        u32 l = s->lock_rect[0], t = s->lock_rect[1];
        off = f->format ? t * pitch + l * f->bpp : (t / 4) * pitch + (l / 4) * f->bpp;
    }
    s->locked = 1;
    s->lock_flags = flags;
    wr32(g_mem, locked_rect, s->pitch);
    wr32(g_mem, locked_rect + 4, s->mem + off);
    return D3D_OK;
}

u64 g_movie_drawn_ns, g_movie_overlay_ns;

// Кадр ролика Bink (d3d8_movie_frame: D3DDrv выводит его прямо в залоченный задний буфер): только его
// прямоугольник — в постоянную текстуру и blit в цель, растянутый с сохранением пропорций (на ПК 640x480/800x600
// почти занимали экран, а в 1920x1080 были бы окошком), поля вокруг — чёрные.
// 0 — прямоугольник вне блокировки (тогда — обычный путь)
static int write_movie(d3dsurf *s, const d3dfmt *f)
{
    GLint mx = (GLint)s->movie[0], my = (GLint)s->movie[1], mw = (GLint)s->movie[2], mh = (GLint)s->movie[3],
          W = (GLint)s->w, H = (GLint)s->h;
    if (mx < (GLint)s->lock_rect[0] || my < (GLint)s->lock_rect[1] || mx + mw > (GLint)s->lock_rect[2] ||
        my + mh > (GLint)s->lock_rect[3])
        return 0;
    if (!s->movie_tex || s->movie_tw != (u32)mw || s->movie_th != (u32)mh) {
        if (!s->movie_tex)
            glGenTextures(1, &s->movie_tex);
        glBindTexture(GL_TEXTURE_2D, s->movie_tex);
        cs_TexImage2D(GL_TEXTURE_2D, 0, (GLint)f->internal, mw, mh, f->format, f->type, NULL, 0);
        s->movie_tw = (u32)mw;
        s->movie_th = (u32)mh;
    } else
        glBindTexture(GL_TEXTURE_2D, s->movie_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    cs_TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, mw, mh, f->format, f->type,
                     g_mem + s->mem + (u32)my * s->pitch + (u32)mx * f->bpp, (size_t)mw * f->bpp, s->pitch);
    glBindTexture(GL_TEXTURE_2D, 0);
    GLuint fb[2];
    glGenFramebuffers(2, fb);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fb[0]);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s->movie_tex, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb[1]);
    d3d8_fbo_attach(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, s);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    float sc = fminf((float)W / (float)mw, (float)H / (float)mh);
    if (sc > 1.05f) {
        GLint w = (GLint)((float)mw * sc), h = (GLint)((float)mh * sc);
        if (w < W || h < H) {
            // поля вокруг — чёрные, как на ПК в 4:3, где ролик закрывал экран (иначе на 16:9 по бокам — мир под ним)
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
        }
        glBlitFramebuffer(0, 0, mw, mh, (W - w) / 2, (H - h) / 2, (W - w) / 2 + w, (H - h) / 2 + h,
                          GL_COLOR_BUFFER_BIT, GL_LINEAR);
    } else
        glBlitFramebuffer(0, 0, mw, mh, mx, my, mx + mw, my + mh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(2, fb);
    d3d8_bind_target(d3d8_dev());  // и маски/scissor — заново перед следующей отрисовкой
    g_movie_drawn_ns = rt_ticks_ns();
    return 1;
}

// Запись гостя в залоченную цель рендера: данные — во временную текстуру, затем blit заблокированного
// прямоугольника в renderbuffer (оба «сверху вниз»); кадр ролика — write_movie.
static void write_target(d3dsurf *s, const d3dfmt *f)
{
    GLint l = (GLint)s->lock_rect[0], t = (GLint)s->lock_rect[1], r = (GLint)s->lock_rect[2],
          b = (GLint)s->lock_rect[3];
    int was_movie = s->movie_mode, movie = s->has_movie;
    s->has_movie = 0;
    s->movie_mode = movie && write_movie(s, f);
    if (s->movie_mode || was_movie || overlay_target(s))
        return;  // кадр ролика залит; или цель не читалась при Lock (ролик кончился, ролик рисует порт)
    if (r <= l || b <= t)
        return;
    GLuint tex, fb[2];
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)s->w);  // только заблокированный прямоугольник
    cs_TexImage2D(GL_TEXTURE_2D, 0, (GLint)f->internal, r - l, b - t, f->format, f->type,
                  g_mem + s->mem + (u32)t * s->pitch + (u32)l * f->bpp,
                  (size_t)(b - t - 1) * s->pitch + (size_t)(r - l) * f->bpp);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glGenFramebuffers(2, fb);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fb[0]);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb[1]);
    d3d8_fbo_attach(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, s);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBlitFramebuffer(0, 0, r - l, b - t, l, t, r, b, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(2, fb);
    glDeleteTextures(1, &tex);
    d3d8_bind_target(d3d8_dev());  // и маски/scissor — заново перед следующей отрисовкой
}

void d3d8_movie_frame(u32 dest, u32 pitch, u32 x, u32 y, u32 w, u32 h)
{
    d3ddev *d = d3d8_dev();
    d3dsurf *s = d ? d->backbuffer : NULL;
    if (!s || !s->locked || !s->mem || s->pitch != pitch || dest < s->mem || !w || !h)
        return;
    const d3dfmt *f = d3d8_format(s->fmt);
    u32 off = dest - s->mem;
    s->movie[0] = (off % pitch) / f->bpp + x;
    s->movie[1] = off / pitch + y;
    s->movie[2] = w;
    s->movie[3] = h;
    s->has_movie = 1;
}

static u32 surf_unlock(d3dsurf *s)
{
    if (!s->locked)
        return D3DERR_INVALIDCALL;
    s->locked = 0;
    if (s->own_mem)
        return D3D_OK;
    if (!(s->lock_flags & LOCK_READONLY)) {
        if (s->tex) {
            upload_level(s->tex, s->face, (u32)s->level, g_mem + s->mem);
            if (s->face == 0 && s->level < 32)
                s->tex->filled |= 1u << s->level;
        } else if (s->rb || s->color_tex) {
            const d3dfmt *f = d3d8_format(s->fmt);
            if (f->depth)
                hle_todo("d3d8: writing into a locked depth buffer is ignored");
            else
                write_target(s, f);
        }
    }
    if (s->mem != s->rb_mem)
        gfree(s->mem);
    s->mem = 0;
    return D3D_OK;
}

// ================================================================ CopyRects / UpdateTexture
static int is_fbo_color_fmt(u32 fmt)
{
    switch (fmt) {
    case FMT_A8R8G8B8: case FMT_X8R8G8B8: case FMT_R5G6B5: case FMT_X1R5G5B5: case FMT_A1R5G5B5:
    case FMT_A4R4G4B4: case FMT_X4R4G4B4: case FMT_A8: case FMT_L8: case FMT_A8L8:
        return 1;
    default:
        return 0;
    }
}

// Распаковка/упаковка одного пикселя через 8-битный ARGB для копирования между разными форматами
static void unpack_argb(u32 fmt, const u8 *p, u8 out[4])
{
    u8 a = 255, r = 0, g = 0, b = 0;
    switch (fmt) {
    case FMT_A8R8G8B8: b = p[0]; g = p[1]; r = p[2]; a = p[3]; break;
    case FMT_X8R8G8B8: b = p[0]; g = p[1]; r = p[2]; a = 255; break;
    case FMT_R8G8B8:   b = p[0]; g = p[1]; r = p[2]; a = 255; break;
    case FMT_R5G6B5: {
        u16 v = (u16)(p[0] | (u16)p[1] << 8);
        r = (u8)(((v >> 11) & 31) * 255 / 31);
        g = (u8)(((v >> 5) & 63) * 255 / 63);
        b = (u8)((v & 31) * 255 / 31);
        break;
    }
    case FMT_A1R5G5B5: case FMT_X1R5G5B5: {
        u16 v = (u16)(p[0] | (u16)p[1] << 8);
        a = fmt == FMT_A1R5G5B5 ? ((v & 0x8000u) ? 255 : 0) : 255;
        r = (u8)(((v >> 10) & 31) * 255 / 31);
        g = (u8)(((v >> 5) & 31) * 255 / 31);
        b = (u8)((v & 31) * 255 / 31);
        break;
    }
    case FMT_A4R4G4B4: case FMT_X4R4G4B4: {
        u16 v = (u16)(p[0] | (u16)p[1] << 8);
        a = fmt == FMT_A4R4G4B4 ? (u8)(((v >> 12) & 15) * 17) : 255;
        r = (u8)(((v >> 8) & 15) * 17);
        g = (u8)(((v >> 4) & 15) * 17);
        b = (u8)((v & 15) * 17);
        break;
    }
    case FMT_A8:   a = p[0]; r = g = b = 0; break;
    case FMT_L8:   r = g = b = p[0]; a = 255; break;
    case FMT_A8L8: r = g = b = p[0]; a = p[1]; break;
    default: break;
    }
    out[0] = b; out[1] = g; out[2] = r; out[3] = a;
}

static void pack_argb(u32 fmt, const u8 in[4], u8 *p)
{
    u8 b = in[0], g = in[1], r = in[2], a = in[3];
    switch (fmt) {
    case FMT_A8R8G8B8: p[0] = b; p[1] = g; p[2] = r; p[3] = a; break;
    case FMT_X8R8G8B8: p[0] = b; p[1] = g; p[2] = r; p[3] = 255; break;
    case FMT_R8G8B8:   p[0] = b; p[1] = g; p[2] = r; break;
    case FMT_R5G6B5: {
        u16 v = (u16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        p[0] = (u8)v; p[1] = (u8)(v >> 8);
        break;
    }
    case FMT_A1R5G5B5: case FMT_X1R5G5B5: {
        u16 v = (u16)((((fmt == FMT_A1R5G5B5 ? (a >= 128) : 1) ? 1u : 0u) << 15) |
                      ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
        p[0] = (u8)v; p[1] = (u8)(v >> 8);
        break;
    }
    case FMT_A4R4G4B4: case FMT_X4R4G4B4: {
        u16 v = (u16)(((fmt == FMT_A4R4G4B4 ? (a >> 4) : 15u) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4));
        p[0] = (u8)v; p[1] = (u8)(v >> 8);
        break;
    }
    case FMT_A8:   p[0] = a; break;
    case FMT_L8:   p[0] = r; break;
    case FMT_A8L8: p[0] = r; p[1] = a; break;
    default: break;
    }
}

static void read_surf_cpu(d3dsurf *s, const d3dfmt *f, u8 *buf)
{
    if (s->tex) {
        read_level(s->tex, s->face, (u32)s->level, buf);
    } else if (s->rb || s->color_tex) {
        GLuint fb;
        glGenFramebuffers(1, &fb);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fb);
        d3d8_fbo_attach(GL_READ_FRAMEBUFFER, f->depth ? GL_DEPTH_ATTACHMENT : GL_COLOR_ATTACHMENT0, s);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glReadPixels(0, 0, (GLsizei)s->w, (GLsizei)s->h, f->format, f->type, buf);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fb);
        if (d3d8_dev())
            d3d8_bind_target(d3d8_dev());
    }
}

static void write_rt_cpu(d3dsurf *s, const d3dfmt *f, const u8 *buf)
{
    GLuint tex, fb[2];
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    cs_TexImage2D(GL_TEXTURE_2D, 0, (GLint)f->internal, (GLsizei)s->w, (GLsizei)s->h, f->format, f->type, buf,
                  (size_t)d3d8_pitch(f, s->w) * d3d8_rows(f, s->h));
    glGenFramebuffers(2, fb);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fb[0]);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb[1]);
    d3d8_fbo_attach(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, s);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBlitFramebuffer(0, 0, (GLint)s->w, (GLint)s->h, 0, 0, (GLint)s->w, (GLint)s->h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(2, fb);
    glDeleteTextures(1, &tex);
    if (d3d8_dev())
        d3d8_bind_target(d3d8_dev());
}

static u32 copy_rects_impl(d3dsurf *src, d3dsurf *dst, u32 src_rects_va, u32 count, u32 dst_pts_va,
                           const s32 *single_rect)
{
    if (!src || !dst)
        return D3DERR_INVALIDCALL;
    const d3dfmt *sf = d3d8_format(src->fmt), *df = d3d8_format(dst->fmt);
    if (!sf || !df || sf->depth || df->depth)
        return D3DERR_INVALIDCALL;
    if ((!sf->format || !df->format) && src->fmt != dst->fmt)
        return D3DERR_INVALIDCALL;

    // Подготовка списка прямоугольников (src_rects_va == 0 -> single_rect или вся поверхность src)
    s32 full_r[4] = {0, 0, (s32)src->w, (s32)src->h};
    if (single_rect)
        memcpy(full_r, single_rect, sizeof full_r);
    u32 n = src_rects_va ? count : 1u;
    if (!n)
        return D3D_OK;

    // Быстрый путь GPU -> GPU для совпадающих цветовых форматов без конвертации
    int same_alpha_fmt = src->fmt == dst->fmt || (src->fmt == FMT_A8R8G8B8 && dst->fmt == FMT_X8R8G8B8);
    if (!src->own_mem && !dst->own_mem && sf->format && df->format && same_alpha_fmt &&
        is_fbo_color_fmt(src->fmt) && is_fbo_color_fmt(dst->fmt)) {
        GLuint fb[2];
        glGenFramebuffers(2, fb);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fb[0]);
        d3d8_fbo_attach(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, src);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb[1]);
        d3d8_fbo_attach(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, dst);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        for (u32 i = 0; i < n; i++) {
            s32 l = src_rects_va ? (s32)rd32(g_mem, src_rects_va + 16u * i) : full_r[0];
            s32 t = src_rects_va ? (s32)rd32(g_mem, src_rects_va + 16u * i + 4u) : full_r[1];
            s32 r = src_rects_va ? (s32)rd32(g_mem, src_rects_va + 16u * i + 8u) : full_r[2];
            s32 b = src_rects_va ? (s32)rd32(g_mem, src_rects_va + 16u * i + 12u) : full_r[3];
            s32 dx = dst_pts_va ? (s32)rd32(g_mem, dst_pts_va + 8u * i) : l;
            s32 dy = dst_pts_va ? (s32)rd32(g_mem, dst_pts_va + 8u * i + 4u) : t;
            if (l < 0 || t < 0 || r <= l || b <= t || (s64)r > (s64)src->w || (s64)b > (s64)src->h)
                continue;
            s32 w = r - l, h = b - t;
            if (dx < 0 || dy < 0 || (s64)dx + (s64)w > (s64)dst->w || (s64)dy + (s64)h > (s64)dst->h)
                continue;
            glBlitFramebuffer(l, t, r, b, dx, dy, dx + w, dy + h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(2, fb);
        if (dst->tex && dst->face == 0 && dst->level < 32)
            dst->tex->filled |= 1u << dst->level;
        if (d3d8_dev())
            d3d8_bind_target(d3d8_dev());
        return D3D_OK;
    }

    // Общий путь через CPU (CPU<->CPU, CPU<->GPU, сжатые форматы DXT, конвертация форматов, GetFrontBuffer)
    u32 spitch = src->own_mem ? src->pitch : d3d8_pitch(sf, src->w);
    u32 srows = d3d8_rows(sf, src->h);
    u8 *sown = NULL;
    const u8 *sbuf;
    if (src->own_mem)
        sbuf = g_mem + src->mem;
    else {
        sown = calloc(1, (size_t)spitch * srows);
        if (!sown)
            return D3DERR_OUTOFVIDEOMEMORY;
        read_surf_cpu(src, sf, sown);
        sbuf = sown;
    }

    u32 dpitch = dst->own_mem ? dst->pitch : d3d8_pitch(df, dst->w);
    u32 drows = d3d8_rows(df, dst->h);
    int full_replace = !src_rects_va && !dst_pts_va && full_r[0] == 0 && full_r[1] == 0 &&
                       full_r[2] == (s32)dst->w && full_r[3] == (s32)dst->h;
    u8 *down = NULL, *dbuf;
    if (dst->own_mem)
        dbuf = g_mem + dst->mem;
    else {
        down = calloc(1, (size_t)dpitch * drows);
        if (!down) {
            free(sown);
            return D3DERR_OUTOFVIDEOMEMORY;
        }
        if (!full_replace && (!dst->tex || (dst->tex->filled & (1u << dst->level))))
            read_surf_cpu(dst, df, down);
        dbuf = down;
    }

    for (u32 i = 0; i < n; i++) {
        s32 l = src_rects_va ? (s32)rd32(g_mem, src_rects_va + 16u * i) : full_r[0];
        s32 t = src_rects_va ? (s32)rd32(g_mem, src_rects_va + 16u * i + 4u) : full_r[1];
        s32 r = src_rects_va ? (s32)rd32(g_mem, src_rects_va + 16u * i + 8u) : full_r[2];
        s32 b = src_rects_va ? (s32)rd32(g_mem, src_rects_va + 16u * i + 12u) : full_r[3];
        s32 dx = dst_pts_va ? (s32)rd32(g_mem, dst_pts_va + 8u * i) : l;
        s32 dy = dst_pts_va ? (s32)rd32(g_mem, dst_pts_va + 8u * i + 4u) : t;
        if (l < 0 || t < 0 || r <= l || b <= t || (s64)r > (s64)src->w || (s64)b > (s64)src->h)
            continue;
        s32 w = r - l, h = b - t;
        if (dx < 0 || dy < 0 || (s64)dx + (s64)w > (s64)dst->w || (s64)dy + (s64)h > (s64)dst->h)
            continue;
        if (!sf->format) {
            // Сжатый формат (блоки 4x4)
            u32 bx0 = (u32)l / 4u, by0 = (u32)t / 4u, dbx0 = (u32)dx / 4u, dby0 = (u32)dy / 4u;
            u32 bw = ((u32)w + 3u) / 4u, bh = ((u32)h + 3u) / 4u;
            for (u32 y = 0; y < bh; y++)
                memcpy(dbuf + (size_t)(dby0 + y) * dpitch + (size_t)dbx0 * df->bpp,
                       sbuf + (size_t)(by0 + y) * spitch + (size_t)bx0 * sf->bpp, (size_t)bw * sf->bpp);
        } else if (src->fmt == dst->fmt) {
            size_t row_bytes = (size_t)w * sf->bpp;
            for (s32 y = 0; y < h; y++)
                memcpy(dbuf + (size_t)(dy + y) * dpitch + (size_t)dx * df->bpp,
                       sbuf + (size_t)(t + y) * spitch + (size_t)l * sf->bpp, row_bytes);
        } else {
            for (s32 y = 0; y < h; y++) {
                const u8 *sr = sbuf + (size_t)(t + y) * spitch + (size_t)l * sf->bpp;
                u8 *dr = dbuf + (size_t)(dy + y) * dpitch + (size_t)dx * df->bpp;
                for (s32 x = 0; x < w; x++) {
                    u8 px[4];
                    unpack_argb(src->fmt, sr + (size_t)x * sf->bpp, px);
                    pack_argb(dst->fmt, px, dr + (size_t)x * df->bpp);
                }
            }
        }
    }

    if (!dst->own_mem) {
        if (dst->tex) {
            upload_level(dst->tex, dst->face, (u32)dst->level, down);
            if (dst->face == 0 && dst->level < 32)
                dst->tex->filled |= 1u << dst->level;
        } else if (dst->rb || dst->color_tex) {
            write_rt_cpu(dst, df, down);
        }
    }
    free(sown);
    free(down);
    return D3D_OK;
}

u32 d3d8_copy_rects(d3dsurf *src, d3dsurf *dst, u32 src_rects_va, u32 count, u32 dst_pts_va)
{
    return copy_rects_impl(src, dst, src_rects_va, count, dst_pts_va, NULL);
}

u32 d3d8_copy_surface(d3dsurf *src, d3dsurf *dst)
{
    if (!src || !dst)
        return D3DERR_INVALIDCALL;
    u32 w = src->w < dst->w ? src->w : dst->w;
    u32 h = src->h < dst->h ? src->h : dst->h;
    if (!w || !h)
        return D3D_OK;
    s32 r[4] = {0, 0, (s32)w, (s32)h};
    return copy_rects_impl(src, dst, 0, 0, 0, r);
}

u32 d3d8_update_texture(d3dtex *src, d3dtex *dst)
{
    if (!src || !dst || src->cube != dst->cube || src->fmt != dst->fmt)
        return D3DERR_INVALIDCALL;
    // По спецификации D3D8: если у src больше мип-уровней, начинаем с уровня, совпадающего по размеру с dst
    u32 src_base = src->levels;
    for (u32 l = 0; l < src->levels; l++) {
        if (mip(src->w, l) == dst->w && mip(src->h, l) == dst->h) {
            src_base = l;
            break;
        }
    }
    if (src_base >= src->levels)
        return D3DERR_INVALIDCALL;
    u32 levels = dst->levels;
    if (src_base + levels > src->levels)
        levels = src->levels - src_base;
    int faces = src->cube ? 6 : 1;
    for (int f = 0; f < faces; f++) {
        for (u32 l = 0; l < levels; l++) {
            d3dsurf *ss = tex_surface(src, f, src_base + l);
            d3dsurf *ds = tex_surface(dst, f, l);
            u32 hr = d3d8_copy_rects(ss, ds, 0, 0, 0);
            if (hr != D3D_OK)
                return hr;
        }
    }
    return D3D_OK;
}

// ================================================================ COM: общее для ресурсов
static void res_QueryInterface(CPU *c)
{
    wr32(g_mem, ARG(2), 0);
    RET(3, E_NOINTERFACE);
}

static void res_GetDevice(CPU *c)
{
    com_addref(g_dev_obj);
    wr32(g_mem, ARG(1), g_dev_obj);
    RET(2, D3D_OK);
}

static void res_SetPrivateData(CPU *c) { RET(5, D3D_OK); }
static void res_GetPrivateData(CPU *c) { RET(4, 0x88760866u); }  // D3DERR_NOTFOUND
static void res_FreePrivateData(CPU *c) { RET(2, D3D_OK); }
static void res_SetPriority(CPU *c) { RET(2, 0); }
static void res_GetPriority(CPU *c) { RET(1, 0); }
static void res_PreLoad(CPU *c) { RET(1, 0); }

// ================================================================ IDirect3DTexture8 / IDirect3DCubeTexture8
#define TEX(c) ((d3dtex *)com_host(ARG(0)))

static void tex_GetType(CPU *c) { RET(1, TEX(c)->cube ? RTYPE_CUBETEXTURE : RTYPE_TEXTURE); }

static void tex_SetLOD(CPU *c)
{
    d3dtex *t = TEX(c);
    u32 old = t->lod;
    t->lod = ARG(1) < t->levels ? ARG(1) : t->levels - 1;
    glBindTexture(tex_target(t), t->gl);
    glTexParameteri(tex_target(t), GL_TEXTURE_BASE_LEVEL, (GLint)t->lod);
    RET(2, old);
}

static void tex_GetLOD(CPU *c) { RET(1, TEX(c)->lod); }
static void tex_GetLevelCount(CPU *c) { RET(1, TEX(c)->levels); }

// D3DSURFACE_DESC: Format, Type, Usage, Pool, Size, MultiSampleType, Width, Height
static void put_desc(u32 p, const d3dsurf *s)
{
    const d3dfmt *f = d3d8_format(s->fmt);
    wr32(g_mem, p, s->fmt);
    wr32(g_mem, p + 4, RTYPE_SURFACE);
    wr32(g_mem, p + 8, s->usage);
    wr32(g_mem, p + 12, s->pool);
    wr32(g_mem, p + 16, d3d8_pitch(f, s->w) * d3d8_rows(f, s->h));
    wr32(g_mem, p + 20, 0);
    wr32(g_mem, p + 24, s->w);
    wr32(g_mem, p + 28, s->h);
}

static void tex_GetLevelDesc(CPU *c)
{
    d3dtex *t = TEX(c);
    if (ARG(1) >= t->levels)
        RET(3, D3DERR_INVALIDCALL);
    put_desc(ARG(2), tex_surface(t, 0, ARG(1)));
    RET(3, D3D_OK);
}

static void tex_GetSurfaceLevel(CPU *c)
{
    d3dtex *t = TEX(c);
    if (ARG(1) >= t->levels) {
        wr32(g_mem, ARG(2), 0);
        RET(3, D3DERR_INVALIDCALL);
    }
    com_addref(t->obj);  // поверхность уровня делит счётчик с контейнером
    wr32(g_mem, ARG(2), tex_surface(t, 0, ARG(1))->obj);
    RET(3, D3D_OK);
}

static void tex_LockRect(CPU *c)  // (Level, pLockedRect, pRect, Flags)
{
    d3dtex *t = TEX(c);
    if (ARG(1) >= t->levels)
        RET(5, D3DERR_INVALIDCALL);
    RET(5, surf_lock(tex_surface(t, 0, ARG(1)), ARG(2), ARG(3), ARG(4)));
}

static void tex_UnlockRect(CPU *c)
{
    d3dtex *t = TEX(c);
    if (ARG(1) >= t->levels)
        RET(2, D3DERR_INVALIDCALL);
    RET(2, surf_unlock(tex_surface(t, 0, ARG(1))));
}

static void tex_AddDirtyRect(CPU *c) { RET(2, D3D_OK); }

static void cube_GetCubeMapSurface(CPU *c)  // (FaceType, Level, pp)
{
    d3dtex *t = TEX(c);
    if (ARG(1) >= 6 || ARG(2) >= t->levels) {
        wr32(g_mem, ARG(3), 0);
        RET(4, D3DERR_INVALIDCALL);
    }
    com_addref(t->obj);
    wr32(g_mem, ARG(3), tex_surface(t, (int)ARG(1), ARG(2))->obj);
    RET(4, D3D_OK);
}

static void cube_LockRect(CPU *c)  // (FaceType, Level, pLockedRect, pRect, Flags)
{
    d3dtex *t = TEX(c);
    if (ARG(1) >= 6 || ARG(2) >= t->levels)
        RET(6, D3DERR_INVALIDCALL);
    RET(6, surf_lock(tex_surface(t, (int)ARG(1), ARG(2)), ARG(3), ARG(4), ARG(5)));
}

static void cube_UnlockRect(CPU *c)
{
    d3dtex *t = TEX(c);
    if (ARG(1) >= 6 || ARG(2) >= t->levels)
        RET(3, D3DERR_INVALIDCALL);
    RET(3, surf_unlock(tex_surface(t, (int)ARG(1), ARG(2))));
}

static void cube_AddDirtyRect(CPU *c) { RET(3, D3D_OK); }

// ================================================================ IDirect3DSurface8
#define SURF(c) ((d3dsurf *)com_host(ARG(0)))

static void surf_AddRef(CPU *c)
{
    d3dsurf *s = SURF(c);
    RET(1, s && s->tex ? com_addref(s->tex->obj) : com_addref(ARG(0)));
}

static void surf_Release(CPU *c)
{
    d3dsurf *s = SURF(c);
    RET(1, s && s->tex ? com_release(s->tex->obj) : com_release(ARG(0)));
}

static void surf_GetContainer(CPU *c)  // (riid, ppContainer)
{
    d3dsurf *s = SURF(c);
    u32 obj = s->tex ? s->tex->obj : g_dev_obj;
    com_addref(obj);
    wr32(g_mem, ARG(2), obj);
    RET(3, D3D_OK);
}

static void surf_GetDesc(CPU *c)
{
    put_desc(ARG(1), SURF(c));
    RET(2, D3D_OK);
}

static void surf_LockRect(CPU *c) { RET(4, surf_lock(SURF(c), ARG(1), ARG(2), ARG(3))); }
static void surf_UnlockRect(CPU *c) { RET(1, surf_unlock(SURF(c))); }

// ================================================================ буферы
// Бюджет кольцевых zero-copy слотов для USAGE_DYNAMIC в гостевой куче (чтобы не тратить лишние десятки МБ
// при редких огромных буферах — сверх бюджета буфер безопасно работает через копирование в g_ring).
#define DYN_ZC_MAX_BUF (2u << 20)
#define DYN_ZC_BUDGET (32u << 20)
static _Atomic size_t g_dyn_slot_bytes;

typedef struct {
    d3dbuf *b;
    const void *data;
    void *heap;
    u32 off, len;
    u8 orphan, discard;
} cmd_buf_upload_t;

static void buf_upload_gl(d3dbuf *b, const void *src, u32 off, u32 len, u8 orphan, u8 discard)
{
    if (!b || !b->gl || !src || !len)
        return;
    glBindBuffer(GL_ARRAY_BUFFER, b->gl);
    GLenum gl_usage = (b->usage & USAGE_DYNAMIC) ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW;
    static int glthread = -1;
    if (glthread < 0)
        glthread = getenv("SNX_GLTHREAD") != NULL;
    if (orphan) {
        if (discard && (off > 0 || len < b->size)) {
            glBufferData(GL_ARRAY_BUFFER, b->size, NULL, gl_usage);
            if (glthread)
                glBufferSubData(GL_ARRAY_BUFFER, off, len, src);
            else
                cs_BufferWriteUnsync(GL_ARRAY_BUFFER, off, len, src);
        } else {
            glBufferData(GL_ARRAY_BUFFER, b->size, src, gl_usage);
        }
    } else if (glthread) {
        glBufferSubData(GL_ARRAY_BUFFER, off, len, src);
    } else {
        cs_BufferWriteUnsync(GL_ARRAY_BUFFER, off, len, src);
    }
}

static void x_buf_upload_zc(void *p)
{
    const cmd_buf_upload_t *q = p;
    buf_upload_gl(q->b, q->data, q->off, q->len, q->orphan, q->discard);
}

static void x_buf_upload_copy(void *p)
{
    const cmd_buf_upload_t *q = p;
    buf_upload_gl(q->b, q->data, q->off, q->len, q->orphan, q->discard);
    free(q->heap);
}

static void x_buf_destroy(void *p)
{
    d3dbuf *b = *(d3dbuf **)p;
    d3ddev *d = d3d8_dev();
    if (d) {
        for (int i = 0; i < MAX_STREAMS; i++)
            if (d->r.stream[i] == b)
                d->r.stream[i] = NULL;
        if (d->r.indices == b)
            d->r.indices = NULL;
    }
    if (b->gl)
        glDeleteBuffers(1, &b->gl);
    for (int i = 0; i < b->nzslots; i++) {
        if (b->zmem[i]) {
            gfree(b->zmem[i]);
            atomic_fetch_sub_explicit(&g_dyn_slot_bytes, b->size, memory_order_relaxed);
        }
    }
    gfree(b->mem);
    free(b);
}

static void buf_destroy(void *p)
{
    d3dbuf *b = p;
    d3ddev *d = d3d8_dev();
    if (d) {  // устройство держит ссылку на привязанные буферы, так что здесь они уже отвязаны
        for (int i = 0; i < MAX_STREAMS; i++)
            if (d->stream[i] == b)
                d->stream[i] = NULL;
        if (d->indices == b)
            d->indices = NULL;
    }
    // Освобождение GL-буфера, гостевых zero-copy слотов и free(b) — на Core 2 после всех предшествующих команд
    cs_cmd_push(x_buf_destroy, &b, sizeof b);
}

d3dbuf *d3d8_buf_create(int index, u32 size, u32 usage, u32 fvf_or_fmt, u32 pool)
{
    d3dbuf *b = calloc(1, sizeof *b);
    b->index = index;
    b->size = size;
    b->usage = usage;
    b->pool = pool;
    b->cur_zslot = -1;
    if (index)
        b->fmt = fvf_or_fmt;
    else
        b->fvf = fvf_or_fmt;
    b->mem = galloc(size);
    if (!b->mem) {
        free(b);
        return NULL;
    }
    memset(g_mem + b->mem, 0, size);
    // Для динамических буферов (VB/IB) выделяем DYN_SLOTS кольцевых слотов в гостевой памяти, чтобы Unlock
    // передавал в gl_cs только 48-байтную ссылку без memcpy вершин/индексов на Core 1.
    if ((usage & USAGE_DYNAMIC) && size > 0 && size <= DYN_ZC_MAX_BUF) {
        for (int i = 0; i < DYN_SLOTS; i++) {
            size_t cur = atomic_load_explicit(&g_dyn_slot_bytes, memory_order_relaxed);
            if (cur + size > DYN_ZC_BUDGET)
                break;
            u32 m = galloc(size);
            if (!m)
                break;
            memset(g_mem + m, 0, size);
            b->zmem[b->nzslots++] = m;
            atomic_fetch_add_explicit(&g_dyn_slot_bytes, size, memory_order_relaxed);
        }
    }
    glGenBuffers(1, &b->gl);
    glBindBuffer(GL_ARRAY_BUFFER, b->gl);
    glBufferData(GL_ARRAY_BUFFER, size, NULL, (usage & USAGE_DYNAMIC) ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
    b->obj = com_create_st(index ? g_vt_ib : g_vt_vb, b, buf_destroy);
    return b;
}

// Чистая логика выбора zero-copy слота для динамического буфера:
// - LOCK_DISCARD: ищем слот, чей zfence уже пройден Core 2 (tail >= zfence[k]); если все заняты — -1 (fallback).
// - LOCK_NOOVERWRITE: при активном zero-copy слоте разрешаем запись напрямую, если диапазон не пересекается
//   с уже отправленным (off >= cur_zhi) или Core 2 уже дочитал этот слот (tail >= zfence[cur_zslot]); иначе -1.
int d3d8_dyn_slot_pick(int nzslots, int cur_zslot, u32 cur_zhi, const u64 *zfence, uint64_t tail, u32 off, u32 flags)
{
    if (nzslots <= 0)
        return -1;
    if (flags & LOCK_DISCARD) {
        int start = cur_zslot >= 0 ? (cur_zslot + 1) % nzslots : 0;
        for (int i = 0; i < nzslots; i++) {
            int k = (start + i) % nzslots;
            if (zfence[k] == 0 || tail >= zfence[k])
                return k;
        }
        return -1;
    }
    if ((flags & LOCK_NOOVERWRITE) && cur_zslot >= 0 && cur_zslot < nzslots) {
        if (off >= cur_zhi || zfence[cur_zslot] == 0 || tail >= zfence[cur_zslot])
            return cur_zslot;
        return -1;
    }
    return -1;
}

static void buf_unlock_flush(d3dbuf *b)
{
    if (!b || !b->locked)
        return;
    b->locked = 0;
    u32 flags = b->lock_flags, off = b->lock_off, len = b->lock_size;
    if ((flags & LOCK_READONLY) || !len)
        return;
    u8 discard = (flags & LOCK_DISCARD) != 0;
    u8 orphan = !(flags & LOCK_NOOVERWRITE);
    if (b->lock_zc && b->cur_zslot >= 0) {
        int k = b->cur_zslot;
        cmd_buf_upload_t cmd = {b, g_mem + b->zmem[k] + off, NULL, off, len, orphan, discard};
        cs_cmd_push(x_buf_upload_zc, &cmd, sizeof cmd);
        b->zfence[k] = cs_enabled() ? cs_wr_pos() : 0;
        if (off + len > b->cur_zhi)
            b->cur_zhi = off + len;
    } else if (orphan && !discard) {
        // Без DISCARD и без NOOVERWRITE: сохранить прежние данные вне [off, off + len)
        cmd_buf_upload_t cmd = {b, g_mem + b->mem, NULL, 0, b->size, 1, 0};
        cs_cmd_push_var(x_buf_upload_copy, &cmd, sizeof cmd, g_mem + b->mem, b->size,
                        offsetof(cmd_buf_upload_t, data), offsetof(cmd_buf_upload_t, heap));
    } else {
        // Fallback при занятых слотах или static-буфере с DISCARD/NOOVERWRITE: копируем в кольцо только [off, off+len)
        cmd_buf_upload_t cmd = {b, g_mem + b->mem + off, NULL, off, len, orphan, discard};
        cs_cmd_push_var(x_buf_upload_copy, &cmd, sizeof cmd, g_mem + b->mem + off, len,
                        offsetof(cmd_buf_upload_t, data), offsetof(cmd_buf_upload_t, heap));
    }
}

void d3d8_buf_sync(d3dbuf *b)
{
    // Заливка происходит на Unlock(); здесь только защитный сброс, если приложение забыло Unlock перед Draw.
    if (b && b->locked)
        buf_unlock_flush(b);
}

#define BUF(c) ((d3dbuf *)com_host(ARG(0)))

static void buf_GetType(CPU *c) { RET(1, BUF(c)->index ? RTYPE_INDEXBUFFER : RTYPE_VERTEXBUFFER); }

static void buf_Lock(CPU *c)  // (OffsetToLock, SizeToLock, ppbData, Flags)
{
    d3dbuf *b = BUF(c);
    u32 off = ARG(1), size = ARG(2), flags = ARG(4);
    if (off > b->size)
        RET(5, D3DERR_INVALIDCALL);
    if (!size || off + size > b->size)
        size = b->size - off;
    if (b->locked)
        buf_unlock_flush(b);
    b->locked = 1;
    b->lock_off = off;
    b->lock_size = size;
    b->lock_flags = flags;
    b->lock_zc = 0;
    u32 base = b->mem;
    if (flags & LOCK_READONLY) {
        base = b->cur_zslot >= 0 ? b->zmem[b->cur_zslot] : b->mem;
    } else if (b->nzslots > 0 && (flags & (LOCK_DISCARD | LOCK_NOOVERWRITE))) {
        uint64_t tail = cs_tail_pos();
        int k = d3d8_dyn_slot_pick(b->nzslots, b->cur_zslot, b->cur_zhi, b->zfence, tail, off, flags);
        if (k >= 0) {
            if (flags & LOCK_DISCARD) {
                b->cur_zslot = k;
                b->cur_zhi = 0;
            }
            b->lock_zc = 1;
            base = b->zmem[k];
        } else {
            if (flags & LOCK_DISCARD) {
                b->cur_zslot = -1;
                b->cur_zhi = 0;
            }
            base = b->mem;
        }
    } else if (!(flags & (LOCK_DISCARD | LOCK_NOOVERWRITE)) && b->cur_zslot >= 0) {
        // Редкий Lock(0) после zero-copy DISCARD: переносим актуальное содержимое слота в b->mem (только чтение слота)
        memcpy(g_mem + b->mem, g_mem + b->zmem[b->cur_zslot], b->size);
        b->cur_zslot = -1;
        b->cur_zhi = 0;
        base = b->mem;
    }
    wr32(g_mem, ARG(3), base + off);
    RET(5, D3D_OK);
}

static void buf_Unlock(CPU *c)
{
    buf_unlock_flush(BUF(c));
    RET(1, D3D_OK);
}

// D3DVERTEXBUFFER_DESC: Format, Type, Usage, Pool, Size, FVF; D3DINDEXBUFFER_DESC: Format, Type, Usage, Pool, Size
static void buf_GetDesc(CPU *c)
{
    d3dbuf *b = BUF(c);
    u32 p = ARG(1);
    wr32(g_mem, p, b->index ? b->fmt : FMT_VERTEXDATA);
    wr32(g_mem, p + 4, b->index ? RTYPE_INDEXBUFFER : RTYPE_VERTEXBUFFER);
    wr32(g_mem, p + 8, b->usage);
    wr32(g_mem, p + 12, b->pool);
    wr32(g_mem, p + 16, b->size);
    if (!b->index)
        wr32(g_mem, p + 20, b->fvf);
    RET(2, D3D_OK);
}

// ================================================================ vtables
void d3d8_res_init(u32 device_obj)
{
    g_dev_obj = device_obj;
    if (g_vt_tex)
        return;
#define RES_METHODS(get_type)                                                                                  \
    res_QueryInterface, com_AddRef, com_Release, res_GetDevice, res_SetPrivateData, res_GetPrivateData,        \
        res_FreePrivateData, res_SetPriority, res_GetPriority, res_PreLoad, get_type
    guest_fn tex[19] = {RES_METHODS(tex_GetType), tex_SetLOD, tex_GetLOD, tex_GetLevelCount, tex_GetLevelDesc,
                        tex_GetSurfaceLevel, tex_LockRect, tex_UnlockRect, tex_AddDirtyRect};
    g_vt_tex = com_vtable("IDirect3DTexture8", tex, 19);
    guest_fn cube[19] = {RES_METHODS(tex_GetType), tex_SetLOD, tex_GetLOD, tex_GetLevelCount, tex_GetLevelDesc,
                         cube_GetCubeMapSurface, cube_LockRect, cube_UnlockRect, cube_AddDirtyRect};
    g_vt_cube = com_vtable("IDirect3DCubeTexture8", cube, 19);
    guest_fn vb[14] = {RES_METHODS(buf_GetType), buf_Lock, buf_Unlock, buf_GetDesc};
    g_vt_vb = com_vtable("IDirect3DVertexBuffer8", vb, 14);
    g_vt_ib = com_vtable("IDirect3DIndexBuffer8", vb, 14);
#undef RES_METHODS
    guest_fn surf[11] = {res_QueryInterface, surf_AddRef,    surf_Release,  res_GetDevice,
                         res_SetPrivateData, res_GetPrivateData, res_FreePrivateData, surf_GetContainer,
                         surf_GetDesc,       surf_LockRect,  surf_UnlockRect};
    g_vt_surf = com_vtable("IDirect3DSurface8", surf, 11);
}
